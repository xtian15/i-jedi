#include <string>
#include <numeric>
#include <cmath>

#include "eckit/config/Configuration.h"
#include "eckit/exception/Exceptions.h"

#include "atlas/array.h"

#include "oops/base/Variables.h"
#include "oops/util/Logger.h"
#include "oops/util/missingValues.h"

#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/base/GeometryBase.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "ijedi/Geometry/mpas/GeometryMPAS.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"

// -------------------------------------------------------------------------------------------------
namespace ijedi {
// -----------------------------------------------------------------------------------------------
Geometry::Geometry(const eckit::Configuration &geomConf, const eckit::mpi::Comm &comm)
    : comm_(comm) {
  // Trace
  oops::Log::trace() << "Geometry constructor starting" << std::endl;

  // Get the type
  if (geomConf.has("geometry_type")) {
    type_ = geomConf.getString("geometry_type");
  } else {
    // Abort
    std::stringstream errorMsg;
    errorMsg << "Geometry type (geometry_type) not specified in configuration.";
    throw eckit::BadValue(errorMsg.str(), Here());
  }

  // Create the geometry implementation (which will set numLevels_)
  geometryImpl_ = GeometryBase::create(geomConf, comm, modelData_, functionspace_, fields_,
                                       levelsAreTopDown_, numberLevels_, mpasContext_);

  // Construct the fields metadata object using numLevels from the base class
  fieldsMeta_ = std::make_shared<FieldsMetadata>(numberLevels_);

  // Set up levels information for each variable using the fields metadata
  levelsPerVariable_ = fieldsMeta_->levelsPerVariable();

  // Expose vertical ordering to downstream components such as Vader recipes.
  modelData_.set("levels_are_top_down", levelsAreTopDown_);

  // Reference pressure column used by vertical localization, now chosen by "vertical
  // coordinate source". The default prefers one published by the model geometry (which can
  // follow that model's own mid-layer convention, see GeometryFV3), then the generic
  // hybrid-sigma version, then level indices.
  const std::string vertCoordSource = geomConf.getString("vertical coordinate source", "model");
  if (vertCoordSource != "model" && vertCoordSource != "hybrid sigma" &&
      vertCoordSource != "level index") {
    throw eckit::BadValue(
        "ijedi::Geometry: 'vertical coordinate source' must be 'model', "
        "'hybrid sigma' or 'level index'",
        Here());
  }

  const bool hasModelColumn = modelData_.has("vertical_coordinate_reference_pressure");
  const bool hasHybridCoeffs = modelData_.has("sigma_pressure_hybrid_coordinate_a_coefficient") &&
                               modelData_.has("sigma_pressure_hybrid_coordinate_b_coefficient");

  if (vertCoordSource == "model" && hasModelColumn) {
    verticalCoord_ = modelData_.getDoubleVector("vertical_coordinate_reference_pressure");
  } else if (vertCoordSource != "level index" && hasHybridCoeffs) {
    const std::vector<double> ak =
        modelData_.getDoubleVector("sigma_pressure_hybrid_coordinate_a_coefficient");
    const std::vector<double> bk =
        modelData_.getDoubleVector("sigma_pressure_hybrid_coordinate_b_coefficient");
    if (ak.size() != bk.size() || ak.size() != static_cast<size_t>(numberLevels_ + 1)) {
      throw eckit::BadValue(
          "ijedi::Geometry: hybrid-sigma ak/bk must contain nlevels+1 "
          "interface coefficients",
          Here());
    }
    verticalCoord_.resize(numberLevels_);
    constexpr double referenceSurfacePressure = 100000.0;
    for (int level = 0; level < numberLevels_; ++level) {
      const double upper = ak[level] + bk[level] * referenceSurfacePressure;
      const double lower = ak[level + 1] + bk[level + 1] * referenceSurfacePressure;
      verticalCoord_[level] = 0.5 * (upper + lower);
    }
  } else if (vertCoordSource == "hybrid sigma") {
    throw eckit::BadValue(
        "ijedi::Geometry: 'vertical coordinate source: hybrid sigma' needs "
        "the hybrid-sigma ak/bk coefficients in the model data",
        Here());
  } else {
    verticalCoord_.resize(numberLevels_);
    std::iota(verticalCoord_.begin(), verticalCoord_.end(), 0.0);
  }

  // Build GeometryData
  geomData_.reset(new oops::GeometryData(functionspace_, fields_, levelsAreTopDown_, comm));

  // Enable grid-point iteration. Default of 2 (whole columns) is what
  // oops::VerticalLocEV relies on when populating its eigenvectors.
  const int iteratorDimension = geomConf.getInt("iterator dimension", 2);
  if (iteratorDimension != 2 && iteratorDimension != 3) {
    throw eckit::BadValue("ijedi::Geometry: 'iterator dimension' must be 2 or 3", Here());
  }
  iteratorDimension_ = static_cast<size_t>(iteratorDimension);
  const auto ghost = atlas::array::make_view<int, 1>(functionspace_.ghost());
  for (atlas::idx_t node = 0; node < ghost.shape(0); ++node) {
    if (ghost(node) == 0) {
      ownedNodeIndices_.push_back(node);
    }
  }

  // Trace
  oops::Log::trace() << "Geometry constructor finished" << std::endl;
}
// -----------------------------------------------------------------------------------------------
Geometry::~Geometry() {}

std::vector<double> Geometry::verticalCoord(std::string &units) const {
  if (isMpas()) {
    return geometryImpl_->verticalCoord(units);
  }
  return verticalCoord_;
}

const MpasAtlasGeometry &Geometry::mpasAtlasGeometry() const {
  const auto *mpas = dynamic_cast<const GeometryMPAS *>(geometryImpl_.get());
  if (!mpas) {
    throw eckit::BadValue("Native MPAS Atlas geometry requested for another model", Here());
  }
  return mpas->atlasGeometry();
}

std::vector<size_t> Geometry::variableSizes(const oops::Variables &vars) const {
  std::vector<size_t> sizes;
  sizes.reserve(vars.size());
  for (const auto &var : vars) {
    if (mpasContext_) {
      const size_t descriptorLevels = mpasContext_->typedFieldLevels(var);
      sizes.push_back(descriptorLevels);
      continue;
    }
    const auto found = levelsPerVariable_.find(var.name());
    if (found == levelsPerVariable_.end()) {
      throw eckit::BadValue("ijedi::Geometry: no level metadata for variable '" + var.name() + "'",
                            Here());
    }
    sizes.push_back(found->second);
  }
  return sizes;
}

GeometryIterator Geometry::begin() const { return GeometryIterator(*this, 0, 0); }

void Geometry::validateIterationBoundary() const {
  if (isMpas()) mpasAtlasGeometry().validateStorage();
}

const oops::GeometryData &Geometry::geometryData() const {
  validateIterationBoundary();
  return *geomData_;
}

eckit::geometry::Point3 Geometry::iteratorPoint(size_t node, size_t level) const {
  const auto index = ownedNodeIndices_.at(node);
  if (isMpas()) {
    const auto point = mpasAtlasGeometry().authenticatedCellCoordinates(index);
    return eckit::geometry::Point3(point[0], point[1], iteratorVerticalCoord(level));
  }
  const auto coordinates = atlas::array::make_view<double, 2>(functionspace_.lonlat());
  return eckit::geometry::Point3(coordinates(index, 0), coordinates(index, 1),
                                iteratorVerticalCoord(level));
}

const std::shared_ptr<MpasBackendContext> &Geometry::mpasContext() const {
  if (!mpasContext_) {
    throw eckit::BadValue("I-JEDI geometry does not own an MPAS backend context", Here());
  }
  return mpasContext_;
}

int Geometry::closestTask(double latitude, double longitude) const {
  if (!std::isfinite(latitude) || !std::isfinite(longitude)) {
    throw eckit::BadValue("ijedi::Geometry::closestTask received a nonfinite coordinate", Here());
  }
  if (mpasContext_) {
    if (comm_.size() != 1) {
      throw eckit::BadValue(
          "MPAS observation partitioning is supported only for the serial-global release", Here());
    }
    return 0;
  }
  return geomData_->closestTask(latitude, longitude);
}

GeometryIterator Geometry::end() const {
  return GeometryIterator(*this, ownedNodeIndices_.size(), 0);
}

double Geometry::iteratorVerticalCoord(size_t level) const {
  if (iteratorDimension_ == 2) {
    return 0.0;
  }
  return verticalCoord_.at(level);
}
// -----------------------------------------------------------------------------------------------
void Geometry::print(std::ostream &os) const {
  // Write a general message about the geometry and the implementation provider
  os << std::endl
     << "--------------------------------------------------"
        "--------------------------------------------------";
  os << std::endl << "Geometry Information (from type: " + type_ + "):" << std::endl;
  geometryImpl_->print(os);
  os << std::endl
     << "--------------------------------------------------"
        "--------------------------------------------------";
}

// -----------------------------------------------------------------------------------------------

}  // namespace ijedi
