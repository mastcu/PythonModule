//PythonPlugin.cpp : Defines the initialization routines for the DLL.
//

/* This project was created in Visual Studio 2003 as an MFC extension DLL
  Namely, select a New Project of MFC DLL type, then under Application Settings
  select MFC Extension DLL.  (Note that a Generic DLL could not call into SerialEM,
  and a Regular MFC DLL had instructions to insert a macro call at the beginning of 
  every function if the DLL was going to call into MFC.)

  Edit stdafx.h to add the line:
  #define DLL_IM_EX _declspec(dllimport)

  Modify the project properties for all configurations to:
  Add the SerialEM directory as an additional include directory (C++ = general)
  Add SerialEM\SerialEM.lib to linker input as an additional dependency


*/


#include "stdafx.h"
#include <afxdllx.h>
#include "Python.h"

// Add includes from SerialEM as needed, but only call global functions, not class members
// Plugin flags are defined in SerialEM.h, use PLUGFLAG_SCRIPT_LANG
// You do will need to include SerialEM.h, MacroProcessor.h and SEMUtilities.h,
// and link with the SerialEM import library
#include "SerialEM.h"
#include "MacroProcessor.h"
#include "ParameterIO.h"           // Not needed unless testing as in the RunScript below
#include "Utilities/SEMUtilities.h"

// Our own declarations
//#include "PythonPlugin.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

static AFX_EXTENSION_MODULE PythonPlugin = { NULL, NULL };

extern "C" int APIENTRY
DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
	// Remove this if you use lpReserved
	UNREFERENCED_PARAMETER(lpReserved);

	if (dwReason == DLL_PROCESS_ATTACH)
	{
		TRACE0("PythonPlugin.DLL Initializing!\n");
		
		// Extension DLL one-time initialization
		if (!AfxInitExtensionModule(PythonPlugin, hInstance))
			return 0;

		// Insert this DLL into the resource chain
		// NOTE: If this Extension DLL is being implicitly linked to by
		//  an MFC Regular DLL (such as an ActiveX Control)
		//  instead of an MFC application, then you will want to
		//  remove this line from DllMain and put it in a separate
		//  function exported from this Extension DLL.  The Regular DLL
		//  that uses this Extension DLL should then explicitly call that
		//  function to initialize this Extension DLL.  Otherwise,
		//  the CDynLinkLibrary object will not be attached to the
		//  Regular DLL's resource chain, and serious problems will
		//  result.

		new CDynLinkLibrary(PythonPlugin);

	}
	else if (dwReason == DLL_PROCESS_DETACH)
	{
		TRACE0("PythonPlugin.DLL Terminating!\n");

		// Terminate the library before destructors are called
		AfxTermExtensionModule(PythonPlugin);
	}
	return 1;   // ok
}

// Define the name that will be used in macro calls, including an optional version number
#define STR(X) #X
#define ASSTR(X) STR(X)
const char *sPlugName = "Python" ASSTR(PY_MAJOR_VERSION) "." ASSTR(PY_MINOR_VERSION);

#if PY_MAJOR_VERSION > 3 || (PY_MAJOR_VERSION == 3 && PY_MINOR_VERSION >= 4)
#define BUFFER_TYPE
#endif

// Your functions must use this calling convention
#define DLLEXPORT extern "C" __declspec (dllexport)

#define INT_NO_VALUE -2000000000
#define ERR_BUF_SIZE 320
static char sErrorBuf[ERR_BUF_SIZE] = {0x00};
static ScriptLangData *sScriptData;
static HANDLE sDoneEvent = NULL;
#if PY_MAJOR_VERSION >= 3
static wchar_t *sProgram = NULL;
#else
static char *sProgram = NULL;
#endif
static PyObject *sSerialEMError;
static PyObject *sExitedError;
static FILE *sFpErrLog;
static bool sExitWasCalled;
static CString sErrLogName;

/*
* Static functions
*/
static PyObject *RunCommand(int funcCode, const char *name, const char *keys,
  PyObject *args);
static int fgetline(FILE *fp, char s[], int limit);


// This is the initialization function to tell SerialEM about the plugin
DLLEXPORT int SEMPluginInfo(const char **namep, int *flags, int *numFunc)
{
  *numFunc = 0;
  *flags = PLUGFLAG_SCRIPT_LANG;
  *namep = sPlugName;
  return 0;
}

// This function is called repeatedly by SerialEM to get information about the 
// macro-callable functions which we said there are none of
DLLEXPORT int SEMCallInfo(int which, const char **namep, int *numInts, int *numDbls,
                          int *ifString)
{
  return 0;
}

// You need not have any macro-callable functions
// See PluginTest or PluginManager.cpp for the forms of these functions:
/*DLLEXPORT double SpecialFunction(int i1)
{
  SEMTrace('1', "SpecialFunction called with value %d", i1);
  return 10.;
}*/

// These are three handy functions for storing or reporting errors that can be used in
// your plugin, if you are linking with SerialEM.lib
void DebugToLog(const char *message)
{
  SEMTrace('[', "%s", message); // Pick a suitable key letter
}

void ErrorToLog(const char *message) 
{
  strcpy_s(sErrorBuf, ERR_BUF_SIZE, message);
  DebugToLog(message);
}

void EitherToLog(const char *prefix, const char *message, bool saveErr)
{
  if (saveErr)
    ErrorToLog(message);
  else
    DebugToLog(message);
}

/*
* An even more convenient function for debug output
*/
void DebugFmt(char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vsprintf_s(sErrorBuf, ERR_BUF_SIZE, fmt, args);
  va_end(args);
  DebugToLog(sErrorBuf);
}


/*
 * Now for Python handling
 * Macros defining the different types of standardized calls
 */

/*
 * Buffer Protocol-related code is largely copied from the example in
 *  https://jakevdp.github.io/blog/2014/05/05/introduction-to-the-python-buffer-protocol/
 * which is released with a BSD-type license
 */
#ifdef BUFFER_TYPE
 /* This is where we define the PyBufferImage object structure */
typedef struct {
  PyObject_HEAD
    
  /* Type-specific fields go below. */
  void *array;
  int imType;
  int rowBytes;
  int sizeX, sizeY;
  int itemSize;
  Py_ssize_t shape[2];
  Py_ssize_t strides[2];
  char format[8];
} PyBufferImage;


/* This is the __init__ function, implemented in C */
static int PyBufferImage_init(PyBufferImage *bufIm, PyObject *args, PyObject *kwds)
{
  char *strPtr;
  int bufInd, len;
  bufIm->array = NULL;

  if (kwds) {
    PyErr_SetString(PyExc_TypeError, "Keywords are not supported when initializing"
      " a bufferImage");
    return -1;   // -1 works here, no second error
  }

  // Get the buffer argument
  if (!PyArg_ParseTuple(args, "s", &strPtr))
    return -1;
  len = (int)strlen(strPtr);
  if (len == 1 || (len == 2 && strPtr[1] == 'F')) {
    bufInd = strPtr[0] - 'A';
    if (bufInd >= 0 && ((len == 1 && bufInd < MAX_BUFFERS) ||
      (len == 2 && bufInd < MAX_FFT_BUFFERS))) {
      bufIm->array = SEMGetBufferImage(bufInd, len - 1, bufIm->imType,
        bufIm->rowBytes, bufIm->sizeX, bufIm->sizeY);
      if (!bufIm->array) {
        PyErr_Format(sSerialEMError, "There is no image in buffer %s", strPtr);

        // -1 here and below is silent, 0 gives an exception from the exception with
        // "returned a result with an error set"
        return 0;  
      }
      switch (bufIm->imType) {
      case MRC_MODE_BYTE:
        bufIm->itemSize = 1;
        strcpy(bufIm->format, "B");
        break;
      case MRC_MODE_FLOAT:
        bufIm->itemSize = 4;
        strcpy(bufIm->format, "f");
        break;
      case MRC_MODE_SHORT:
        bufIm->itemSize = 2;
        strcpy(bufIm->format, "h");
        break;
      case MRC_MODE_USHORT:
        bufIm->itemSize = 2;
        strcpy(bufIm->format, "H");
        break;
      case MRC_MODE_RGB:
        bufIm->itemSize = 3;
        strcpy(bufIm->format, "BBB");
        break;
      default:
        PyErr_Format(sSerialEMError, "The image in buffer %s has mode %d which is"
          " unsupported", bufInd, bufIm->imType);
        return 0;
      }
      DebugFmt("Initialized image %d %d %d %d", bufIm->imType, bufIm->rowBytes, bufIm->sizeX, bufIm->sizeY);
      return 0;
    }
  }
  PyErr_Format(sSerialEMError, "\"%s\" is not a valid buffer specification", strPtr);
  return 0;
}


/* this function is called when the object is deallocated */
static void PyBufferImage_dealloc(PyBufferImage* bufIm)
{
  Py_TYPE(bufIm)->tp_free((PyObject*)bufIm);
}

// The function to fill the buffer object values.  UNTESTED
static int PyBufferImage_getbuffer(PyObject *obj, Py_buffer *view, int flags)
{
  if (view == NULL) {
    PyErr_SetString(PyExc_ValueError, "NULL view in getbuffer");
    return -1;
  }
  if (flags & PyBUF_WRITABLE) {
    PyErr_SetString(sSerialEMError, "Calling function requested a writable buffer image");
    return -1;
  }

  PyBufferImage* bufIm = (PyBufferImage*)obj;

  // Refuse a request for strictly contiguous code (no strides) if the stride is actually
  // needed
  if (!(flags & PyBUF_ANY_CONTIGUOUS) &&
    bufIm->rowBytes != bufIm->itemSize * bufIm->sizeX) {
    PyErr_SetString(sSerialEMError, "Calling function requested a contiguous buffer image"
      " without strides and the image has padded lines");
    return -1;
  }

  // The first dimension is the slow one
  bufIm->shape[0] = bufIm->sizeY;
  bufIm->shape[1] = bufIm->sizeX;
  bufIm->strides[0] = bufIm->rowBytes;
  bufIm->strides[1] = bufIm->itemSize;

  view->obj = obj;
  view->buf = (void*)bufIm->array;
  view->len = bufIm->rowBytes * bufIm->sizeY;
  view->readonly = 1;
  view->itemsize = sizeof(int);
  view->format = bufIm->format;
  view->ndim = 2;
  view->shape = &bufIm->shape[0];
  view->strides = (flags & PyBUF_ANY_CONTIGUOUS) ? &bufIm->strides[0] : NULL;
  view->suboffsets = NULL;
  view->internal = NULL;

  Py_INCREF(bufIm);  // need to increase the reference count
  return 0;
}

static PyBufferProcs PyBufferImage_as_buffer = {
  // this definition is only compatible with Python 3.3 and above
  (getbufferproc)PyBufferImage_getbuffer,
  (releasebufferproc)0,  // we do not require any special release function
};

/* Here is the type structure: we put the above functions in the appropriate place
in order to actually define the Python object type */
static PyTypeObject PyBufferImageType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  "serialem.PyBufferImage",        /* tp_name */
  sizeof(PyBufferImage),            /* tp_basicsize */
  0,                            /* tp_itemsize */
  (destructor)PyBufferImage_dealloc,/* tp_dealloc */
  0,                            /* tp_print */
  0,                            /* tp_getattr */
  0,                            /* tp_setattr */
  0,                            /* tp_reserved */
  0,                            /* tp_repr */
  0,                            /* tp_as_number */
  0,                            /* tp_as_sequence */
  0,                            /* tp_as_mapping */
  0,                            /* tp_hash  */
  0,                            /* tp_call */
  0,                            /* tp_str */
  0,                            /* tp_getattro */
  0,                            /* tp_setattro */
  &PyBufferImage_as_buffer,     /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT,           /* tp_flags */
  "PyBufferImage object",           /* tp_doc */
  0,                            /* tp_traverse */
  0,                            /* tp_clear */
  0,                            /* tp_richcompare */
  0,                            /* tp_weaklistoffset */
  0,                            /* tp_iter */
  0,                            /* tp_iternext */
  0,                            /* tp_methods */
  0,                            /* tp_members */
  0,                            /* tp_getset */
  0,                            /* tp_base */
  0,                            /* tp_dict */
  0,                            /* tp_descr_get */
  0,                            /* tp_descr_set */
  0,                            /* tp_dictoffset */
  (initproc)PyBufferImage_init,     /* tp_init */
};

// End of buffer protocol code based on the blog */

PyObject *serialem_IsImageValid(PyObject *self, PyObject *args)
{
  int bufInd, ifFFT;
  PyBufferImage *bufIm;
  if (!PyArg_ParseTuple(args, "O", &bufIm))
    return NULL;
  if (SEMIsBufferImageValid(bufIm->array, bufIm->imType, bufIm->rowBytes, bufIm->sizeX,
    bufIm->sizeY, bufInd, ifFFT)) {
    return Py_BuildValue("ii", bufInd, ifFFT);
  } else {
    Py_RETURN_NONE;
  }
}
#endif

// This is both the pattern for making a specialized set as done below,
// and also necessary to define away the rest of entries in the master list
#define MAC_SAME_FUNC(nam, req, flg, fnc, cme)  
#define MAC_SAME_NAME(nam, req, flg, cme) MAC_SAME_FUNC(nam, req, flg, 0, cme)
#define MAC_DIFF_NAME  MAC_SAME_FUNC

// ARGUMENTS
#define MAC_SAME_FUNC_ARG(nam, req, flg, fnc, cme, key)   \
PyObject *serialem_##nam(PyObject *self, PyObject *args) \
{  \
  return RunCommand(CME_##cme, #nam, #key, args);     \
}
#define MAC_SAME_NAME_ARG(nam, req, flg, cme, key) MAC_SAME_FUNC_ARG(nam, req, flg, 0, cme, key)
#define MAC_DIFF_NAME_ARG MAC_SAME_FUNC_ARG

// NO ARG
#define MAC_SAME_FUNC_NOARG(nam, req, flg, fnc, cme)    \
PyObject *serialem_##nam(PyObject *self, PyObject *args) \
{  \
  return RunCommand(CME_##cme, #nam, "", args);     \
}
#define MAC_SAME_NAME_NOARG(nam, req, flg, cme) MAC_SAME_FUNC_NOARG(nam, req, flg, 0, cme)

/*
 * Now include all the calls
 */

#include "MacroMasterList.h"


// Definitions for building up the method table

#define MAC_SAME_FUNC(nam, req, flg, fnc, cme)  
#define MAC_SAME_NAME(nam, req, flg, cme) MAC_SAME_FUNC(nam, req, flg, 0, cme)
#define MAC_DIFF_NAME  MAC_SAME_FUNC

// ARGUMENTS
#define MAC_SAME_FUNC_ARG(nam, req, flg, fnc, cme, key) \
  {#nam, serialem_##nam, METH_VARARGS},
#define MAC_SAME_NAME_ARG(nam, req, flg, cme, key) MAC_SAME_FUNC_ARG(nam, req, flg, 0, cme, key)
#define MAC_DIFF_NAME_ARG MAC_SAME_FUNC_ARG

// NO ARG
#define MAC_SAME_FUNC_NOARG(nam, req, flg, fnc, cme)  \
  MAC_SAME_FUNC_ARG(nam, req, flg, fnc, cme, 0)
#define MAC_SAME_NAME_NOARG(nam, req, flg, cme) MAC_SAME_FUNC_ARG(nam, req, flg, 0, cme, 0)

// Include to make the method table
static PyMethodDef serialemmethods[] = {
#include "MacroMasterList.h"
#ifdef BUFFER_TYPE
  {"isImageValid", serialem_IsImageValid, METH_VARARGS},
#endif
  {NULL, NULL}};

// Define the module
#if PY_MAJOR_VERSION >= 3
static PyModuleDef serialemModule = {
  PyModuleDef_HEAD_INIT, "serialem", NULL, -1, serialemmethods,
  NULL, NULL, NULL, NULL
};
#endif

#if PY_MAJOR_VERSION >= 3
static PyObject *PyInit_serialem(void)
#else
PyMODINIT_FUNC initserialem(void)
#endif
{
  PyObject *mod;

#ifdef BUFFER_TYPE
  PyBufferImageType.tp_new = PyType_GenericNew;
  if (PyType_Ready(&PyBufferImageType) < 0)
    return NULL;
  Py_INCREF(&PyBufferImageType);
#endif

#if PY_MAJOR_VERSION >= 3
  mod = PyModule_Create(&serialemModule);
  if (mod == NULL)
    return NULL;
#else
  mod = Py_InitModule("serialem", serialemmethods);
  if (!mod)
    return;
#endif

  // Set up two kinds of exceptions, error and exit
  sSerialEMError = PyErr_NewException("serialem.SEMerror", NULL, NULL);
  Py_XINCREF(sSerialEMError);
  sExitedError = PyErr_NewException("serialem.SEMexited", NULL, NULL);
  Py_XINCREF(sExitedError);
  if (PyModule_AddObject(mod, "SEMerror", sSerialEMError) < 0 ||
    PyModule_AddObject(mod, "SEMexited", sExitedError) < 0
#ifdef BUFFER_TYPE
    || PyModule_AddObject(mod, "bufferImage", (PyObject *)&PyBufferImageType) < 0
#endif
    ) {

    Py_XDECREF(sSerialEMError);
    Py_CLEAR(sSerialEMError);
    Py_XDECREF(sExitedError);
    Py_CLEAR(sExitedError);
#ifdef BUFFER_TYPE
    Py_DECREF(&PyBufferImageType);
#endif
    Py_DECREF(mod);
#if PY_MAJOR_VERSION >= 3
    return NULL;
#endif
  }

#if PY_MAJOR_VERSION >= 3
  return mod;
#endif
}

/*
* Include Initialize function to get the data structure and the done event and do any
* needed initialization; include Uninitialize if needed for shutdown
*
*/
DLLEXPORT int Initialize()
{
  CString str;
  int nArgs;

  // Make unique name for error log file
  char *tempDir = getenv("TEMP");
  if (!tempDir)
    tempDir = getenv("TMP");
  str = "";
  if (tempDir)
    str = CString(tempDir) + "\\";

  sErrLogName.Format("%s%s-SEMerrors-%04d.log", (LPCTSTR)str, sPlugName, 
    GetTickCount() % 10000);

  sScriptData = SEMGetScriptLangData();
  sDoneEvent = CreateEventA(NULL, FALSE, FALSE, SCRIPT_EVENT_NAME);
  if (!sDoneEvent) {
    DebugFmt("Plugin %s Could not create done event", sPlugName);
    return 1;
  }

  /*FILE *fp;
  if (AllocConsole()) {
    freopen_s(&fp, "CONIN$", "rb", stdin);   // reopen stdin handle as console window input
    freopen_s(&fp, "CONOUT$", "wb", stdout);  // reopen stout handle as console window output
    freopen_s(&fp, "CONOUT$", "wb", stderr);  // reopen stderr handle as console window output
    DebugToLog("Reopened output to console");
  }*/


  LPWSTR programw = CommandLineToArgvW(GetCommandLineW(), &nArgs)[0];
#if PY_MAJOR_VERSION >= 3
#if PY_MINOR_VERSION > 4
  sProgram = Py_DecodeLocale((char *)programw, NULL);
#else
  sProgram = (wchar_t *)programw;
#endif
#else
  bstr_t bst(programw);
  sProgram = (char *)bst;
#endif
  if (!sProgram) {
    DebugFmt("Plugin %s could not get program name", sPlugName);
    return 1;
  }

  // reopen stderr handle to file
  freopen_s(&sFpErrLog, (LPCTSTR)sErrLogName, "wb+", stderr);  
  DebugToLog("Reopened output to " + sErrLogName);

  // Finish initialization
  PyImport_AppendInittab("serialem",
#if PY_MAJOR_VERSION >= 3
    & PyInit_serialem);
#else
    &initserialem);
#endif
  Py_SetProgramName(sProgram);  /* optional but recommended */
  Py_Initialize();
  DebugToLog("Done initializing");
  return 0;
}

// Uninitialize at end: and clean up error log
DLLEXPORT void Uninitialize()
{
  if (!sProgram)
    return;
  Py_Finalize();
#if PY_MAJOR_VERSION >= 3
  PyMem_RawFree(sProgram);
#endif
  fclose(sFpErrLog);
  DeleteFile((LPCTSTR)sErrLogName);
}

/*
 * The function for passing in a script to run.  It should return 0 on completion of 
 * script and 1 on an error
 */
int fgetline(FILE *fp, char s[], int limit);
double wallTime();

DLLEXPORT int RunScript(const char *script)
{
  const int maxLine = 160;
  char buffer[maxLine];
  int err, retval;
  
  sExitWasCalled = false;
  sScriptData->gotExceptionText = false;
  /* This is the alternate way of running the script that avoids having to 
  finalize-initialize just to clear out errors.  It ruins the writing of syntax errors
  to the error file, and would require additional code to try to access the syntax 
  errors, which did not work when it was tried
  PyObject *pdict = PyDict_New();
  PyDict_SetItemString(pdict, "__builtins__", PyEval_GetBuiltins());
  PyObject *obj = PyRun_String(script, Py_file_input, pdict, pdict);
  retval = obj ? 0 : -1; */

  // The simple way to run the script
  retval= PyRun_SimpleString(script);
  DebugFmt("RunScript returning %d", retval);

  // If there was an error return that did not happen after exit called from a wrapper,
  // look for error text in the log.
  if (retval && !sExitWasCalled) {

    // Shut down python! and close the log file, reopen it to read
    sScriptData->strItems[0] = "";
    Py_Finalize();
    fclose(sFpErrLog);
    fopen_s(&sFpErrLog, (LPCTSTR)sErrLogName, "rb");
    if (sFpErrLog) {

      // Read in the lines as usual, pack up text witha prefix
      for (;;) {
        err = fgetline(sFpErrLog, buffer, maxLine);
        if (!err)
          continue;
        if (err == -2 || err == -1)
          break;
        if (!sScriptData->strItems[0].IsEmpty())
          sScriptData->strItems[0] += "\r\n";
        else
          sScriptData->strItems[0] = "\r\nError running Python script:\r\n";
        sScriptData->strItems[0] += buffer;
        sScriptData->gotExceptionText = true;
        if (err < 0)
          break;
      }
      fclose(sFpErrLog);
    } else
      ErrorToLog("Error repoening log file to read message");

    // Open the log file again and re-initialize
    freopen_s(&sFpErrLog, (LPCTSTR)sErrLogName, "wb", stderr);  
    DebugToLog("Reopened output to error log");
    Py_Initialize();

  } else {

    // And this is needed to clean out the set variables etc after a run with SimpleScript
    double start = GetTickCount();
    Py_Finalize();
    Py_Initialize();
    DebugFmt("finalize - init time %.0f", SEMTickInterval(start));
  }
  return retval;
}

/*
 * Function run by all functions called from the script interpreter to do the final  
 * setting of scriptData and handshake with SerialEM.
 * It should respond to errors as appropriate for the scripting language, such as by
 * an exception
 */
static PyObject *RunCommand(int funcCode, const char *name, const char *keys,
  PyObject *args)
{
  char *strPtrs[MAX_SCRIPT_LANG_ARGS];
  void *aP[MAX_SCRIPT_LANG_ARGS];
  int tempInts[MAX_SCRIPT_LANG_ARGS];
  CString format;
  int ind, ond, retval, numArgs = (int)strlen(keys);
  bool gotOpt = false;

  // Parse the keys and create the format string.  Allow for ints
  for (ind = 0; ind < numArgs; ind++) {
    strPtrs[ind] = NULL;
    sScriptData->itemDbl[ind + 1] = EXTRA_NO_VALUE;
    tempInts[ind] = INT_NO_VALUE;

    if ((keys[ind] == 's' || keys[ind] == 'd' || keys[ind] == 'i') && !gotOpt) {
      gotOpt = true;
      format += '|';
    }
    if (keys[ind] == 'S' || keys[ind] == 's') {
      aP[ind] = &strPtrs[ind];
      format += 's';
    } else if (keys[ind] == 'D' || keys[ind] == 'd') {
      aP[ind] = &sScriptData->itemDbl[ind + 1];
      format += 'd';
    } else if (keys[ind] == 'I' || keys[ind] == 'i') {
      aP[ind] = &tempInts[ind];
      format += 'i';
    } else {
      format = "Incorrect character in argument keys for function ";
      format += name;
      ErrorToLog((LPCTSTR)format);
      PyErr_SetString(sSerialEMError, (LPCTSTR)format);
      return NULL;
    }
  }
  format += ":";
  format += name;
  //DebugFmt("Keys %s  num %d  format %s", keys, numArgs, (LPCTSTR)format);

  // There should be a more elegant way to do this...  Parse the arguments
  switch (numArgs) {
  case 0:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format);
    break;
  case 1:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0]);
    break;
  case 2:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1]);
    break;
  case 3:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1], aP[2]);
    break;
  case 4:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3]);
    break;
  case 5:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4]);
    break;
  case 6:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4], 
      aP[5]);
    break;
  case 7:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4], 
      aP[5], aP[6]);
    break;
  case 8:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4], 
      aP[5], aP[6], aP[7]);
    break;
  case 9:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4], 
      aP[5], aP[6], aP[7], aP[8]);
    break;
  case 10:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4],
      aP[5], aP[6], aP[7], aP[8], aP[9]);
    break;
  case 11:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4], 
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10]);
    break;
  case 12:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4],
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10], aP[11]);
    break;
  case 13:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4],
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10], aP[11], aP[12]);
    break;
  case 14:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4],
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10], aP[11], aP[12], aP[13]);
    break;
  case 15:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4],
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10], aP[11], aP[12], aP[13], aP[14]);
    break;
  case 16:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4],
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10], aP[11], aP[12], aP[13], aP[14], aP[15]);
    break;
  case 17:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4],
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10], aP[11], aP[12], aP[13], aP[14], aP[15], 
      aP[16]);
    break;
  case 18:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4], 
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10], aP[11], aP[12], aP[13], aP[14], aP[15], 
      aP[16], aP[17]);
    break;
  case 19:
    retval = PyArg_ParseTuple(args, (LPCTSTR)format, aP[0], aP[1],  aP[2], aP[3], aP[4],
      aP[5], aP[6], aP[7], aP[8], aP[9], aP[10], aP[11], aP[12], aP[13], aP[14], aP[15], 
      aP[16], aP[17], aP[18]);
    break;
  }

  if (!retval)
    return NULL;

  // Move strings and ints into the shared structure and get strings for doubles/ints
  sScriptData->lastNonEmptyInd = 0;
  for (ind = 0; ind < numArgs; ind++) {
    ond = ind + 1;
    if (sScriptData->itemDbl[ond] < EXTRA_VALUE_TEST && !strPtrs[ind] && 
      tempInts[ind] == INT_NO_VALUE)
      break;
    sScriptData->lastNonEmptyInd = ond;
    if (keys[ind] == 'S' || keys[ind] == 's') {
      sScriptData->strItems[ond] = strPtrs[ind];
      sScriptData->itemDbl[ond] = atof(strPtrs[ind]);
    } else if (keys[ind] == 'D' || keys[ind] == 'd') {
      sScriptData->strItems[ond].Format("%f", sScriptData->itemDbl[ond]);
      UtilTrimTrailingZeros(sScriptData->strItems[ond]);
    } else {
      sScriptData->itemDbl[ond] = tempInts[ind];
      sScriptData->strItems[ond].Format("%d", tempInts[ind]);
    }
  }

  // Set the function code and signal ready, wait until done
  sScriptData->functionCode = funcCode;
  sScriptData->commandReady = 1;
  sExitWasCalled = funcCode == CME_EXIT;
  while (WaitForSingleObject(sDoneEvent, 1000)) {
    Sleep(2);
  }

  // When an error flag is set on the other side: Throw an exit exception if it is 
  // signaled to do that, or throw a catchable error exception unless already exiting
  // from an exception
  if (sScriptData->errorOccurred) {
    DebugFmt("PythonPlugin got an error %d", sScriptData->errorOccurred);
    if (sScriptData->errorOccurred == SCRIPT_NORMAL_EXIT) {
      PyErr_SetString(sExitedError, "Normal exit");
    } else if (sScriptData->errorOccurred == SCRIPT_EXIT_NO_EXC) {
      Py_RETURN_NONE;
    } else {
      PyErr_SetString(sSerialEMError, (LPCTSTR)SEMLastNoBoxMessage());
    }
    return NULL;
  }

  // Return None for nothing
  if (sScriptData->highestReportInd < 0) {
    Py_RETURN_NONE;
  }

  // Or return a single value
  if (!sScriptData->highestReportInd) {
    if (sScriptData->repValIsString[0]) {
      SEMTrace('[', "returning string %s", (LPCTSTR)sScriptData->reportedStrs[0]);
      return Py_BuildValue("s", (LPCTSTR)sScriptData->reportedStrs[0]);
    } else {
      SEMTrace('[', "returning float %f", sScriptData->reportedVals[0]);
      return Py_BuildValue("f", sScriptData->reportedVals[0]);
    }
  }

  // Or build up a return tuple
  PyObject *tup = PyTuple_New(sScriptData->highestReportInd + 1);
  for (ind = 0; ind <= sScriptData->highestReportInd; ind++) {
    if (sScriptData->repValIsString[ind]) {
      PyTuple_SET_ITEM(tup, ind,
        PyUnicode_FromString((LPCTSTR)sScriptData->reportedStrs[ind]));
    } else {
      PyTuple_SET_ITEM(tup, ind, PyFloat_FromDouble(sScriptData->reportedVals[ind]));
    }
  }
  return tup;
}

// A minimal version of my getline function
static int fgetline(FILE *fp, char s[], int limit)
{
  int c, i, length;

  for (i = 0; (((c = getc(fp)) != EOF) && (i < (limit - 1)) && (c != '\n')); i++)
    s[i] = c;

  /* 1/25/12: Take off a return too! */
  if (i > 0 && s[i - 1] == '\r')
    i--;

  /* A \n or EOF on the first character leaves i at 0, so there is nothing
  special to be handled about i being 1, 9/18/09 */

  s[i] = '\0';
  length = i;

  if (c == EOF)
    return (-1 * (length + 2));
  else
    return (length);
}

