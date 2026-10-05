#include "ijedi/State/State.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <tuple>

#include "atlas/field.h"
#include "eckit/config/Configuration.h"
#include "eckit/exception/Exceptions.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/Utilities/PrintHelper.h"
#include "oops/base/Variables.h"
#include "oops/util/DateTime.h"
#include "oops/util/Logger.h"
#include "oops/util/for_each.h"
#include "oops/util/FieldSetOperations.h"

#include "ijedi/Io/IoBase.h"

namespace ijedi {
namespace {

std::unique_ptr<StateBackend> createStateBackend(const Geometry &geom, const oops::Variables &vars,
                                                 const util::DateTime &time, bool initToZero) {
  if (geom.isMpas()) {
    if (initToZero) {
      throw eckit::BadParameter(
          "MPAS absolute State cannot be initialized to zero; use a configured initial "
          "State or clone a complete State for restoration", Here());
    }
    return std::make_unique<MpasStateBackend>(geom, vars, time);
  }
  return std::make_unique<AtlasStateBackend>(geom, vars, time, initToZero);
}

}  // namespace

// -----------------------------------------------------------------------------------------------

State::State(const Geometry &geom, const eckit::Configuration &config) : geom_(geom) {
  const oops::Variables variables(config, "state variables");
  const util::DateTime time(config.getString("date"));
  if (geom.isMpas()) {
    backend_ = std::make_unique<MpasStateBackend>(geom, variables, time);
    if (config.has("io") || config.has("analytic init")) {
      throw eckit::BadParameter(
          "MPAS initial state is owned by the receipt-bound geometry configuration; "
          "'io' and 'analytic init' are invalid parallel authorities",
          Here());
    }
    return;
  }
  backend_ = std::make_unique<AtlasStateBackend>(geom, variables, time, false);
  // If config has 'analytic init' then call analytic_init, else if config has 'io' then call read
  if (config.has("analytic init")) {
    analytic_init(config);
  } else if (config.has("io")) {
    read(config);
  } else {
    throw eckit::BadParameter("ijedi::State: config must have 'io' or 'analytic init'", Here());
  }
  setAtlasFieldMetadata();
}

// -----------------------------------------------------------------------------------------------

State::State(const Geometry &geom, const oops::Variables &vars, const util::DateTime &time,
             bool initToZero)
    : geom_(geom), backend_(createStateBackend(geom, vars, time, initToZero)) {
  setAtlasFieldMetadata();
}

// -----------------------------------------------------------------------------------------------

State::State(const Geometry &geom, const State &other)
    : geom_(geom), backend_(other.backend_->clone(geom)) {
  setAtlasFieldMetadata();
}

// -----------------------------------------------------------------------------------------------

State::State(const oops::Variables &vars, const State &other)
    : geom_(other.geom_), backend_(other.backend_->clone(vars)) {
  setAtlasFieldMetadata();
}

State::State(const State &other) : geom_(other.geom_), backend_(other.backend_->clone()) {
  setAtlasFieldMetadata();
}

// -----------------------------------------------------------------------------------------------

State::~State() = default;

// -----------------------------------------------------------------------------------------------

State &State::operator=(const State &rhs) {
  backend_->assign(*rhs.backend_);
  return *this;
}

State &State::operator+=(const Increment &increment) {
  if (validTime() != increment.validTime()) {
    throw eckit::BadParameter("Cannot add an Increment at a different valid time", Here());
  }
  if (isMpas()) {
    throw eckit::NotImplemented(
        "MPAS State increments require the stacked variable-transform PR", Here());
  }
  if (!hasFieldSet()) {
    throw eckit::NotImplemented(
        "Adding an Increment to MPAS State requires the typed analysis control-to-native map", Here());
  }
  if (increment.fieldSet().empty()) {
    throw eckit::BadParameter("Cannot add an empty Increment to State", Here());
  }
  const std::string stateGridUid = util::getGridUid(geom_.functionSpace());
  const std::string incrementGridUid = util::getGridUid(increment.fieldSet()[0].functionspace());
  if (stateGridUid == incrementGridUid) {
    util::addFieldSets(fieldSet(), increment.fieldSet());
  } else {
    const Increment remapped(geom_, increment);
    util::addFieldSets(fieldSet(), remapped.fieldSet());
  }
  return *this;
}

// -----------------------------------------------------------------------------------------------

// Required so LocalEnsembleDA links for the current non-inline LETKF path.
// A real implementation is only needed for inline LETKF runs (`Run Inline: true`),
// where forecast states must be redistributed onto the DA-local patch layout.
void State::transpose(const State &, const eckit::mpi::Comm &, int ensNum, int transNum) {
  throw eckit::NotImplemented(
      "ijedi::State::transpose is not implemented. "
      "LETKF inline forecast transposition is unsupported in I-JEDI. "
      "The current LETKF hookup supports the non-inline path only "
      "(ensNum=" +
          std::to_string(ensNum) + ", transNum=" + std::to_string(transNum) + ").",
      Here());
}

// -----------------------------------------------------------------------------------------------

void State::read(const eckit::Configuration &config) {
  if (geom_.isMpas()) {
    throw eckit::NotImplemented(
        "MPAS State generic IO read is unsupported; use authenticated serialization", Here());
  }
  oops::Log::trace() << "ijedi::State::read starting" << std::endl;

  // Create a Parameters object
  StateParameters params;
  params.deserialize(config);

  // Check that there are IO parameters
  if (params.io.value() == boost::none || params.io.value()->ioParameters.value() == nullptr) {
    throw eckit::BadParameter("ijedi::State::read: No IO parameters provided", Here());
  }

  // Get the polymorphic IO parameters
  const IoParametersBase &ioParams = *params.io.value()->ioParameters.value();

  // Create the IO object to use
  // ---------------------------
  std::unique_ptr<IoBase> io(IoFactory::create(geom_, ioParams));

  // Call read method of child
  // -------------------------
  io->readBase(this->fieldSet());

  oops::Log::trace() << "ijedi::State::read done" << std::endl;
}

// -----------------------------------------------------------------------------------------------

void State::write(const eckit::Configuration &config) const {
  if (geom_.isMpas()) {
    throw eckit::NotImplemented(
        "MPAS State generic IO write is unsupported; use authenticated serialization", Here());
  }
  oops::Log::trace() << "ijedi::State::write starting" << std::endl;

  // Create a Parameters object
  StateWriteParameters params;
  params.deserialize(config);

  // Check that there are IO parameters
  if (params.io.value() == boost::none || params.io.value()->ioParameters.value() == nullptr) {
    throw eckit::BadParameter("ijedi::State::write: No IO parameters provided", Here());
  }

  // Get the polymorphic IO parameters
  const IoParametersBase &ioParams = *params.io.value()->ioParameters.value();

  // Create the IO object to use
  // ---------------------------
  std::unique_ptr<IoBase> io(IoFactory::create(geom_, ioParams));

  // Call write method of child
  // --------------------------
  io->writeBase(this->fieldSet());

  oops::Log::trace() << "ijedi::State::write done" << std::endl;
}

// -----------------------------------------------------------------------------------------------

void State::analytic_init(const eckit::Configuration &config) {
  oops::Log::trace() << "ijedi::State::analytic_init starting" << std::endl;
  const eckit::LocalConfiguration analytic(config, "analytic init");
  const std::string method = analytic.getString("method");
  if (method != "zero") {
    throw eckit::BadParameter(
        "ijedi::State: unsupported analytic init method '" + method + "'; supported methods: zero",
        Here());
  }
  this->zero();
  oops::Log::trace() << "ijedi::State::analytic_init done" << std::endl;
}

void State::setAtlasFieldMetadata() {
  if (isMpas() || !hasFieldSet()) {
    return;
  }
  for (auto &field : this->fieldSet()) {
    field.metadata().set("interp_type", "default");
    // A temporary hack for interpolation masks for MOM6.
    // This should be replaced by using a proper mask field for different fields
    // (probably coming from FieldsMetaData)
    if (geom_.fields().has("mask2d")) {
      field.metadata().set("mask", "mask2d");
    }
  }
}

void State::print(std::ostream &os) const {
  os << std::endl
     << "  Valid time: " << this->validTime() << ", nFields = " << this->variables().size();

  if (isMpas() || !hasFieldSet()) {
    os << ", native norm = " << std::scientific << std::setprecision(10) << norm();
    return;
  }
  const auto &comm = geom_.comm();
  const auto &fs = this->fieldSet();
  size_t maxNameLen = 0;
  for (const auto &var : this->variables()) {
    maxNameLen = std::max(maxNameLen, var.name().size());
  }
  for (const auto &var : this->variables()) {
    const atlas::Field &field = fs.field(var.name());
    if (geom_.fields().has("owned")) {
      const atlas::Field owned = geom_.fields().field("owned");
      const auto bounds = fieldMinMaxRMS(comm, field, &owned);
      const double globalMin = std::get<0>(bounds);
      const double globalMax = std::get<1>(bounds);
      const double rms = std::get<2>(bounds);
      os << std::endl
         << std::left << std::setw(maxNameLen) << var.name() << " : " << std::scientific
         << std::setprecision(10) << "Min=" << globalMin << ", Max=" << globalMax
         << ", RMS=" << rms;
    } else {
      const auto bounds = fieldMinMaxRMS(comm, field);
      const double globalMin = std::get<0>(bounds);
      const double globalMax = std::get<1>(bounds);
      const double rms = std::get<2>(bounds);
      os << std::endl
         << std::left << std::setw(maxNameLen) << var.name() << " : " << std::scientific
         << std::setprecision(10) << "Min=" << globalMin << ", Max=" << globalMax
         << ", RMS=" << rms;
    }
  }
}

// -----------------------------------------------------------------------------------------------

}  // namespace ijedi
