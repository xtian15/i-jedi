#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "eckit/mpi/Comm.h"

#include "atlas/grid.h"

#include "ijedi/Geometry/base/GeometryBase.h"

namespace eckit
{
  class Configuration;
}

namespace ijedi
{

  class MpasBackendContext;
  class MpasAtlasGeometry;

  class GeometryMPAS : public GeometryBase
  {
   public:
    GeometryMPAS(const eckit::Configuration &, const eckit::mpi::Comm &,
                 eckit::LocalConfiguration &,
                 atlas::FunctionSpace &, atlas::FieldSet &, bool &, int &,
                 std::shared_ptr<MpasBackendContext> &);
    void print(std::ostream &) const override;
    std::vector<double> verticalCoord(std::string &) const override;
    const MpasAtlasGeometry &atlasGeometry() const { return *atlasGeometry_; }

   private:
    std::string geometryReceipt_;
    int cellCount_ = 0;
    int levelCount_ = 0;
    std::shared_ptr<MpasAtlasGeometry> atlasGeometry_;
  };

}  // namespace ijedi
