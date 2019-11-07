from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
import os
import sys
import setuptools

__version__ = '0.0.5'


class get_pybind_include(object):
    """Helper class to determine the pybind11 include path
    The purpose of this class is to postpone importing pybind11
    until it is actually installed, so that the ``get_include()``
    method can be invoked. """

    def __init__(self, user=False):
        self.user = user

    def __str__(self):
        import pybind11
        return pybind11.get_include(self.user)


ext_modules = [
    Extension(
        'tuxedo',
        ['src/tuxedo.cpp'],
        include_dirs=[
            os.path.join(os.environ['TUXDIR'], 'include'),
            # Path to pybind11 headers
            get_pybind_include(),
            get_pybind_include(user=True),
        ],
        library_dirs=[os.path.join(os.environ['TUXDIR'], 'lib')],
        libraries=['tux', 'fml32', 'tmib'],
        language='c++'
    ),
]

# As of Python 3.6, CCompiler has a `has_flag` method.
# cf http://bugs.python.org/issue26689
def has_flag(compiler, flagname):
    """Return a boolean indicating whether a flag name is supported on
    the specified compiler.
    """
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.cpp') as f:
        f.write('int main (int argc, char **argv) { return 0; }')
        try:
            compiler.compile([f.name], extra_postargs=[flagname])
        except setuptools.distutils.errors.CompileError:
            return False
    return True


def cpp_flag(compiler):
    """Return the -std=c++[11/14/17] compiler flag.
    The newer version is prefered over c++11 (when it is available).
    """
    flags = ['-std=c++17', '-std=c++14', '-std=c++11']

    for flag in flags:
        if has_flag(compiler, flag): return flag

    raise RuntimeError('Unsupported compiler -- at least C++11 support '
                       'is needed!')


class BuildExt(build_ext):
    """A custom build extension for adding compiler-specific options."""
    def build_extensions(self):
        ct = self.compiler.compiler_type
        opts = []
        link_opts = []
        opts.append('-DVERSION_INFO="%s"' % self.distribution.get_version())
        opts.append(cpp_flag(self.compiler))
        if has_flag(self.compiler, '-fvisibility=hidden'):
            opts.append('-fvisibility=hidden')

        for ext in self.extensions:
            ext.extra_compile_args = opts
            ext.extra_link_args = link_opts
        build_ext.build_extensions(self)

setup(
    name='tuxedo',
    version=__version__,
    author='Aivars Kalvans',
    author_email='aivars.kalvans@gmail.com',
    url='https://github.com/aivarsk/tuxedo-python',
    description='Python3 bindings for writing Oracle Tuxedo clients and servers',
    long_description=open('README.rst').read(),
    ext_modules=ext_modules,
    install_requires=['pybind11>=2.4'],
    setup_requires=['pybind11>=2.4'],
    cmdclass={'build_ext': BuildExt},
    classifiers=[
        'Programming Language :: Python :: 3',
        'Programming Language :: C++',
        'Operating System :: POSIX',
        'License :: OSI Approved :: MIT License',
    ],
    zip_safe=False,
)
