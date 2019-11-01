from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext
from setuptools.command.test import test as TestCommand
import glob
import os
import sys
import setuptools
import pybind11
import sysconfig

base_path = os.path.dirname(__file__)

ext_modules = [
    Extension(
        'tuxedo',
        glob.glob(os.path.join(base_path, 'src', '*.cpp')),
        include_dirs=[os.path.join(os.environ['TUXDIR'], 'include')],
        library_dirs=[os.path.join(os.environ['TUXDIR'], 'lib'),
        sysconfig.get_path('include'), sysconfig.get_path('platinclude'),
        pybind11.get_include(), pybind11.get_include(True)],
        libraries=['tux', 'fml32', 'tmib'],
        language='c++',
        undef_macros=["NDEBUG"],
    ),
]

def has_flag(compiler, flagname):
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.cpp') as f:
        f.write('int main (int argc, char **argv) { return 0; }')
        try:
            compiler.compile([f.name], extra_postargs=[flagname])
        except setuptools.distutils.errors.CompileError:
            return False
    return True


def best_cpp(compiler):
    """Return the -std=c++[11/14] compiler flag.

    The c++14 is prefered over c++11 (when it is available).
    """
    if has_flag(compiler, '-std=c++14'):
        return '-std=c++14'
    elif has_flag(compiler, '-std=c++11'):
        return '-std=c++11'
    else:
        raise RuntimeError('Unsupported compiler -- at least C++11 support is needed!')


class BuildExt(build_ext):
    def build_extensions(self):
        for ext in self.extensions:
            ext.extra_compile_args = [best_cpp(self.compiler)]
        build_ext.build_extensions(self)

class PyTest(TestCommand):
    user_options = [('pytest-args=', 'a', "Arguments to pass to py.test")]

    def initialize_options(self):
        TestCommand.initialize_options(self)
        self.pytest_args = []

    def run_tests(self):
        # import here, cause outside the eggs aren't loaded
        import pytest
        errno = pytest.main(self.pytest_args)
        sys.exit(errno)

setup(
    name='tuxedo',
    version='0.0.1',
    author='Aivars Kalvans',
    author_email='aivars.kalvans@gmail.com',
    long_description=open('README.rst').read(),
    ext_modules=ext_modules,
    cmdclass={
        'build_ext': BuildExt,
    },
    zip_safe=False,
    tests_require=['pytest'],
)
