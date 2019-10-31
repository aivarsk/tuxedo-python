#include <atmi.h>
#include <xa.h>
#undef _
#include <pybind11/pybind11.h>
#include <functional>
#include <map>

namespace py = pybind11;

struct xatmibuf {
  xatmibuf() : p(nullptr), len(0) {}
  xatmibuf(const char *type, long len) : p(nullptr), len(len) {
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

  FBFR32 **fbfr() { return reinterpret_cast<FBFR32 **>(&p); }

  char *p;
  long len;

  void swap(xatmibuf &other) noexcept {
    std::swap(p, other.p);
    std::swap(len, other.len);
  }
};

static xatmibuf from_py(py::object obj);
static py::object to_py(char *buf);

void PY(TPSVCINFO *) {}
static int start(int argc, char **argv);

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
      throw std::runtime_error(tpstrerror(tperrno));
    }
  }
  return py::make_tuple(tperrno, tpurcode, to_py(out.p));
}
//int tpcall(char *svc, char *idata, long ilen, char **odata, long \
//   *olen, long flags)

static py::object to_py(FBFR32 *fbfr) {
  FLDID32 fldid = FIRSTFLDID;
  FLDOCC32 oc = 0;
  py::dict result;
  py::list val;

  for (;;) {
    char value[32] __attribute__((aligned(16)));
    FLDLEN32 sendlen = sizeof(value);
    sendlen = sizeof(value);

    int r = Fnext32(fbfr, &fldid, &oc, value, &sendlen);
    if (r == -1) {
      if (Ferror32 != FNOSPACE) {
        throw std::invalid_argument(Fstrerror32(Ferror32));
      }
    } else if (r == 0) {
      break;
    }

    if (oc == 0) {
      val = py::list();

      char *name = Fname32(fldid);
      if (name == nullptr) {
        result[name] = val;
      } else {
        char tmpname[32];
        snprintf(tmpname, sizeof(tmpname), "((FLDID32)%u)", fldid);
        result[tmpname] = val;
      }
    }

    char *ptr;
    switch (Fldtype32(fldid)) {
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
        ptr = Ffind32(fbfr, fldid, oc, nullptr);
        if (ptr == nullptr) {
          throw std::invalid_argument(Fstrerror32(Ferror32));
        }
        val.append(py::str(ptr));
        break;
      case FLD_CARRAY:
        ptr = Ffind32(fbfr, fldid, oc, &sendlen);
        if (ptr == nullptr) {
          throw std::invalid_argument(Fstrerror32(Ferror32));
        }
        val.append(py::bytes(ptr, sendlen));
        break;
      case FLD_FML32:
        ptr = Ffind32(fbfr, fldid, oc, nullptr);
        if (ptr == nullptr) {
          throw std::invalid_argument(Fstrerror32(Ferror32));
        }
        val.append(to_py(reinterpret_cast<FBFR32 *>(ptr)));
        break;
      default:
        throw std::invalid_argument("Unsupported field " +
                                    std::to_string(fldid));
    }
  }
  return result;
}

static py::object to_py(char *data) {
  char type[8];
  char subtype[16];
  if (tptypes(data, type, subtype) == -1) {
    throw std::invalid_argument("Invalid buffer type");
  }
  if (strcmp(type, "STRING") == 0) {
    return py::cast(data);
  } else if (strcmp(type, "FML32") == 0) {
    return to_py(reinterpret_cast<FBFR32 *>(data));
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
        throw std::runtime_error(Fstrerror32(Ferror32));
      }
    } else {
      break;
    }
  }
}

static void from_py(py::dict obj, FBFR32 **fbfr);
static void from_py1(FBFR32 **fbfr, FLDID32 fldid, FLDOCC32 oc, py::handle obj,
                     FBFR32 **f) {
  if (py::isinstance<py::str>(obj)) {
    std::string val = obj.cast<py::str>();
    mutate(fbfr, [&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fldid, oc, const_cast<char *>(val.data()), 0,
                     FLD_STRING);
    });
  } else if (py::isinstance<py::int_>(obj)) {
    long val = obj.cast<py::int_>();
    mutate(fbfr, [&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fldid, oc, reinterpret_cast<char *>(&val), 0,
                     FLD_LONG);
    });

  } else if (py::isinstance<py::float_>(obj)) {
    double val = obj.cast<py::float_>();
    mutate(fbfr, [&](FBFR32 *fbfr) {
      return CFchg32(fbfr, fldid, oc, reinterpret_cast<char *>(&val), 0,
                     FLD_DOUBLE);
    });
  } else if (py::isinstance<py::dict>(obj)) {
    from_py(obj.cast<py::dict>(), f);
    mutate(fbfr, [&](FBFR32 *fbfr) {
      return Fchg32(fbfr, fldid, oc, reinterpret_cast<char *>(*f), 0);
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
    FLDID32 fldid =
        Fldid32(const_cast<char *>(std::string(py::str(it.first)).c_str()));

    py::handle o = it.second;
    if (py::isinstance<py::list>(o)) {
      FLDOCC32 oc = 0;
      for (auto e : o.cast<py::list>()) {
        from_py1(fbfr, fldid, oc++, e, f.fbfr());
      }
    } else {
      // Handle single elements instead of lists for convenience
      from_py1(fbfr, fldid, 0, o, f.fbfr());
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

static void Fprint32(py::object obj) {
  auto d = from_py(obj);
  Fprint32(*d.fbfr());
}

static py::object _roundtrip(py::object obj) { return obj; }

int tpsvrinit(int argc, char *argv[]) { return 0; }

PYBIND11_MODULE(tuxedo, m) {
  m.doc() = "Tuxedo module";

  m.def("Fprint32", &Fprint32, py::arg("data"));

  m.def("tpcall", &pytpcall, py::arg("svc"), py::arg("idata"),
        py::arg("flags"));
  m.def("tpreturn", &pytpreturn, py::arg("rval"), py::arg("rcode"),
        py::arg("data"), py::arg("flags"));
  m.def("tpforward", &pytpforward, py::arg("svc"), py::arg("data"),
        py::arg("flags"));

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

extern "C" {
int _tmrunserver(int);
extern struct xa_switch_t xaosw;
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
  tmsvrargs.xa_switch = NULL;
  return &tmsvrargs;
}

static int start(int argc, char **argv) {
  _tmbuilt_with_thread_option = 1;
  return _tmstartserver(argc, argv, _tmgetsvrargs());
}
