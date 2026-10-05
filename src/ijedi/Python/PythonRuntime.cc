/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Python/PythonRuntime.h"

#include <stdexcept>

namespace py = pybind11;

namespace ijedi {
namespace {

std::unique_ptr<py::scoped_interpreter> startInterpreter(
    const std::string &pythonExecutable) {
  if (pythonExecutable.empty()) {
    throw std::invalid_argument("embedded Python executable must not be empty");
  }

  PyConfig config;
  PyConfig_InitPythonConfig(&config);
  config.parse_argv = 0;
  config.install_signal_handlers = 0;
  // The official JEDI container exports a global PYTHONPATH.  Accepting it
  // would put its CUDA-enabled Torch ahead of the exact MPAS virtual
  // environment even when program_name names the correct interpreter.
  config.use_environment = 0;
  // Environment-derived Python flags are intentionally ignored. Set this
  // explicitly: PYTHONDONTWRITEBYTECODE cannot protect the immutable SDK here.
  config.write_bytecode = 0;
  config.user_site_directory = 0;
  config.safe_path = 1;
  const PyStatus status =
      PyConfig_SetBytesString(&config, &config.program_name, pythonExecutable.c_str());
  if (PyStatus_Exception(status) != 0) {
    const std::string message = PyStatus_IsError(status) != 0
                                    ? status.err_msg
                                    : "failed to configure embedded Python";
    PyConfig_Clear(&config);
    throw std::runtime_error(message);
  }
  const char *argv[] = {pythonExecutable.c_str()};
  return std::make_unique<py::scoped_interpreter>(&config, 1, argv, false);
}

}  // namespace

PythonRuntime::PythonRuntime(const std::string &pythonExecutable)
    : interpreter_(startInterpreter(pythonExecutable)) {
  if (executable() != pythonExecutable) {
    throw std::runtime_error("embedded Python executable mismatch: requested '" +
                             pythonExecutable + "', initialized '" + executable() + "'");
  }
}

PythonRuntime::~PythonRuntime() = default;

py::object PythonRuntime::importModule(const std::string &name) const {
  py::gil_scoped_acquire acquire;
  try {
    return py::module_::import(name.c_str());
  } catch (const py::error_already_set &error) {
    throw std::runtime_error("failed to import Python module '" + name + "':\n" + error.what());
  }
}

std::string PythonRuntime::executable() const {
  py::gil_scoped_acquire acquire;
  return py::module_::import("sys").attr("executable").cast<std::string>();
}

}  // namespace ijedi
