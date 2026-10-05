#include "ijedi/Increment/Increment.h"

#include <optional>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <memory>
#include <utility>
#include <tuple>

#include "atlas/field.h"
#include "atlas/util/Earth.h"
#include "eckit/config/Configuration.h"
#include "eckit/config/LocalConfiguration.h"
#include "eckit/exception/Exceptions.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/GeometryIterator.h"
#include "ijedi/State/State.h"
#include "ijedi/Increment/MpasIncrementBackend.h"
#include "ijedi/Utilities/PrintHelper.h"
#include "oops/base/GeometryData.h"
#include "oops/base/Variables.h"
#include "oops/generic/GlobalInterpolator.h"
#include "oops/util/DateTime.h"
#include "oops/util/Logger.h"
#include "oops/util/for_each.h"
#include "oops/util/FieldSetHelpers.h"
#include "oops/util/FieldSetOperations.h"

namespace ijedi {
namespace {

std::unique_ptr<IncrementBackend> storage(const Geometry &geom, const oops::Variables &vars,
                                          const util::DateTime &time) {
  if (geom.isMpas()) {
    return std::make_unique<MpasIncrementBackend>(geom, vars, time);
  }
  return std::make_unique<AtlasIncrementBackend>(time, geom.comm());
}

atlas::FieldSet allocateFields(const Geometry &geom, const oops::Variables &vars) {
  atlas::FieldSet fields =
      util::createFieldSet(geom.functionSpace(), geom.variableSizes(vars), vars.variables());
  for (auto &field : fields) {
    field.metadata().set("interp_type", "default");
  }
  return fields;
}

void setMissingFieldMetadata(const Geometry &geom, atlas::FieldSet &fields) {
  for (auto &field : fields) {
    if (!field.metadata().has("interp_type")) {
      field.metadata().set("interp_type", "default");
    }
  }
}

void shareFields(const atlas::FieldSet &source, atlas::FieldSet &target) {
  target.clear();
  for (const auto &field : source) {
    target.add(field);
  }
}

eckit::LocalConfiguration interpolationConfig() {
  eckit::LocalConfiguration config;
  config.set("local interpolator type", "oops unstructured grid interpolator");
  return config;
}

void interpolate(const Geometry &sourceGeom, const atlas::FieldSet &source,
                 const Geometry &targetGeom, atlas::FieldSet &target) {
  const oops::GlobalInterpolator interpolation(interpolationConfig(), sourceGeom.geometryData(),
                                               targetGeom.functionSpace(), targetGeom.comm());
  interpolation.apply(source, target);
}

void interpolateAdjoint(const Geometry &targetGeom, atlas::FieldSet &target,
                        const Geometry &sourceGeom, const atlas::FieldSet &source) {
  // This is the adjoint of target -> source, hence its result lives on target.
  const oops::GlobalInterpolator interpolation(interpolationConfig(), targetGeom.geometryData(),
                                               sourceGeom.functionSpace(), sourceGeom.comm());
  interpolation.applyAD(target, source);
}

void accumulate(atlas::FieldSet &target, double weight, const atlas::FieldSet &source) {
  for (const auto &sourceField : source) {
    atlas::Field targetField = target.field(sourceField.name());
    util::for_each_value(
        util::IndexRange::include_halo,
        [weight](const double rhs, double &lhs) { lhs += weight * rhs; }, sourceField, targetField);
    targetField.set_dirty(targetField.dirty() || sourceField.dirty());
  }
}

}  // namespace

// -----------------------------------------------------------------------------------------------

Increment::Increment(const Geometry &geom, const oops::Variables &vars, const util::DateTime &time)
    : geom_(geom), backend_(storage(geom, vars, time)) {
  if (isMpas()) {
    return;
  }
  atlasFields().deepCopy(allocateFields(geom, vars));
  atlasFields().zero();
}

// -----------------------------------------------------------------------------------------------

Increment::Increment(const Geometry &geom, const Increment &other, const bool ad)
    : geom_(geom), backend_(storage(geom, other.variables(), other.validTime())) {
  if (isMpas() || other.isMpas()) {
    if (!isMpas() || !other.isMpas() ||
        geom_.mpasContext()->geometryReceipt() != other.geom_.mpasContext()->geometryReceipt() ||
        geom_.mpasContext()->configurationReceipt() !=
            other.geom_.mpasContext()->configurationReceipt() ||
        geom_.mpasContext()->stateSchemaDigest() !=
            other.geom_.mpasContext()->stateSchemaDigest()) {
      throw eckit::BadParameter("MPAS Increment resolution/backend/support change is unsupported",
                                Here());
    }
    backend_->typed().setArrays(other.analysisArrays());
    return;
  }
  atlasFields().deepCopy(allocateFields(geom, other.variables()));
  atlasFields().zero();
  if (&geom == &other.geom_ || atlasFields().getGridUid() == other.atlasFields().getGridUid()) {
    atlasFields().deepCopy(other.atlasFields());
  } else if (ad) {
    interpolateAdjoint(geom_, atlasFields().fieldSet(), other.geom_, other.fieldSet());
  } else {
    interpolate(other.geom_, other.fieldSet(), geom_, atlasFields().fieldSet());
  }
}

// -----------------------------------------------------------------------------------------------

Increment::Increment(const Increment &other, const bool copy)
    : geom_(other.geom_), backend_(storage(geom_, other.variables(), other.validTime())) {
  if (isMpas()) {
    if (copy) {
      backend_->typed().setArrays(other.analysisArrays());
    }
    return;
  }
  if (copy) {
    atlasFields().deepCopy(other.atlasFields());
  } else {
    atlasFields().deepCopy(allocateFields(geom_, other.variables()));
    atlasFields().zero();
  }
}

// -----------------------------------------------------------------------------------------------

Increment::~Increment() = default;

// -----------------------------------------------------------------------------------------------

Increment &Increment::operator=(const Increment &rhs) {
  if (isMpas() || rhs.isMpas()) {
    Increment compatible(geom_, rhs);
    backend_.swap(compatible.backend_);
    return *this;
  }
  if (&geom_ != &rhs.geom_ && atlasFields().getGridUid() != rhs.atlasFields().getGridUid()) {
    throw eckit::BadParameter("Cannot assign Increments on different grids", Here());
  }
  atlasFields().validTime() = rhs.validTime();
  atlasFields().deepCopy(rhs.atlasFields());
  return *this;
}

void Increment::diff(const State &x1, const State &x2) {
  if (isMpas()) {
    if (analysisNamespace() != "native" || !x1.isMpas() || !x2.isMpas() ||
        x1.validTime() != x2.validTime() || validTime() != x1.validTime()) {
      throw eckit::BadParameter("MPAS State difference requires same-time native coordinates",
                                Here());
    }
    for (const auto *state : {&x1, &x2}) {
      if (geom_.mpasContext()->geometryReceipt() !=
              state->geometry().mpasContext()->geometryReceipt() ||
          geom_.mpasContext()->configurationReceipt() !=
              state->geometry().mpasContext()->configurationReceipt() ||
          geom_.mpasContext()->stateSchemaDigest() !=
              state->geometry().mpasContext()->stateSchemaDigest()) {
        throw eckit::BadParameter("MPAS State difference owner/support mismatch", Here());
      }
    }
    auto first = x1.nativeAnalysisValues();
    const auto second = x2.nativeAnalysisValues();
    auto result = analysisArrays();
    for (auto &[name, array] : result) {
      for (size_t i = 0; i < array.values.size(); ++i) {
        array.values[i] = first.at(name).values.at(i) - second.at(name).values.at(i);
      }
    }
    backend_->typed().setArrays(result);
    return;
  }
  if (x1.validTime() != x2.validTime()) {
    throw eckit::BadParameter("Cannot difference States at different valid times", Here());
  }
  const oops::Variables requested(variables());
  State first(geom_, x1);
  State second(geom_, x2);
  atlas::FieldSet firstSelected;
  atlas::FieldSet secondSelected;
  for (const auto &variable : requested) {
    if (!first.fieldSet().has(variable.name()) || !second.fieldSet().has(variable.name())) {
      throw eckit::BadValue(
          "Cannot difference States missing Increment variable '" + variable.name() + "'", Here());
    }
    firstSelected.add(first.fieldSet().field(variable.name()));
    secondSelected.add(second.fieldSet().field(variable.name()));
  }
  atlasFields().validTime() = x1.validTime();
  atlasFields().deepCopy(firstSelected);
  util::subtractFieldSets(atlasFields().fieldSet(), secondSelected);
  setMissingFieldMetadata(geom_, atlasFields().fieldSet());
}

void Increment::ones() {
  if (isMpas()) {
    backend_->typed().fill(1.);
    return;
  }
  for (auto &field : atlasFields().fieldSet()) {
    atlas::array::make_view<double, 2>(field).assign(1.0);
  }
}

Increment &Increment::operator+=(const Increment &rhs) {
  if (isMpas()) {
    backend_->typed().axpy(1., rhs.backend_->typed());
    return *this;
  }
  atlasFields() += rhs.atlasFields();
  return *this;
}

Increment &Increment::operator-=(const Increment &rhs) {
  if (isMpas()) {
    backend_->typed().axpy(-1., rhs.backend_->typed());
    return *this;
  }
  atlasFields() -= rhs.atlasFields();
  return *this;
}

Increment &Increment::operator*=(double weight) {
  if (isMpas()) {
    backend_->typed().scale(weight);
    return *this;
  }
  atlasFields() *= weight;
  return *this;
}

void Increment::axpy(double weight, const Increment &rhs, bool checkTimesEqual) {
  if (isMpas()) {
    backend_->typed().axpy(weight, rhs.backend_->typed(), checkTimesEqual);
    return;
  }
  if (checkTimesEqual && validTime() != rhs.validTime()) {
    throw eckit::BadParameter("Increment::axpy valid-time mismatch", Here());
  }
  if (variables() != rhs.variables()) {
    throw eckit::BadParameter("Increment::axpy variable mismatch", Here());
  }
  accumulate(atlasFields().fieldSet(), weight, rhs.fieldSet());
}

double Increment::dot_product_with(const Increment &rhs) const {
  if (isMpas()) {
    return backend_->typed().dot(rhs.backend_->typed());
  }
  return atlasFields().dot_product_with(rhs.atlasFields(), atlasFields().variables());
}

void Increment::schur_product_with(const Increment &rhs) {
  if (isMpas()) {
    backend_->typed().schur(rhs.backend_->typed());
    return;
  }
  atlasFields() *= rhs.atlasFields();
}

void Increment::random() {
  if (isMpas()) {
    backend_->typed().random();
    return;
  }
  atlasFields().deepCopy(util::createRandomFieldSet(geom_.comm(), geom_.functionSpace(),
                                                    geom_.variableSizes(variables()),
                                                    variables().variables()));
  setMissingFieldMetadata(geom_, atlasFields().fieldSet());
}

void Increment::accumul(double weight, const State &state) {
  if (isMpas()) {
    if (analysisNamespace() != "native" || !state.isMpas() || state.validTime() != validTime()) {
      throw eckit::BadParameter("MPAS accumulation requires same-time native coordinates", Here());
    }
    if (geom_.mpasContext()->geometryReceipt() !=
            state.geometry().mpasContext()->geometryReceipt() ||
        geom_.mpasContext()->configurationReceipt() !=
            state.geometry().mpasContext()->configurationReceipt() ||
        geom_.mpasContext()->stateSchemaDigest() !=
            state.geometry().mpasContext()->stateSchemaDigest()) {
      throw eckit::BadParameter("MPAS State accumulation owner/support mismatch", Here());
    }
    Increment native(geom_, variables(), validTime());
    auto selected = native.analysisArrays();
    const auto values = state.nativeAnalysisValues();
    for (auto &[name, array] : selected) {
      array = values.at(name);
    }
    native.backend_->typed().setArrays(selected);
    backend_->typed().axpy(weight, native.backend_->typed());
    return;
  }
  atlas::FieldSet selected;
  for (const auto &var : variables()) {
    selected.add(state.fieldSet().field(var.name()));
  }
  accumulate(atlasFields().fieldSet(), weight, selected);
}

oops::LocalIncrement Increment::getLocal(const GeometryIterator &iter) const {
  if (isMpas()) {
    throw eckit::NotImplemented("MPAS DA-local patch packing is outside serial-global MPAS interfaces",
                                Here());
  }
  std::vector<double> values;
  std::vector<int> lengths;
  const atlas::idx_t node = geom_.ownedNodeIndices().at(iter.nodeIndex());
  for (const auto &var : variables()) {
    const auto view = atlas::array::make_view<double, 2>(fieldSet().field(var.name()));
    if (geom_.iteratorDimension() == 3) {
      values.push_back(view(node, iter.levelIndex()));
      lengths.push_back(1);
    } else {
      lengths.push_back(view.shape(1));
      for (atlas::idx_t level = 0; level < view.shape(1); ++level) {
        values.push_back(view(node, level));
      }
    }
  }
  return oops::LocalIncrement(variables(), values, lengths);
}

void Increment::setLocal(const oops::LocalIncrement &local, const GeometryIterator &iter) {
  if (isMpas()) {
    throw eckit::NotImplemented("MPAS DA-local patch unpacking is outside serial-global MPAS interfaces",
                                Here());
  }
  const std::vector<double> values = local.getVals();
  const atlas::idx_t node = geom_.ownedNodeIndices().at(iter.nodeIndex());
  size_t cursor = 0;
  for (const auto &var : variables()) {
    auto view = atlas::array::make_view<double, 2>(fieldSet().field(var.name()));
    if (geom_.iteratorDimension() == 3) {
      view(node, iter.levelIndex()) = values.at(cursor++);
    } else {
      for (atlas::idx_t level = 0; level < view.shape(1); ++level) {
        view(node, level) = values.at(cursor++);
      }
    }
  }
  if (cursor != values.size()) {
    throw eckit::BadParameter("LocalIncrement value count does not match geometry column", Here());
  }
}

void Increment::toFieldSet(atlas::FieldSet &target) const {
  if (isMpas()) {
    backend_->typed().toFieldSet(target);
    return;
  }
  shareFields(fieldSet(), target);
}

void Increment::fromFieldSet(const atlas::FieldSet &source) {
  if (isMpas()) {
    backend_->typed().fromFieldSet(source);
    return;
  }
  atlasFields().deepCopy(source);
  setMissingFieldMetadata(geom_, atlasFields().fieldSet());
}

void Increment::deserialize(const std::vector<double> &buffer, size_t &index) {
  if (isMpas()) {
    backend_->typed().deserialize(buffer, index);
    return;
  }
  size_t timeIndex = index;
  util::DateTime serializedTime;
  serializedTime.deserialize(buffer, timeIndex);
  atlasFields().validTime() = serializedTime;
  atlasFields().deserialize(buffer, index);
}

// -----------------------------------------------------------------------------------------------

void Increment::read(const eckit::Configuration &config) {
  if (isMpas()) {
    throw eckit::NotImplemented(
        "MPAS Increment external IO is outside the typed in-process boundary", Here());
  }
  oops::Log::trace() << "ijedi::Increment::read starting" << std::endl;

  // Create a Parameters object
  IncrementParameters params;
  params.deserialize(config);

  // Check that there are IO parameters
  if (params.io.value() == boost::none || params.io.value()->ioParameters.value() == nullptr) {
    throw eckit::BadParameter("ijedi::Increment::read: No IO parameters provided", Here());
  }

  // Get the polymorphic IO parameters
  const IoParametersBase &ioParams = *params.io.value()->ioParameters.value();

  // Create the IO object to use
  // ---------------------------
  std::unique_ptr<IoBase> io(IoFactory::create(geom_, ioParams));

  // Call read method of child
  // -------------------------
  io->readBase(this->fieldSet());

  oops::Log::trace() << "ijedi::Increment::read done" << std::endl;
}

// -----------------------------------------------------------------------------------------------

void Increment::write(const eckit::Configuration &config) const {
  if (isMpas()) {
    throw eckit::NotImplemented(
        "MPAS Increment external IO is outside the typed in-process boundary", Here());
  }
  oops::Log::trace() << "ijedi::Increment::write starting" << std::endl;

  // Create a Parameters object
  IncrementWriteParameters params;
  params.deserialize(config);

  // Check that there are IO parameters
  if (params.io.value() == boost::none || params.io.value()->ioParameters.value() == nullptr) {
    throw eckit::BadParameter("ijedi::Increment::write: No IO parameters provided", Here());
  }

  // Get the polymorphic IO parameters
  const IoParametersBase &ioParams = *params.io.value()->ioParameters.value();

  // Create the IO object to use
  // ---------------------------
  std::unique_ptr<IoBase> io(IoFactory::create(geom_, ioParams));

  // Call write method of child
  // --------------------------
  io->writeBase(this->fieldSet());

  oops::Log::trace() << "ijedi::Increment::write done" << std::endl;
}

// -----------------------------------------------------------------------------------------------

void Increment::dirac(const eckit::Configuration &config) {
  if (isMpas()) {
    throw eckit::NotImplemented(
        "MPAS model-specific Dirac is outside the typed in-process boundary", Here());
  }
  oops::Log::trace() << "ijedi::Increment::dirac starting" << std::endl;

  // Create a Parameters object
  DiracParameters params;
  params.deserialize(config);

  // Grid-agnostic lon/lat dirac: the globally-nearest owned grid node is found
  // via the geometry's shared KD-tree (the same tree used by interpolation).
  // Validate that all vectors are the same length.
  const std::vector<double> &lon = params.lon.value();
  const std::vector<double> &lat = params.lat.value();
  const std::vector<int> &level = params.level.value();
  const std::vector<std::string> &vars = params.variable.value();
  const size_t nDiracs = lon.size();
  ASSERT_MSG(lat.size() == nDiracs, "Dirac: 'lat' inconsistent length");
  ASSERT_MSG(level.size() == nDiracs, "Dirac: 'level' inconsistent length");
  ASSERT_MSG(vars.size() == nDiracs, "Dirac: 'variable' inconsistent length");

  const auto &comm = this->geom_.comm();
  const auto &geomData = this->geom_.geometryData();
  const auto lonlatView = atlas::array::make_view<double, 2>(geomData.functionSpace().lonlat());

  // Search radius for the nearest owned node. The global tree returns the
  // closest point within this chord distance (meters); a value larger than
  // any grid spacing guarantees a hit. Use a quarter of the Earth's
  // circumference so even the coarsest grids resolve.
  const double searchRadius = 0.25 * 2.0 * M_PI * atlas::util::Earth::radius();

  // Start from zero so only the requested points are nonzero.
  this->zero();

  for (size_t jdir = 0; jdir < nDiracs; ++jdir) {
    atlas::Field field = this->fieldSet().field(vars[jdir]);

    // The MPI task owning the globally-nearest node, then (on that task) the
    // task-local functionspace index of that node.
    const int localTask = geomData.closestTask(lat[jdir], lon[jdir]);

    // level input is 1-based -> 0-based array index.
    const int lev = level[jdir] - 1;
    double lonDir = 0.0;
    double latDir = 0.0;
    if (static_cast<size_t>(localTask) == comm.rank()) {
      const std::optional<int> index =
          geomData.closestPointWithinRadius(lat[jdir], lon[jdir], searchRadius);
      ASSERT_MSG(index.has_value(), "Dirac: no owned grid node found near requested point");
      auto view = atlas::array::make_view<double, 2>(field);
      view(*index, lev) = 1.0;
      lonDir = lonlatView(*index, 0);
      latDir = lonlatView(*index, 1);
    }

    // Log the resolved location (sum reduction picks up the owning task's value).
    comm.allReduceInPlace(lonDir, eckit::mpi::sum());
    comm.allReduceInPlace(latDir, eckit::mpi::sum());
    oops::Log::info() << "ijedi::Increment::dirac point #" << jdir << " (" << vars[jdir]
                      << "): requested " << lon[jdir] << "/" << lat[jdir] << ", placed at "
                      << lonDir << "/" << latDir << ", level " << level[jdir] << std::endl;
  }

  oops::Log::trace() << "ijedi::Increment::dirac done" << std::endl;
}

// -----------------------------------------------------------------------------------------------

void Increment::print(std::ostream &os) const {
  if (isMpas()) {
    os << "MPAS typed " << analysisNamespace() << " Increment: time=" << validTime()
       << ", fields=" << variables().size() << ", geometric_L2_norm=" << norm();
    return;
  }
  os << std::endl
     << "  Valid time: " << this->validTime() << ", nFields = " << this->variables().size();

  const auto &comm = geom_.comm();
  const auto &fs = this->fieldSet();
  size_t maxNameLen = 0;
  for (const auto &var : this->variables()) {
    maxNameLen = std::max(maxNameLen, var.name().size());
  }
  for (const auto &var : this->variables()) {
    const atlas::Field &field = fs.field(var.name());
    const auto bounds = fieldMinMaxRMS(comm, field);
    const double globalMin = std::get<0>(bounds);
    const double globalMax = std::get<1>(bounds);
    const double rms = std::get<2>(bounds);
    os << std::endl
       << std::left << std::setw(maxNameLen) << var.name() << " : " << std::scientific
       << std::setprecision(10) << "Min=" << globalMin << ", Max=" << globalMax << ", RMS=" << rms;
  }
}

// -----------------------------------------------------------------------------------------------

bool Increment::isMpas() const { return backend_->isMpas(); }

const util::DateTime Increment::validTime() const {
  return isMpas() ? backend_->typed().validTime() : atlasFields().validTime();
}
void Increment::updateTime(const util::Duration &duration) {
  if (isMpas()) {
    backend_->typed().updateTime(duration);
  } else {
    atlasFields().validTime() += duration;
  }
}
const oops::Variables &Increment::variables() const {
  return isMpas() ? backend_->typed().variables() : atlasFields().variables();
}
void Increment::zero() {
  if (isMpas()) {
    backend_->typed().zero();
  } else {
    atlasFields().zero();
  }
}
void Increment::zero(const util::DateTime &time) {
  if (isMpas()) {
    auto next = storage(geom_, variables(), time);
    backend_.swap(next);
  } else {
    atlasFields().validTime() = time;
    atlasFields().zero();
  }
}
void Increment::sqrt() {
  if (isMpas()) {
    backend_->typed().squareRoot();
  } else {
    atlasFields().sqrt();
  }
}
double Increment::norm() const {
  return isMpas() ? backend_->typed().norm() : atlasFields().norm(variables());
}
atlas::FieldSet &Increment::fieldSet() {
  return isMpas() ? backend_->typed().fieldSet() : atlasFields().fieldSet();
}
const atlas::FieldSet &Increment::fieldSet() const {
  return isMpas() ? backend_->typed().fieldSet() : atlasFields().fieldSet();
}
size_t Increment::serialSize() const {
  return isMpas() ? backend_->typed().serialSize() : atlasFields().serialSize();
}
void Increment::serialize(std::vector<double> &buffer) const {
  if (isMpas()) {
    backend_->typed().serialize(buffer);
  } else {
    atlasFields().serialize(buffer);
  }
}
MpasAnalysisArrays Increment::analysisArrays() const { return backend_->typed().arrays(); }
MpasAnalysisArrays Increment::completeAnalysisArrays() const {
  return backend_->typed().completeArrays();
}
MpasAnalysisArrays Increment::analysisMeasures() const { return backend_->typed().measures(); }
const std::string &Increment::analysisNamespace() const { return backend_->typed().nameSpace(); }
const oops::Variables &Increment::analysisCarriedVariables() const {
  return backend_->typed().carriedVariables();
}
void Increment::replaceAnalysis(const oops::Variables &vars, const MpasAnalysisArrays &arrays) {
  if (!isMpas()) {
    throw eckit::BadParameter("typed analysis replacement requires MPAS Increment", Here());
  }
  auto next = std::make_unique<MpasIncrementBackend>(geom_, vars, validTime());
  next->setArrays(arrays);
  backend_ = std::move(next);
}

}  // namespace ijedi
