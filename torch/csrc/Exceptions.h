#pragma once

#include <exception>
#include <string>
#include <memory>
#include <queue>

#include <torch/csrc/THP_export.h>
#include <torch/csrc/utils/auto_gil.h>
#include <torch/csrc/jit/script/jit_exception.h>
#include <torch/csrc/WindowsTorchApiMacro.h>
#include "c10/util/StringUtil.h"
#include "c10/util/Exception.h"

/// NOTE [ Conversion Cpp Python Warning ]
/// Python warning semantic is different from the cpp one in that
/// they can raise errors. This leads to the following cases:
/// - The GIL is acquired in the EnforceWarningBuffer destructor
///   - If there is no Error raised in the inner try/catch, the
///     bufferred warnings are processed as python warnings.
///     - If they don't raise an error, the function process with the
///       original return code.
///     - If any of them raise an error, the error code is set and
///       the destructor will raise a python_error() that will be
///       caught by the outer try/catch that will be able to change
///       the return value of the function to reflect the error.
///   - If an Error was raised in the inner try/catch, the inner try/catch
///     must set the python error. The buffered warnings are then
///     processed as cpp warnings as we cannot predict before hand
///     whether a python warning will raise an error or not and we
///     cannot handle two errors at the same time.
#define HANDLE_TH_ERRORS                                           \
  try {                                                            \
    torch::EnforceWarningBuffer __enforce_warning_buffer;          \
    try{

#define CATCH_TH_ERRORS(retstmnt)                                      \
    catch (python_error & e) {                                       \
      retstmnt;                                                 \
    }                                                                \
    catch (const c10::IndexError& e) {                               \
      auto msg = torch::processErrorMsg(e.what_without_backtrace()); \
      PyErr_SetString(PyExc_IndexError, msg.c_str());                \
      retstmnt;                                                 \
    }                                                                \
    catch (const c10::Error& e) {                                    \
      auto msg = torch::processErrorMsg(e.what_without_backtrace()); \
      PyErr_SetString(PyExc_RuntimeError, msg.c_str());              \
      retstmnt;                                                 \
    }                                                                \
    catch (torch::PyTorchError & e) {                                \
      auto msg = torch::processErrorMsg(e.what());                   \
      PyErr_SetString(e.python_type(), msg.c_str());                 \
      retstmnt;                                                 \
    }                                                                \
    catch (const std::exception& e) {                                \
      auto msg = torch::processErrorMsg(e.what());                   \
      PyErr_SetString(PyExc_RuntimeError, msg.c_str());              \
      retstmnt;                                                 \
    }

#define END_HANDLE_TH_ERRORS_PYBIND                              \
    }                                                                \
    catch (py::error_already_set & e) {                                       \
      /* Unpack already stored error to be detectable by warning code */ \
      e.restore(); \
      throw;                                                 \
    }                                                                \
    catch (py::builtin_exception & e) {                                       \
      /* Unpack already stored error to be detectable by warning code */ \
      e.set_error(); \
      throw;                                                 \
    }                                                                \
    catch (torch::jit::JITException & e) {\
      /* Special case for JITException that are explicitely unpacked by pybind */ \
      /* Set a temporary python error to be detectable by warning code */ \
      PyErr_SetString(PyExc_RuntimeError, "JITException");              \
      throw; \
    } \
    CATCH_TH_ERRORS(throw)                                          \
  }                                                                  \
  catch (py::error_already_set & e) {                                       \
    /* Repack already stored error */ \
    throw py::error_already_set();                                                 \
  }                                                                \
  catch (py::builtin_exception & e) {                                       \
    /* Repack already stored error */ \
    throw py::error_already_set();                                                 \
  }                                                                \
  catch (torch::jit::JITException & e) {\
    /* Special case for JITException that are explicitely unpacked by pybind */ \
    /* Clear the temporary error message we used */ \
    PyErr_Clear(); \
    throw; \
  } \
  CATCH_TH_ERRORS(throw py::error_already_set())

#define END_HANDLE_TH_ERRORS_RET(retval)                             \
    }                                                                \
    CATCH_TH_ERRORS(return retval)                                          \
  }                                                                  \
  CATCH_TH_ERRORS(return retval)

#define END_HANDLE_TH_ERRORS END_HANDLE_TH_ERRORS_RET(nullptr)

extern PyObject *THPException_FatalError;

// Throwing this exception means that the python error flags have been already
// set and control should be immediately returned to the interpreter.
struct python_error : public std::exception {
  python_error() : type(nullptr), value(nullptr), traceback(nullptr) {}

  python_error(const python_error &other) : type(other.type), value(other.value), traceback(other.traceback) {
    AutoGIL gil;
    Py_XINCREF(type);
    Py_XINCREF(value);
    Py_XINCREF(traceback);
  }

  python_error(python_error&& other) {
    type = other.type;
    value = other.value;
    traceback = other.traceback;
    other.type = nullptr;
    other.value = nullptr;
    other.traceback = nullptr;
  }

  ~python_error() override {
    if (type || value || traceback) {
      AutoGIL gil;
      Py_XDECREF(type);
      Py_XDECREF(value);
      Py_XDECREF(traceback);
    }
  }

  /** Saves the exception so that it can be re-thrown on a different thread */
  inline void persist() {
    if (type) return; // Don't overwrite exceptions
    // PyErr_Fetch overwrites the pointers
    AutoGIL gil;
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
    PyErr_Fetch(&type, &value, &traceback);
  }

  /** Sets the current Python error from this exception */
  inline void restore() {
    if (!type) return;
    // PyErr_Restore steals references
    AutoGIL gil;
    Py_XINCREF(type);
    Py_XINCREF(value);
    Py_XINCREF(traceback);
    PyErr_Restore(type, value, traceback);
  }

  PyObject* type;
  PyObject* value;
  PyObject* traceback;
};

#ifdef _THP_CORE

bool THPException_init(PyObject *module);
#endif

namespace torch {

THP_CLASS std::string processErrorMsg(std::string str);

// Abstract base class for exceptions which translate to specific Python types
struct PyTorchError : public std::exception {
  virtual PyObject* python_type() = 0;
  const char* what() const noexcept override {
    return msg.c_str();
  }
  std::string msg;
};

// Translates to Python IndexError
struct IndexError : public PyTorchError {
  IndexError(const char *format, ...);
  PyObject* python_type() override {
    return PyExc_IndexError;
  }
};

// Translates to Python TypeError
struct TypeError : public PyTorchError {
  TORCH_API TypeError(const char *format, ...);
  PyObject* python_type() override {
    return PyExc_TypeError;
  }
};

// Translates to Python ValueError
struct ValueError : public PyTorchError {
  ValueError(const char *format, ...);
  PyObject* python_type() override {
    return PyExc_ValueError;
  }
};

struct EnforceWarningBuffer {
public:
/// See NOTE [ Conversion Cpp Python Warning ] for noexcept justification
  EnforceWarningBuffer() noexcept(true);
  ~EnforceWarningBuffer() noexcept(false);

private:
  c10::Warning::handler_t prev_handler;
};

} // namespace torch
