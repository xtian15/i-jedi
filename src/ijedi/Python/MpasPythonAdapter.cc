/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Python/MpasPythonAdapter.h"

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

std::string tensorSha256(const py::object &tensor) {
  const py::object bytes = tensor.attr("detach")()
                               .attr("cpu")()
                               .attr("contiguous")()
                               .attr("numpy")()
                               .attr("tobytes")();
  return py::module_::import("hashlib")
      .attr("sha256")(bytes)
      .attr("hexdigest")()
      .cast<std::string>();
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

py::dict boundaryManifest(const py::dict &boundary, const py::object &keys,
                          const py::object &torch) {
  py::dict fields;
  for (const py::handle keyHandle : keys) {
    const std::string name = py::cast<std::string>(keyHandle);
    const py::object tensor = boundary[py::str(name)];
    py::dict item;
    item["dtype"] = py::str(tensor.attr("dtype"));
    item["shape"] = py::tuple(tensor.attr("shape"));
    item["sha256"] = tensorSha256(tensor);
    item["finite"] = torch.attr("isfinite")(tensor).attr("all")().attr("item")();
    fields[py::str(name)] = item;
  }
  return fields;
}

void requireEqualManifest(const py::dict &left, const py::dict &right,
                          const std::string &message) {
  if (!left.equal(right)) throw std::runtime_error(message);
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

MpasPythonAdapter::MpasPythonAdapter(const PythonRuntime &runtime) : runtime_(runtime) {}

std::string MpasPythonAdapter::runTwoStepAudit(const std::string &wheelPath,
                                               const std::string &expectedWheelSha256,
                                               const std::string &initPath,
                                               const std::string &gridPath,
                                               const std::string &namelistPath,
                                               double timeStepSeconds) const {
  py::gil_scoped_acquire acquire;
  try {
    py::module_::import("faulthandler").attr("enable")();
    const py::object pathlib = py::module_::import("pathlib");
    const py::object json = py::module_::import("json");

    const py::object wheel = pathlib.attr("Path")(wheelPath).attr("resolve")();
    const std::string wheelDigest = fileSha256(wheel);
    if (wheelDigest != expectedWheelSha256) {
      throw std::runtime_error("MPAS wheel digest mismatch: expected " + expectedWheelSha256 +
                               ", got " + wheelDigest);
    }
    const py::object expectedPackageFile = verifyInstalledWheel(wheel);
    const py::object torch = py::module_::import("torch");
    torch.attr("set_num_threads")(1);
    torch.attr("set_num_interop_threads")(1);
    if (!py::module_::import("sys").attr("dont_write_bytecode").cast<bool>()) {
      throw std::runtime_error("embedded Python bytecode policy is not immutable");
    }
    const py::object mpas = runtime_.importModule("mpas_pytorch");
    const py::object contracts = runtime_.importModule("mpas_pytorch.ijedi_contracts");
    const py::object contractError =
        runtime_.importModule("mpas_pytorch.exceptions").attr("MpasContractError");
    const py::object actualPackageFile =
        pathlib.attr("Path")(mpas.attr("__file__")).attr("resolve")();
    if (!actualPackageFile.equal(expectedPackageFile)) {
      throw std::runtime_error("imported MPAS package is not the distribution verified "
                               "against the exact wheel");
    }

    const py::dict capabilities = contracts.attr("get_ijedi_interface_capabilities")();
    const py::dict release = capabilities["release"];
    const py::dict support = release["support"];
    const py::dict unsupported = release["unsupported"];
    if (support["devices"].cast<py::list>().size() != 1 ||
        support["devices"].cast<py::list>()[0].cast<std::string>() != "cpu" ||
        support["dtypes"].cast<py::list>()[0].cast<std::string>() != "float64" ||
        support["process_model"].cast<py::list>()[0].cast<std::string>() !=
            "single_process") {
      throw std::runtime_error("installed package does not expose the required CPU float64 "
                               "single-process capability");
    }
    if (!unsupported["devices"].cast<py::list>().contains("cuda") ||
        !unsupported["process_model"].cast<py::list>().contains("MPI") ||
        !unsupported["geometry"].cast<py::list>().contains("regional") ||
        !unsupported["workflows"].cast<py::list>().contains("DA")) {
      throw std::runtime_error("installed package failed unsupported-capability preflight");
    }

    const py::dict config = mpas.attr("load_config_from_namelist")(namelistPath);
    const double configuredTimeStep = config["config_dt"].cast<double>();
    if (configuredTimeStep != timeStepSeconds) {
      throw std::runtime_error("requested timestep differs from namelist config_dt");
    }
    const py::object configurationReceipt = contracts.attr("build_configuration_receipt")(
        namelistPath, config);
    const py::tuple loaded = mpas.attr("load_initial_state")(
        initPath, gridPath, py::arg("mesh_support_path") = py::none(),
        py::arg("config") = config);
    const py::dict initial = loaded[0];
    const py::dict mesh = loaded[1];
    const py::object snapshot = contracts.attr("build_geometry_snapshot")(
        initial, mesh, py::arg("init_path") = initPath, py::arg("grid_path") = gridPath,
        py::arg("configuration_receipt") = configurationReceipt);

    const py::dict first = mpas.attr("run_simulation")(
        contracts.attr("clone_state")(initial), mesh, py::arg("nsteps") = 1,
        py::arg("config") = config);
    const py::object schema = contracts.attr("build_state_storage_schema")(
        first, mesh,
        py::arg("horizontal_geometry_receipt") = snapshot.attr("horizontal_receipt"),
        py::arg("static_vertical_geometry_receipt") =
            snapshot.attr("static_vertical_receipt"),
        py::arg("bundle_receipt") = snapshot.attr("receipt"),
        py::arg("configuration_receipt") = configurationReceipt);
    const py::dict boundary1 = contracts.attr("extract_continuation_boundary")(
        first, py::arg("schema") = schema);
    contracts.attr("validate_boundary_compatibility")(boundary1, py::arg("schema") = schema);

    const py::object continuationKeys = contracts.attr("CONTINUATION_TENSOR_KEYS");
    const py::object continuationMetadataKeys =
        contracts.attr("CONTINUATION_METADATA_KEYS");
    const py::object outputKeys = contracts.attr("REGRESSION_OUTPUT_TENSOR_KEYS");
    const py::dict beforeAdvance = boundaryManifest(first, outputKeys, torch);
    const py::dict resumed = contracts.attr("compose_continuation_state")(
        first, boundary1, py::arg("schema") = schema);
    const py::dict second = mpas.attr("run_simulation")(
        resumed, mesh, py::arg("nsteps") = 1, py::arg("config") = config);
    const py::dict boundary2 = contracts.attr("extract_continuation_boundary")(
        second, py::arg("schema") = schema);
    contracts.attr("validate_boundary_compatibility")(boundary2, py::arg("schema") = schema);

    const py::dict boundary1Again = contracts.attr("extract_continuation_boundary")(
        first, py::arg("schema") = schema);
    requireEqualManifest(boundaryManifest(boundary1, continuationKeys, torch),
                         boundaryManifest(boundary1Again, continuationKeys, torch),
                         "advancing a clone mutated the original MPAS state");

    py::dict edited = contracts.attr("clone_state")(boundary1);
    const py::object editedU = edited["u"];
    const py::object flatEditedU = editedU.attr("reshape")(-1);
    const py::object oldValue = flatEditedU.attr("__getitem__")(0);
    flatEditedU.attr("__setitem__")(0, torch.attr("add")(oldValue, 1.0));
    bool mutationRejected = false;
    try {
      contracts.attr("validate_boundary_compatibility")(edited, py::arg("schema") = schema);
    } catch (const py::error_already_set &error) {
      if (!error.matches(contractError.ptr())) throw;
      mutationRejected = true;
    }
    if (!mutationRejected) {
      throw std::runtime_error("one-value prognostic mutation did not invalidate continuation");
    }

    py::dict editedDiagnostic = contracts.attr("clone_state")(boundary1);
    const py::object editedExner = editedDiagnostic["exner"];
    const py::object flatEditedExner = editedExner.attr("reshape")(-1);
    const py::object oldExner = flatEditedExner.attr("__getitem__")(0);
    flatEditedExner.attr("__setitem__")(0, torch.attr("add")(oldExner, 1.0));
    bool diagnosticMutationRejected = false;
    try {
      contracts.attr("validate_boundary_compatibility")(editedDiagnostic,
                                                          py::arg("schema") = schema);
    } catch (const py::error_already_set &error) {
      if (!error.matches(contractError.ptr())) throw;
      diagnosticMutationRejected = true;
    }
    if (!diagnosticMutationRejected) {
      throw std::runtime_error("one-value diagnostic mutation did not invalidate continuation");
    }

    py::dict missing = contracts.attr("clone_state")(boundary1);
    missing.attr("pop")("exner");
    bool omissionRejected = false;
    try {
      contracts.attr("validate_boundary_compatibility")(missing, py::arg("schema") = schema);
    } catch (const py::error_already_set &error) {
      if (!error.matches(contractError.ptr())) throw;
      omissionRejected = true;
    }
    if (!omissionRejected) {
      throw std::runtime_error("missing exact diagnostic did not invalidate continuation");
    }

    py::dict result;
    result["schema_version"] = 2;
    result["python_executable"] = runtime_.executable();
    result["package_file"] = py::str(actualPackageFile);
    result["package_version"] = mpas.attr("__version__");
    result["wheel_path"] = py::str(wheel);
    result["wheel_sha256"] = wheelDigest;
    result["horizontal_geometry_receipt"] = snapshot.attr("horizontal_receipt");
    result["static_vertical_geometry_receipt"] =
        snapshot.attr("static_vertical_receipt");
    result["geometry_bundle_receipt"] = snapshot.attr("receipt");
    result["configuration_receipt"] = configurationReceipt;
    py::dict inputFiles;
    for (const auto &entry : {std::pair<const char *, std::string>("init", initPath),
                              std::pair<const char *, std::string>("grid", gridPath),
                              std::pair<const char *, std::string>("namelist", namelistPath)}) {
      const py::object inputPath = pathlib.attr("Path")(entry.second).attr("resolve")();
      py::dict identity;
      identity["name"] = inputPath.attr("name");
      identity["sha256"] = fileSha256(inputPath);
      inputFiles[py::str(entry.first)] = identity;
    }
    result["input_files"] = inputFiles;
    result["dt_seconds"] = configuredTimeStep;
    result["second_step_state_source"] =
        "receipt_checked_compact_boundary_plus_static_support";
    result["state_schema_digest"] = schema.attr("digest");
    result["continuation_tensor_count"] = py::len(continuationKeys);
    result["continuation_metadata_count"] = py::len(continuationMetadataKeys);
    result["regression_output_tensor_count"] = py::len(outputKeys);
    result["step1"] = beforeAdvance;
    result["step2"] = boundaryManifest(second, outputKeys, torch);
    result["negative_controls"] = py::dict(
        py::arg("prognostic_mutation_rejected") = mutationRejected,
        py::arg("diagnostic_mutation_rejected") = diagnosticMutationRejected,
        py::arg("diagnostic_omission_rejected") = omissionRejected);
    return json.attr("dumps")(result, py::arg("sort_keys") = true,
                              py::arg("separators") = py::make_tuple(",", ":"))
        .cast<std::string>();
  } catch (const py::error_already_set &error) {
    throw std::runtime_error(std::string("embedded MPAS call failed:\n") + error.what());
  }
}

}  // namespace ijedi
