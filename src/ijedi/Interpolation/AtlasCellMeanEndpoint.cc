/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Interpolation/AtlasCellMeanEndpoint.h"

#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>

#include "atlas/array.h"
#include "atlas/functionspace/CellColumns.h"
#include "atlas/mesh/HybridElements.h"
#include "atlas/mesh/Nodes.h"
#include "atlas/meshgenerator.h"
#include "atlas/parallel/mpi/mpi.h"
#include "atlas/util/ConvexSphericalPolygon.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/Interpolation/AtlasMeshIntegrity.h"

namespace ijedi {
AtlasCellMeanEndpoint::AtlasCellMeanEndpoint(atlas::FunctionSpace space,
                                             std::vector<double> measures, std::string receipt,
                                             std::string content,
                                             std::shared_ptr<const AtlasMeshIntegrity> integrity)
    : space_(std::move(space)),
      measures_(std::move(measures)),
      receipt_(std::move(receipt)),
      contentReceipt_(std::move(content)),
      integrity_(std::move(integrity)) {}

AtlasCellMeanEndpoint AtlasCellMeanEndpoint::fromMpas(const MpasAtlasGeometry &geometry) {
  geometry.validateStorage();
  auto measures = geometry.cellMeanMeasures();
  const double radiusSquared = geometry.sphereRadiusMetres() * geometry.sphereRadiusMetres();
  long double total = 0;
  for (double &area : measures) {
    area /= radiusSquared;
    if (!std::isfinite(area) || area <= 0) {
      throw std::invalid_argument("native MPAS declared area has invalid unit-sphere conversion");
    }
    total += area;
  }
  if (std::abs(total / (4.L * std::acos(-1.L)) - 1.L) > 2.e-14L) {
    throw std::invalid_argument(
        "native MPAS declared areas do not cover the sphere at their owned radius");
  }
  return {geometry.cellMeans(), std::move(measures), geometry.receipt(),
          geometry.atlasInputReceipt(), geometry.storageIntegrity()};
}

AtlasCellMeanEndpoint AtlasCellMeanEndpoint::fromAtlasGrid(const atlas::Grid &grid) {
  if (atlas::mpi::comm().size() != 1 || !grid || !grid.domain().global()) {
    throw std::invalid_argument("Atlas cell means require a serial global analysis grid");
  }
  // Use the same native mesh generator and spherical polygon primitive as
  // Atlas conservative interpolation. Do not normalize areas or repair weights.
  const atlas::Mesh mesh = atlas::MeshGenerator(grid.meshgenerator()).generate(grid);
  const atlas::FunctionSpace space = atlas::functionspace::CellColumns(mesh);
  if (space.size() != mesh.cells().size() || space.size() <= 0) {
    throw std::invalid_argument("Atlas analysis cell ordering/coverage is unsupported");
  }
  const auto coordinates = atlas::array::make_view<double, 2>(mesh.nodes().lonlat());
  const auto nodeIds = atlas::array::make_view<atlas::gidx_t, 1>(mesh.nodes().global_index());
  const auto cellIds = atlas::array::make_view<atlas::gidx_t, 1>(mesh.cells().global_index());
  const auto &connectivity = mesh.cells().node_connectivity();
  AtlasOperatorReceipt hash;
  hash.string("atlas-global-cell-means-unit-sphere-v1");
  hash.string(grid.uid());
  hash.integer(mesh.nodes().size());
  hash.integer(mesh.cells().size());
  for (atlas::idx_t i = 0; i < mesh.nodes().size(); ++i) {
    hash.integer(nodeIds(i));
    for (size_t k = 0; k < 2; ++k) {
      if (!std::isfinite(coordinates(i, k))) {
        throw std::invalid_argument("Atlas analysis mesh has nonfinite coordinates");
      }
      hash.real(coordinates(i, k));
    }
  }
  std::set<atlas::gidx_t> ids;
  std::vector<double> measures;
  measures.reserve(space.size());
  long double total = 0;
  for (atlas::idx_t i = 0; i < mesh.cells().size(); ++i) {
    if (cellIds(i) <= 0 || !ids.insert(cellIds(i)).second || connectivity.cols(i) < 3 ||
        connectivity.cols(i) > atlas::util::ConvexSphericalPolygon::MAX_GRIDCELL_EDGES) {
      throw std::invalid_argument("Atlas analysis cell ID or polygon extent is invalid");
    }
    hash.integer(cellIds(i));
    hash.integer(connectivity.cols(i));
    std::vector<atlas::PointLonLat> points;
    for (atlas::idx_t j = 0; j < connectivity.cols(i); ++j) {
      const auto node = connectivity(i, j);
      if (node < 0 || node >= mesh.nodes().size()) {
        throw std::invalid_argument("Atlas analysis cell references an invalid node");
      }
      hash.integer(node);
      points.emplace_back(coordinates(node, 0), coordinates(node, 1));
    }
    const atlas::util::ConvexSphericalPolygon polygon(points);
    const double area = polygon.area();
    if (!polygon || !std::isfinite(area) || area <= 0) {
      throw std::invalid_argument("Atlas analysis cell has invalid winding or area");
    }
    measures.push_back(area);
    hash.real(area);
    total += area;
  }
  const long double sphere = 4.L * std::acos(-1.L);
  if (std::abs(total / sphere - 1.L) > 2.e-14L) {
    throw std::invalid_argument("Atlas analysis grid does not cover the global sphere");
  }
  const std::string receipt = hash.finish();
  return {space, std::move(measures), receipt, receipt,
          std::make_shared<const AtlasMeshIntegrity>(std::vector<atlas::Mesh>{mesh},
                                                     std::vector<atlas::FunctionSpace>{space})};
}
}  // namespace ijedi
