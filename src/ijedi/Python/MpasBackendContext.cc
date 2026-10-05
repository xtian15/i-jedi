/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Python/MpasBackendContext.h"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <set>

#include "eckit/config/Configuration.h"
#include "eckit/thread/AutoLock.h"
#include "eckit/thread/Mutex.h"
#include "eckit/thread/StaticMutex.h"
#include "pybind11/stl.h"

#include "ijedi/Python/MpasPythonAdapter.h"
#include "ijedi/Python/PythonRuntime.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "oops/base/Variable.h"
#include "oops/base/Variables.h"
#include "oops/util/Logger.h"

namespace py = pybind11;

namespace ijedi {
namespace {

eckit::StaticMutex runtimeMutex;
std::weak_ptr<PythonRuntime> processRuntime;
bool runtimeStarted = false;

// Call with the GIL held. A Python exception must be destroyed here, before
// caller unwinding can release the last runtime and finalize the interpreter.
template <typename Call>
auto pythonBoundary(const char *operation, Call &&call) -> decltype(call()) {
  try {
    return call();
  } catch (const py::error_already_set &error) {
    throw std::runtime_error(std::string("MPAS ") + operation + " failed:\n" + error.what());
  }
}

std::shared_ptr<PythonRuntime> acquireRuntime(const std::string &pythonExecutable) {
  eckit::AutoLock<eckit::StaticMutex> guard(runtimeMutex);
  if (auto runtime = processRuntime.lock()) {
    const std::string activeExecutable = runtime->executable();
    if (activeExecutable != pythonExecutable) {
      throw std::runtime_error(
          "I-JEDI supports one embedded Python runtime per process; requested '" +
          pythonExecutable + "' after initializing '" + activeExecutable + "'");
    }
    return runtime;
  }
  // Torch's extension types do not survive CPython finalization/reinitialization.
  // Reject this unsupported lifecycle before a second interpreter can start;
  // ordinary contexts/handles share and retain the one active runtime.
  if (runtimeStarted) {
    throw std::logic_error("MPAS embedded runtime cannot restart after finalization");
  }
  auto runtime = std::make_shared<PythonRuntime>(pythonExecutable);
  runtimeStarted = true;
  processRuntime = runtime;
  return runtime;
}

template <typename T>
std::vector<T> tensorToOwnedVector(const py::object &tensor) {
  const auto array = tensor.attr("detach")()
                         .attr("cpu")()
                         .attr("contiguous")()
                         .attr("reshape")(-1)
                         .attr("numpy")()
                         .cast<py::buffer>();
  const auto buffer = array.request();
  if (buffer.ndim != 1 || !buffer.item_type_is_equivalent_to<T>() || buffer.size < 0 ||
      static_cast<size_t>(buffer.size) > 256 * 1024 * 1024 / sizeof(T) ||
      (buffer.size != 0 && (buffer.ptr == nullptr || buffer.strides.at(0) != sizeof(T)))) {
    throw std::invalid_argument(
        "MPAS output transport requires a bounded native contiguous buffer");
  }
  // Copy bytes into C++ ownership; never retain a mutable alias into Python.
  // Native signed int64 format codes may differ (l/q) at the same ABI width;
  // pybind11's equivalence check rejects unsigned, floating or swapped types.
  std::vector<T> result(static_cast<size_t>(buffer.size));
  if (!result.empty()) {
    std::memcpy(result.data(), buffer.ptr, result.size() * sizeof(T));
  }
  return result;
}

std::vector<double> tensorToDoubleVector(const py::object &tensor) {
  return tensorToOwnedVector<double>(tensor);
}

std::vector<std::int64_t> tensorToInt64Vector(const py::object &tensor) {
  return tensorToOwnedVector<std::int64_t>(tensor);
}

// Diagnostic count of unique tensor storage retained by the context's four
// flat model mappings. It excludes allocator caches and nested runtime plans.
size_t retainedTensorBytes(const py::object &torch, const std::vector<py::object> &mappings) {
  std::set<std::uint64_t> seen;
  size_t bytes = 0;
  for (const auto &mapping : mappings) {
    for (const auto &item : mapping.cast<py::dict>()) {
      if (!py::isinstance(item.second, torch.attr("Tensor"))) {
        continue;
      }
      const auto storage = item.second.attr("untyped_storage")();
      if (seen.insert(storage.attr("data_ptr")().cast<std::uint64_t>()).second) {
        bytes += storage.attr("nbytes")().cast<size_t>();
      }
    }
  }
  return bytes;
}

std::string manifestJson(const py::dict &payload, const py::object &names, const py::object &torch,
                         const py::object &json) {
  py::dict fields;
  for (const py::handle nameHandle : names) {
    const std::string name = py::cast<std::string>(nameHandle);
    if (!payload.contains(py::str(name))) {
      throw std::runtime_error("MPAS manifest requested absent tensor '" + name + "'");
    }
    const py::object tensor = payload[py::str(name)];
    const py::object raw =
        tensor.attr("detach")().attr("cpu")().attr("contiguous")().attr("numpy")().attr(
            "tobytes")();
    py::dict item;
    item["dtype"] = py::str(tensor.attr("dtype"));
    item["shape"] = py::tuple(tensor.attr("shape"));
    item["sha256"] = py::module_::import("hashlib").attr("sha256")(raw).attr("hexdigest")();
    item["finite"] = torch.attr("isfinite")(tensor).attr("all")().attr("item")();
    fields[py::str(name)] = item;
  }
  return json
      .attr("dumps")(fields, py::arg("sort_keys") = true,
                     py::arg("separators") = py::make_tuple(",", ":"))
      .cast<std::string>();
}

}  // namespace

struct MpasStateHandle::Impl {
  std::shared_ptr<PythonRuntime> runtime;
  std::shared_ptr<const std::uint8_t> owner;
  py::dict state;
  py::dict lastOutput;
  py::dict transformState;
  bool compact = false;
  std::uint64_t generation = 0;
};

struct MpasBackendContext::Impl {
  std::shared_ptr<PythonRuntime> runtime;
  const std::shared_ptr<const std::uint8_t> owner = std::make_shared<const std::uint8_t>(0);
  py::object mpas;
  py::object contracts;
  py::object assimilation;
  py::object analysis;
  py::object torch;
  py::object json;
  py::object config;
  py::object mesh;
  py::object initial;
  py::object initialBoundary;
  py::object snapshot;
  py::object schema;
  py::object staticSupport;
  py::object packageIdentity;
  std::vector<double> lonDegrees;
  std::vector<double> latDegrees;
  std::vector<double> areas;
  std::vector<std::int64_t> globalIds;
  MpasHorizontalSnapshot horizontalSnapshot;
  MpasStaticVerticalSnapshot staticVerticalSnapshot;
  int levels = 0;
  double dtSeconds = 0.0;
  std::string initialValidTime;
  std::string horizontalGeometryReceipt;
  std::string staticVerticalGeometryReceipt;
  std::string bundleReceipt;
  std::string configurationReceipt;
  std::string expectedStateSchemaDigest;
  std::string namelistPath;
  mutable eckit::Mutex mutex;

  py::dict transformBindings() const {
    py::dict bindings;
    bindings["horizontal_geometry_receipt"] = horizontalGeometryReceipt;
    bindings["static_vertical_geometry_receipt"] = staticVerticalGeometryReceipt;
    bindings["bundle_receipt"] = bundleReceipt;
    bindings["state_schema_digest"] = schema.attr("digest");
    bindings["configuration_receipt"] = configurationReceipt;
    bindings["package_identity"] = packageIdentity;
    return bindings;
  }

  py::dict trajectory(const py::dict &state, std::uint64_t generation,
                      const std::string &validTime) const {
    return assimilation
        .attr("build_transform_trajectory_receipt")(
            state, py::arg("horizontal_geometry_receipt") = horizontalGeometryReceipt,
            py::arg("static_vertical_geometry_receipt") = staticVerticalGeometryReceipt,
            py::arg("bundle_receipt") = bundleReceipt,
            py::arg("state_schema_digest") = schema.attr("digest"),
            py::arg("configuration_receipt") = configurationReceipt,
            py::arg("valid_time") = validTime, py::arg("state_generation") = generation,
            py::arg("package_identity") = packageIdentity)
        .cast<py::dict>();
  }
};

MpasStateHandle::MpasStateHandle(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

MpasStateHandle::~MpasStateHandle() {
  if (!impl_) {
    return;
  }
  // Opaque handles may legitimately outlive their creating Context. Release
  // every Python reference under the GIL, then finalize the last runtime only
  // after the GIL guard is gone; do not leave empty dicts to decref afterwards.
  const auto runtime = impl_->runtime;
  {
    py::gil_scoped_acquire acquire;
    impl_.reset();
  }
}

void MpasBackendContext::requireOwned(const MpasStateHandle &state, bool mutableState) const {
  if (!state.impl_ || state.impl_->owner != impl_->owner ||
      state.impl_->runtime != impl_->runtime) {
    throw std::invalid_argument("MPAS opaque State handle belongs to a different Context");
  }
  // OOPS checkpoint framing represents the generation as binary64. Reject
  // exhaustion before any model call or partial mutation, not at later save.
  constexpr std::uint64_t maximumGeneration = std::uint64_t{1} << 53;
  if (state.impl_->generation > maximumGeneration ||
      (mutableState && state.impl_->generation == maximumGeneration)) {
    throw std::overflow_error("MPAS State generation exceeds exact checkpoint framing");
  }
}

MpasBackendContext::MpasBackendContext(const eckit::Configuration &config)
    : impl_(std::make_unique<Impl>()) {
  const std::string pythonExecutable = config.getString("python executable");
  const std::string runtimeReceiptPath = config.getString("runtime receipt path");
  const std::string wheelPath = config.getString("wheel path");
  const std::string wheelSha256 = config.getString("wheel sha256");
  const std::string initPath = config.getString("init path");
  const std::string gridPath = config.getString("grid path");
  const std::string namelistPath = config.getString("namelist path");
  impl_->namelistPath = namelistPath;
  const std::string expectedHorizontalGeometryReceipt =
      config.getString("horizontal geometry receipt");
  const std::string expectedStaticVerticalGeometryReceipt =
      config.getString("static vertical geometry receipt");
  const std::string expectedBundleReceipt = config.getString("geometry bundle receipt");
  const std::string expectedConfigurationReceipt = config.getString("configuration receipt");
  impl_->expectedStateSchemaDigest = config.getString("state schema digest");
  const std::string expectedPythonVersion = config.getString("python version");
  const std::string expectedTorchVersion = config.getString("torch version");
  const std::string expectedNumpyVersion = config.getString("numpy version");
  const std::string expectedNetcdf4Version = config.getString("netcdf4 version");
  const std::string expectedMpasVersion = config.getString("mpas-pytorch version");
  const std::string expectedMpasSourceCommit = config.getString("mpas-pytorch source commit");
  const int intraopThreads = config.getInt("torch intraop threads");
  const int interopThreads = config.getInt("torch interop threads");
  if (intraopThreads != 1 || interopThreads != 1) {
    throw std::runtime_error("P0 dependency graph requires one Torch intraop and interop thread");
  }

  impl_->runtime = acquireRuntime(pythonExecutable);
  py::gil_scoped_acquire acquire;
  try {
    verifyInstalledMpasWheel(*impl_->runtime, wheelPath, wheelSha256);
    impl_->mpas = impl_->runtime->importModule("mpas_pytorch");
    impl_->contracts = impl_->runtime->importModule("mpas_pytorch.ijedi_contracts");
    impl_->assimilation = impl_->runtime->importModule("mpas_pytorch.assimilation_variables");
    impl_->analysis = impl_->runtime->importModule("mpas_pytorch.analysis_coordinates");
    impl_->torch = impl_->runtime->importModule("torch");
    impl_->json = impl_->runtime->importModule("json");
    impl_->schema = py::none();
    impl_->packageIdentity = py::dict();
    const py::object sys = impl_->runtime->importModule("sys");
    const std::string pythonVersion =
        std::to_string(sys.attr("version_info").attr("major").cast<int>()) + "." +
        std::to_string(sys.attr("version_info").attr("minor").cast<int>()) + "." +
        std::to_string(sys.attr("version_info").attr("micro").cast<int>());
    const std::string torchVersion = impl_->torch.attr("__version__").cast<std::string>();
    const std::string numpyVersion =
        impl_->runtime->importModule("numpy").attr("__version__").cast<std::string>();
    const std::string netcdf4Version =
        impl_->runtime->importModule("netCDF4").attr("__version__").cast<std::string>();
    const std::string mpasVersion = impl_->mpas.attr("__version__").cast<std::string>();
    if (pythonVersion != expectedPythonVersion || torchVersion != expectedTorchVersion ||
        numpyVersion != expectedNumpyVersion || netcdf4Version != expectedNetcdf4Version ||
        mpasVersion != expectedMpasVersion) {
      throw std::runtime_error(
          "embedded Python dependency graph mismatch: Python=" + pythonVersion +
          ", Torch=" + torchVersion + ", NumPy=" + numpyVersion + ", netCDF4=" + netcdf4Version +
          ", MPAS-PyTorch=" + mpasVersion);
    }
    const py::object pathlib = impl_->runtime->importModule("pathlib");
    const py::object runtimeReceiptFile =
        pathlib.attr("Path")(runtimeReceiptPath).attr("resolve")();
    const py::dict runtimeReceipt = impl_->json.attr("loads")(
        runtimeReceiptFile.attr("read_text")(py::arg("encoding") = "utf-8"));
    const py::dict receiptMpas = runtimeReceipt["mpas_pytorch"].cast<py::dict>();
    if (runtimeReceipt["schema_version"].cast<int>() != 2 ||
        runtimeReceipt["python"].cast<std::string>() != expectedPythonVersion ||
        receiptMpas["version"].cast<std::string>() != expectedMpasVersion ||
        receiptMpas["source_commit"].cast<std::string>() != expectedMpasSourceCommit ||
        receiptMpas["wheel_sha256"].cast<std::string>() != wheelSha256) {
      throw std::runtime_error(
          "embedded Python runtime receipt does not match the configured package identity");
    }
    impl_->packageIdentity["version"] = mpasVersion;
    impl_->packageIdentity["source_commit"] = expectedMpasSourceCommit;
    impl_->packageIdentity["wheel_sha256"] = wheelSha256;
    // PyTorch permits set_num_interop_threads() only before inter-op work starts and
    // effectively only once per process.  Geometry objects share one embedded runtime,
    // so a second, independently validated geometry must not try to reapply an already
    // satisfied process-wide setting.  A conflicting request still calls the setter and
    // therefore fails loudly rather than silently accepting the wrong execution graph.
    if (impl_->torch.attr("get_num_threads")().cast<int>() != intraopThreads) {
      impl_->torch.attr("set_num_threads")(intraopThreads);
    }
    if (impl_->torch.attr("get_num_interop_threads")().cast<int>() != interopThreads) {
      impl_->torch.attr("set_num_interop_threads")(interopThreads);
    }
    if (impl_->torch.attr("get_num_threads")().cast<int>() != intraopThreads ||
        impl_->torch.attr("get_num_interop_threads")().cast<int>() != interopThreads) {
      throw std::runtime_error("Torch thread configuration did not take effect");
    }

    const py::dict capabilities = impl_->contracts.attr("get_ijedi_interface_capabilities")();
    const py::dict support = capabilities["release"].cast<py::dict>()["support"].cast<py::dict>();
    if (support["devices"].cast<py::list>().size() != 1 ||
        support["devices"].cast<py::list>()[0].cast<std::string>() != "cpu" ||
        support["dtypes"].cast<py::list>().size() != 1 ||
        support["dtypes"].cast<py::list>()[0].cast<std::string>() != "float64" ||
        support["process_model"].cast<py::list>().size() != 1 ||
        support["process_model"].cast<py::list>()[0].cast<std::string>() != "single_process") {
      throw std::runtime_error(
          "MPAS package capability does not match CPU/float64/single-process P0");
    }
    if (!impl_->torch.attr("version").attr("cuda").is_none()) {
      throw std::runtime_error(
          "P0 requires a CPU-only PyTorch runtime (torch.version.cuda != None)");
    }

    impl_->config = impl_->mpas.attr("load_config_from_namelist")(namelistPath);
    impl_->dtSeconds = impl_->config["config_dt"].cast<double>();
    const py::object configurationReceipt =
        impl_->contracts.attr("build_configuration_receipt")(namelistPath, impl_->config);
    impl_->configurationReceipt = configurationReceipt.cast<std::string>();
    if (impl_->configurationReceipt != expectedConfigurationReceipt) {
      throw std::runtime_error("MPAS configuration receipt mismatch: expected " +
                               expectedConfigurationReceipt + ", got " +
                               impl_->configurationReceipt);
    }
    const py::tuple loaded = impl_->mpas.attr("load_initial_state")(
        initPath, gridPath, py::arg("mesh_support_path") = py::none(),
        py::arg("config") = impl_->config);
    impl_->initial = loaded[0];
    impl_->mesh = loaded[1];
    impl_->snapshot = impl_->contracts.attr("load_ijedi_geometry_snapshot")(
        initPath, gridPath, py::arg("configuration_receipt") = configurationReceipt);
    impl_->snapshot.attr("validate")();
    impl_->horizontalGeometryReceipt =
        impl_->snapshot.attr("horizontal_receipt").cast<std::string>();
    impl_->staticVerticalGeometryReceipt =
        impl_->snapshot.attr("static_vertical_receipt").cast<std::string>();
    impl_->bundleReceipt = impl_->snapshot.attr("receipt").cast<std::string>();
    if (impl_->horizontalGeometryReceipt != expectedHorizontalGeometryReceipt ||
        impl_->staticVerticalGeometryReceipt != expectedStaticVerticalGeometryReceipt ||
        impl_->bundleReceipt != expectedBundleReceipt) {
      throw std::runtime_error(
          "MPAS geometry receipt mismatch: expected horizontal/static/bundle=" +
          expectedHorizontalGeometryReceipt + "/" + expectedStaticVerticalGeometryReceipt + "/" +
          expectedBundleReceipt + ", got " + impl_->horizontalGeometryReceipt + "/" +
          impl_->staticVerticalGeometryReceipt + "/" + impl_->bundleReceipt);
    }

    const py::dict metadata = impl_->snapshot.attr("metadata").cast<py::dict>();
    const py::dict counts = metadata["counts"].cast<py::dict>();
    impl_->levels = counts["nVertLevels"].cast<int>();
    constexpr double radiansToDegrees = 180.0 / 3.141592653589793238462643383279502884;
    impl_->lonDegrees = tensorToDoubleVector(impl_->snapshot.attr("field")("lonCell"));
    impl_->latDegrees = tensorToDoubleVector(impl_->snapshot.attr("field")("latCell"));
    for (double &value : impl_->lonDegrees) {
      value *= radiansToDegrees;
    }
    for (double &value : impl_->latDegrees) {
      value *= radiansToDegrees;
    }
    impl_->areas = tensorToDoubleVector(impl_->snapshot.attr("field")("areaCell"));
    impl_->globalIds = tensorToInt64Vector(impl_->snapshot.attr("field")("indexToCellID"));
    auto &horizontal = impl_->horizontalSnapshot;
    horizontal.cells = counts["nCells"].cast<size_t>();
    horizontal.edges = counts["nEdges"].cast<size_t>();
    horizontal.vertices = counts["nVertices"].cast<size_t>();
    horizontal.receipt = impl_->horizontalGeometryReceipt;
    const py::dict horizontalMetadata =
        impl_->snapshot.attr("horizontal").attr("metadata").cast<py::dict>();
    const py::dict sourceMetadata = horizontalMetadata["source"].cast<py::dict>();
    const py::dict sourceUnits = sourceMetadata["variable_units"].cast<py::dict>();
    horizontal.sphereRadiusMetres =
        sourceMetadata["global_attributes"].cast<py::dict>()["sphere_radius"].cast<double>();
    horizontal.angleUnits = sourceUnits["lonCell"].cast<std::string>();
    horizontal.lengthUnits = sourceUnits["xCell"].cast<std::string>();
    horizontal.areaUnits = sourceUnits["areaCell"].cast<std::string>();
    for (const std::string suffix : {"Cell", "Edge", "Vertex"}) {
      for (const std::string prefix : {"lat", "lon"}) {
        if (sourceUnits[py::str(prefix + suffix)].cast<std::string>() != horizontal.angleUnits) {
          throw std::runtime_error("MPAS angular geometry fields declare inconsistent units");
        }
      }
      for (const std::string prefix : {"x", "y", "z"}) {
        if (sourceUnits[py::str(prefix + suffix)].cast<std::string>() != horizontal.lengthUnits) {
          throw std::runtime_error("MPAS Cartesian geometry fields declare inconsistent units");
        }
      }
    }
    const double radiusMean = horizontalMetadata["normalization"]
                                  .cast<py::dict>()["coordinate_radius_mean"]
                                  .cast<double>();
    if (!std::isfinite(horizontal.sphereRadiusMetres) || horizontal.sphereRadiusMetres <= 0 ||
        !std::isfinite(radiusMean) || radiusMean <= 0 ||
        std::abs(radiusMean / horizontal.sphereRadiusMetres - 1.) > 2.e-14) {
      throw std::runtime_error("MPAS declared sphere radius disagrees with owned coordinates");
    }
    const auto verticesOnCell = impl_->snapshot.attr("field")("verticesOnCell");
    horizontal.cellWidth = verticesOnCell.attr("shape").cast<py::tuple>()[1].cast<size_t>();
    for (const std::string name : {"lonCell", "latCell", "lonVertex", "latVertex", "lonEdge",
                                   "latEdge", "areaCell", "edgeNormalVectors"}) {
      horizontal.reals.emplace(name, tensorToDoubleVector(impl_->snapshot.attr("field")(name)));
    }
    for (const std::string name :
         {"indexToCellID", "indexToEdgeID", "indexToVertexID", "nEdgesOnCell", "verticesOnCell",
          "cellsOnVertex", "verticesOnEdge", "cellsOnEdge"}) {
      horizontal.integers.emplace(name, tensorToInt64Vector(impl_->snapshot.attr("field")(name)));
    }
    if (impl_->lonDegrees.empty() || impl_->lonDegrees.size() != impl_->latDegrees.size() ||
        impl_->lonDegrees.size() != impl_->areas.size() ||
        impl_->lonDegrees.size() != impl_->globalIds.size()) {
      throw std::runtime_error("MPAS canonical cell geometry arrays have inconsistent sizes");
    }
    const py::dict verticalMetadata =
        impl_->snapshot.attr("static_vertical").attr("metadata").cast<py::dict>();
    const py::dict verticalUnits =
        verticalMetadata["source"].cast<py::dict>()["variable_units"].cast<py::dict>();
    auto &vertical = impl_->staticVerticalSnapshot;
    vertical.cells = horizontal.cells;
    vertical.layers = static_cast<size_t>(impl_->levels);
    vertical.receipt = impl_->staticVerticalGeometryReceipt;
    vertical.horizontalReceipt = impl_->horizontalGeometryReceipt;
    vertical.heightUnits = verticalUnits["zgrid"].cast<std::string>();
    vertical.metricUnits = verticalUnits["zz"].cast<std::string>();
    vertical.direction =
        verticalMetadata["vertical_axis"].cast<py::dict>()["direction"].cast<std::string>();
    vertical.interfaceHeights = tensorToDoubleVector(impl_->snapshot.attr("field")("zgrid"));
    vertical.layerMetrics = tensorToDoubleVector(impl_->snapshot.attr("field")("zz"));

    // OOPS queries serialSize() in every State constructor. Freeze the
    // model-owned storage/support contract now so generation zero is already
    // a complete, authenticated continuation boundary.
    const py::object preparedInitial = impl_->mpas.attr("prepare_initial_state")(
        impl_->contracts.attr("clone_state")(impl_->initial),
        impl_->contracts.attr("clone_state")(impl_->mesh), py::arg("config") = impl_->config);
    const py::dict reference =
        impl_->mpas
            .attr("run_simulation")(impl_->contracts.attr("clone_state")(impl_->initial),
                                    impl_->contracts.attr("clone_state")(impl_->mesh),
                                    py::arg("nsteps") = 1, py::arg("config") = impl_->config)
            .cast<py::dict>();
    impl_->schema = impl_->contracts.attr("build_state_storage_schema")(
        reference, impl_->mesh,
        py::arg("horizontal_geometry_receipt") = impl_->horizontalGeometryReceipt,
        py::arg("static_vertical_geometry_receipt") = impl_->staticVerticalGeometryReceipt,
        py::arg("bundle_receipt") = impl_->bundleReceipt,
        py::arg("configuration_receipt") = impl_->configurationReceipt);
    const std::string actualSchemaDigest = impl_->schema.attr("digest").cast<std::string>();
    if (actualSchemaDigest != impl_->expectedStateSchemaDigest) {
      impl_->schema = py::none();
      throw std::runtime_error("MPAS state schema digest mismatch: expected " +
                               impl_->expectedStateSchemaDigest + ", got " + actualSchemaDigest);
    }
    // This fresh model result is already privately owned. Composition clones
    // every admitted support tensor; no caller can mutate this frozen receipt
    // authority, so duplicating the complete result here is unnecessary.
    impl_->staticSupport = reference;
    const py::dict initialBoundary =
        impl_->contracts
            .attr("extract_continuation_boundary")(preparedInitial,
                                                   py::arg("schema") = impl_->schema)
            .cast<py::dict>();
    impl_->contracts.attr("validate_boundary_compatibility")(initialBoundary,
                                                             py::arg("schema") = impl_->schema);
    const size_t beforeBytes = retainedTensorBytes(
        impl_->torch, {impl_->initial, impl_->mesh, preparedInitial, impl_->staticSupport});
    // Retain precisely the owner's static support inventory. A reference
    // forecast authenticates these values once at construction; its prognostic,
    // diagnostic and scratch outputs are not context-lifetime authorities.
    py::dict frozenSupport;
    for (const auto &field : impl_->schema.attr("fields")) {
      const auto role = field.attr("role").cast<std::string>();
      if (role == "static_geometry" || role == "static_support") {
        const py::str name = field.attr("name");
        frozenSupport[name] = reference[name];
      }
    }
    for (const auto &name : impl_->schema.attr("support_metadata_keys")) {
      frozenSupport[name] = reference[name];
    }
    // Verify the replacement before dropping any source authority. The model
    // checks inventory, shapes, values and its support digest in this call.
    impl_->contracts.attr("compose_continuation_state")(frozenSupport, initialBoundary,
                                                        py::arg("schema") = impl_->schema);
    impl_->staticSupport = std::move(frozenSupport);
    impl_->initialBoundary = initialBoundary;
    impl_->initial = py::dict();
    const size_t afterBytes = retainedTensorBytes(
        impl_->torch, {impl_->initial, impl_->mesh, impl_->initialBoundary, impl_->staticSupport});
    oops::Log::info() << "MPAS context flat unique tensor bytes: before=" << beforeBytes
                      << ", after=" << afterBytes << std::endl;
  } catch (const py::error_already_set &error) {
    throw std::runtime_error(std::string("MPAS context construction failed:\n") + error.what());
  }
}

MpasBackendContext::~MpasBackendContext() {
  if (!impl_) {
    return;
  }
  py::gil_scoped_acquire acquire;
  impl_->staticSupport = py::object();
  impl_->schema = py::object();
  impl_->packageIdentity = py::object();
  impl_->snapshot = py::object();
  impl_->initialBoundary = py::object();
  impl_->initial = py::object();
  impl_->mesh = py::object();
  impl_->config = py::object();
  impl_->json = py::object();
  impl_->torch = py::object();
  impl_->contracts = py::object();
  impl_->assimilation = py::object();
  impl_->analysis = py::object();
  impl_->mpas = py::object();
}

std::shared_ptr<MpasStateHandle> MpasBackendContext::initialState() const {
  py::gil_scoped_acquire acquire;
  return pythonBoundary("initial State", [&] {
    auto state = std::make_unique<MpasStateHandle::Impl>();
    state->runtime = impl_->runtime;
    state->owner = impl_->owner;
    // Construction already exported the raw model state into an authenticated
    // portable boundary. Clone that representation; extracting it again would
    // conflate the raw byte seal with the wire format.
    state->state = impl_->contracts.attr("clone_state")(impl_->initialBoundary).cast<py::dict>();
    impl_->contracts.attr("validate_boundary_compatibility")(state->state,
                                                             py::arg("schema") = impl_->schema);
    state->transformState =
        impl_->contracts
            .attr("compose_continuation_state")(impl_->staticSupport, state->state,
                                                py::arg("schema") = impl_->schema)
            .cast<py::dict>();
    state->compact = true;
    return std::shared_ptr<MpasStateHandle>(new MpasStateHandle(std::move(state)));
  });
}

std::shared_ptr<MpasStateHandle> MpasBackendContext::clone(const MpasStateHandle &source) const {
  requireOwned(source);
  py::gil_scoped_acquire acquire;
  return pythonBoundary("State clone", [&] {
    auto state = std::make_unique<MpasStateHandle::Impl>();
    state->runtime = impl_->runtime;
    state->owner = impl_->owner;
    state->state = impl_->contracts.attr("clone_state")(source.impl_->state).cast<py::dict>();
    if (py::len(source.impl_->transformState) != 0) {
      state->transformState =
          impl_->contracts.attr("clone_state")(source.impl_->transformState).cast<py::dict>();
    }
    if (py::len(source.impl_->lastOutput) != 0) {
      // Both read-only output surfaces refer to the same owned trajectory.
      // Clone it once, retaining neither an alias to the source State nor a
      // second complete numerical copy inside the clone.
      state->lastOutput = state->transformState;
    }
    state->compact = source.impl_->compact;
    state->generation = source.impl_->generation;
    return std::shared_ptr<MpasStateHandle>(new MpasStateHandle(std::move(state)));
  });
}

void MpasBackendContext::advance(MpasStateHandle &state, double seconds) const {
  requireOwned(state, true);
  if (seconds != impl_->dtSeconds) {
    throw std::runtime_error("OOPS model timestep differs from MPAS namelist config_dt");
  }
  eckit::AutoLock<eckit::Mutex> guard(impl_->mutex);
  py::gil_scoped_acquire acquire;
  try {
    py::dict input;
    if (state.impl_->compact) {
      if (impl_->schema.is_none() || py::len(impl_->staticSupport) == 0) {
        throw std::runtime_error("compact MPAS state has no process-local schema/support");
      }
      impl_->contracts.attr("validate_boundary_compatibility")(state.impl_->state,
                                                               py::arg("schema") = impl_->schema);
      input = impl_->contracts
                  .attr("compose_continuation_state")(impl_->staticSupport, state.impl_->state,
                                                      py::arg("schema") = impl_->schema)
                  .cast<py::dict>();
    } else {
      input = impl_->contracts.attr("clone_state")(state.impl_->state).cast<py::dict>();
    }
    py::dict output = impl_->mpas
                          .attr("run_simulation")(input, impl_->mesh, py::arg("nsteps") = 1,
                                                  py::arg("config") = impl_->config)
                          .cast<py::dict>();

    py::dict boundary =
        impl_->contracts
            .attr("extract_continuation_boundary")(output, py::arg("schema") = impl_->schema)
            .cast<py::dict>();
    impl_->contracts.attr("validate_boundary_compatibility")(boundary,
                                                             py::arg("schema") = impl_->schema);

    state.impl_->lastOutput = output;
    state.impl_->transformState = std::move(output);
    state.impl_->state = std::move(boundary);
    state.impl_->compact = true;
    ++state.impl_->generation;
  } catch (const py::error_already_set &error) {
    throw std::runtime_error(std::string("embedded MPAS OOPS step failed:\n") + error.what());
  }
}

double MpasBackendContext::norm(const MpasStateHandle &state,
                                const std::vector<std::string> &variables) const {
  requireOwned(state);
  py::gil_scoped_acquire acquire;
  return pythonBoundary("State norm", [&] {
    py::object total = impl_->torch.attr("zeros")(py::make_tuple(),
                                                  py::arg("dtype") = impl_->torch.attr("float64"));
    for (const std::string &name : variables) {
      if (!state.impl_->state.contains(py::str(name))) {
        throw std::runtime_error("MPAS State norm requested absent tensor '" + name + "'");
      }
      const py::object value = state.impl_->state[py::str(name)];
      total = total + impl_->torch.attr("sum")(value * value);
    }
    return impl_->torch.attr("sqrt")(total).attr("item")().cast<double>();
  });
}

std::string MpasBackendContext::regressionManifest(const MpasStateHandle &state) const {
  requireOwned(state);
  py::gil_scoped_acquire acquire;
  return pythonBoundary("regression manifest", [&] {
    if (py::len(state.impl_->lastOutput) == 0) {
      throw std::runtime_error("MPAS regression manifest is unavailable before the first step");
    }
    return manifestJson(state.impl_->lastOutput,
                        impl_->contracts.attr("REGRESSION_OUTPUT_TENSOR_KEYS"), impl_->torch,
                        impl_->json);
  });
}

std::string MpasBackendContext::continuationManifest(const MpasStateHandle &state) const {
  requireOwned(state);
  py::gil_scoped_acquire acquire;
  return pythonBoundary("continuation manifest", [&] {
    if (!state.impl_->compact) {
      throw std::runtime_error("MPAS continuation manifest is unavailable before the first step");
    }
    return manifestJson(state.impl_->state, impl_->contracts.attr("CONTINUATION_TENSOR_KEYS"),
                        impl_->torch, impl_->json);
  });
}

std::string MpasBackendContext::typedTransformManifest(const MpasStateHandle &state,
                                                       const std::string &validTime) const {
  requireOwned(state);
  py::gil_scoped_acquire acquire;
  if (py::len(state.impl_->transformState) == 0 || impl_->schema.is_none()) {
    throw std::runtime_error("MPAS typed transforms require a bound trajectory");
  }
  try {
    const py::dict receipt =
        impl_->assimilation
            .attr("build_transform_trajectory_receipt")(
                state.impl_->transformState,
                py::arg("horizontal_geometry_receipt") = impl_->horizontalGeometryReceipt,
                py::arg("static_vertical_geometry_receipt") = impl_->staticVerticalGeometryReceipt,
                py::arg("bundle_receipt") = impl_->bundleReceipt,
                py::arg("state_schema_digest") = impl_->schema.attr("digest"),
                py::arg("configuration_receipt") = impl_->configurationReceipt,
                py::arg("valid_time") = validTime,
                py::arg("state_generation") = state.impl_->generation,
                py::arg("package_identity") = impl_->packageIdentity)
            .cast<py::dict>();
    const py::list names = py::cast(std::vector<std::string>{
        "air_pressure", "air_pressure_levels", "air_pressure_at_surface", "air_temperature",
        "dry_air_density", "water_vapor_mixing_ratio_wrt_dry_air",
        "water_vapor_mixing_ratio_wrt_moist_air", "eastward_wind", "northward_wind",
        "height_above_mean_sea_level", "height_above_mean_sea_level_levels",
        "height_above_mean_sea_level_at_surface"});
    const py::dict geovals =
        impl_->analysis
            .attr("diagnose_analysis_geovals")(
                state.impl_->transformState, impl_->mesh, py::arg("schema") = impl_->schema,
                py::arg("config") = impl_->config, py::arg("static_support") = impl_->staticSupport,
                py::arg("namelist_path") = impl_->namelistPath,
                py::arg("geometry") = impl_->snapshot, py::arg("requested_fields") = names,
                py::arg("trajectory_receipt") = receipt, py::arg("expected_valid_time") = validTime,
                py::arg("expected_state_generation") = state.impl_->generation,
                py::arg("expected_bindings") = impl_->transformBindings())
            .cast<py::dict>();
    py::dict payload;
    payload["trajectory_receipt"] = receipt;
    payload["descriptor_registry"] = impl_->assimilation.attr("descriptor_registry_manifest")();
    payload["geovals"] =
        impl_->json.attr("loads")(manifestJson(geovals, names, impl_->torch, impl_->json));
    return impl_->json
        .attr("dumps")(payload, py::arg("sort_keys") = true,
                       py::arg("separators") = py::make_tuple(",", ":"))
        .cast<std::string>();
  } catch (const py::error_already_set &error) {
    throw std::runtime_error(std::string("MPAS typed transform failed:\n") + error.what());
  }
}

std::vector<MpasTypedField> MpasBackendContext::materializeTypedFields(
    const MpasStateHandle &state, const std::vector<std::string> &names,
    const std::string &validTime, bool stateViews) const {
  requireOwned(state);
  if (names.empty()) {
    throw std::runtime_error("MPAS typed field request must not be empty");
  }
  eckit::AutoLock<eckit::Mutex> guard(impl_->mutex);
  py::gil_scoped_acquire acquire;
  if (py::len(state.impl_->transformState) == 0 || impl_->schema.is_none()) {
    throw std::runtime_error("MPAS typed fields require a bound trajectory");
  }
  try {
    const py::dict receipt =
        impl_->assimilation
            .attr("build_transform_trajectory_receipt")(
                state.impl_->transformState,
                py::arg("horizontal_geometry_receipt") = impl_->horizontalGeometryReceipt,
                py::arg("static_vertical_geometry_receipt") = impl_->staticVerticalGeometryReceipt,
                py::arg("bundle_receipt") = impl_->bundleReceipt,
                py::arg("state_schema_digest") = impl_->schema.attr("digest"),
                py::arg("configuration_receipt") = impl_->configurationReceipt,
                py::arg("valid_time") = validTime,
                py::arg("state_generation") = state.impl_->generation,
                py::arg("package_identity") = impl_->packageIdentity)
            .cast<py::dict>();
    std::map<std::string, std::string> namespaces;
    std::vector<std::string> geovalNames;
    for (const std::string &name : names) {
      std::string nameSpace = "geoval";
      if (stateViews) {
        const auto aliases = impl_->assimilation.attr("FIELD_ALIASES");
        const py::object canonical = aliases.attr("get")(name, name);
        nameSpace = impl_->assimilation.attr("FIELD_DESCRIPTORS")
                        .attr("__getitem__")(canonical)
                        .attr("namespace")
                        .cast<std::string>();
        if (nameSpace != "native" && nameSpace != "geoval" && nameSpace != "static") {
          throw std::runtime_error("MPAS State views cannot represent control increments: " + name);
        }
      }
      if (!namespaces.emplace(name, nameSpace).second) {
        throw std::runtime_error("duplicate MPAS State field request: " + name);
      }
      if (nameSpace == "geoval") {
        geovalNames.push_back(name);
      }
    }
    const py::dict geovals =
        geovalNames.empty()
            ? py::dict()
            : impl_->analysis
                  .attr("diagnose_analysis_geovals")(
                      state.impl_->transformState, impl_->mesh, py::arg("schema") = impl_->schema,
                      py::arg("config") = impl_->config,
                      py::arg("static_support") = impl_->staticSupport,
                      py::arg("namelist_path") = impl_->namelistPath,
                      py::arg("geometry") = impl_->snapshot,
                      py::arg("requested_fields") = geovalNames,
                      py::arg("trajectory_receipt") = receipt,
                      py::arg("expected_valid_time") = validTime,
                      py::arg("expected_state_generation") = state.impl_->generation,
                      py::arg("expected_bindings") = impl_->transformBindings())
                  .cast<py::dict>();
    const std::string receiptDigest = receipt["digest"].cast<std::string>();
    const py::object asdict = impl_->runtime->importModule("dataclasses").attr("asdict");
    const py::dict registry = impl_->assimilation.attr("descriptor_registry_manifest")();
    py::dict context = impl_->transformBindings();
    context["valid_time"] = validTime;
    context["state_generation"] = state.impl_->generation;
    context["trajectory_receipt"] = receiptDigest;
    context["analysis_coordinates"] = "native_five_prognostics_v1";
    context["vector_basis_geometry_receipt"] = impl_->horizontalGeometryReceipt;
    if (stateViews) {
      context["runtime_support_receipt"] = impl_->schema.attr("support_digest");
      context["native_state_receipt"] =
          impl_->runtime->importModule("hashlib")
              .attr("sha256")(py::bytes(manifestJson(state.impl_->state,
                                                     impl_->analysis.attr("NATIVE_ANALYSIS_KEYS"),
                                                     impl_->torch, impl_->json)))
              .attr("hexdigest")();
      context["space_kind"] = "versioned_native_State_view_not_increment_v1";
    }
    std::vector<MpasTypedField> result;
    result.reserve(names.size());
    for (const std::string &name : names) {
      const std::string &nameSpace = namespaces.at(name);
      const py::object descriptor = impl_->assimilation.attr("resolve_field_descriptor")(
          name, py::arg("namespace") = nameSpace);
      if (descriptor.attr("dtype").cast<std::string>() != "float64" ||
          (descriptor.attr("horizontal_location").cast<std::string>() != "cell" &&
           descriptor.attr("horizontal_location").cast<std::string>() != "edge")) {
        throw std::runtime_error("I-JEDI typed State views require float64 cell/edge fields: " +
                                 name);
      }
      py::object tensor;
      if (nameSpace == "geoval") {
        if (!geovals.contains(py::str(name))) {
          throw std::runtime_error("model-owned transform omitted requested field: " + name);
        }
        tensor = geovals[py::str(name)];
      } else {
        const auto nativeName = descriptor.attr("native_name").cast<std::string>();
        tensor = state.impl_->transformState[py::str(nativeName)].attr("__getitem__")(
            py::slice(py::none(), py::int_(-1), py::none()));
        if (!descriptor.attr("tracer_index").is_none()) {
          tensor = tensor.attr("__getitem__")(py::make_tuple(
              py::slice(py::none(), py::none(), py::none()),
              py::slice(py::none(), py::none(), py::none()), descriptor.attr("tracer_index")));
        }
      }
      const int dimensions = tensor.attr("ndim").cast<int>();
      const py::tuple shape = tensor.attr("shape").cast<py::tuple>();
      const size_t horizontalCount =
          descriptor.attr("horizontal_location").cast<std::string>() == "edge"
              ? impl_->horizontalSnapshot.edges
              : impl_->horizontalSnapshot.cells;
      if (dimensions < 1 || dimensions > 3 || shape.size() != dimensions ||
          shape[0].cast<size_t>() != horizontalCount) {
        throw std::runtime_error("typed MPAS field has an invalid location or rank: " + name);
      }
      MpasTypedField field;
      field.name = name;
      field.nameSpace = nameSpace;
      field.semanticId = descriptor.attr("semantic_id").cast<std::string>();
      field.units = descriptor.attr("units").cast<std::string>();
      field.horizontalLocation = descriptor.attr("horizontal_location").cast<std::string>();
      field.verticalStagger = descriptor.attr("vertical_stagger").cast<std::string>();
      field.componentBasis = descriptor.attr("component_basis").cast<std::string>();
      field.trajectoryReceipt = receiptDigest;
      field.horizontalGeometryReceipt = impl_->horizontalGeometryReceipt;
      field.levels = dimensions == 1 ? 1 : shape[1].cast<size_t>();
      for (const py::handle extent : shape) {
        field.shape.push_back(py::cast<size_t>(extent));
      }
      if (dimensions == 1) {
        field.shape.push_back(1);
      }
      py::dict binding;
      binding["descriptor"] = asdict(descriptor);
      binding["descriptor_registry_digest"] = registry["digest"];
      binding["context"] = context;
      impl_->assimilation.attr("validate_atlas_field_binding")(
          name, py::arg("namespace") = nameSpace, py::arg("metadata") = binding,
          py::arg("shape") = py::tuple(py::cast(field.shape)),
          py::arg("n_cells") = impl_->horizontalSnapshot.cells,
          py::arg("n_edges") = impl_->horizontalSnapshot.edges, py::arg("n_levels") = impl_->levels,
          py::arg("expected_context") = context);
      field.descriptorBinding =
          impl_->json
              .attr("dumps")(binding, py::arg("sort_keys") = true, py::arg("allow_nan") = false,
                             py::arg("separators") = py::make_tuple(",", ":"))
              .cast<std::string>();
      field.payloadReceipt = impl_->assimilation.attr("_tensor_manifest")(tensor)
                                 .cast<py::dict>()["sha256"]
                                 .cast<std::string>();
      field.values = tensorToDoubleVector(tensor);
      size_t elementCount = 1;
      for (size_t extent : field.shape) {
        elementCount *= extent;
      }
      if (field.values.size() != elementCount) {
        throw std::runtime_error("typed MPAS field extent changed during transfer: " + name);
      }
      AtlasOperatorReceipt transferred;
      for (double value : field.values) {
        if (!std::isfinite(value)) {
          throw std::runtime_error("typed MPAS field contains a nonfinite value: " + name);
        }
        transferred.real(value);
      }
      if (transferred.finish() != field.payloadReceipt) {
        throw std::runtime_error("typed MPAS field changed bytes across the Python boundary: " +
                                 name);
      }
      result.push_back(std::move(field));
    }
    return result;
  } catch (const py::error_already_set &error) {
    throw std::runtime_error(std::string("MPAS typed field materialization failed:\n") +
                             error.what());
  }
}

void MpasBackendContext::validateModelVariables(const oops::Variables &variables) const {
  if (variables.size() == 0) throw std::invalid_argument("MPAS model inventory must not be empty");
  std::set<std::string> seen;
  for (const auto &variable : variables) {
    if (!seen.insert(variable.name()).second) {
      throw std::invalid_argument("duplicate MPAS model variable: " + variable.name());
    }
    py::gil_scoped_acquire acquire;
    pythonBoundary("model inventory", [&] {
      if (!impl_->analysis.attr("NATIVE_ANALYSIS_KEYS").attr("__contains__")(variable.name())
               .cast<bool>()) {
        throw std::invalid_argument("unsupported MPAS model variable: " + variable.name());
      }
    });
    (void)typedFieldLevels(variable);
  }
}

size_t MpasBackendContext::typedFieldLevels(const oops::Variable &variable) const {
  const size_t levels = typedFieldLevels(variable.name());
  if (variable.getLevels() < -1 ||
      (variable.getLevels() >= 0 && static_cast<size_t>(variable.getLevels()) != levels) ||
      variable.dataType() != oops::ModelDataType::Real64 ||
      variable.metaData().domain() != oops::ModelVariableDomain::Atmosphere) {
    throw std::invalid_argument("MPAS variable request metadata mismatch: " + variable.name());
  }
  if (variable.stagger() != oops::VerticalStagger::CENTER) {
    py::gil_scoped_acquire acquire;
    const auto canonical = impl_->assimilation.attr("FIELD_ALIASES").attr("get")(
        variable.name(), variable.name());
    const auto descriptor = impl_->assimilation.attr("FIELD_DESCRIPTORS").attr("__getitem__")(
        canonical);
    if (variable.stagger() != oops::VerticalStagger::INTERFACE ||
        descriptor.attr("vertical_stagger").cast<std::string>() != "interface") {
      throw std::invalid_argument("MPAS variable request metadata mismatch: " + variable.name());
    }
  }
  return levels;
}

size_t MpasBackendContext::typedFieldLevels(const std::string &name) const {
  py::gil_scoped_acquire acquire;
  try {
    const py::object aliases = impl_->assimilation.attr("FIELD_ALIASES");
    const py::object canonical = aliases.attr("get")(name, name);
    const py::object descriptor =
        impl_->assimilation.attr("FIELD_DESCRIPTORS").attr("__getitem__")(canonical);
    if (descriptor.attr("dtype").cast<std::string>() != "float64" ||
        (descriptor.attr("horizontal_location").cast<std::string>() != "cell" &&
         descriptor.attr("horizontal_location").cast<std::string>() != "edge")) {
      throw std::runtime_error("MPAS Atlas columns require a float64 cell/edge descriptor: " +
                               name);
    }
    const std::string stagger = descriptor.attr("vertical_stagger").cast<std::string>();
    if (stagger == "layer") {
      return static_cast<size_t>(impl_->levels);
    }
    if (stagger == "interface") {
      return static_cast<size_t>(impl_->levels + 1);
    }
    if (stagger == "surface") {
      return 1;
    }
    throw std::runtime_error("unsupported MPAS vertical stagger '" + stagger + "'");
  } catch (const py::error_already_set &error) {
    throw std::runtime_error(std::string("MPAS typed descriptor lookup failed:\n") + error.what());
  }
}

std::string MpasBackendContext::serializeState(const MpasStateHandle &state,
                                               const std::string &validTime) const {
  requireOwned(state);
  eckit::AutoLock<eckit::Mutex> guard(impl_->mutex);
  py::gil_scoped_acquire acquire;
  return pythonBoundary("State serialization", [&] {
    if (!state.impl_->compact) {
      throw std::runtime_error(
          "MPAS State serialization requires a completed continuation boundary");
    }
    if (impl_->schema.is_none() || py::len(impl_->staticSupport) == 0) {
      throw std::runtime_error("cannot serialize compact MPAS state without schema/support");
    }
    impl_->contracts.attr("validate_boundary_compatibility")(state.impl_->state,
                                                             py::arg("schema") = impl_->schema);
    const py::bytes envelope = impl_->contracts.attr("serialize_continuation_envelope")(
        state.impl_->state, py::arg("schema") = impl_->schema,
        py::arg("package_identity") = impl_->packageIdentity,
        py::arg("state_mode") = "compact_continuation", py::arg("valid_time") = validTime,
        py::arg("time_step_seconds") = impl_->dtSeconds,
        py::arg("state_generation") = state.impl_->generation);
    return envelope.cast<std::string>();
  });
}

std::shared_ptr<MpasStateHandle> MpasBackendContext::deserializeState(
    const std::string &bytes, const std::string &validTime, std::uint64_t stateGeneration) const {
  if (stateGeneration > (std::uint64_t{1} << 53)) {
    throw std::overflow_error("MPAS restored generation exceeds exact checkpoint framing");
  }
  eckit::AutoLock<eckit::Mutex> guard(impl_->mutex);
  py::gil_scoped_acquire acquire;
  try {
    const py::bytes envelope(bytes);
    const py::object schema = impl_->contracts.attr("inspect_continuation_envelope")(
        envelope, py::arg("expected_package_identity") = impl_->packageIdentity,
        py::arg("expected_state_mode") = "compact_continuation",
        py::arg("expected_valid_time") = validTime,
        py::arg("expected_time_step_seconds") = impl_->dtSeconds,
        py::arg("expected_state_generation") = stateGeneration,
        py::arg("expected_horizontal_geometry_receipt") = impl_->horizontalGeometryReceipt,
        py::arg("expected_static_vertical_geometry_receipt") = impl_->staticVerticalGeometryReceipt,
        py::arg("expected_bundle_receipt") = impl_->bundleReceipt,
        py::arg("expected_configuration_receipt") = impl_->configurationReceipt,
        py::arg("expected_state_schema_digest") = impl_->expectedStateSchemaDigest);
    py::dict boundary = impl_->contracts
                            .attr("deserialize_continuation_envelope")(
                                envelope, py::arg("expected_schema") = schema,
                                py::arg("expected_package_identity") = impl_->packageIdentity,
                                py::arg("expected_state_mode") = "compact_continuation",
                                py::arg("expected_valid_time") = validTime,
                                py::arg("expected_time_step_seconds") = impl_->dtSeconds,
                                py::arg("expected_state_generation") = stateGeneration)
                            .cast<py::dict>();
    py::dict transformState = impl_->contracts
                                  .attr("compose_continuation_state")(
                                      impl_->staticSupport, boundary, py::arg("schema") = schema)
                                  .cast<py::dict>();
    auto state = std::make_unique<MpasStateHandle::Impl>();
    state->runtime = impl_->runtime;
    state->owner = impl_->owner;
    state->state = std::move(boundary);
    state->transformState = std::move(transformState);
    state->compact = true;
    state->generation = stateGeneration;
    auto restored = std::shared_ptr<MpasStateHandle>(new MpasStateHandle(std::move(state)));
    // Construction already rebuilt support from authenticated files and a
    // reference forecast, including in a fresh process. Restore never installs
    // support from the envelope or mutates this context-lifetime authority.
    return restored;
  } catch (const py::error_already_set &error) {
    throw std::runtime_error(std::string("failed to deserialize MPAS State:\n") + error.what());
  }
}

std::uint64_t MpasBackendContext::stateGeneration(const MpasStateHandle &state) const {
  requireOwned(state);
  return state.impl_->generation;
}

const MpasHorizontalSnapshot &MpasBackendContext::horizontalSnapshot() const {
  return impl_->horizontalSnapshot;
}

const MpasStaticVerticalSnapshot &MpasBackendContext::staticVerticalSnapshot() const {
  return impl_->staticVerticalSnapshot;
}

const std::vector<double> &MpasBackendContext::cellLongitudesDegrees() const {
  return impl_->lonDegrees;
}
const std::vector<double> &MpasBackendContext::cellLatitudesDegrees() const {
  return impl_->latDegrees;
}
const std::vector<double> &MpasBackendContext::cellAreas() const { return impl_->areas; }
const std::vector<std::int64_t> &MpasBackendContext::cellGlobalIds() const {
  return impl_->globalIds;
}
int MpasBackendContext::numberLevels() const { return impl_->levels; }
double MpasBackendContext::timeStepSeconds() const { return impl_->dtSeconds; }
const std::string &MpasBackendContext::initialValidTime() const {
  eckit::AutoLock<eckit::Mutex> guard(impl_->mutex);
  if (impl_->initialValidTime.empty()) {
    py::gil_scoped_acquire acquire;
    pythonBoundary("initial clock", [&] {
      const auto ownedConfig = impl_->config.cast<py::dict>();
      if (!ownedConfig.contains("config_start_time")) {
        throw std::invalid_argument(
            "MPAS initial clock requires a model-owned explicit config_start_time");
      }
      const std::string start = ownedConfig["config_start_time"].cast<std::string>();
      const auto datetime = impl_->runtime->importModule("datetime");
      const auto parsed = datetime.attr("datetime").attr("strptime")(start, "%Y-%m-%d_%H:%M:%S");
      if (parsed.attr("strftime")("%Y-%m-%d_%H:%M:%S").cast<std::string>() != start) {
        throw std::invalid_argument("MPAS config_start_time is not canonical UTC model time");
      }
      impl_->initialValidTime =
          parsed.attr("replace")(py::arg("tzinfo") = datetime.attr("timezone").attr("utc"))
              .attr("isoformat")(py::arg("timespec") = "seconds")
              .attr("replace")("+00:00", "Z")
              .cast<std::string>();
    });
  }
  return impl_->initialValidTime;
}
const std::string &MpasBackendContext::horizontalGeometryReceipt() const {
  return impl_->horizontalGeometryReceipt;
}
const std::string &MpasBackendContext::staticVerticalGeometryReceipt() const {
  return impl_->staticVerticalGeometryReceipt;
}
const std::string &MpasBackendContext::geometryReceipt() const { return impl_->bundleReceipt; }
const std::string &MpasBackendContext::configurationReceipt() const {
  return impl_->configurationReceipt;
}
std::string MpasBackendContext::stateSchemaDigest() const {
  py::gil_scoped_acquire acquire;
  return pythonBoundary("State schema digest", [&] {
    return impl_->schema.is_none() ? std::string()
                                   : impl_->schema.attr("digest").cast<std::string>();
  });
}

}  // namespace ijedi
