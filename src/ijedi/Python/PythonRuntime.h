/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <memory>
#include <string>

#include "pybind11/embed.h"

namespace ijedi {

class PythonRuntime {
 public:
  explicit PythonRuntime(const std::string &pythonExecutable);
  ~PythonRuntime();

  PythonRuntime(const PythonRuntime &) = delete;
  PythonRuntime &operator=(const PythonRuntime &) = delete;
  PythonRuntime(PythonRuntime &&) = delete;
  PythonRuntime &operator=(PythonRuntime &&) = delete;

  pybind11::object importModule(const std::string &) const;
  std::string executable() const;

 private:
  std::unique_ptr<pybind11::scoped_interpreter> interpreter_;
};

}  // namespace ijedi
