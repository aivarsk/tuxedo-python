#include <atmi.h>
#include <userlog.h>
#include <xa.h>
#undef _
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <functional>
#include <map>

namespace py = pybind11;

static py::object server;

class xatmi_exception : public std::exception {
 public:
  xatmi_exception(int err) : err(err), msg(tpstrerror(err)) {}
  int err;
  std::string msg;

  const char *what() const noexcept override { return msg.c_str(); }
};

class fml32_exception : public std::exception {
 public:
  fml32_exception(int err) : err(err), msg(Fstrerror32(err)) {}
  int err;
  std::string msg;

  const char *what() const noexcept override { return msg.c_str(); }
};

struct xatmibuf {
  xatmibuf() : p(nullptr), pp(&p), len(0) {}
  xatmibuf(TPSVCINFO *svcinfo) : p(nullptr), pp(&svcinfo->data), len(0) {}
  xatmibuf(const char *type, long len) : p(nullptr), pp(&p), len(len) {
    p = tpalloc(const_cast<char *>(type), nullptr, len);
    if (p == nullptr) {
      throw std::bad_alloc();
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

  char *p;
  char **pp;
  long len;

  void swap(xatmibuf &other) noexcept {
    std::swap(p, other.p);
    std::swap(len, other.len);
  }
};

static xatmibuf from_py(py::object obj);
static py::object to_py(xatmibuf buf);

static py::object pytpreturn(int rval, long rcode, py::object data,
                             long flags) {
  return py::make_tuple(rval, rcode, data);
}
static py::object pytpforward(const std::string &svc, py::object data,
                              long flags) {
  return py::make_tuple(svc, data);
}

static py::object pytpcall(const char *svc, py::object idata, long flags) {
  auto in = from_py(idata);
  xatmibuf out("FML32", 1024);
  int rc =
      tpcall(const_cast<char *>(svc), in.p, in.len, &out.p, &out.len, flags);
  if (rc == -1) {
    if (tperrno != TPESVCFAIL) {
      throw xatmi_exception(tperrno);
    }
  }
  return py::make_tuple(tperrno, tpurcode, to_py(std::move(out)));
}

static void pyuserlog(const char *message) { userlog("%s", message); }

static py::object to_py(FBFR32 *fbfr) {
  FLDID32 fieldid = FIRSTFLDID;
  FLDOCC32 oc = 0;
  py::dict result;
  py::list val;

  for (;;) {
    char value[32] __attribute__((aligned(16)));
    FLDLEN32 sendlen = sizeof(value);
    sendlen = sizeof(value);

    int r = Fnext32(fbfr, &fieldid, &oc, value, &sendlen);
    if (r == -1) {
      if (Ferror32 != FNOSPACE) {
        throw fml32_exception(Ferror32);
      }
    } else if (r == 0) {
      break;
    }

    if (oc == 0) {
      val = py::list();

      char *name = Fname32(fieldid);
      if (name != nullptr) {
        result[name] = val;
      } else {
        char tmpname[32];
        snprintf(tmpname, sizeof(tmpname), "((FLDID32)%u)", fieldid);
        result[tmpname] = val;
      }
    }

    char *ptr;
    switch (Fldtype32(fieldid)) {
      case FLD_CHAR:
        val.append(py::cast(value[0]));
        break;
      case FLD_SHORT:
        val.append(py::cast(*reinterpret_cast<short *>(value)));
        break;
      case FLD_LONG:
        val.append(py::cast(*reinterpret_cast<long *>(value)));
        break;
      case FLD_FLOAT:
        val.append(py::cast(*reinterpret_cast<float *>(value)));
        break;
      case FLD_DOUBLE:
        val.append(py::cast(*reinterpret_cast<double *>(value)));
        break;
      case FLD_STRING:
        ptr = Ffind32(fbfr, fieldid, oc, nullptr);
        if (ptr == nullptr) {
          throw fml32_exception(Ferror32);
        }
        val.append(py::str(ptr));
        break;
      case FLD_CARRAY:
        ptr = Ffind32(fbfr, fieldid, oc, &sendlen);
        if (ptr == nullptr) {
          throw fml32_exception(Ferror32);
        }
        val.append(py::bytes(ptr, sendlen));
        break;
      case FLD_FML32:
        ptr = Ffind32(fbfr, fieldid, oc, nullptr);
        if (ptr == nullptr) {
          throw fml32_exception(Ferror32);
        }
        val.append(to_py(reinterpret_cast<FBFR32 *>(ptr)));
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
  } else if (strcmp(type, "FML32") == 0) {
    return to_py(reinterpret_cast<FBFR32 *>(*buf.pp));
  } else {
    throw std::invalid_argument("Unsupported buffer type");
  }
}

static void mutate(FBFR32 **fbfr, std::function<int(FBFR32 *)> f) {
  while (true) {
    int rc = f(*fbfr);
    if (rc == -1) {
      if (Ferror32 == FNOSPACE) {
        *fbfr = reinterpret_cast<FBFR32 *>(
            tprealloc(reinterpret_cast<char *>(*fbfr), Fsizeof32(*fbfr) * 2));
      } else {
        throw fml32_exception(Ferror32);
      }
    } else {
      break;
    }
  }
}

static void from_py(py::dict obj, FBFR32 **fbfr);
static void from_py1(FBFR32 **fbfr, FLDID32 fieldid, FLDOCC32 oc,
                     py::handle obj, FBFR32 **f) {
  if (py::isinstance<py::str>(obj)) {
    std::string val = obj.cast<py::str>();
    mutate(fbfr, [&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fieldid, oc, const_cast<char *>(val.data()), 0,
                     FLD_STRING);
    });
  } else if (py::isinstance<py::int_>(obj)) {
    long val = obj.cast<py::int_>();
    mutate(fbfr, [&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fieldid, oc, reinterpret_cast<char *>(&val), 0,
                     FLD_LONG);
    });

  } else if (py::isinstance<py::float_>(obj)) {
    double val = obj.cast<py::float_>();
    mutate(fbfr, [&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fieldid, oc, reinterpret_cast<char *>(&val), 0,
                     FLD_DOUBLE);
    });
  } else if (py::isinstance<py::dict>(obj)) {
    from_py(obj.cast<py::dict>(), f);
    mutate(fbfr, [&](FBFR32 *fbfr) {
      return Fchg32(fbfr, fieldid, oc, reinterpret_cast<char *>(*f), 0);
    });
  } else {
    throw std::invalid_argument("Unsupported type");
  }
}

static void from_py(py::dict obj, FBFR32 **fbfr) {
  if (*fbfr == nullptr) {
    *fbfr = reinterpret_cast<FBFR32 *>(
        tpalloc(const_cast<char *>("FML32"), nullptr, 1024));
    if (*fbfr == nullptr) {
      throw std::bad_alloc();
    }
  } else {
    Finit32(*fbfr, Fsizeof32(*fbfr));
  }

  xatmibuf f;

  for (auto it : obj) {
    FLDID32 fieldid =
        Fldid32(const_cast<char *>(std::string(py::str(it.first)).c_str()));

    py::handle o = it.second;
    if (py::isinstance<py::list>(o)) {
      FLDOCC32 oc = 0;
      for (auto e : o.cast<py::list>()) {
        from_py1(fbfr, fieldid, oc++, e, f.fbfr());
      }
    } else {
      // Handle single elements instead of lists for convenience
      from_py1(fbfr, fieldid, 0, o, f.fbfr());
    }
  }
}

static xatmibuf from_py(py::object obj) {
  if (py::isinstance<py::str>(obj)) {
    std::string s = obj.str();
    xatmibuf buf("STRING", s.size() + 1);
    strcpy(buf.p, s.c_str());
    return buf;
  } else if (py::isinstance<py::dict>(obj)) {
    xatmibuf buf("FML32", 1024);

    from_py(static_cast<py::dict>(obj), buf.fbfr());

    return buf;
  } else {
    throw std::invalid_argument("Unsupported buffer type");
  }
}

static void pyFprint32(py::object obj) {
  auto d = from_py(obj);
  Fprint32(*d.fbfr());
}

static py::object _roundtrip(py::object obj) { return to_py(from_py(obj)); }

int tpsvrinit(int argc, char *argv[]) {
  if (tpopen() == -1) {
    userlog(const_cast<char *>("Failed tpopen() = %d / %s"), tperrno,
            tpstrerror(tperrno));
    return -1;
  }
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
  if (hasattr(server, __func__)) {
    server.attr(__func__)();
  }
}
int tpsvrthrinit(int argc, char *argv[]) {
  if (tpopen() == -1) {
    userlog(const_cast<char *>("Failed tpopen() = %d / %s"), tperrno,
            tpstrerror(tperrno));
    return -1;
  }
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
  if (hasattr(server, __func__)) {
    server.attr(__func__)();
  }
}
void PY(TPSVCINFO *svcinfo) {
  int rval = TPEXIT;
  long rcode = 0;
  char *odata = nullptr;
  char name[XATMI_SERVICE_NAME_LENGTH];
  bool forward = false;
  try {
    auto idata = to_py(xatmibuf(svcinfo));
    auto o = server.attr(svcinfo->name)(idata);

    if (py::isinstance<py::tuple>(o)) {
      auto ret = o.cast<py::tuple>();
      if (py::len(ret) == 3) {
        rval = ret[0].cast<int>();
        rcode = ret[1].cast<long>();
        odata = from_py(ret[2]).release();
      } else if (py::len(ret) == 2) {
        std::string tmpname = ret[0].cast<py::str>();
        strncpy(name, tmpname.c_str(), sizeof(name));
        odata = from_py(ret[1]).release();
        forward = true;
      }
    }
  } catch (const std::exception &e) {
    userlog(const_cast<char *>("%s"), e.what());
  }

  if (forward) {
    tpforward(name, odata, 0, 0);
  } else {
    tpreturn(rval, rcode, odata, 0, 0);
  }
}

static void pytpadvertisex(std::string name, long flags) {
  if (tpadvertisex(const_cast<char *>(name.c_str()), PY, flags) == -1) {
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
    {(char *)"", (char *)"PY", PY, 0, 0}, {NULL, NULL, NULL, 0, 0}};

static struct tmsvrargs_t tmsvrargs = {NULL,        &_tmdsptchtbl[0],
                                       0,           tpsvrinit,
                                       tpsvrdone,   _tmrunserver,
                                       NULL,        NULL,
                                       NULL,        NULL,
                                       tprminit,    tpsvrthrinit,
                                       tpsvrthrdone};

struct tmsvrargs_t *_tmgetsvrargs(void) {
  tmsvrargs.reserved1 = NULL;
  tmsvrargs.reserved2 = NULL;
  // tmsvrargs.xa_switch = &xaosw;
  tmsvrargs.xa_switch = &tmnull_switch;
  return &tmsvrargs;
}

static int pyrun(py::object svr, std::vector<std::string> args) {
  server = svr;
  _tmbuilt_with_thread_option = 1;
  char *argv[args.size()];
  for (int i = 0; i < args.size(); i++) {
    argv[i] = const_cast<char *>(args[i].c_str());
  }
  return _tmstartserver(args.size(), argv, _tmgetsvrargs());
}

PYBIND11_MODULE(tuxedo, m) {
  m.doc() = "Tuxedo module";

  static py::exception<xatmi_exception> exc1(m, "XatmiException");
  static py::exception<fml32_exception> exc2(m, "Fml32Exception");

  m.def("userlog", &pyuserlog, py::arg("message"));
  m.def("tpadvertisex", &pytpadvertisex, py::arg("name"), py::arg("flags") = 0);
  m.def("run", &pyrun, py::arg("server"), py::arg("args"));

  m.def("Fprint32", &pyFprint32, py::arg("data"));
  m.def("_roundtrip", &_roundtrip, py::arg("data"));

  m.def("tpcall", &pytpcall, py::arg("svc"), py::arg("idata"),
        py::arg("flags") = 0);
  m.def("tpreturn", &pytpreturn, py::arg("rval"), py::arg("rcode"),
        py::arg("data"), py::arg("flags") = 0);
  m.def("tpforward", &pytpforward, py::arg("svc"), py::arg("data"),
        py::arg("flags") = 0);

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
  m.attr("RESERVED_BIT1") = py::int_(RESERVED_BIT1);
  m.attr("TPCONV") = py::int_(TPCONV);
  m.attr("TPSENDONLY") = py::int_(TPSENDONLY);
  m.attr("TPRECVONLY") = py::int_(TPRECVONLY);
  m.attr("TPACK") = py::int_(TPACK);
  m.attr("TPACK_INTL") = py::int_(TPACK_INTL);
  m.attr("TPNOCOPY") = py::int_(TPNOCOPY);

  m.attr("TPSINGLETON") = py::int_(TPSINGLETON);
  m.attr("TPSECONDARYRQ") = py::int_(TPSECONDARYRQ);

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
  m.attr("TPENOSINGLETON") = py::int_(TPENOSINGLETON);
  m.attr("TPENOSECONDARYRQ") = py::int_(TPENOSECONDARYRQ);
}
