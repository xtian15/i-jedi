#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "eckit/config/LocalConfiguration.h"
#include "eckit/mpi/Comm.h"

#include "atlas/field.h"
#include "atlas/functionspace.h"

namespace eckit
{
  class Configuration;
}

namespace ijedi
{

  class MpasBackendContext;

  class GeometryBase
  {
   public:
    virtual ~GeometryBase() = default;

    static std::shared_ptr<GeometryBase> create(const eckit::Configuration &,
                                                const eckit::mpi::Comm &,
                                                eckit::LocalConfiguration &,
                                                atlas::FunctionSpace &,
                                                atlas::FieldSet &,
                                                bool &, int &,
                                                std::shared_ptr<MpasBackendContext> &);
    virtual void print(std::ostream &) const = 0;
    virtual std::vector<double> verticalCoord(std::string &) const = 0;

    /// \brief Hook for models to add Vader ingredient fields that are not state
    ///        variables (e.g. coordinates, masks, area fractions) to \p fset,
    ///        sourced from the already-built geometry fields \p geomFields or
    ///        the helpers below. Default: no-op.
    virtual void addModelVaderIngredients(const atlas::FieldSet & geomFields,
                                          atlas::FieldSet & fset, int nlevels) const {}

   protected:
    /// \brief Add single-level "longitude"/"latitude" ingredient fields to
    ///        \p fset, sourced from its own function space, so any model can
    ///        opt in regardless of how its geometry names coordinate fields.
    ///        Fields already present are kept.
    static void addLonLatIngredients(atlas::FieldSet & fset);

    /// \brief Add geometry field \p geomName to \p fset under \p ingredientName,
    ///        broadcasting the single-level source across \p nlevels (Vader
    ///        recipes index ingredient fields per level). No-op if the ingredient
    ///        is already present or the source field is missing.
    static void addBroadcastIngredient(const atlas::FieldSet & geomFields,
                                       const std::string & geomName,
                                       const std::string & ingredientName,
                                       int nlevels, atlas::FieldSet & fset);
  };

}  // namespace ijedi
