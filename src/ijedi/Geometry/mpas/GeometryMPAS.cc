#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "atlas/array.h"
#include "atlas/field.h"
#include "atlas/option.h"
#include "atlas/library/Library.h"
#include "eckit/config/Configuration.h"
#include "eckit/config/LocalConfiguration.h"
#include "eckit/exception/Exceptions.h"
#include "eckit/thread/AutoLock.h"
#include "eckit/thread/StaticMutex.h"

#include "oops/util/abor1_cpp.h"
#include "oops/util/Logger.h"

#include "ijedi/Geometry/mpas/GeometryMPAS.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Python/MpasBackendContext.h"

namespace ijedi {
namespace {

eckit::StaticMutex contextCacheMutex;
std::unordered_map<std::string, std::weak_ptr<MpasBackendContext>> contextCache;

std::string contextCacheKey(const eckit::Configuration &config) {
  // Every input consumed by MpasBackendContext belongs in the cache identity.
  // Length-prefixing prevents ambiguous concatenations without depending on
  // Configuration's presentation order.
  std::ostringstream key;
  const auto add = [&key](const std::string &name, const std::string &value) {
    key << name.size() << ':' << name << value.size() << ':' << value;
  };
  for (const std::string &name :
       {"python executable", "runtime receipt path", "wheel path", "wheel sha256", "init path",
        "grid path", "namelist path", "horizontal geometry receipt",
        "static vertical geometry receipt", "geometry bundle receipt", "configuration receipt",
        "state schema digest", "python version", "torch version", "numpy version",
        "netcdf4 version", "mpas-pytorch version", "mpas-pytorch source commit",
        "vertical coordinate source"}) {
    add(name, config.getString(name));
  }
  add("torch intraop threads", std::to_string(config.getInt("torch intraop threads")));
  add("torch interop threads", std::to_string(config.getInt("torch interop threads")));
  return key.str();
}

std::shared_ptr<MpasBackendContext> acquireContext(const eckit::Configuration &config) {
  const std::string key = contextCacheKey(config);
  eckit::AutoLock<eckit::StaticMutex> guard(contextCacheMutex);
  auto found = contextCache.find(key);
  if (found != contextCache.end()) {
    if (auto context = found->second.lock()) {
      return context;
    }
    contextCache.erase(found);
  }
  auto context = std::make_shared<MpasBackendContext>(config);
  contextCache.emplace(key, context);
  return context;
}

}  // namespace

// -----------------------------------------------------------------------------------------------

GeometryMPAS::GeometryMPAS(const eckit::Configuration &geomConfig, const eckit::mpi::Comm &comm,
                           eckit::LocalConfiguration &geomVariables,
                           atlas::FunctionSpace &functionSpace, atlas::FieldSet &fieldSet,
                           bool &levelsAreTopDown, int &numberLevels,
                           std::shared_ptr<MpasBackendContext> &context) {
  if (comm.size() != 1) {
    throw eckit::BadParameter("The pinned MPAS-PyTorch release supports exactly one MPI rank",
                              Here());
  }
  const std::string compiler = geomConfig.getString("atlas compiler identity");
  if (compiler.size() != 40 || atlas::Library::instance().gitsha1(40) != compiler) {
    throw eckit::BadParameter("MPAS geometry Atlas compiler does not match the loaded library",
                              Here());
  }
  context = acquireContext(geomConfig);
  const auto &lon = context->cellLongitudesDegrees();
  const auto &lat = context->cellLatitudesDegrees();
  const auto &area = context->cellAreas();
  const auto &globalIds = context->cellGlobalIds();
  if (lon.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw eckit::BadValue("MPAS cell count exceeds the supported Atlas index range", Here());
  }
  cellCount_ = static_cast<int>(lon.size());
  levelCount_ = context->numberLevels();
  numberLevels = levelCount_;
  levelsAreTopDown = false;
  geometryReceipt_ = context->geometryReceipt();

  atlasGeometry_ = std::make_shared<MpasAtlasGeometry>(context->horizontalSnapshot());
  functionSpace = atlasGeometry_->cellNodes();

  atlas::Field areaField =
      functionSpace.createField<double>(atlas::option::name("area") | atlas::option::levels(1));
  auto areaView = atlas::array::make_view<double, 2>(areaField);
  atlas::Field idField = functionSpace.createField<atlas::gidx_t>(
      atlas::option::name("global_index") | atlas::option::levels(1));
  auto idView = atlas::array::make_view<atlas::gidx_t, 2>(idField);
  atlas::Field ownedField = functionSpace.createField<int>(atlas::option::name("owned"));
  auto ownedView = atlas::array::make_view<int, 1>(ownedField);
  for (int cell = 0; cell < cellCount_; ++cell) {
    areaView(cell, 0) = area[cell];
    idView(cell, 0) = static_cast<atlas::gidx_t>(globalIds[cell]);
    ownedView(cell) = 1;
  }
  fieldSet.add(areaField);
  fieldSet.add(idField);
  fieldSet.add(ownedField);
  const auto verticalFields =
      atlasGeometry_->staticVerticalFields(context->staticVerticalSnapshot());
  for (const auto &field : verticalFields) {
    fieldSet.add(field);
  }

  geomVariables.set("geometry_receipt", geometryReceipt_);
  geomVariables.set("configuration_receipt", context->configurationReceipt());
  geomVariables.set("atlas_compiler_identity", compiler);
  geomVariables.set("nCells", cellCount_);
  geomVariables.set("nVertLevels", levelCount_);
}

// -----------------------------------------------------------------------------------------------

void GeometryMPAS::print(std::ostream &os) const {
  os << "MPAS-PyTorch canonical cell geometry: " << cellCount_ << " cells, " << levelCount_
     << " levels, receipt=" << geometryReceipt_;
}

// -----------------------------------------------------------------------------------------------

std::vector<double> GeometryMPAS::verticalCoord(std::string &vcUnits) const {
  if (!vcUnits.empty() && vcUnits != "model_level_index_bottom_to_top") {
    throw eckit::BadValue(
        "MPAS Geometry verticalCoord supports only model_level_index_bottom_to_top; "
        "pressure and height are column-dependent typed fields", Here());
  }
  vcUnits = "model_level_index_bottom_to_top";
  std::vector<double> levels(levelCount_);
  std::iota(levels.begin(), levels.end(), 0.0);
  return levels;
}

// -----------------------------------------------------------------------------------------------

}  // namespace ijedi
