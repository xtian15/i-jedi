/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <vector>

namespace eckit {
class Configuration;
}
namespace oops {
class Variables;
class Variable;
}

namespace ijedi {

class MpasStateHandle;

// An owned copy of the authenticated model snapshot, never a second grid.nc
// decoder. Connectivity is zero-based, with -1 only in inactive slots.
struct MpasHorizontalSnapshot {
  size_t cells = 0, edges = 0, vertices = 0, cellWidth = 0;
  double sphereRadiusMetres = 0;
  std::string angleUnits, lengthUnits, areaUnits;
  std::string receipt;
  std::map<std::string, std::vector<double>> reals;
  std::map<std::string, std::vector<std::int64_t>> integers;
};

struct MpasTypedField {
  std::string name;
  std::string nameSpace;
  std::string semanticId;
  std::string units;
  std::string horizontalLocation;
  std::string verticalStagger;
  std::string componentBasis;
  std::string trajectoryReceipt;
  std::string horizontalGeometryReceipt;
  // Complete model-owned descriptor and private-context binding, serialized
  // canonically. The scalar labels above are conveniences, not write authority.
  std::string descriptorBinding;
  std::string payloadReceipt;
  std::vector<size_t> shape;
  size_t levels = 0;
  std::vector<double> values;
};

// Narrow byte-checked tensor transport for real embedded owner API calls.
// Scientific equations and storage validation remain inside MPAS-PyTorch.
struct MpasAnalysisArray {
  std::vector<size_t> shape;
  std::vector<double> values;
};
using MpasAnalysisArrays = std::map<std::string, MpasAnalysisArray>;

// Model-owned unpadded static vertical support, separate from prognostics and
// the horizontal mesh. These are physical fields, not verticalCoord indices.
struct MpasStaticVerticalSnapshot {
  size_t cells = 0, layers = 0;
  std::string receipt, horizontalReceipt, heightUnits, metricUnits, direction;
  std::vector<double> interfaceHeights, layerMetrics;
};

/// Process-local owner of the authenticated MPAS-PyTorch runtime, immutable
/// mesh/configuration support, geometry receipt and state-storage schema.
/// Python implementation details are hidden so public I-JEDI headers do not
/// expose pybind11 or permit a second numerical engine.
class MpasBackendContext {
 public:
  explicit MpasBackendContext(const eckit::Configuration &);
  ~MpasBackendContext();

  MpasBackendContext(const MpasBackendContext &) = delete;
  MpasBackendContext &operator=(const MpasBackendContext &) = delete;

  std::shared_ptr<MpasStateHandle> initialState() const;
  std::shared_ptr<MpasStateHandle> clone(const MpasStateHandle &) const;
  void advance(MpasStateHandle &, double seconds) const;

  double norm(const MpasStateHandle &, const std::vector<std::string> &) const;
  std::string regressionManifest(const MpasStateHandle &) const;
  std::string continuationManifest(const MpasStateHandle &) const;
  std::string typedTransformManifest(const MpasStateHandle &, const std::string &validTime) const;
  std::vector<MpasTypedField> materializeTypedFields(const MpasStateHandle &,
                                                     const std::vector<std::string> &,
                                                     const std::string &validTime,
                                                     bool stateViews = false) const;
  size_t typedFieldLevels(const std::string &) const;
  // Validate the complete OOPS request before reducing it to a name. CENTER
  // is OOPS' unspecified/default stagger for every name in this pinned graph;
  // the model descriptor supplies its canonical layer/interface/surface meaning.
  size_t typedFieldLevels(const oops::Variable &) const;
  // Model declarations are views of the owner's native analysis inventory,
  // not an arbitrary list ignored by the numerical step.
  void validateModelVariables(const oops::Variables &) const;
  std::string serializeState(const MpasStateHandle &, const std::string &validTime) const;
  std::shared_ptr<MpasStateHandle> deserializeState(const std::string &,
                                                    const std::string &validTime,
                                                    std::uint64_t stateGeneration) const;
  std::uint64_t stateGeneration(const MpasStateHandle &) const;

  const std::vector<double> &cellLongitudesDegrees() const;
  const MpasHorizontalSnapshot &horizontalSnapshot() const;
  const MpasStaticVerticalSnapshot &staticVerticalSnapshot() const;
  const std::vector<double> &cellLatitudesDegrees() const;
  const std::vector<double> &cellAreas() const;
  const std::vector<std::int64_t> &cellGlobalIds() const;
  int numberLevels() const;
  double timeStepSeconds() const;
  const std::string &initialValidTime() const;
  const std::string &horizontalGeometryReceipt() const;
  const std::string &staticVerticalGeometryReceipt() const;
  const std::string &geometryReceipt() const;
  const std::string &configurationReceipt() const;
  std::string stateSchemaDigest() const;

 private:
  void requireOwned(const MpasStateHandle &, bool mutableState = false) const;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Opaque authoritative state owned by MPAS-PyTorch. Every handle contains a
/// receipt-sealed continuation boundary, including generation zero, plus
/// model-owned read-only trajectory/output views where available.
class MpasStateHandle {
 public:
  ~MpasStateHandle();

  MpasStateHandle(const MpasStateHandle &) = delete;
  MpasStateHandle &operator=(const MpasStateHandle &) = delete;

 private:
  friend class MpasBackendContext;
  struct Impl;
  explicit MpasStateHandle(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};

}  // namespace ijedi
