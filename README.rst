Tuxedo-Python
-------------

I'm a fan of the way `tuxmodule <https://github.com/henschkowski/tuxmodule/blob/master/README.md>`_ enables you to interact with Oracle Tuxedo. Unfortunately, it's out-dated and somehow limited. So I cloned tuxmodule and started to clean up compiler warnings and work on some features I had in mind:

- A multi-threaded server
- Support nested FML32 buffers and more types
- Support newest Oracle Tuxedo features like ``tpadvertisex()`` and ``tpappthrinit()``
- Receive response even when the service returns TPFAIL (instead of exception)

But I realized that's too much of C for me, so I decided to write my own Python module for Oracle Tuxedo in C++ and `pybind11 <https://github.com/pybind/pybind11>`_ focusing on the parts I find most important first.
