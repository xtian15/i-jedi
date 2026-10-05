#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/shared_ptr.hpp>

#include "eckit/mpi/Comm.h"

#include "atlas/field.h"
#include "atlas/functionspace.h"

#include "oops/util/ObjectCounter.h"
#include "oops/util/Printable.h"
#include "oops/base/GeometryData.h"

#include "ijedi/Geometry/base/GeometryBase.h"
#include "ijedi/Geometry/GeometryIterator.h"

#include "ijedi/FieldMetadata/FieldsMetadata.h"

// Forward declarations
namespace eckit
{
  class Configuration;
}

namespace oops
{
  class Variables;
}

namespace ijedi
{

  class MpasBackendContext;
  class MpasAtlasGeometry;

  // -----------------------------------------------------------------------------
  // Geometry handles geometry.

  class Geometry : public util::Printable, private util::ObjectCounter<Geometry>
  {
   public:
    static const std::string classname() { return "ijedi::Geometry"; }

    Geometry(const eckit::Configuration &, const eckit::mpi::Comm &);
    ~Geometry();

    const eckit::mpi::Comm &comm() const { return comm_; }
    const eckit::mpi::Comm &getComm() const { return comm_; }
    const atlas::FunctionSpace &functionSpace() const { return functionspace_; }
    const atlas::FieldSet &fields() const { return fields_; }
    bool levelsAreTopDown() const { return levelsAreTopDown_; }
    std::vector<size_t> variableSizes(const oops::Variables &) const;
    const oops::GeometryData &geometryData() const;
    eckit::LocalConfiguration modelData() const { return modelData_; }

    GeometryIterator begin() const;
    GeometryIterator end() const;
    std::vector<double> verticalCoord(std::string &) const;

    const std::vector<atlas::idx_t> &ownedNodeIndices() const { return ownedNodeIndices_; }
    size_t iteratorDimension() const { return iteratorDimension_; }
    size_t iteratorLevels() const { return iteratorDimension_ == 3 ? numberLevels_ : 1; }
    double iteratorVerticalCoord(size_t level) const;
    eckit::geometry::Point3 iteratorPoint(size_t node, size_t level) const;
    void validateIterationBoundary() const;
    const std::string &type() const { return type_; }
    bool isMpas() const { return static_cast<bool>(mpasContext_); }
    const std::shared_ptr<MpasBackendContext> &mpasContext() const;
    const MpasAtlasGeometry &mpasAtlasGeometry() const;
    int closestTask(double latitude, double longitude) const;

    const int &numLevels() const { return numberLevels_; }

    // Function to access field metadata
    const FieldsMetadata &getFieldMetadata() const { return *fieldsMeta_; }

    // Add ingredient fields required by some Vader recipes that are not state
    // variables. All sourcing is model-specific, via the per-model hook
    // GeometryBase::addModelVaderIngredients (default no-op).
    void addVaderIngredients(atlas::FieldSet &fset) const
      { geometryImpl_->addModelVaderIngredients(fields(), fset, numberLevels_); }

   private:
    Geometry &operator=(const Geometry &);
    void print(std::ostream &) const;
    std::shared_ptr<FieldsMetadata> fieldsMeta_;
    std::shared_ptr<GeometryBase> geometryImpl_;
    int numberLevels_;
    std::string type_;
    const eckit::mpi::Comm &comm_;
    atlas::FunctionSpace functionspace_;
    atlas::FieldSet fields_;
    bool levelsAreTopDown_ = true;
    std::unordered_map<std::string, size_t> levelsPerVariable_;
    std::unique_ptr<oops::GeometryData> geomData_;
    eckit::LocalConfiguration modelData_;
    size_t iteratorDimension_ = 2;
    std::vector<atlas::idx_t> ownedNodeIndices_;
    std::vector<double> verticalCoord_;
    std::shared_ptr<MpasBackendContext> mpasContext_;
  };
  // -----------------------------------------------------------------------------

}  // namespace ijedi
