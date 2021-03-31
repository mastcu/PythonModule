This project is for building a plugin to SerialEM that embeds a Python
interpreter.

It is currently configured with the following assumptions:
The SerialEM source directory is adjacent to the parent directory.

32-bit Pythons are installed in C:\Program Files (x86)\Pythonxy
64-bit Pythons are installed in C:\Program Files\Pythonxy
where x and y are the major and minor version numbers, with no dot separating
them.

Environment variables PYTH_MAJOR and PYTH_MINOR are set to the major and minor
version numbers

The project file originates in VS 2015.
Useful solution configurations are:
Release - Win32   for building 32-bit versions up to 3.4, with v90 toolset
Release - x64     for building 64-bit versions up to 3.4, with v90 toolset
v140 Release - Win32  for building 32-bit versions with v140 toolset (untested)
v140 Release - x64    for building 64-bit versions with v140 toolset
