#include "ijedi/Increment/Increment.h"

#include <optional>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <memory>

#include "atlas/field.h"
#include "atlas/util/Earth.h"
#include "eckit/config/Configuration.h"
#include "eckit/config/LocalConfiguration.h"
#include "eckit/exception/Exceptions.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/GeometryIterator.h"
#include "ijedi/State/State.h"
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

atlas::FieldSet allocateFields(const Geometry &geom, const oops::Variables &vars) {
  if (geom.isMpas()) {
    throw eckit::NotImplemented(
        "MPAS typed Increment requires the stacked variable-transform PR", Here());
  }
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
  for (const auto &field : source) target.add(field);
}

eckit::LocalConfiguration interpolationConfig() {
  eckit::LocalConfiguration config;
  config.set("local interpolator type", "oops unstructured grid interpolator");
  return config;
}

void interpolate(const Geometry &sourceGeom, const atlas::FieldSet &source,
                 const Geometry &targetGeom, atlas::FieldSet &target) {
  const oops::GlobalInterpolator interpolation(
      interpolationConfig(), sourceGeom.geometryData(), targetGeom.functionSpace(),
      targetGeom.comm());
  interpolation.apply(source, target);
}

void interpolateAdjoint(const Geometry &targetGeom, atlas::FieldSet &target,
                        const Geometry &sourceGeom, const atlas::FieldSet &source) {
  // This is the adjoint of target -> source, hence its result lives on target.
  const oops::GlobalInterpolator interpolation(
      interpolationConfig(), targetGeom.geometryData(), sourceGeom.functionSpace(),
      sourceGeom.comm());
  interpolation.applyAD(target, source);
}

void accumulate(atlas::FieldSet &target, double weight, const atlas::FieldSet &source) {
  for (const auto &sourceField : source) {
    atlas::Field targetField = target.field(sourceField.name());
    util::for_each_value(
        util::IndexRange::include_halo,
        [weight](const double rhs, double &lhs) { lhs += weight * rhs; },
        sourceField, targetField);
    targetField.set_dirty(targetField.dirty() || sourceField.dirty());
  }
}

}  // namespace

  // -----------------------------------------------------------------------------------------------

  Increment::Increment(const Geometry & geom, const oops::Variables & vars,
                       const util::DateTime & time)
      : geom_(geom), fields_(time, geom.comm()) {
    fields_.deepCopy(allocateFields(geom, vars));
    fields_.zero();
  }

  // -----------------------------------------------------------------------------------------------

  Increment::Increment(const Geometry & geom, const Increment & other, const bool ad)
      : geom_(geom), fields_(other.validTime(), geom.comm()) {
    fields_.deepCopy(allocateFields(geom, other.variables()));
    fields_.zero();
    if (&geom == &other.geom_ || fields_.getGridUid() == other.fields_.getGridUid()) {
      fields_.deepCopy(other.fields_);
    } else if (ad) {
      interpolateAdjoint(geom_, fields_.fieldSet(), other.geom_, other.fieldSet());
    } else {
      interpolate(other.geom_, other.fieldSet(), geom_, fields_.fieldSet());
    }
  }

  // -----------------------------------------------------------------------------------------------

  Increment::Increment(const Increment & other, const bool copy)
      : geom_(other.geom_), fields_(other.validTime(), other.geom_.comm()) {
    if (copy) {
      fields_.deepCopy(other.fields_);
    } else {
      fields_.deepCopy(allocateFields(geom_, other.variables()));
      fields_.zero();
    }
  }

  // -----------------------------------------------------------------------------------------------

  Increment::~Increment() = default;

  // -----------------------------------------------------------------------------------------------

  Increment & Increment::operator=(const Increment & rhs) {
    if (&geom_ != &rhs.geom_ && fields_.getGridUid() != rhs.fields_.getGridUid()) {
      throw eckit::BadParameter("Cannot assign Increments on different grids", Here());
    }
    fields_.validTime() = rhs.validTime();
    fields_.deepCopy(rhs.fields_);
    return *this;
  }

  void Increment::diff(const State &x1, const State &x2) {
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
            "Cannot difference States missing Increment variable '" + variable.name() + "'",
            Here());
      }
      firstSelected.add(first.fieldSet().field(variable.name()));
      secondSelected.add(second.fieldSet().field(variable.name()));
    }
    fields_.validTime() = x1.validTime();
    fields_.deepCopy(firstSelected);
    util::subtractFieldSets(fields_.fieldSet(), secondSelected);
    setMissingFieldMetadata(geom_, fields_.fieldSet());
  }

  void Increment::ones() {
    for (auto &field : fields_.fieldSet()) {
      atlas::array::make_view<double, 2>(field).assign(1.0);
    }
  }

  Increment &Increment::operator+=(const Increment &rhs) {
    fields_ += rhs.fields_;
    return *this;
  }

  Increment &Increment::operator-=(const Increment &rhs) {
    fields_ -= rhs.fields_;
    return *this;
  }

  Increment &Increment::operator*=(double weight) {
    fields_ *= weight;
    return *this;
  }

  void Increment::axpy(double weight, const Increment &rhs, bool checkTimesEqual) {
    if (checkTimesEqual && validTime() != rhs.validTime()) {
      throw eckit::BadParameter("Increment::axpy valid-time mismatch", Here());
    }
    if (variables() != rhs.variables()) {
      throw eckit::BadParameter("Increment::axpy variable mismatch", Here());
    }
    accumulate(fields_.fieldSet(), weight, rhs.fieldSet());
  }

  double Increment::dot_product_with(const Increment &rhs) const {
    return fields_.dot_product_with(rhs.fields_, fields_.variables());
  }

  void Increment::schur_product_with(const Increment &rhs) { fields_ *= rhs.fields_; }

  void Increment::random() {
    fields_.deepCopy(util::createRandomFieldSet(
        geom_.comm(), geom_.functionSpace(), geom_.variableSizes(variables()),
        variables().variables()));
    setMissingFieldMetadata(geom_, fields_.fieldSet());
  }

  void Increment::accumul(double weight, const State &state) {
    atlas::FieldSet selected;
    for (const auto &var : variables()) selected.add(state.fieldSet().field(var.name()));
    accumulate(fields_.fieldSet(), weight, selected);
  }

  oops::LocalIncrement Increment::getLocal(const GeometryIterator &iter) const {
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
      throw eckit::BadParameter("LocalIncrement value count does not match geometry column",
                                Here());
    }
  }

  void Increment::toFieldSet(atlas::FieldSet &target) const { shareFields(fieldSet(), target); }

  void Increment::fromFieldSet(const atlas::FieldSet &source) {
    fields_.deepCopy(source);
    setMissingFieldMetadata(geom_, fields_.fieldSet());
  }

  void Increment::deserialize(const std::vector<double> &buffer, size_t &index) {
    size_t timeIndex = index;
    util::DateTime serializedTime;
    serializedTime.deserialize(buffer, timeIndex);
    fields_.validTime() = serializedTime;
    fields_.deserialize(buffer, index);
  }

  // -----------------------------------------------------------------------------------------------

  void Increment::read(const eckit::Configuration & config) {
    oops::Log::trace() << "ijedi::Increment::read starting" << std::endl;

    // Create a Parameters object
    IncrementParameters params;
    params.deserialize(config);

    // Check that there are IO parameters
    if (params.io.value() == boost::none ||
        params.io.value()->ioParameters.value() == nullptr)
    {
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

  void Increment::write(const eckit::Configuration & config) const {
    oops::Log::trace() << "ijedi::Increment::write starting" << std::endl;

    // Create a Parameters object
    IncrementWriteParameters params;
    params.deserialize(config);

    // Check that there are IO parameters
    if (params.io.value() == boost::none ||
        params.io.value()->ioParameters.value() == nullptr)
    {
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

  void Increment::dirac(const eckit::Configuration & config) {
    oops::Log::trace() << "ijedi::Increment::dirac starting" << std::endl;

    // Create a Parameters object
    DiracParameters params;
    params.deserialize(config);

    // Grid-agnostic lon/lat dirac: the globally-nearest owned grid node is found
    // via the geometry's shared KD-tree (the same tree used by interpolation).
    // Validate that all vectors are the same length.
    const std::vector<double> & lon = params.lon.value();
    const std::vector<double> & lat = params.lat.value();
    const std::vector<int> & level = params.level.value();
    const std::vector<std::string> & vars = params.variable.value();
    const size_t nDiracs = lon.size();
    ASSERT_MSG(lat.size() == nDiracs, "Dirac: 'lat' inconsistent length");
    ASSERT_MSG(level.size() == nDiracs, "Dirac: 'level' inconsistent length");
    ASSERT_MSG(vars.size() == nDiracs, "Dirac: 'variable' inconsistent length");

    const auto & comm = this->geom_.comm();
    const auto & geomData = this->geom_.geometryData();
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
                        << "): requested " << lon[jdir] << "/" << lat[jdir]
                        << ", placed at " << lonDir << "/" << latDir
                        << ", level " << level[jdir] << std::endl;
    }

    oops::Log::trace() << "ijedi::Increment::dirac done" << std::endl;
  }

  // -----------------------------------------------------------------------------------------------

  void Increment::print(std::ostream & os) const {
    os << std::endl
       << "  Valid time: " << this->validTime()
       << ", nFields = " << this->variables().size();

    const auto & comm = geom_.comm();
    const auto & fs   = this->fieldSet();
    size_t maxNameLen = 0;
    for (const auto & var : this->variables()) {
      maxNameLen = std::max(maxNameLen, var.name().size());
    }
    for (const auto & var : this->variables()) {
      const atlas::Field & field            = fs.field(var.name());
      const auto[globalMin, globalMax, rms] = fieldMinMaxRMS(comm, field);
      os << std::endl
         << std::left << std::setw(maxNameLen) << var.name()
         << " : " << std::scientific << std::setprecision(10)
         << "Min=" << globalMin << ", Max=" << globalMax << ", RMS=" << rms;
    }
  }

  // -----------------------------------------------------------------------------------------------

  bool Increment::isMpas() const { return geom_.isMpas(); }

}  // namespace ijedi
