#if defined(_WIN32) || defined(_WIN64)
#define _CRT_SECURE_NO_WARNINGS
//#ifdef _MSC_VER
#include <windows.h>
#define strcasecmp _stricmp
#else
#include <dlfcn.h>
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"

// Conflicts with pyconfig.h, Windows-only?
#if defined(_WIN32) || defined(_WIN64)
#define pid_t dummy_pid_t
#include <tmenv.h>
#undef pid_t
#else
#include <tmenv.h>
#endif

#include <atmi.h>
#include <tpadm.h>
#include <userlog.h>
#include <xa.h>
#undef _
#pragma GCC diagnostic pop

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <functional>
#include <map>

namespace py = pybind11;

struct xatmi_exception : public std::exception {
 private:
  int code_;
  std::string message_;

 public:
  explicit xatmi_exception(int code)
      : code_(code), message_(tpstrerror(code)) {}

  const char *what() const noexcept override { return message_.c_str(); }
  int code() const noexcept { return code_; }
};

struct fml32_exception : public std::exception {
 private:
  int code_;
  std::string message_;

 public:
  explicit fml32_exception(int code)
      : code_(code), message_(Fstrerror32(code)) {}

  const char *what() const noexcept override { return message_.c_str(); }
  int code() const noexcept { return code_; }
};

static PyObject *TuxedoException_tp_str(PyObject *selfPtr) {
  py::str ret;
  try {
    py::handle self(selfPtr);
    py::tuple args = self.attr("args");
    ret = py::str(args[0]);
  } catch (py::error_already_set &e) {
    ret = "";
  }

  ret.inc_ref();
  return ret.ptr();
}

static PyObject *TuxedoException_code(PyObject *selfPtr, void *closure) {
  try {
    py::handle self(selfPtr);
    py::tuple args = self.attr("args");
    py::object code = args[1];
    code.inc_ref();
    return code.ptr();
  } catch (py::error_already_set &e) {
    py::none ret;
    ret.inc_ref();
    return ret.ptr();
  }
}

static PyGetSetDef TuxedoException_getsetters[] = {
    {const_cast<char *>("code"), TuxedoException_code, nullptr, nullptr,
     nullptr},
    {nullptr}};

struct client {
  client() {}
  // TODO: real params
  explicit client(bool client) {
    TPCONTEXT_T context;
    if (tpgetctxt(&context, 0) != -1 && context >= 0) {
      return;
    }
    _init(
        [client](TPINIT *tpinfo) {
          if (client) {
            if (tpinit(tpinfo) == -1) {
              throw xatmi_exception(tperrno);
            }
          } else {
            if (tpappthrinit(tpinfo) == -1) {
              throw xatmi_exception(tperrno);
            }
          }
        },
        nullptr, "tpsysadm", nullptr, nullptr, TPMULTICONTEXTS);
  }
  client(std::function<void(TPINIT *)> f, const char *usrname,
         const char *cltname, const char *passwd, const char *grpname,
         long flags) {
    _init(f, usrname, cltname, passwd, grpname, flags);
  }

  void _init(std::function<void(TPINIT *)> f, const char *usrname,
             const char *cltname, const char *passwd, const char *grpname,
             long flags) {
    std::unique_ptr<char, decltype(&tpfree)> guard(
        tpalloc(const_cast<char *>("TPINIT"), nullptr, TPINITNEED(16)),
        &tpfree);
    TPINIT *tpinfo = reinterpret_cast<TPINIT *>(guard.get());
    memset(tpinfo, 0, sizeof(*tpinfo));

    if (usrname != nullptr) {
      strncpy(tpinfo->usrname, usrname, sizeof(tpinfo->usrname));
    }
    if (cltname != nullptr) {
      strncpy(tpinfo->cltname, cltname, sizeof(tpinfo->cltname));
    }
    if (passwd != nullptr) {
      strncpy(tpinfo->passwd, passwd, sizeof(tpinfo->passwd));
    }
    if (grpname != nullptr) {
      strncpy(tpinfo->grpname, grpname, sizeof(tpinfo->grpname));
    }
    tpinfo->flags = flags;
    f(tpinfo);
  }

  ~client() {}
  client(const client &) = delete;
  client &operator=(const client &) = delete;
  client(client &&) = delete;
  client &operator=(client &&) = delete;
};

struct svcresult {
  int rval;
  long rcode;
  char *odata;
  long olen;
  char name[XATMI_SERVICE_NAME_LENGTH];
  bool forward;
  bool clean;
};

struct xatmibuf {
  xatmibuf() : pp(&p), len(0), p(nullptr) {}
  xatmibuf(TPSVCINFO *svcinfo)
      : pp(&svcinfo->data), len(svcinfo->len), p(nullptr) {}
  xatmibuf(const char *type, long len) : pp(&p), len(len), p(nullptr) {
    reinit(type, len);
  }
  void reinit(const char *type, long len_) {
    if (*pp == nullptr) {
      len = len_;
      *pp = tpalloc(const_cast<char *>(type), nullptr, len);
      if (*pp == nullptr) {
        throw std::bad_alloc();
      }
    } else {
      FBFR32 *fbfr = reinterpret_cast<FBFR32 *>(*pp);
      Finit32(fbfr, Fsizeof32(fbfr));
    }
  }
  xatmibuf(xatmibuf &&other) : xatmibuf() { swap(other); }
  xatmibuf &operator=(xatmibuf &&other) {
    swap(other);
    return *this;
  }
  ~xatmibuf() {
    if (p != nullptr) {
      tpfree(p);
    }
  }

  xatmibuf(const xatmibuf &) = delete;
  xatmibuf &operator=(const xatmibuf &) = delete;

  char *release() {
    char *ret = p;
    p = nullptr;
    return ret;
  }

  FBFR32 **fbfr() { return reinterpret_cast<FBFR32 **>(pp); }

  char **pp;
  long len;

  void mutate(std::function<int(FBFR32 *)> f) {
    while (true) {
      int rc = f(*fbfr());
      if (rc == -1) {
        if (Ferror32 == FNOSPACE) {
          len *= 2;
          *pp = tprealloc(*pp, len);
        } else {
          throw fml32_exception(Ferror32);
        }
      } else {
        break;
      }
    }
  }

  char *p;

 private:
  void swap(xatmibuf &other) noexcept {
    std::swap(p, other.p);
    std::swap(len, other.len);
  }
};

struct pytpreply {
  int rval;
  long rcode;
  py::object data;
  int cd;

  pytpreply(int rval, long rcode, py::object data, int cd = -1)
      : rval(rval), rcode(rcode), data(data), cd(cd) {}
};

static py::object server;
static thread_local std::unique_ptr<client> tclient;
static thread_local svcresult tsvcresult;

static void default_client() {
  if (!tclient) {
    tclient.reset(new client(server.ptr() == nullptr));
  }
}

static py::object to_py(FBFR32 *fbfr, FLDLEN32 buflen = 0) {
  FLDID32 fieldid = FIRSTFLDID;
  FLDOCC32 oc = 0;

  py::dict result;
  py::list val;

  if (buflen == 0) {
    buflen = Fsizeof32(fbfr);
  }
  std::unique_ptr<char> value(new char[buflen]);

  for (;;) {
    FLDLEN32 len = buflen;

    int r = Fnext32(fbfr, &fieldid, &oc, value.get(), &len);
    if (r == -1) {
      throw fml32_exception(Ferror32);
    } else if (r == 0) {
      break;
    }

    if (oc == 0) {
      val = py::list();

      char *name = Fname32(fieldid);
      if (name != nullptr) {
        result[name] = val;
      } else {
        result[py::int_(fieldid)] = val;
      }
    }

    switch (Fldtype32(fieldid)) {
      case FLD_CHAR:
        val.append(py::cast(value.get()[0]));
        break;
      case FLD_SHORT:
        val.append(py::cast(*reinterpret_cast<short *>(value.get())));
        break;
      case FLD_LONG:
        val.append(py::cast(*reinterpret_cast<long *>(value.get())));
        break;
      case FLD_FLOAT:
        val.append(py::cast(*reinterpret_cast<float *>(value.get())));
        break;
      case FLD_DOUBLE:
        val.append(py::cast(*reinterpret_cast<double *>(value.get())));
        break;
      case FLD_STRING:
        val.append(
#if PY_MAJOR_VERSION >= 3
            py::str(PyUnicode_DecodeLocale(value.get(), "surrogateescape"))
#else
            py::bytes(value.get(), len - 1)
#endif
        );
        break;
      case FLD_CARRAY:
        val.append(py::bytes(value.get(), len));
        break;
      case FLD_FML32:
        val.append(to_py(reinterpret_cast<FBFR32 *>(value.get()), buflen));
        break;
      default:
        throw std::invalid_argument("Unsupported field " +
                                    std::to_string(fieldid));
    }
  }
  return result;
}

static py::object to_py(xatmibuf buf) {
  char type[8];
  char subtype[16];
  if (tptypes(*buf.pp, type, subtype) == -1) {
    throw std::invalid_argument("Invalid buffer type");
  }
  if (strcmp(type, "STRING") == 0) {
    return py::cast(*buf.pp);
  } else if (strcmp(type, "CARRAY") == 0 || strcmp(type, "X_OCTET") == 0) {
    return py::bytes(*buf.pp, buf.len);
  } else if (strcmp(type, "FML32") == 0) {
    return to_py(*buf.fbfr());
  } else {
    throw std::invalid_argument("Unsupported buffer type");
  }
}

static void from_py(py::dict obj, xatmibuf &b);
static void from_py1(xatmibuf &buf, FLDID32 fieldid, FLDOCC32 oc,
                     py::handle obj, xatmibuf &b) {
  if (obj.is_none()) {
    // pass
#if PY_MAJOR_VERSION >= 3
  } else if (py::isinstance<py::bytes>(obj)) {
    std::string val(PyBytes_AsString(obj.ptr()), PyBytes_Size(obj.ptr()));

    buf.mutate([&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fieldid, oc, const_cast<char *>(val.data()),
                     val.size(), FLD_CARRAY);
    });
#endif
  } else if (py::isinstance<py::str>(obj)) {
#if PY_MAJOR_VERSION >= 3
    py::bytes b = py::reinterpret_steal<py::bytes>(
        PyUnicode_EncodeLocale(obj.ptr(), "surrogateescape"));
    std::string val(PyBytes_AsString(b.ptr()), PyBytes_Size(b.ptr()));
#else
    if (PyUnicode_Check(obj.ptr())) {
      obj = PyUnicode_AsEncodedString(obj.ptr(), "utf-8", "surrogateescape");
    }
    std::string val(PyString_AsString(obj.ptr()), PyString_Size(obj.ptr()));
#endif
    buf.mutate([&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fieldid, oc, const_cast<char *>(val.data()),
                     val.size(), FLD_CARRAY);
    });
  } else if (py::isinstance<py::int_>(obj)) {
    long val = obj.cast<py::int_>();
    buf.mutate([&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fieldid, oc, reinterpret_cast<char *>(&val), 0,
                     FLD_LONG);
    });

  } else if (py::isinstance<py::float_>(obj)) {
    double val = obj.cast<py::float_>();
    buf.mutate([&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fieldid, oc, reinterpret_cast<char *>(&val), 0,
                     FLD_DOUBLE);
    });
  } else if (py::isinstance<py::dict>(obj)) {
    from_py(obj.cast<py::dict>(), b);
    buf.mutate([&](FBFR32 *fbfr) {
      return Fchg32(fbfr, fieldid, oc, reinterpret_cast<char *>(*b.fbfr()), 0);
    });
  } else {
    throw std::invalid_argument("Unsupported type");
  }
}

static void from_py(py::dict obj, xatmibuf &b) {
  b.reinit("FML32", 1024);
  xatmibuf f;

  for (auto it : obj) {
    FLDID32 fieldid;
    if (py::isinstance<py::int_>(it.first)) {
      fieldid = it.first.cast<py::int_>();
    } else {
      fieldid =
          Fldid32(const_cast<char *>(std::string(py::str(it.first)).c_str()));
    }

    py::handle o = it.second;
    if (py::isinstance<py::list>(o)) {
      FLDOCC32 oc = 0;
      for (auto e : o.cast<py::list>()) {
        from_py1(b, fieldid, oc++, e, f);
      }
    } else {
      // Handle single elements instead of lists for convenience
      from_py1(b, fieldid, 0, o, f);
    }
  }
}

static xatmibuf from_py(py::object obj) {
  if (py::isinstance<py::bytes>(obj)) {
    xatmibuf buf("CARRAY", PyBytes_Size(obj.ptr()));
    memcpy(*buf.pp, PyBytes_AsString(obj.ptr()), PyBytes_Size(obj.ptr()));
    return buf;
  } else if (py::isinstance<py::str>(obj)) {
    std::string s = py::str(obj);
    xatmibuf buf("STRING", s.size() + 1);
    strcpy(*buf.pp, s.c_str());
    return buf;
  } else if (py::isinstance<py::dict>(obj)) {
    xatmibuf buf("FML32", 1024);

    from_py(static_cast<py::dict>(obj), buf);

    return buf;
  } else {
    throw std::invalid_argument("Unsupported buffer type");
  }
}

static void pytpreturn(int rval, long rcode, py::object data, long flags) {
  if (!tsvcresult.clean) {
    throw std::runtime_error("tpreturn already called");
  }
  tsvcresult.clean = false;
  tsvcresult.rval = rval;
  tsvcresult.rcode = rcode;
  auto &&odata = from_py(data);
  tsvcresult.olen = odata.len;
  tsvcresult.odata = odata.release();
  tsvcresult.forward = false;
}
static void pytpforward(const std::string &svc, py::object data, long flags) {
  if (!tsvcresult.clean) {
    throw std::runtime_error("tpreturn already called");
  }
  tsvcresult.clean = false;
  strncpy(tsvcresult.name, svc.c_str(), sizeof(tsvcresult.name));
  auto &&odata = from_py(data);
  tsvcresult.olen = odata.len;
  tsvcresult.odata = odata.release();
  tsvcresult.forward = true;
}

static py::object pytpexport(py::object idata, long flags) {
  auto in = from_py(idata);
  std::vector<char> ostr;
  ostr.resize(512 + in.len * 2);

  long olen = ostr.capacity();
  int rc = tpexport(in.p, in.len, &ostr[0], &olen, flags);
  if (rc == -1) {
    throw xatmi_exception(tperrno);
  }

  if (flags == 0) {
    return py::bytes(&ostr[0], olen);
  }
  return py::str(&ostr[0]);
}

static py::object pytpimport(const std::string istr, long flags) {
  xatmibuf obuf("FML32", istr.size());

  long olen = 0;
  int rc = tpimport(const_cast<char *>(istr.c_str()), istr.size(), obuf.pp,
                    &olen, flags);
  if (rc == -1) {
    throw xatmi_exception(tperrno);
  }

  return to_py(std::move(obuf));
}

static void pytppost(const std::string eventname, py::object data, long flags) {
  auto in = from_py(data);

  {
    py::gil_scoped_release release;
    int rc =
        tppost(const_cast<char *>(eventname.c_str()), *in.pp, in.len, flags);
    if (rc == -1) {
      throw xatmi_exception(tperrno);
    }
  }
}

static pytpreply pytpcall(const char *svc, py::object idata, long flags) {
  default_client();
  auto in = from_py(idata);
  xatmibuf out("FML32", 1024);
  {
    py::gil_scoped_release release;
    int rc = tpcall(const_cast<char *>(svc), *in.pp, in.len, out.pp, &out.len,
                    flags);
    if (rc == -1) {
      if (tperrno != TPESVCFAIL) {
        throw xatmi_exception(tperrno);
      }
    }
  }
  return pytpreply(tperrno, tpurcode, to_py(std::move(out)));
}

static pytpreply pytpadmcall(py::object idata, long flags) {
  auto in = from_py(idata);
  xatmibuf out("FML32", 1024);
  {
    py::gil_scoped_release release;
    int rc = tpadmcall(*in.fbfr(), out.fbfr(), flags);
    if (rc == -1) {
      if (tperrno != TPESVCFAIL) {
        throw xatmi_exception(tperrno);
      }
    }
  }
  return pytpreply(tperrno, 0, to_py(std::move(out)));
}

static int pytpacall(const char *svc, py::object idata, long flags) {
  default_client();
  auto in = from_py(idata);

  py::gil_scoped_release release;
  int rc = tpacall(const_cast<char *>(svc), *in.pp, in.len, flags);
  if (rc == -1) {
    throw xatmi_exception(tperrno);
  }
  return rc;
}

static pytpreply pytpgetrply(int cd, long flags) {
  default_client();
  xatmibuf out("FML32", 1024);
  {
    py::gil_scoped_release release;
    int rc = tpgetrply(&cd, out.pp, &out.len, flags);
    if (rc == -1) {
      if (tperrno != TPESVCFAIL) {
        throw xatmi_exception(tperrno);
      }
    }
  }
  return pytpreply(tperrno, tpurcode, to_py(std::move(out)), cd);
}

int tpsvrinit(int argc, char *argv[]) {
  if (!tclient) {
    tclient.reset(new client());
  }
  if (tpopen() == -1) {
    userlog(const_cast<char *>("Failed tpopen() = %d / %s"), tperrno,
            tpstrerror(tperrno));
    return -1;
  }
  py::gil_scoped_acquire acquire;
  if (hasattr(server, __func__)) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
      args.push_back(argv[i]);
    }
    return server.attr(__func__)(args).cast<int>();
  }
  return 0;
}
void tpsvrdone() {
  py::gil_scoped_acquire acquire;
  if (hasattr(server, __func__)) {
    server.attr(__func__)();
  }
}
int tpsvrthrinit(int argc, char *argv[]) {
  if (!tclient) {
    tclient.reset(new client());
  }
  if (tpopen() == -1) {
    userlog(const_cast<char *>("Failed tpopen() = %d / %s"), tperrno,
            tpstrerror(tperrno));
    return -1;
  }
  py::gil_scoped_acquire acquire;
  if (hasattr(server, __func__)) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
      args.push_back(argv[i]);
    }
    return server.attr(__func__)(args).cast<int>();
  }
  return 0;
}
void tpsvrthrdone() {
  py::gil_scoped_acquire acquire;
  if (hasattr(server, __func__)) {
    server.attr(__func__)();
  }
}
void PY(TPSVCINFO *svcinfo) {
  if (!tclient) {
    tclient.reset(new client());
  }
  tsvcresult.clean = true;

  try {
    py::gil_scoped_acquire acquire;
    auto idata = to_py(xatmibuf(svcinfo));

    auto &&func = server.attr(svcinfo->name);
    auto &&argspec = py::module::import("inspect").attr("getargspec")(func);
    auto &&args = argspec.attr("args");
    py::dict kwargs;
    if (args.contains(py::str("name"))) {
      kwargs[py::str("name")] = py::str(svcinfo->name);
    }
    if (args.contains(py::str("flags"))) {
      kwargs[py::str("flags")] = py::int_(svcinfo->flags);
    }
    if (args.contains(py::str("cd"))) {
      kwargs[py::str("cd")] = py::int_(svcinfo->cd);
    }
    if (args.contains(py::str("appkey"))) {
      kwargs[py::str("appkey")] = py::int_(svcinfo->appkey);
    }
    if (args.contains(py::str("cltid"))) {
      kwargs[py::str("cltid")] = py::bytes(
          reinterpret_cast<char *>(&svcinfo->cltid), sizeof(svcinfo->cltid));
    }

    func(idata, **kwargs);

    if (tsvcresult.clean) {
      userlog(const_cast<char *>("tpreturn() not called"));
      tpreturn(TPEXIT, 0, nullptr, 0, 0);
    }
  } catch (const std::exception &e) {
    userlog(const_cast<char *>("%s"), e.what());
    tpreturn(TPEXIT, 0, nullptr, 0, 0);
  }

  if (tsvcresult.forward) {
    tpforward(tsvcresult.name, tsvcresult.odata, tsvcresult.olen, 0);
  } else {
    tpreturn(tsvcresult.rval, tsvcresult.rcode, tsvcresult.odata,
             tsvcresult.olen, 0);
  }
}

static void pytpadvertisex(std::string svcname, long flags) {
#if defined(TPSINGLETON) && defined(TPSECONDARYRQ)
  if (tpadvertisex(const_cast<char *>(svcname.c_str()), PY, flags) == -1) {
#else
  if (flags != 0) {
    throw std::invalid_argument("flags not supported");
  }
  if (tpadvertise(const_cast<char *>(svcname.c_str()), PY) == -1) {
#endif
    throw xatmi_exception(tperrno);
  }
}

extern "C" {
int _tmrunserver(int);
extern struct xa_switch_t xaosw;
extern struct xa_switch_t tmnull_switch;
extern int _tmbuilt_with_thread_option;
}

static struct tmdsptchtbl_t _tmdsptchtbl[] = {
    {(char *)"", (char *)"PY", PY, 0, 0}, {nullptr, nullptr, nullptr, 0, 0}};

static struct tmsvrargs_t tmsvrargs = {
    nullptr,      &_tmdsptchtbl[0], 0,           tpsvrinit, tpsvrdone,
    _tmrunserver, nullptr,          nullptr,     nullptr,   nullptr,
    tprminit,     tpsvrthrinit,     tpsvrthrdone};

typedef void *(xao_svc_ctx)(void *);
static xao_svc_ctx *xao_svc_ctx_ptr;
struct tmsvrargs_t *_tmgetsvrargs(const char *rmname) {
  tmsvrargs.reserved1 = nullptr;
  tmsvrargs.reserved2 = nullptr;
  // tmsvrargs.xa_switch = &xaosw;
  if (strcasecmp(rmname, "NONE") == 0) {
    tmsvrargs.xa_switch = &tmnull_switch;
#if defined(_WIN32) || defined(_WIN64)
#else
  } else if (strcasecmp(rmname, "Oracle_XA") == 0) {
    const char *orahome = getenv("ORACLE_HOME");
    auto lib =
        std::string((orahome == nullptr ? "" : orahome)) + "/lib/libclntsh.so";
    void *handle = dlopen(lib.c_str(), RTLD_NOW);
    if (!handle) {
      throw std::runtime_error(
          std::string("Failed loading $ORACLE_HOME/lib/libclntsh.so ") +
          dlerror());
    }
    tmsvrargs.xa_switch =
        reinterpret_cast<xa_switch_t *>(dlsym(handle, "xaosw"));
    if (tmsvrargs.xa_switch == nullptr) {
      throw std::runtime_error("xa_switch_t named xaosw not found");
    }
    xao_svc_ctx_ptr =
        reinterpret_cast<xao_svc_ctx *>(dlsym(handle, "xaoSvcCtx"));
    if (xao_svc_ctx_ptr == nullptr) {
      throw std::runtime_error("xa_switch_t named xaosw not found");
    }
#endif
  } else {
    throw std::invalid_argument("Unsupported Resource Manager");
  }
  return &tmsvrargs;
}

static void pyrun(py::object svr, std::vector<std::string> args,
                  const char *rmname) {
  server = svr;
  try {
    py::gil_scoped_release release;
    _tmbuilt_with_thread_option = 1;
    std::vector<char *> argv(args.size());
    for (size_t i = 0; i < args.size(); i++) {
      argv[i] = const_cast<char *>(args[i].c_str());
    }
    (void)_tmstartserver(args.size(), &argv[0], _tmgetsvrargs(rmname));
    server = py::none();
  } catch (...) {
    server = py::none();
    throw;
  }
}

PYBIND11_MODULE(tuxedo, m) {
  // Poor man's namedtuple
  py::class_<pytpreply>(m, "TpReply")
      .def_readonly("rval", &pytpreply::rval)
      .def_readonly("rcode", &pytpreply::rcode)
      .def_readonly("data", &pytpreply::data)
      .def_readonly("cd", &pytpreply::cd)  // Does not unpack as the use is
                                           // rare case of tpgetrply(TPGETANY)
      .def("__getitem__", [](const pytpreply &s, size_t i) -> py::object {
        if (i == 0) {
          return py::int_(s.rval);
        } else if (i == 1) {
          return py::int_(s.rcode);
        } else if (i == 2) {
          return s.data;
        } else {
          throw py::index_error();
        }
      });

  static PyObject *XatmiException =
      PyErr_NewException("tuxedo.XatmiException", nullptr, nullptr);

  if (XatmiException) {
    PyTypeObject *as_type = reinterpret_cast<PyTypeObject *>(XatmiException);
    as_type->tp_str = TuxedoException_tp_str;
    PyObject *descr = PyDescr_NewGetSet(as_type, TuxedoException_getsetters);
    auto dict = py::reinterpret_borrow<py::dict>(as_type->tp_dict);
    dict[py::handle(((PyDescrObject *)(descr))->d_name)] = py::handle(descr);

    Py_XINCREF(XatmiException);
    m.add_object("XatmiException", py::handle(XatmiException));
  }
  static PyObject *Fml32Exception =
      PyErr_NewException("tuxedo.Fml32Exception", nullptr, nullptr);
  if (Fml32Exception) {
    PyTypeObject *as_type = reinterpret_cast<PyTypeObject *>(Fml32Exception);
    as_type->tp_str = TuxedoException_tp_str;
    PyObject *descr = PyDescr_NewGetSet(as_type, TuxedoException_getsetters);
    auto dict = py::reinterpret_borrow<py::dict>(as_type->tp_dict);
    dict[py::handle(((PyDescrObject *)(descr))->d_name)] = py::handle(descr);

    Py_XINCREF(Fml32Exception);
    m.add_object("Fml32Exception", py::handle(Fml32Exception));
  }

  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) {
        std::rethrow_exception(p);
      }
    } catch (const xatmi_exception &e) {
      py::tuple args(2);
      args[0] = e.what();
      args[1] = e.code();
      PyErr_SetObject(XatmiException, args.ptr());
    } catch (const fml32_exception &e) {
      py::tuple args(2);
      args[0] = e.what();
      args[1] = e.code();
      PyErr_SetObject(Fml32Exception, args.ptr());
    }
  });

  m.def(
      "xaoSvcCtx",
      []() {
        if (xao_svc_ctx_ptr == nullptr) {
          throw std::runtime_error("xaoSvcCtx is null");
        }
        return reinterpret_cast<unsigned long long>(
            (*xao_svc_ctx_ptr)(nullptr));
      },
      "Returns the OCI service handle for a given XA connection");

  m.def(
      "tpinit",
      [](const char *usrname, const char *cltname, const char *passwd,
         const char *grpname, long flags) {
        py::gil_scoped_release release;
        tclient.reset(new client(
            [](TPINIT *tpinfo) {
              if (tpinit(tpinfo) == -1) {
                throw xatmi_exception(tperrno);
              }
            },
            usrname, cltname, passwd, grpname, flags));
      },
      "Joins an application", py::arg("usrname") = nullptr,
      py::arg("cltname") = nullptr, py::arg("passwd") = nullptr,
      py::arg("grpname") = nullptr, py::arg("flags") = 0);

  m.def(
      "tpterm",
      []() {
        py::gil_scoped_release release;
        tclient.reset();
        if (tpterm() == -1) {
          throw xatmi_exception(tperrno);
        }
      },
      "Leaves an application");

  m.def(
      "tpappthrinit",
      [](const char *usrname, const char *cltname, const char *passwd,
         const char *grpname, long flags) {
        py::gil_scoped_release release;
        tclient.reset(new client(
            [](TPINIT *tpinfo) {
              if (tpappthrinit(tpinfo) == -1) {
                throw xatmi_exception(tperrno);
              }
            },
            usrname, cltname, passwd, grpname, flags));
      },
      "Joins an application", py::arg("usrname") = nullptr,
      py::arg("cltname") = nullptr, py::arg("passwd") = nullptr,
      py::arg("grpname") = nullptr, py::arg("flags") = 0);

  m.def(
      "tpappthrterm",
      []() {
        py::gil_scoped_release release;
        tclient.reset();
        if (tpappthrterm() == -1) {
          throw xatmi_exception(tperrno);
        }
      },
      "Routine for creating and initializing a new Tuxedo context in an "
      "application-created server thread.");

  m.def(
      "tpbegin",
      [](unsigned long timeout, long flags) {
        py::gil_scoped_release release;
        if (tpbegin(timeout, flags) == -1) {
          throw xatmi_exception(tperrno);
        }
      },
      "Routine for beginning a transaction", py::arg("timeout"),
      py::arg("flags") = 0);

  m.def(
      "tpcommit",
      [](long flags) {
        py::gil_scoped_release release;
        if (tpcommit(flags) == -1) {
          throw xatmi_exception(tperrno);
        }
      },
      "Routine for committing current transaction", py::arg("flags") = 0);

  m.def(
      "tpabort",
      [](long flags) {
        py::gil_scoped_release release;
        if (tpabort(flags) == -1) {
          throw xatmi_exception(tperrno);
        }
      },
      "Routine for aborting current transaction", py::arg("flags") = 0);

  m.def(
      "tpgetlev",
      []() {
        int rc;
        if ((rc = tpgetlev()) == -1) {
          throw xatmi_exception(tperrno);
        }
        return py::bool_(rc);
      },
      "Routine for checking if a transaction is in progress");

  m.def(
      "userlog",
      [](const char *message) {
        py::gil_scoped_release release;
        userlog(const_cast<char *>("%s"), message);
      },
      "Writes a message to the Oracle Tuxedo ATMI system central event log",
      py::arg("message"));

#if defined(TPSINGLETON) && defined(TPSECONDARYRQ)
  m.def("tpadvertisex", &pytpadvertisex,
        "Routine for advertising a service with unique service name in a "
        "domain, or advertising a service on the secondary request queue of a "
        "Tuxedo server.",
        py::arg("svcname"), py::arg("flags") = 0);
#endif
  m.def(
      "tpadvertise",
      [](const char *svcname) { pytpadvertisex(svcname, 0); },
      "Routine for advertising a service name", py::arg("svcname"));

  m.def("run", &pyrun, "Run Tuxedo server", py::arg("server"), py::arg("args"),
        py::arg("rmname") = "NONE");

  m.def("tpcall", &pytpcall,
        "Routine for sending service request and awaiting its reply",
        py::arg("svc"), py::arg("idata"), py::arg("flags") = 0);

  m.def("tpadmcall", &pytpadmcall, "Administers unbooted application",
        py::arg("idata"), py::arg("flags") = 0);

  m.def("tpacall", &pytpacall, "Routine for sending a service request",
        py::arg("svc"), py::arg("idata"), py::arg("flags") = 0);
  m.def("tpgetrply", &pytpgetrply,
        "Routine for getting a reply from a previous request", py::arg("cd"),
        py::arg("flags") = 0);

  m.def("tpreturn", &pytpreturn, "Routine for returning from a service routine",
        py::arg("rval"), py::arg("rcode"), py::arg("data"),
        py::arg("flags") = 0);
  m.def("tpforward", &pytpforward,
        "Routine for forwarding a service request to another service routine",
        py::arg("svc"), py::arg("data"), py::arg("flags") = 0);

  m.def("tpexport", &pytpexport,
        "Converts a typed message buffer into an exportable, "
        "machine-independent string representation, that includes digital "
        "signatures and encryption seals",
        py::arg("ibuf"), py::arg("flags") = 0);
  m.def("tpimport", &pytpimport,
        "Converts an exported representation back into a typed message buffer",
        py::arg("istr"), py::arg("flags") = 0);

  m.def("tppost", &pytppost, "Posts an event", py::arg("eventname"),
        py::arg("data"), py::arg("flags") = 0);

  m.def(
      "Fldtype32", [](FLDID32 fieldid) { return Fldtype32(fieldid); },
      "Maps field identifier to field type", py::arg("fieldid"));
  m.def(
      "Fldno32", [](FLDID32 fieldid) { return Fldno32(fieldid); },
      "Maps field identifier to field number", py::arg("fieldid"));
  m.def(
      "Fmkfldid32", [](int type, FLDID32 num) { return Fmkfldid32(type, num); },
      "Makes a field identifier", py::arg("type"), py::arg("num"));

  m.def(
      "Fname32",
      [](FLDID32 fieldid) {
        auto *name = Fname32(fieldid);
        if (name == nullptr) {
          throw fml32_exception(Ferror32);
        }
        return name;
      },
      "Maps field identifier to field name", py::arg("fieldid"));
  m.def(
      "Fldid32",
      [](const char *name) {
        auto id = Fldid32(const_cast<char *>(name));
        if (id == BADFLDID) {
          throw fml32_exception(Ferror32);
        }
        return id;
      },
      "Maps field name to field identifier", py::arg("name"));

  m.def(
      "Fboolev32",
      [](py::object fbfr, const char *expression) {
        std::unique_ptr<char, decltype(&free)> guard(
            Fboolco32(const_cast<char *>(expression)), &free);
        if (guard.get() == nullptr) {
          throw fml32_exception(Ferror32);
        }
        auto buf = from_py(fbfr);
        auto rc = Fboolev32(*buf.fbfr(), guard.get());
        if (rc == -1) {
          throw fml32_exception(Ferror32);
        }
        return rc == 1;
      },
      "Evaluates buffer against expression", py::arg("fbfr"),
      py::arg("expression"));

  m.def(
      "Ffloatev32",
      [](py::object fbfr, const char *expression) {
        std::unique_ptr<char, decltype(&free)> guard(
            Fboolco32(const_cast<char *>(expression)), &free);
        if (guard.get() == nullptr) {
          throw fml32_exception(Ferror32);
        }
        auto buf = from_py(fbfr);
        auto rc = Ffloatev32(*buf.fbfr(), guard.get());
        if (rc == -1) {
          throw fml32_exception(Ferror32);
        }
        return rc;
      },
      "Returns value of expression as a double", py::arg("fbfr"),
      py::arg("expression"));

  m.def(
      "Ffprint32",
      [](py::object fbfr, py::object iop) {
        auto buf = from_py(fbfr);
        int fd = iop.attr("fileno")().cast<py::int_>();
        std::unique_ptr<FILE, decltype(&fclose)> fiop(fdopen(dup(fd), "w"),
                                                      &fclose);
        auto rc = Ffprint32(*buf.fbfr(), fiop.get());
        if (rc == -1) {
          throw fml32_exception(Ferror32);
        }
      },
      "Prints fielded buffer to specified stream", py::arg("fbfr"),
      py::arg("iop"));

  m.def(
      "Fextread32",
      [](py::object iop) {
        xatmibuf obuf("FML32", 1024);
        int fd = iop.attr("fileno")().cast<py::int_>();
        std::unique_ptr<FILE, decltype(&fclose)> fiop(fdopen(dup(fd), "r"),
                                                      &fclose);

        obuf.mutate([&](FBFR32 *fbfr) { return Fextread32(fbfr, fiop.get()); });
        return to_py(std::move(obuf));
      },
      "Builds fielded buffer from printed format", py::arg("iop"));

  m.attr("TPNOFLAGS") = py::int_(TPNOFLAGS);

  m.attr("TPNOBLOCK") = py::int_(TPNOBLOCK);
  m.attr("TPSIGRSTRT") = py::int_(TPSIGRSTRT);
  m.attr("TPNOREPLY") = py::int_(TPNOREPLY);
  m.attr("TPNOTRAN") = py::int_(TPNOTRAN);
  m.attr("TPTRAN") = py::int_(TPTRAN);
  m.attr("TPNOTIME") = py::int_(TPNOTIME);
  m.attr("TPABSOLUTE") = py::int_(TPABSOLUTE);
  m.attr("TPGETANY") = py::int_(TPGETANY);
  m.attr("TPNOCHANGE") = py::int_(TPNOCHANGE);
  m.attr("TPCONV") = py::int_(TPCONV);
  m.attr("TPSENDONLY") = py::int_(TPSENDONLY);
  m.attr("TPRECVONLY") = py::int_(TPRECVONLY);
  m.attr("TPACK") = py::int_(TPACK);
  m.attr("TPACK_INTL") = py::int_(TPACK_INTL);
  m.attr("TPNOCOPY") = py::int_(TPNOCOPY);

#ifdef TPSINGLETON
  m.attr("TPSINGLETON") = py::int_(TPSINGLETON);
#endif
#ifdef TPSECONDARYRQ
  m.attr("TPSECONDARYRQ") = py::int_(TPSECONDARYRQ);
#endif

  m.attr("TPFAIL") = py::int_(TPFAIL);
  m.attr("TPSUCCESS") = py::int_(TPSUCCESS);
  m.attr("TPEXIT") = py::int_(TPEXIT);

  m.attr("TPEABORT") = py::int_(TPEABORT);
  m.attr("TPEBADDESC") = py::int_(TPEBADDESC);
  m.attr("TPEBLOCK") = py::int_(TPEBLOCK);
  m.attr("TPEINVAL") = py::int_(TPEINVAL);
  m.attr("TPELIMIT") = py::int_(TPELIMIT);
  m.attr("TPENOENT") = py::int_(TPENOENT);
  m.attr("TPEOS") = py::int_(TPEOS);
  m.attr("TPEPERM") = py::int_(TPEPERM);
  m.attr("TPEPROTO") = py::int_(TPEPROTO);
  m.attr("TPESVCERR") = py::int_(TPESVCERR);
  m.attr("TPESVCFAIL") = py::int_(TPESVCFAIL);
  m.attr("TPESYSTEM") = py::int_(TPESYSTEM);
  m.attr("TPETIME") = py::int_(TPETIME);
  m.attr("TPETRAN") = py::int_(TPETRAN);
  m.attr("TPGOTSIG") = py::int_(TPGOTSIG);
  m.attr("TPERMERR") = py::int_(TPERMERR);
  m.attr("TPEITYPE") = py::int_(TPEITYPE);
  m.attr("TPEOTYPE") = py::int_(TPEOTYPE);
  m.attr("TPERELEASE") = py::int_(TPERELEASE);
  m.attr("TPEHAZARD") = py::int_(TPEHAZARD);
  m.attr("TPEHEURISTIC") = py::int_(TPEHEURISTIC);
  m.attr("TPEEVENT") = py::int_(TPEEVENT);
  m.attr("TPEMATCH") = py::int_(TPEMATCH);
  m.attr("TPEDIAGNOSTIC") = py::int_(TPEDIAGNOSTIC);
  m.attr("TPEMIB") = py::int_(TPEMIB);
#ifdef TPENOSINGLETON
  m.attr("TPENOSINGLETON") = py::int_(TPENOSINGLETON);
#endif
#ifdef TPENOSECONDARYRQ
  m.attr("TPENOSECONDARYRQ") = py::int_(TPENOSECONDARYRQ);
#endif

  m.attr("FLD_SHORT") = py::int_(FLD_SHORT);
  m.attr("FLD_LONG") = py::int_(FLD_LONG);
  m.attr("FLD_CHAR") = py::int_(FLD_CHAR);
  m.attr("FLD_FLOAT") = py::int_(FLD_FLOAT);
  m.attr("FLD_DOUBLE") = py::int_(FLD_DOUBLE);
  m.attr("FLD_STRING") = py::int_(FLD_STRING);
  m.attr("FLD_CARRAY") = py::int_(FLD_CARRAY);
  m.attr("FLD_FML32") = py::int_(FLD_FML32);
  m.attr("BADFLDID") = py::int_(BADFLDID);

  m.attr("TPEX_STRING") = py::int_(TPEX_STRING);

  m.attr("TPMULTICONTEXTS") = py::int_(TPMULTICONTEXTS);

  m.attr("MIB_PREIMAGE") = py::int_(MIB_PREIMAGE);
  m.attr("MIB_LOCAL") = py::int_(MIB_LOCAL);
  m.attr("MIB_SELF") = py::int_(MIB_SELF);

  m.attr("TAOK") = py::int_(TAOK);
  m.attr("TAUPDATED") = py::int_(TAUPDATED);
  m.attr("TAPARTIAL") = py::int_(TAPARTIAL);

  m.doc() =
      R"(Python3 bindings for writing Oracle Tuxedo clients and servers

Flags to service routines:

- TPNOBLOCK - non-blocking send/rcv
- TPSIGRSTRT - restart rcv on interrupt
- TPNOREPLY - no reply expected
- TPNOTRAN - not sent in transaction mode
- TPTRAN - sent in transaction mode
- TPNOTIME - no timeout
- TPABSOLUTE - absolute value on tmsetprio
- TPGETANY - get any valid reply
- TPNOCHANGE - force incoming buffer to match
- RESERVED_BIT1 - reserved for future use
- TPCONV - conversational service
- TPSENDONLY - send-only mode
- TPRECVONLY - recv-only mode

Flags to tpreturn:

- TPFAIL - service FAILURE for tpreturn
- TPEXIT - service FAILURE with server exit
- TPSUCCESS - service SUCCESS for tpreturn
)";
}
