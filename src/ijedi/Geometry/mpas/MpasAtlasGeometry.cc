/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>

#include "atlas/array.h"
#include "atlas/functionspace/CellColumns.h"
#include "atlas/functionspace/EdgeColumns.h"
#include "atlas/functionspace/NodeColumns.h"
#include "atlas/mesh/ElementType.h"
#include "atlas/mesh/HybridElements.h"
#include "atlas/mesh/Nodes.h"
#include "atlas/option.h"
#include "eckit/thread/AutoLock.h"
#include "ijedi/Interpolation/AtlasMeshIntegrity.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "ijedi/Interpolation/MpasAtlasPointOperator.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"

namespace ijedi {
namespace {

template <typename T>
const std::vector<T> &require(const std::map<std::string, std::vector<T>> &fields,
                              const std::string &name, size_t size) {
  const auto found = fields.find(name);
  if (found == fields.end() || found->second.size() != size) {
    throw std::invalid_argument("MPAS Atlas snapshot extent mismatch: " + name);
  }
  return found->second;
}

void nodes(atlas::Mesh &mesh, const MpasHorizontalSnapshot &snapshot, const std::string &suffix,
           size_t count) {
  const auto &lon = require(snapshot.reals, "lon" + suffix, count);
  const auto &lat = require(snapshot.reals, "lat" + suffix, count);
  const auto &ids = require(snapshot.integers, "indexTo" + suffix + "ID", count);
  if (std::set<std::int64_t>(ids.begin(), ids.end()).size() != count) {
    throw std::invalid_argument("MPAS Atlas snapshot has duplicate global IDs");
  }
  mesh.nodes().resize(count);
  auto ll = atlas::array::make_view<double, 2>(mesh.nodes().lonlat());
  auto xy = atlas::array::make_view<double, 2>(mesh.nodes().xy());
  auto global = atlas::array::make_view<atlas::gidx_t, 1>(mesh.nodes().global_index());
  auto partition = atlas::array::make_view<int, 1>(mesh.nodes().partition());
  auto ghost = atlas::array::make_view<int, 1>(mesh.nodes().ghost());
  auto halo = atlas::array::make_view<int, 1>(mesh.nodes().halo());
  constexpr double degrees = 180.0 / 3.141592653589793238462643383279502884;
  for (size_t i = 0; i < count; ++i) {
    if (!std::isfinite(lon[i]) || !std::isfinite(lat[i]) ||
        std::abs(lat[i]) > 3.141592653589793238462643383279502884 / 2 || ids[i] <= 0) {
      throw std::invalid_argument("MPAS Atlas snapshot coordinate/global ID is invalid");
    }
    ll(i, 0) = lon[i] * degrees;
    ll(i, 1) = lat[i] * degrees;
    global(i) = ids[i];
    partition(i) = ghost(i) = halo(i) = 0;
  }
  xy.assign(ll);
  mesh.metadata().set("halo", 0);
  mesh.metadata().set("mpas_horizontal_receipt", snapshot.receipt);
}

atlas::idx_t checkedIndex(std::int64_t value, size_t size) {
  if (value < 0 || static_cast<size_t>(value) >= size) {
    throw std::invalid_argument("MPAS Atlas snapshot connectivity outside extent");
  }
  return static_cast<atlas::idx_t>(value);
}

std::array<double, 3> radial(const MpasHorizontalSnapshot &s, const std::string &suffix, size_t i) {
  const double lon = s.reals.at("lon" + suffix)[i], lat = s.reals.at("lat" + suffix)[i];
  return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

double orientedVolume(const std::array<double, 3> &a, const std::array<double, 3> &b,
                      const std::array<double, 3> &centre) {
  return (a[1] * b[2] - a[2] * b[1]) * centre[0] + (a[2] * b[0] - a[0] * b[2]) * centre[1] +
         (a[0] * b[1] - a[1] * b[0]) * centre[2];
}

}  // namespace

MpasAtlasGeometry::MpasAtlasGeometry(const MpasHorizontalSnapshot &s)
    : sphereRadiusMetres_(s.sphereRadiusMetres), receipt_(s.receipt) {
  const auto maximum = static_cast<size_t>(std::numeric_limits<atlas::idx_t>::max());
  if (s.receipt.size() != 64 ||
      std::any_of(s.receipt.begin(), s.receipt.end(),
                  [](char c) { return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')); }) ||
      !s.cells || !s.edges || !s.vertices || s.cells > maximum || s.edges > maximum ||
      s.vertices > maximum || s.cellWidth < 6 || s.cellWidth > 64) {
    throw std::invalid_argument("MPAS Atlas requires a bounded authenticated global snapshot");
  }
  if (s.vertices + s.cells != s.edges + 2) {
    throw std::invalid_argument("MPAS Atlas snapshot is not a closed spherical topology");
  }
  if (!std::isfinite(s.sphereRadiusMetres) || s.sphereRadiusMetres <= 0 ||
      !std::isfinite(s.sphereRadiusMetres * s.sphereRadiusMetres) ||
      s.sphereRadiusMetres * s.sphereRadiusMetres == 0 || s.angleUnits != "rad" ||
      s.lengthUnits != "m" || s.areaUnits != "m^2") {
    throw std::invalid_argument(
        "MPAS Atlas requires declared radian/metre/square-metre geometry and a finite radius");
  }
  const auto &counts = require(s.integers, "nEdgesOnCell", s.cells);
  const auto &vertices = require(s.integers, "verticesOnCell", s.cells * s.cellWidth);
  const auto &triangles = require(s.integers, "cellsOnVertex", s.vertices * 3);
  const auto &cellIds = require(s.integers, "indexToCellID", s.cells);
  const auto &vertexIds = require(s.integers, "indexToVertexID", s.vertices);
  const auto &edgeIds = require(s.integers, "indexToEdgeID", s.edges);
  const auto &edgeVertices = require(s.integers, "verticesOnEdge", s.edges * 2);
  const auto &edgeCells = require(s.integers, "cellsOnEdge", s.edges * 2);
  const auto &areas = require(s.reals, "areaCell", s.cells);
  const auto &edgeLon = require(s.reals, "lonEdge", s.edges);
  const auto &edgeLat = require(s.reals, "latEdge", s.edges);
  const auto &normals = require(s.reals, "edgeNormalVectors", s.edges * 3);
  if (std::set<std::int64_t>(edgeIds.begin(), edgeIds.end()).size() != s.edges ||
      std::any_of(edgeIds.begin(), edgeIds.end(), [](auto id) { return id <= 0; })) {
    throw std::invalid_argument("MPAS Atlas snapshot edge IDs are invalid");
  }
  nodes(primal_, s, "Vertex", s.vertices);
  nodes(dual_, s, "Cell", s.cells);
  nativeToCells_.resize(s.cells);
  for (const int width : {5, 6}) {
    const size_t count = std::count(counts.begin(), counts.end(), width);
    if (!count) {
      continue;
    }
    const auto block = primal_.cells().add(
        atlas::mesh::ElementType::create(width == 5 ? "Pentagon" : "Hexagon"), count);
    size_t row = 0;
    for (size_t c = 0; c < s.cells; ++c) {
      if (counts[c] != width) {
        continue;
      }
      std::vector<atlas::idx_t> face(width);
      for (int j = 0; j < width; ++j) {
        face[j] = checkedIndex(vertices[c * s.cellWidth + j], s.vertices);
      }
      if (std::set<atlas::idx_t>(face.begin(), face.end()).size() != face.size()) {
        throw std::invalid_argument("MPAS Atlas snapshot has duplicate face vertices");
      }
      const auto centre = radial(s, "Cell", c);
      double winding = 0;
      for (int j = 0; j < width; ++j) {
        winding += orientedVolume(radial(s, "Vertex", face[j]),
                                  radial(s, "Vertex", face[(j + 1) % width]), centre);
      }
      if (!std::isfinite(winding) || winding <= 0) {
        throw std::invalid_argument("MPAS Atlas primal face has reversed or degenerate winding");
      }
      for (size_t j = width; j < s.cellWidth; ++j) {
        if (vertices[c * s.cellWidth + j] != -1) {
          throw std::invalid_argument("MPAS Atlas snapshot inactive connectivity is not -1");
        }
      }
      if (!std::isfinite(areas[c]) || areas[c] <= 0) {
        throw std::invalid_argument("MPAS Atlas snapshot has invalid declared area");
      }
      primal_.cells().node_connectivity().block(block).set(row++, face.data());
      nativeToCells_[c] = cellsToNative_.size();
      cellsToNative_.push_back(c);
      cellMeanMeasures_.push_back(areas[c]);
    }
  }
  if (cellsToNative_.size() != s.cells || std::count(counts.begin(), counts.end(), 5) != 12) {
    throw std::invalid_argument("MPAS Atlas requires twelve pentagons and otherwise hexagons");
  }
  cellNodeMeasures_ = areas;
  AtlasOperatorReceipt measureReceipt;
  measureReceipt.string("mpas-native-cell-area-per-index-level-v1");
  measureReceipt.string(receipt_);
  measureReceipt.integer(s.cells);
  for (double measure : cellNodeMeasures_) {
    measureReceipt.real(measure);
  }
  cellNodeMeasureReceipt_ = measureReceipt.finish();
  auto primalIds = atlas::array::make_view<atlas::gidx_t, 1>(primal_.cells().global_index());
  for (size_t c = 0; c < s.cells; ++c) {
    primalIds(c) = cellIds[cellsToNative_[c]];
  }
  const auto block = dual_.cells().add(atlas::mesh::ElementType::create("Triangle"), s.vertices);
  auto dualIds = atlas::array::make_view<atlas::gidx_t, 1>(dual_.cells().global_index());
  for (size_t v = 0; v < s.vertices; ++v) {
    atlas::idx_t triangle[3];
    for (size_t j = 0; j < 3; ++j) {
      triangle[j] = checkedIndex(triangles[3 * v + j], s.cells);
    }
    if (triangle[0] == triangle[1] || triangle[1] == triangle[2] || triangle[0] == triangle[2]) {
      throw std::invalid_argument("MPAS Atlas snapshot has a degenerate dual triangle");
    }
    // Canonical MPAS cellsOnVertex legitimately has both cyclic orientations
    // (10,240 of each on x1.10242). Preserve its order; reject degeneracy, not
    // an invented all-positive orientation requirement.
    const double determinant =
        orientedVolume(radial(s, "Cell", triangle[0]), radial(s, "Cell", triangle[1]),
                       radial(s, "Cell", triangle[2]));
    if (!std::isfinite(determinant) || determinant == 0) {
      throw std::invalid_argument("MPAS Atlas dual face is geometrically degenerate");
    }
    dual_.cells().node_connectivity().block(block).set(v, triangle);
    dualIds(v) = vertexIds[v];
  }
  // EdgeColumns invokes Atlas' generic mixed-element edge builder. Its order
  // is never assumed to match MPAS; match by unordered native endpoint IDs.
  edgeNormals_ = atlas::functionspace::EdgeColumns(primal_, atlas::option::halo(0));
  if (primal_.edges().size() != static_cast<atlas::idx_t>(s.edges)) {
    throw std::invalid_argument("Atlas-built primal edge count differs from MPAS");
  }
  using Pair = std::pair<atlas::idx_t, atlas::idx_t>;
  std::map<Pair, size_t> nativeEdges;
  for (size_t e = 0; e < s.edges; ++e) {
    const auto a = checkedIndex(edgeVertices[2 * e], s.vertices);
    const auto b = checkedIndex(edgeVertices[2 * e + 1], s.vertices);
    if (a == b || !nativeEdges.emplace(std::minmax(a, b), e).second) {
      throw std::invalid_argument("MPAS Atlas snapshot has duplicate/degenerate edges");
    }
  }
  nativeToEdges_.resize(s.edges);
  edgesToNative_.resize(s.edges);
  edgeCellOrientation_.resize(s.edges);
  auto atlasEdgeIds = atlas::array::make_view<atlas::gidx_t, 1>(primal_.edges().global_index());
  atlas::Field edgeCoordinates("lonlat", atlas::array::make_datatype<double>(),
                               atlas::array::make_shape(s.edges, 2));
  atlas::Field edgeNormalField("mpas_canonical_edge_normal", atlas::array::make_datatype<double>(),
                               atlas::array::make_shape(s.edges, 3));
  edgeNormalField.metadata().set("component_basis", "canonical_cartesian_xyz_cells_on_edge_0_to_1");
  edgeNormalField.metadata().set("horizontal_geometry_receipt", receipt_);
  auto ell = atlas::array::make_view<double, 2>(edgeCoordinates);
  auto env = atlas::array::make_view<double, 2>(edgeNormalField);
  const auto &atlasVertices = primal_.edges().node_connectivity();
  const auto &atlasCells = primal_.edges().cell_connectivity();
  for (size_t a = 0; a < s.edges; ++a) {
    const auto found = nativeEdges.find(std::minmax(atlasVertices(a, 0), atlasVertices(a, 1)));
    if (found == nativeEdges.end()) {
      throw std::invalid_argument("Atlas edge is absent from MPAS");
    }
    const size_t e = found->second;
    const auto first = checkedIndex(edgeCells[2 * e], s.cells);
    const auto second = checkedIndex(edgeCells[2 * e + 1], s.cells);
    const auto left = checkedIndex(atlasCells(a, 0), s.cells);
    const auto right = checkedIndex(atlasCells(a, 1), s.cells);
    if (first == second ||
        std::minmax(cellsToNative_[left], cellsToNative_[right]) != std::minmax(first, second)) {
      throw std::invalid_argument("Atlas edge adjacency differs from MPAS");
    }
    nativeToEdges_[e] = a;
    edgesToNative_[a] = e;
    edgeCellOrientation_[a] = cellsToNative_[left] == first ? 1 : -1;
    atlasEdgeIds(a) = edgeIds[e];
    constexpr double degrees = 180.0 / 3.141592653589793238462643383279502884;
    if (!std::isfinite(edgeLon[e]) || !std::isfinite(edgeLat[e]) ||
        std::abs(edgeLat[e]) > 3.141592653589793238462643383279502884 / 2) {
      throw std::invalid_argument("MPAS Atlas edge coordinate is invalid");
    }
    ell(a, 0) = edgeLon[e] * degrees;
    ell(a, 1) = edgeLat[e] * degrees;
    double norm2 = 0, orientation = 0;
    const auto radial = [&](size_t cell) {
      const double lon = s.reals.at("lonCell")[cell], lat = s.reals.at("latCell")[cell];
      return std::array<double, 3>{std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon),
                                   std::sin(lat)};
    };
    const auto r0 = radial(first), r1 = radial(second);
    for (size_t k = 0; k < 3; ++k) {
      const double normal = normals[3 * e + k];
      if (!std::isfinite(normal)) {
        throw std::invalid_argument("MPAS Atlas edge normal is nonfinite");
      }
      env(a, k) = normal;
      norm2 += normal * normal;
      orientation += normal * (r1[k] - r0[k]);
    }
    if (!std::isfinite(norm2) || std::abs(norm2 - 1) > 2.e-14 || orientation <= 0) {
      throw std::invalid_argument(
          "MPAS Atlas edge normal has invalid length or native orientation");
    }
  }
  if (primal_.edges().has_field("lonlat")) {
    primal_.edges().remove_field("lonlat");
  }
  primal_.edges().add(edgeCoordinates);
  primal_.edges().add(edgeNormalField);
  atlas::Field cellCoordinates("lonlat", atlas::array::make_datatype<double>(),
                               atlas::array::make_shape(s.cells, 2));
  auto cll = atlas::array::make_view<double, 2>(cellCoordinates);
  const auto nll = atlas::array::make_view<double, 2>(dual_.nodes().lonlat());
  for (size_t a = 0; a < s.cells; ++a) {
    for (size_t k = 0; k < 2; ++k) {
      cll(a, k) = nll(cellsToNative_[a], k);
    }
  }
  primal_.cells().add(cellCoordinates);
  cellNodes_ = atlas::functionspace::NodeColumns(dual_, atlas::option::halo(0));
  cellMeans_ = atlas::functionspace::CellColumns(primal_, atlas::option::halo(0));
  vertexNodes_ = atlas::functionspace::NodeColumns(primal_, atlas::option::halo(0));
  AtlasOperatorReceipt content;
  content.string("mpas-atlas-native-geometry-inputs-radians-int64-float64-v2");
  content.string(receipt_);
  content.real(s.sphereRadiusMetres);
  content.string(s.angleUnits);
  content.string(s.lengthUnits);
  content.string(s.areaUnits);
  for (size_t count : {s.cells, s.edges, s.vertices, s.cellWidth}) {
    content.integer(count);
  }
  content.integer(s.integers.size());
  for (const auto &[name, values] : s.integers) {
    content.string(name);
    content.integer(values.size());
    for (auto value : values) {
      content.integer(static_cast<std::uint64_t>(value));
    }
  }
  content.integer(s.reals.size());
  for (const auto &[name, values] : s.reals) {
    content.string(name);
    content.integer(values.size());
    for (double value : values) {
      content.real(value);
    }
  }
  atlasInputReceipt_ = content.finish();
  integrity_ = std::make_shared<const AtlasMeshIntegrity>(
      std::vector<atlas::Mesh>{primal_, dual_},
      std::vector<atlas::FunctionSpace>{cellNodes_, cellMeans_, edgeNormals_, vertexNodes_});
  traversalCoordinates_ = cellNodes_.lonlat();
  const auto coordinates = atlas::array::make_view<double, 2>(traversalCoordinates_);
  traversalCoordinateBytes_.reserve(2 * coordinates.shape(0));
  for (atlas::idx_t row = 0; row < coordinates.shape(0); ++row) {
    traversalCoordinateBytes_.push_back(coordinates(row, 0));
    traversalCoordinateBytes_.push_back(coordinates(row, 1));
  }
}

void MpasAtlasGeometry::validateStorage() const { integrity_->validate(); }

std::array<double, 2> MpasAtlasGeometry::authenticatedCellCoordinates(size_t row) const {
  const auto field = cellNodes_.lonlat();
  const auto &nodes = dual_.nodes();
  if (field.get() != traversalCoordinates_.get() ||
      !nodes.has_field("lonlat") || nodes.field("lonlat").get() != field.get() ||
      nodes.lonlat().get() != field.get() || field.name() != "lonlat" ||
      field.rank() != 2 || field.datatype() != atlas::array::make_datatype<double>() ||
      field.shape(0) * 2 != traversalCoordinateBytes_.size() || field.shape(1) != 2 ||
      row >= traversalCoordinateBytes_.size() / 2) {
    throw std::invalid_argument("MPAS iterator geometry storage changed: coordinate layout/alias");
  }
  const auto coordinates = atlas::array::make_view<double, 2>(field);
  const std::array<double, 2> point{coordinates(row, 0), coordinates(row, 1)};
  if (std::memcmp(&point[0], &traversalCoordinateBytes_.at(2 * row), sizeof(double)) != 0 ||
      std::memcmp(&point[1], &traversalCoordinateBytes_.at(2 * row + 1), sizeof(double)) != 0) {
    throw std::invalid_argument("MPAS iterator geometry storage changed: coordinate row");
  }
  return point;
}

std::shared_ptr<const MpasAtlasPointOperator> MpasAtlasGeometry::pointOperator(
    const std::vector<double> &lat, const std::vector<double> &lon,
    const std::string &compiler) const {
  validateStorage();
  const auto key = MpasAtlasPointOperator::keyFor(*this, lat, lon, compiler);
  eckit::AutoLock<eckit::Mutex> guard(pointCacheMutex_);
  const auto found = pointCache_.find(key);
  if (found != pointCache_.end()) {
    return found->second;
  }
  if (pointCache_.size() == pointCacheCapacity_) {
    throw std::invalid_argument(
        "MPAS Geometry point cache exceeds 64 distinct ordered target requests");
  }
  auto compiled = std::make_shared<const MpasAtlasPointOperator>(*this, lat, lon, compiler);
  pointCache_[key] = compiled;
  return compiled;
}

atlas::FieldSet MpasAtlasGeometry::staticVerticalFields(const MpasStaticVerticalSnapshot &s) const {
  validateStorage();
  const bool validReceipt =
      s.receipt.size() == 64 && std::all_of(s.receipt.begin(), s.receipt.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
  if (!validReceipt || s.horizontalReceipt != receipt_ ||
      s.cells != static_cast<size_t>(cellNodes_.size()) || s.layers == 0 ||
      s.layers >= static_cast<size_t>(std::numeric_limits<int>::max()) ||
      s.cells > std::numeric_limits<size_t>::max() / (s.layers + 1) ||
      s.interfaceHeights.size() != s.cells * (s.layers + 1) ||
      s.layerMetrics.size() != s.cells * s.layers || s.heightUnits != "m MSL" ||
      s.metricUnits != "unitless" || s.direction != "bottom_to_top") {
    throw std::invalid_argument(
        "MPAS static vertical snapshot identity/shape/unit/stagger mismatch");
  }
  for (size_t cell = 0; cell < s.cells; ++cell) {
    for (size_t level = 0; level <= s.layers; ++level) {
      const double height = s.interfaceHeights[cell * (s.layers + 1) + level];
      if (!std::isfinite(height) ||
          (level && height <= s.interfaceHeights[cell * (s.layers + 1) + level - 1])) {
        throw std::invalid_argument(
            "MPAS interface heights must be finite and strictly bottom-to-top");
      }
    }
    for (size_t level = 0; level < s.layers; ++level) {
      const double metric = s.layerMetrics[cell * s.layers + level];
      if (!std::isfinite(metric) || metric <= 0.) {
        throw std::invalid_argument("MPAS layer metric must be finite and positive");
      }
    }
  }
  AtlasOperatorReceipt content;
  content.string("mpas-atlas-static-vertical-fields-float64-v1");
  content.string(s.horizontalReceipt);
  content.string(s.receipt);
  content.integer(s.cells);
  content.integer(s.layers);
  content.string(s.heightUnits);
  content.string(s.metricUnits);
  content.string(s.direction);
  for (double value : s.interfaceHeights) {
    content.real(value);
  }
  for (double value : s.layerMetrics) {
    content.real(value);
  }
  const std::string inputReceipt = content.finish();
  atlas::FieldSet result;
  for (const std::string name : {"zz", "zgrid"}) {
    const bool interfaces = name == "zgrid";
    const size_t levels = s.layers + static_cast<size_t>(interfaces);
    const auto &values = interfaces ? s.interfaceHeights : s.layerMetrics;
    auto field = cellNodes_.createField<double>(atlas::option::name(name) |
                                                atlas::option::levels(static_cast<int>(levels)));
    auto view = atlas::array::make_view<double, 2>(field);
    for (size_t cell = 0; cell < s.cells; ++cell) {
      for (size_t level = 0; level < levels; ++level) {
        view(cell, level) = values[cell * levels + level];
      }
    }
    field.metadata().set("units", interfaces ? s.heightUnits : s.metricUnits);
    field.metadata().set("semantic_id",
                         interfaces ? "mpas.static.interface_height_msl" : "mpas.static.dzeta_dz");
    field.metadata().set("horizontal_location", "cell");
    field.metadata().set("vertical_stagger", interfaces ? "interface" : "layer");
    field.metadata().set("vertical_direction", s.direction);
    field.metadata().set("static_vertical_geometry_receipt", s.receipt);
    field.metadata().set("horizontal_geometry_receipt", s.horizontalReceipt);
    field.metadata().set("atlas_input_receipt", inputReceipt);
    field.metadata().set("persistence", "static_geometry");
    field.metadata().set("writable_state", false);
    result.add(field);
  }
  return result;
}

}  // namespace ijedi
