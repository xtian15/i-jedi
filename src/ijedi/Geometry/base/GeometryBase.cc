#include <algorithm>
#include <memory>
#include <string>

#include "eckit/config/Configuration.h"
#include "eckit/config/LocalConfiguration.h"
#include "eckit/exception/Exceptions.h"

#include "atlas/array.h"
#include "atlas/option.h"

#include "ijedi/Geometry/atlas/GeometryAtlas.h"
#include "ijedi/Geometry/base/GeometryBase.h"
#include "ijedi/Geometry/fv3/GeometryFV3.h"
#include "ijedi/Geometry/gsibec/GeometryGsibec.h"
#include "ijedi/Geometry/mpas/GeometryMPAS.h"
#include "ijedi/Geometry/mom6/GeometryMOM6.h"

namespace ijedi
{
  std::shared_ptr<GeometryBase> GeometryBase::create(const eckit::Configuration &geomConf,
                                                     const eckit::mpi::Comm &comm,
                                                     eckit::LocalConfiguration &geomVars,
                                                     atlas::FunctionSpace &functionSpace,
                                                     atlas::FieldSet &fieldSet,
                                                     bool &levelsAreTopDown, int &numLevels,
                                                     std::shared_ptr<MpasBackendContext>
                                                         &mpasContext)
  {
    // Get the type
    std::string type;
    type = geomConf.getString("geometry_type");

    if (type == "fv3")
    {
      return std::make_shared<GeometryFV3>(geomConf, comm, geomVars, functionSpace, fieldSet,
                                           levelsAreTopDown, numLevels);
    }
    if (type == "mpas")
    {
      return std::make_shared<GeometryMPAS>(geomConf, comm, geomVars, functionSpace, fieldSet,
                                            levelsAreTopDown, numLevels, mpasContext);
    }
    if (type == "mom6")
    {
      return std::make_shared<GeometryMOM6>(geomConf, comm, geomVars, functionSpace, fieldSet,
                                            levelsAreTopDown, numLevels);
    }
    if (type == "atlas")
    {
      return std::make_shared<GeometryAtlas>(geomConf, comm, geomVars, functionSpace, fieldSet,
                                             levelsAreTopDown, numLevels);
    }
    if (type == "gsibec")
    {
      return std::make_shared<GeometryGsibec>(geomConf, comm, geomVars, functionSpace, fieldSet,
                                              levelsAreTopDown, numLevels);
    }

    throw eckit::BadValue("Unsupported geometry type: " + type,
                          Here());
  }

  void GeometryBase::addLonLatIngredients(atlas::FieldSet &fset)
  {
    // Source from the fieldset's own function space, so this works regardless
    // of how the model geometry names its coordinate fields.
    const atlas::FunctionSpace &fs = fset.field(0).functionspace();
    const auto lonlat = atlas::array::make_view<double, 2>(fs.lonlat());
    atlas::Field lonF = fs.createField<double>(
        atlas::option::name("longitude") | atlas::option::levels(1));
    atlas::Field latF = fs.createField<double>(
        atlas::option::name("latitude") | atlas::option::levels(1));
    auto lonView = atlas::array::make_view<double, 2>(lonF);
    auto latView = atlas::array::make_view<double, 2>(latF);
    for (atlas::idx_t n = 0; n < lonF.shape(0); ++n)
    {
      lonView(n, 0) = lonlat(n, 0);
      latView(n, 0) = lonlat(n, 1);
    }
    if (!fset.has("longitude")) fset.add(lonF);
    if (!fset.has("latitude")) fset.add(latF);
  }

  void GeometryBase::addBroadcastIngredient(const atlas::FieldSet &geomFields,
                                            const std::string &geomName,
                                            const std::string &ingredientName,
                                            int nlevels, atlas::FieldSet &fset)
  {
    if (fset.has(ingredientName) || !geomFields.has(geomName)) return;
    const atlas::Field &src = geomFields.field(geomName);
    atlas::Field dst = src.functionspace().createField<double>(
        atlas::option::name(ingredientName) | atlas::option::levels(nlevels));
    const auto srcView = atlas::array::make_view<double, 2>(src);
    auto dstView = atlas::array::make_view<double, 2>(dst);
    for (atlas::idx_t n = 0; n < dst.shape(0); ++n)
    {
      for (int k = 0; k < nlevels; ++k)
      {
        dstView(n, k) = srcView(n, 0);
      }
    }
    fset.add(dst);
  }

}  // namespace ijedi
