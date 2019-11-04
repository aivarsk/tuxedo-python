#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#include <atmi.h>
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

struct client {
  client() {}
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
  char name[XATMI_SERVICE_NAME_LENGTH];
  bool forward;
  bool clean;
};

struct xatmi_exception : public std::exception {
 public:
  explicit xatmi_exception(int code) : code(code), message(tpstrerror(code)) {}
  int code;
  std::string message;

  const char *what() const noexcept override { return message.c_str(); }
};

struct fml32_exception : public std::exception {
  explicit fml32_exception(int code) : code(code), message(Fstrerror32(code)) {}
  int code;
  std::string message;

  const char *what() const noexcept override { return message.c_str(); }
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

static xatmibuf from_py(py::object obj);
static py::object to_py(xatmibuf buf);

static void pytpreturn(int rval, long rcode, py::object data, long flags) {
  if (!tsvcresult.clean) {
    throw std::runtime_error("tpreturn already called");
  }
  tsvcresult.clean = false;
  tsvcresult.rval = rval;
  tsvcresult.rcode = rcode;
  tsvcresult.odata = from_py(data).release();
  tsvcresult.forward = false;
}
static void pytpforward(const std::string &svc, py::object data, long flags) {
  if (!tsvcresult.clean) {
    throw std::runtime_error("tpreturn already called");
  }
  tsvcresult.clean = false;
  strncpy(tsvcresult.name, svc.c_str(), sizeof(tsvcresult.name));
  tsvcresult.odata = from_py(data).release();
  tsvcresult.forward = true;
}

static pytpreply pytpcall(const char *svc, py::object idata, long flags) {
  auto in = from_py(idata);
  xatmibuf out("FML32", 1024);
  int rc =
      tpcall(const_cast<char *>(svc), in.p, in.len, &out.p, &out.len, flags);
  if (rc == -1) {
    if (tperrno != TPESVCFAIL) {
      throw xatmi_exception(tperrno);
    }
  }
  return pytpreply(tperrno, tpurcode, to_py(std::move(out)));
}

static int pytpacall(const char *svc, py::object idata, long flags) {
  auto in = from_py(idata);
  int rc = tpacall(const_cast<char *>(svc), in.p, in.len, flags);
  if (rc == -1) {
    throw xatmi_exception(tperrno);
  }
  return rc;
}

static pytpreply pytpgetrply(int cd, long flags) {
  xatmibuf out("FML32", 1024);
  int rc = tpgetrply(&cd, &out.p, &out.len, flags);
  if (rc == -1) {
    if (tperrno != TPESVCFAIL) {
      throw xatmi_exception(tperrno);
    }
  }
  return pytpreply(tperrno, tpurcode, to_py(std::move(out)), cd);
}

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
    std::string s = py::str(obj);
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

int tpsvrinit(int argc, char *argv[]) {
  if (!tclient) {
    tclient.reset(new client());
  }
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
  if (!tclient) {
    tclient.reset(new client());
  }
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
  if (!tclient) {
    tclient.reset(new client());
  }
  tsvcresult.clean = true;

  try {
    auto idata = to_py(xatmibuf(svcinfo));
    server.attr(svcinfo->name)(idata);
    if (tsvcresult.clean) {
      userlog(const_cast<char *>("tpreturn() not called"));
      tpreturn(TPEXIT, 0, nullptr, 0, 0);
    }
  } catch (const std::exception &e) {
    userlog(const_cast<char *>("%s"), e.what());
    tpreturn(TPEXIT, 0, nullptr, 0, 0);
  }

  if (tsvcresult.forward) {
    tpforward(tsvcresult.name, tsvcresult.odata, 0, 0);
  } else {
    tpreturn(tsvcresult.rval, tsvcresult.rcode, tsvcresult.odata, 0, 0);
  }
}

static void pytpadvertisex(std::string svcname, std::string func, long flags) {
  if (tpadvertisex(const_cast<char *>(svcname.c_str()), PY, flags) == -1) {
    throw xatmi_exception(tperrno);
  }
  if (svcname != func) {
    server.attr(svcname.c_str()) = server.attr(func.c_str());
  }
}

static void pytpadvertise(std::string svcname) {
  pytpadvertisex(svcname, svcname, 0);
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
  for (size_t i = 0; i < args.size(); i++) {
    argv[i] = const_cast<char *>(args[i].c_str());
  }
  return _tmstartserver(args.size(), argv, _tmgetsvrargs());
}

PYBIND11_MODULE(tuxedo, m) {
  m.doc() = "Tuxedo module";

  // Poor man's namedtuple
  py::class_<pytpreply>(m, "TpReply")
      .def_readonly("rval", &pytpreply::rval)
      .def_readonly("rcode", &pytpreply::rcode)
      .def_readonly("data", &pytpreply::data)
      .def_readonly("cd", &pytpreply::cd)  // Does not unpack as the use is rare
                                           // case of tpgetrply(TPGETANY)
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

  //  py::class_<xatmi_exception>(m, "XatmiException")
  //      .def_readonly("code", &xatmi_exception::code)
  //      .def_readonly("message", &xatmi_exception::message);
  //  py::class_<fml32_exception>(m, "Fml32Exception")
  //      .def_readonly("code", &fml32_exception::code)
  //      .def_readonly("message", &fml32_exception::message);

  static py::exception<xatmi_exception> exc1(m, "XatmiException");
  static py::exception<fml32_exception> exc2(m, "Fml32Exception");
  py::register_exception_translator([](std::exception_ptr p) {
    try {
      if (p) std::rethrow_exception(p);
    } catch (const xatmi_exception &e) {
      exc1(e.what());
    } catch (const fml32_exception &e) {
      exc2(e.what());
    }
  });

  m.def(
      "tpbegin",
      [](unsigned long timeout, long flags) {
        if (tpbegin(timeout, flags) == -1) {
          throw xatmi_exception(tperrno);
        }
      },
      py::arg("timeout"), py::arg("flags") = 0);

  m.def(
      "tpcommit",
      [](long flags) {
        if (tpcommit(flags) == -1) {
          throw xatmi_exception(tperrno);
        }
      },
      py::arg("flags") = 0);

  m.def(
      "tpabort",
      [](long flags) {
        if (tpcommit(flags) == -1) {
          throw xatmi_exception(tperrno);
        }
      },
      py::arg("flags") = 0);

  m.def("tpgetlev", []() {
    int rc;
    if ((rc = tpgetlev()) == -1) {
      throw xatmi_exception(tperrno);
    }
    return py::bool_(rc);
  });

  m.def(
      "userlog",
      [](const char *message) { userlog(const_cast<char *>("%s"), message); },
      py::arg("message"));

  m.def("tpadvertisex", &pytpadvertisex, py::arg("svcname"), py::arg("func"),
        py::arg("flags") = 0);
  m.def("tpadvertise", &pytpadvertise, py::arg("svcname"));

  m.def("run", &pyrun, py::arg("server"), py::arg("args"));

  m.def("Fprint32", &pyFprint32, py::arg("data"));

  m.def("tpcall", &pytpcall, py::arg("svc"), py::arg("idata"),
        py::arg("flags") = 0);
  m.def("tpacall", &pytpacall, py::arg("svc"), py::arg("idata"),
        py::arg("flags") = 0);
  m.def("tpgetrpy", &pytpgetrply, py::arg("cd"), py::arg("flags") = 0);

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
