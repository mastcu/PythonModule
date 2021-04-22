#!/usr/bin/python
#
# distutils setup.py file for building python serialem module
#

from distutils.core import setup, Extension
serialemmodule = Extension('serialem',
                           define_macros = [('MAJOR_VERSION', '1'),
                                            ('MINOR_VERSION', '0')],
                           include_dirs = ['../SerialEM', '../SerialEM/Shared'],
                           sources = ['SerialEMModule.cpp',
                                      'PySEMSocket.cpp'],
                           libraries = ['wsock32'],
                           depends = ['PySEMSocket.h'])

setup (name = 'serialem',
       version = '1.0',
       description = 'Python module for SerialEM',
       author = 'David Mastronarde',
       author_email = 'mast@colorado.edu',
       maintainer = 'David Mastronarde',
       maintainer_email = 'mast@colorado.edu',
       url = 'http://bio3d.colorado.edu/SerialEM',
       long_description = '''
Python module for using Python scripting in SerialEM
''',
       ext_modules = [serialemmodule])
