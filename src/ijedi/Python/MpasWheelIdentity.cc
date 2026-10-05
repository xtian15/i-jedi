/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Python/MpasWheelIdentity.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "pybind11/stl.h"

namespace py = pybind11;

namespace ijedi {
namespace {

std::string fileSha256(const py::object &path) {
  const py::object hashlib = py::module_::import("hashlib");
  const py::object builtins = py::module_::import("builtins");
  const py::object digest = hashlib.attr("sha256")();
  py::object stream = builtins.attr("open")(path, "rb");
  while (true) {
    const py::bytes chunk = stream.attr("read")(1024 * 1024);
    if (py::len(chunk) == 0) break;
    digest.attr("update")(chunk);
  }
  stream.attr("close")();
  return digest.attr("hexdigest")().cast<std::string>();
}

py::object verifyInstalledWheel(const py::object &wheel) {
  const py::object pathlib = py::module_::import("pathlib");
  const py::object metadata = py::module_::import("importlib.metadata");
  const py::object zipfile = py::module_::import("zipfile");
  const py::object distribution = metadata.attr("distribution")("mpas-pytorch");
  const py::object installRoot = pathlib.attr("Path")(
      distribution.attr("locate_file")(""));
  const py::object archive = zipfile.attr("ZipFile")(wheel, "r");
  py::set expectedPackageFiles;
  for (const py::handle nameHandle : archive.attr("namelist")()) {
    const std::string name = py::cast<std::string>(nameHandle);
    if (name.empty() || name.back() == '/' || name.find(".dist-info/RECORD") != std::string::npos) {
      continue;
    }
    const py::object installedPath = installRoot.attr("joinpath")(name);
    if (!installedPath.attr("is_file")().cast<bool>()) {
      archive.attr("close")();
      throw std::runtime_error("installed MPAS distribution is missing wheel member: " + name);
    }
    const py::object wheelBytes = archive.attr("read")(name);
    const py::object installedBytes = installedPath.attr("read_bytes")();
    if (!wheelBytes.equal(installedBytes)) {
      archive.attr("close")();
      throw std::runtime_error("installed MPAS distribution differs from wheel member: " + name);
    }
    if (name.rfind("mpas_pytorch/", 0) == 0) expectedPackageFiles.add(py::str(name));
  }
  archive.attr("close")();

  const py::object packageRoot = installRoot.attr("joinpath")("mpas_pytorch");
  py::set installedPackageFiles;
  for (const py::handle pathHandle : packageRoot.attr("rglob")("*")) {
    const py::object path = py::reinterpret_borrow<py::object>(pathHandle);
    if (!path.attr("is_file")().cast<bool>()) continue;
    const std::string pathString = py::str(path);
    const bool isBytecode =
        pathString.size() >= 4 && pathString.compare(pathString.size() - 4, 4, ".pyc") == 0;
    if (pathString.find("/__pycache__/") != std::string::npos || isBytecode) {
      continue;
    }
    installedPackageFiles.add(py::str(path.attr("relative_to")(installRoot)));
  }
  if (!installedPackageFiles.equal(expectedPackageFiles)) {
    throw std::runtime_error(
        "installed MPAS package file inventory differs from the exact wheel");
  }
  return packageRoot.attr("joinpath")("__init__.py").attr("resolve")();
}

}  // namespace

void verifyInstalledMpasWheel(const PythonRuntime &runtime,
                              const std::string &wheelPath,
                              const std::string &expectedWheelSha256) {
  py::gil_scoped_acquire acquire;
  const py::object pathlib = py::module_::import("pathlib");
  const py::object wheel = pathlib.attr("Path")(wheelPath).attr("resolve")();
  const std::string wheelDigest = fileSha256(wheel);
  if (wheelDigest != expectedWheelSha256) {
    throw std::runtime_error("MPAS wheel digest mismatch: expected " +
                             expectedWheelSha256 + ", got " + wheelDigest);
  }
  const py::object expectedPackageFile = verifyInstalledWheel(wheel);
  const py::object mpas = runtime.importModule("mpas_pytorch");
  const py::object actualPackageFile =
      pathlib.attr("Path")(mpas.attr("__file__")).attr("resolve")();
  if (!actualPackageFile.equal(expectedPackageFile)) {
    throw std::runtime_error("imported MPAS package is not the distribution verified "
                             "against the exact wheel");
  }
}

}  // namespace ijedi
