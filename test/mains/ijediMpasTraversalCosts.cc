/* (C) Copyright 2026 IC Weather LLC. CC0-1.0. */
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "atlas/array.h"
#include "atlas/library/Library.h"
#include "eckit/config/LocalConfiguration.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "oops/runs/Application.h"
#include "oops/runs/Run.h"
#include "oops/mpi/mpi.h"
namespace {
using Json=nlohmann::json;
using Clock=std::chrono::steady_clock;
double median(std::vector<double> values) {
  std::sort(values.begin(),values.end()); return values.at(values.size()/2);
}
class PublicTraversal final : public oops::Application {
 public:
  using oops::Application::Application;
  int execute(const eckit::Configuration &config) const override {
    const ijedi::Geometry geometry(config.getSubConfiguration("geometry"),getComm());
    if (!geometry.isMpas() || geometry.ownedNodeIndices().size()!=10242 ||
        geometry.iteratorDimension()!=2) throw std::runtime_error("requires retained serial-global 10242-cell 2D Geometry");
    const bool forced=config.getBool("forced scan",false);
    const auto field=geometry.mpasAtlasGeometry().cellNodes().lonlat();
    const auto coordinates=atlas::array::make_view<double,2>(field);
    ijedi::AtlasOperatorReceipt expected;
    for (const auto row : geometry.ownedNodeIndices()) {
      expected.real(coordinates(row,0)); expected.real(coordinates(row,1));
    }
    const auto digest=expected.finish();
    std::vector<double> guard, traversal;
    for (int repeat=0; repeat<5; ++repeat) {
      auto start=Clock::now(); geometry.validateIterationBoundary();
      guard.push_back(std::chrono::duration<double>(Clock::now()-start).count());
      ijedi::AtlasOperatorReceipt actual; size_t count=0;
      start=Clock::now();
      const auto finish=geometry.end();
      for (auto point=geometry.begin(); point!=finish; ++point) {
        if (forced) geometry.validateIterationBoundary();
        const auto value=*point;
        const auto row=geometry.ownedNodeIndices().at(count);
        if (value[0]!=coordinates(row,0) || value[1]!=coordinates(row,1) || value[2]!=0.) {
          throw std::runtime_error("public traversal coordinate differs from authenticated Atlas point");
        }
        actual.real(value[0]); actual.real(value[1]); ++count;
      }
      traversal.push_back(std::chrono::duration<double>(Clock::now()-start).count());
      if (count!=10242 || actual.finish()!=digest) throw std::runtime_error("public complete traversal changed count/coordinate bytes");
    }
    const double ratio=median(traversal)/median(guard);
    std::ifstream status("/proc/self/status");
    std::string line, affinity;
    while (std::getline(status,line)) {
      if (line.find("Cpus_allowed_list:")==0) affinity=line;
    }
    Json result{{"scope","actual complete public GeometryIterator; construction excluded"},
      {"cells",10242},{"points",10242},{"threads",1},{"forced_scan",forced},
      {"mpi_ranks",getComm().size()},{"actual_process_affinity",affinity},
      {"atlas_commit",atlas::Library::instance().gitsha1(40)},
      {"guard_seconds",guard},{"traversal_seconds",traversal},
      {"coordinate_receipt",digest},{"complete_to_single_guard_ratio",ratio},
      {"maximum_ratio",256}};
    std::ofstream(config.getString("probe output")) << result.dump(2) << std::endl;
    if (!(ratio<256.)) throw std::runtime_error("public traversal cost indicates per-point full-mesh scans");
    return 0;
  }
 private:
  std::string appname() const override { return "ijedi::PublicTraversalTest"; }
};
}
int main(int argc,char **argv) {
  oops::Run run(argc,argv);
  const PublicTraversal app(oops::mpi::world());
  return run.execute(app);
}
