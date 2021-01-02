=============
Tuxedo-Python
=============

Python3 bindings for writing Oracle Tuxedo clients and servers. With Python2 support.

.. image:: https://github.com/aivarsk/tuxedo-python/workflows/CI/badge.svg
    :target: https://github.com/aivarsk/tuxedo-python

Why?
----

I'm a fan of the way `tuxmodule <https://github.com/henschkowski/tuxmodule/blob/master/README.md>`_ enables you to interact with Oracle Tuxedo. Unfortunately, it's out-dated and somehow limited. So I cloned tuxmodule and started to clean up compiler warnings and work on some features I had in mind:

- A multi-threaded server
- Support nested FML32 buffers and more types
- Support newest Oracle Tuxedo features like ``tpadvertisex()`` and ``tpappthrinit()``
- Receive response even when the service returns TPFAIL (instead of exception)

But I realized that's too much of C for me, so I decided to write my own Python module for Oracle Tuxedo in C++ and `pybind11 <https://github.com/pybind/pybind11>`_ focusing on the parts I find most important first.

Windows runtime requirements
----------------------------

On Windows, the Visual C++ redistributable packages are a runtime requirement for this project. It can be found `here <https://support.microsoft.com/en-us/help/2977003/the-latest-supported-visual-c-downloads>`_.

I have successfully built the module with Python 3.7.7.

Python 3.8.3 fails to import the module `similar to <https://github.com/psycopg/psycopg2/issues/1006>`_ but I have no solution yet.

.. code:: Python

  ImportError: DLL load failed while importing tuxedo: The specified module could not be found.

Alternatives to Oracle Tuxedo
-----------------------------

Tuxedo-Python can also be used with `Open Source alternative to Oracle Tuxedo called Fuxedo <https://github.com/fuxedo/fuxedo>`_. Just export ``TUXDIR`` pointing to the folder where `Fuxedo <http://fuxedo.io>`_ is installed everything should work.

All demo code provided with the module works with both Oracle Tuxedo and Fuxedo and you can avoid vendor lock-in by using Python and Tuxedo-Python module.

General
-------

``tuxedo`` module supports only ``STRING``, ``CARRAY`` and ``FML32`` buffer types at the moment.

``CARRAY`` is mapped to/from Python ``bytes`` type.

``STRING`` is mapped to/from Python ``str`` type.

``FML32`` is mapped to/from Python ``dict`` type with field names (``str``) as keys and lists (``list``) of different types (``int``, ``str``, ``float`` or ``dict``) as values. ``dict`` to ``FML32`` conversion also treats types ``int``, ``str``, ``float`` or ``dict`` as lists with a single element:

.. code:: python

  {'TA_CLASS': 'Single value'}

converted to ``FML32`` and then back to ``dict`` becomes

.. code:: python

  {'TA_CLASS': ['Single value']}


All XATMI functions that take buffer and length arguments in C take only buffer argument in Python.

Calling a service
-----------------

``tuxedo.tpcall()`` and ``tuxedo.tpgetrply()`` functions return a tuple with 3 elements or throw an exception when no data is received. This is the part I believe ``tuxmodule`` got wrong: a service may return a response
both when it succeeds (``TPSUCCESS``) and fails (``TPFAIL``) and often the failure response contains some important information.

- 0 or ``TPESVCFAIL``
- ``tpurcode`` (the second argument to ``tpreturn``)
- data buffer

.. code:: python

  rval, rcode, data = t.tpcall('.TMIB', {'TA_CLASS': 'T_SVCGRP', 'TA_OPERATION': 'GET'})
  if rval == 0:
    # Service returned TPSUCCESS
  else:
    # rval == tuxedo.TPESVCFAIL
    # Service returned TPFAIL 

Writing servers
---------------

Tuxedo servers are written as Python classes. ``tpsvrinit`` method of object will be called when Tuxedo calls ``tpsvrinit(3c)`` function and it must return 0 on success or -1 on error. A common task for ``tpsvrinit`` is to advertise services the server provides by calling ``tuxedo.tpadvertise()`` with a service name. A method with the same name must exist. ``tpsvrdone``, ``tpsvrthrinit`` and ``tpsvrthrdone`` will be called when Tuxedo calls corresponding functions. All of these 4 methods are optional and ``tuxedo`` module always calls ``tpopen()`` and ``tpclose()`` functions before calling user-supplied methods.

Each service method receives a single argument with incoming buffer and service must end with either call to ``tuxedo.tpreturn()`` or ``tuxedo.tpforward()``. Unlike in C ``tuxedo.tpreturn()`` and ``tuxedo.tpforward()`` do not perform ``longjmp`` but set up arguments for those calls once service method will return. You can have a code that will execute after Python's ``tpreturn`` and it plays nicely with context managers. Following two code fragments are equivalent but I believe the first one is less error-prone.

.. code:: python

      def ECHO(self, args):
          return t.tpreturn(t.TPSUCCESS, 0, args)

.. code:: python

      def ECHO(self, args):
          t.tpreturn(t.TPSUCCESS, 0, args)


After that ``tuxedo.run()`` must be called with an instance of the class and command-line arguments to start Tuxedo server's main loop.

.. code:: python

  #!/usr/bin/env python3
  import sys
  import tuxedo as t

  class Server:
      def tpsvrinit(self, args):
          t.tpadvertise('ECHO')
          return 0

      def tpsvrthrinit(self, args):
          return 0

      def tpsvrthrdone(self):
          pass

      def tpsvrdone(self):
          pass

      def ECHO(self, args):
          return t.tpreturn(t.TPSUCCESS, 0, args)

  if __name__ == '__main__':
      t.run(Server(), sys.argv)

UBBCONFIG
---------

To use Python code as Tuxedo server the file itself must be executable (``chmod +x *.py``) and it must contain shebang line with Python:

.. code:: python

  #!/usr/bin/env python3

After that you can use the ``*.py`` file as server executable in ``UBBCONFIG``:

.. code::

  "api.py" SRVGRP=GROUP1 SRVID=20 RQADDR="api" MIN=1 SECONDARYRQ=Y REPLYQ=Y

Writing clients
---------------

Nothing special is needed to implement Tuxedo clients, just import the module and start calling XATMI functions.

.. code:: python

  #!/usr/bin/env python3
  import sys
  import tuxedo as t

  rval, rcode, data = t.tpcall('.TMIB', {'TA_CLASS': 'T_SVCGRP', 'TA_OPERATION': 'GET'})

Using Oracle Database
---------------------

You can access Oracle database with ``cx_Oracle`` library and local transactions by just following the documentation of ``cx_Oracle``.

If you want a server written in Python to participate in the global transaction first specify a resource manager name to use (similar to ``buidserver``). ``tuxedo`` module currently supports:

- NONE default "null" resource manager
- Oracle_XA for Oracle Database

.. code:: python

    t.run(Server(), sys.argv, 'Oracle_XA')


After that you should create a database connection in ``tpsvrinit`` by using ``tuxedo.xaoSvcCtx()`` function:

.. code:: python

    def tpsvrinit(self, args):
        self.db = cx_Oracle.connect(handle=t.xaoSvcCtx())

That is the only difference from standard ``cx_Oracle`` use case. Here is a complete example for a single-threaded server:

.. code:: python

  #!/usr/bin/env python3

  import sys
  import tuxedo as t
  import cx_Oracle

  class Server:
      def tpsvrinit(self, args):
          t.userlog('Server startup')
          self.db = cx_Oracle.connect(handle=t.xaoSvcCtx())
          t.tpadvertise('DB')
          return 0

      def DB(self, args):
          dbc = self.db.cursor()
          dbc.execute('insert into pymsg(msg) values (:1)', ['Hello from python'])
          return t.tpreturn(t.TPSUCCESS, 0, args)

  if __name__ == '__main__':
      t.run(Server(), sys.argv, 'Oracle_XA')

For a multi-threaded server new connections for each thread must be created in ``tpsvrthrinit()`` (instead of ``tpsvrinit()``) and stored in thread-local storage of ``threading.local()``.

Server must belong to a group with ``Oracle_XA`` as resource manager, something like this in ``UBBCONFIG``

.. code::

  *GROUPS
  GROUP2 LMID=tuxapp GRPNO=2 TMSNAME=ORACLETMS OPENINFO="Oracle_XA:Oracle_XA+Objects=true+Acc=P/scott/tiger+SqlNet=ORCL+SesTm=60+LogDir=/tmp+Threads=true"
  *SERVERS
  "db.py" SRVGRP=GROUP2 SRVID=2 CLOPT="-A"


tpadmcall
---------

``tpadmcall`` is made available for application administration even while application is down. It also has no service call overhead compared to calling ``.TMIB`` service. The Python function looks and behaves similary to ``tpcall`` except ``rcode`` (2nd element in result tuple) is always a constant 0.

.. code:: python

  #!/usr/bin/env python3
  import tuxedo as t

  rval, _, data = t.tpadmcall({'TA_CLASS': 'T_DOMAIN', 'TA_OPERATION': 'GET'})


Global transactions
-------------------

Transactions can be started and committed or aborted by using ``tuxedo.tpbegin()``, ``tuxedo.tpcommit()``, ``tuxedo.tpabort()``. These functions take the same arguments as their corresponding C functions.

Buffer export and import
------------------------



FML32 identifiers
-----------------

``Fname32`` and ``Fldid32`` are available to find map from field identifier to name or the other way.

Functions to determine field number and type from identifier:

.. code:: python

  assert t.Fldtype32(t.Fmkfldid32(t.FLD_STRING, 10)) == t.FLD_STRING
  assert t.Fldno32(t.Fmkfldid32(t.FLD_STRING, 10)) == 10

Exceptions
----------

On errors either ``XatmiException`` or ``Fml32Exception`` are raised by the module. Exceptions contain additional attirbute ``code`` that contains the Tuxedo error code and you can compare it with defined errors like ``TPENOENT`` or ``TPESYSTEM``.

.. code:: python

  try:
    t.tpcall("whatever", {})
  except t.XatmiException as e:
    if e.code == t.TPENOENT:
      print("Service does not exist")


Demo
----

``demo/`` folder has some proof-of-concept code:

- ``client.py`` Oracle Tuxedo client
- ``api.py`` HTTP+JSON server running inside Oracle Tuxedo server
- ``ecb.py`` HTTP+XML client running inside Oracle Tuxedo server
- ``mem.py`` multi-threaded in-memory cache
- ``db.py`` Access Oracle Database using cx_Oracle module within global transaction
- ``buf.py`` Demo of tpimport/tpexport and FML32 identifiers

TODO
----

- Implementing few more useful APIs
