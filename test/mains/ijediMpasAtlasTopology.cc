/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <random>
#include <cmath>
#include <chrono>
#include <limits>
#include <functional>
#include <sys/resource.h>

#include <nlohmann/json.hpp>
#include "atlas/array.h"
#include "atlas/library/Library.h"
#include "atlas/interpolation/Interpolation.h"
#include "atlas/interpolation/Cache.h"
#include "atlas/linalg/sparse/MakeEckitSparseMatrix.h"
#include "atlas/option.h"
#include "atlas/util/Config.h"
#include "atlas/mesh/HybridElements.h"
#include "atlas/mesh/Nodes.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Python/MpasBackendContext.h"
#include "ijedi/Interpolation/MpasAtlasPointOperator.h"
#include "ijedi/Interpolation/MpasAtlasConservativeOperator.h"
#include "ijedi/Interpolation/AtlasCellMeanEndpoint.h"

namespace {
using Json = nlohmann::json;

Json atlasHandleProbe(const ijedi::MpasHorizontalSnapshot &snapshot) {
  ijedi::MpasAtlasGeometry geometry(snapshot);
  const std::string compiler = atlas::Library::instance().gitsha1(40);
  std::weak_ptr<const ijedi::MpasAtlasPointOperator> observed;
  {
    const auto op = geometry.pointOperator({0.}, {0.}, compiler);
    observed = op;
  }
  const bool retained = !observed.expired();
  const auto again = geometry.pointOperator({0.}, {0.}, compiler);
  const auto originalReceipt = geometry.atlasInputReceipt();
  auto coordinates = atlas::array::make_view<double, 2>(geometry.cellNodes().lonlat());
  const double original = coordinates(0, 0);
  coordinates(0, 0) += 0.01;
  bool rejected = false;
  try {
    (void)geometry.pointOperator({30.}, {40.}, compiler);
  } catch (const std::exception &) {
    rejected = true;
  }
  coordinates(0, 0) = original;
  return {{"geometry_owns_cached_operator", retained},
          {"mutable_coordinate_rejected", rejected},
          {"quoted_input_receipt_unchanged", geometry.atlasInputReceipt() == originalReceipt}};
}

Json atlasHandleChecks(const ijedi::MpasHorizontalSnapshot &snapshot) {
  const auto probe = atlasHandleProbe(snapshot);
  if (!probe.at("geometry_owns_cached_operator").get<bool>() ||
      !probe.at("mutable_coordinate_rejected").get<bool>()) {
    throw std::runtime_error("Atlas mesh storage or Geometry-lifetime cache is not protected");
  }
  ijedi::MpasAtlasGeometry geometry(snapshot);
  const std::string compiler = atlas::Library::instance().gitsha1(40);
  const auto point = geometry.pointOperator({0.}, {0.}, compiler);
  const auto endpoint = ijedi::AtlasCellMeanEndpoint::fromMpas(geometry);
  const ijedi::MpasAtlasConservativeOperator conservative(endpoint, endpoint, compiler);
  const std::vector<double> pointValues(point->sourceSize(), 1.);
  const std::vector<double> cellValues(conservative.sourceSize(), 1.);
  geometry.validateStorage();
  (void)point->apply(pointValues, 1);
  (void)conservative.apply(cellValues, 1);
  int mutations = 0, rejections = 0;
  bool completeConnectivityActions = false;
  const auto pointCache = point->serializeCache(), pointReceipt = point->cacheReceipt();
  const auto attack = [&](const std::function<void()> &mutate,
                          const std::function<void()> &restore) {
    mutate();
    try {
      std::vector<std::function<void()>> actions{
               [&] { (void)geometry.pointOperator({0.}, {0.}, compiler); },
               [&] { (void)geometry.pointOperator({30.}, {40.}, compiler); },
               [&] { (void)point->apply(pointValues, 1); },
               [&] { (void)ijedi::AtlasCellMeanEndpoint::fromMpas(geometry); },
               [&] { (void)conservative.apply(cellValues, 1); }};
      if (completeConnectivityActions) {
        actions.emplace_back([&] { geometry.validateStorage(); });
        actions.emplace_back([&] {
          ijedi::MpasAtlasPointOperator restored(geometry, {0.}, {0.}, compiler,
                                                 pointCache, pointReceipt);
        });
      }
      for (const auto &action : actions) {
        bool rejected = false;
        try {
          action();
        } catch (const std::invalid_argument &error) {
          rejected =
              std::string(error.what()).find("geometry storage changed") != std::string::npos;
        }
        if (!rejected) {
          throw std::runtime_error("mutated Atlas storage retained compiler/executor authority");
        }
        ++rejections;
      }
    } catch (...) {
      restore();
      throw;
    }
    restore();
    geometry.validateStorage();
    ++mutations;
  };
  for (auto field : {geometry.cellNodes().lonlat(), geometry.dualMesh().nodes().xy(),
                     geometry.dualMesh().nodes().field("xyz"),
                     geometry.dualMesh().cells().field("centre "), geometry.vertexNodes().lonlat(),
                     geometry.cellMeans().lonlat(), geometry.edgeNormals().lonlat(),
                     geometry.primalMesh().edges().field("mpas_canonical_edge_normal")}) {
    auto values = atlas::array::make_view<double, 2>(field);
    const double original = values(0, 0);
    attack([&] { values(0, 0) += 0.01; }, [&] { values(0, 0) = original; });
  }
  for (auto field :
       {geometry.dualMesh().nodes().global_index(), geometry.primalMesh().cells().global_index(),
        geometry.primalMesh().edges().global_index()}) {
    auto values = atlas::array::make_view<atlas::gidx_t, 1>(field);
    const auto original = values(0);
    attack([&] { values(0) += 1; }, [&] { values(0) = original; });
  }
  auto ghostField = geometry.dualMesh().nodes().ghost();
  auto ghost = atlas::array::make_view<int, 1>(ghostField);
  const int originalGhost = ghost(0);
  attack([&] { ghost(0) = 1; }, [&] { ghost(0) = originalGhost; });
  auto dual = geometry.dualMesh();
  auto &connectivity = dual.cells().node_connectivity();
  const atlas::idx_t originalNode = connectivity(0, 0);
  attack([&] { connectivity.set(0, 0, connectivity(0, 1)); },
         [&] { connectivity.set(0, 0, originalNode); });
  const int originalMutations = mutations, originalRejections = rejections;
  // flags, remote ownership and FE's actual search-radius metadata.
  // Exercise all five existing entry points, not just validateStorage.
  for (auto mesh : {geometry.primalMesh(), geometry.dualMesh()}) {
    for (auto field : {mesh.nodes().flags(), mesh.nodes().remote_index(),
                       mesh.cells().flags(), mesh.cells().remote_index(),
                       mesh.edges().flags(), mesh.edges().remote_index()}) {
      if (field.shape(0) == 0) continue;
      auto values = atlas::array::make_view<int, 1>(field);
      const int original = values(0);
      attack([&] { values(0) ^= 1; }, [&] { values(0) = original; });
    }
    for (const std::string key : {"cell_maximum_diagonal_on_unit_sphere", "halo"}) {
      const bool present = mesh.metadata().has(key);
      const double original = mesh.metadata().getDouble(key, 0.);
      attack([&] { mesh.metadata().set(key, 1.e-20); }, [&] {
        if (present) mesh.metadata().set(key, original);
        else mesh.metadata().remove(key);
      });
    }
  }
  const int handleMutations = mutations - originalMutations;
  const int handleRejections = rejections - originalRejections;
  completeConnectivityActions = true;
  // enumerate all native relation families, including empty layouts.
  for (auto mesh : {geometry.primalMesh(), geometry.dualMesh()}) {
    for (auto *elements : {&mesh.cells(), &mesh.edges()}) {
      for (auto *relation : {&elements->node_connectivity(), &elements->cell_connectivity(),
                             &elements->edge_connectivity()}) {
        const auto relationName = relation->name();
        attack([&] { relation->rename("corrupted_relation"); },
               [&] { relation->rename(relationName); });
        if (relation->rows() && relation->cols(0)) {
          const auto original = (*relation)(0, 0);
          attack([&] { relation->set(0, 0, original + 1); },
                 [&] { relation->set(0, 0, original); });
        }
        struct Block { atlas::idx_t rows, cols; std::vector<atlas::idx_t> data; };
        std::vector<Block> saved;
        for (atlas::idx_t i = 0; i < relation->blocks(); ++i) {
          const auto &block = relation->block(i);
          saved.push_back({block.rows(), block.cols(),
                           {block.data(), block.data() + block.rows() * block.cols()}});
        }
        attack([&] { relation->clear(); relation->add(1, 1); }, [&] {
          relation->clear();
          for (const auto &block : saved) relation->add(block.rows, block.cols, block.data.data(), true);
        });
      }
    }
    for (auto *relation : {&mesh.nodes().cell_connectivity(), &mesh.nodes().edge_connectivity()}) {
      if (relation->rows() && relation->cols(0)) {
        const auto original = (*relation)(0, 0);
        attack([&] { relation->set(0, 0, original + 1); },
               [&] { relation->set(0, 0, original); });
      }
      std::vector<atlas::idx_t> counts, values;
      for (atlas::idx_t row = 0; row < relation->rows(); ++row) {
        counts.push_back(relation->cols(row));
        for (atlas::idx_t col = 0; col < relation->cols(row); ++col) {
          values.push_back((*relation)(row, col));
        }
      }
      attack([&] { relation->clear(); relation->add(1, 1); }, [&] {
        relation->clear();
        relation->add(counts.size(), counts.data());
        size_t offset = 0;
        for (atlas::idx_t row = 0; row < relation->rows(); ++row) {
          for (atlas::idx_t col = 0; col < counts[row]; ++col) relation->set(row, col, values[offset++]);
        }
      });
    }
    // Reciprocal native cell/edge and node adjacency must be real Atlas
    // topology, not merely a sealed inventory.
    const auto &ce = mesh.cells().edge_connectivity();
    const auto &ec = mesh.edges().cell_connectivity();
    for (atlas::idx_t cell = 0; cell < ce.rows(); ++cell) {
      for (atlas::idx_t j = 0; j < ce.cols(cell); ++j) {
        const auto edge = ce(cell, j);
        if (edge < 0) continue;
        bool found = false;
        for (atlas::idx_t k = 0; k < ec.cols(edge); ++k) found |= ec(edge, k) == cell;
        if (!found) throw std::runtime_error("Atlas cell/edge relation is not reciprocal");
      }
    }
    for (auto *elements : {&mesh.cells(), &mesh.edges()}) {
      const auto &en = elements->node_connectivity();
      const auto &ne = elements == &mesh.cells() ? mesh.nodes().cell_connectivity()
                                                : mesh.nodes().edge_connectivity();
      for (atlas::idx_t element = 0; element < en.rows(); ++element) {
        for (atlas::idx_t j = 0; j < en.cols(element); ++j) {
          const auto node = en(element, j);
          bool found = false;
          for (atlas::idx_t k = 0; k < ne.cols(node); ++k) found |= ne(node, k) == element;
          if (!found) throw std::runtime_error("Atlas node adjacency is not reciprocal");
        }
      }
    }
  }
  if (mutations - originalMutations - handleMutations != 36 ||
      rejections - originalRejections - handleRejections != 252) {
    throw std::runtime_error("native relation or consumer attacks were omitted");
  }
  const int beforeCompoundMutations = mutations, beforeCompoundRejections = rejections;
  for (auto mesh : {geometry.primalMesh(), geometry.dualMesh()}) {
    auto &nodes = mesh.nodes();
    for (const std::string name : {"lonlat", "xy", "glb_idx", "partition",
                                    "ghost", "halo", "flags", "remote_idx"}) {
      const atlas::Field original = nodes.field(name);
      auto replacement = original.clone();
      atlas::Field &shortcut = [&]() -> atlas::Field & {
        if (name == "lonlat") return nodes.lonlat();
        if (name == "xy") return nodes.xy();
        if (name == "glb_idx") return nodes.global_index();
        if (name == "partition") return nodes.partition();
        if (name == "ghost") return nodes.ghost();
        if (name == "halo") return nodes.halo();
        if (name == "flags") return nodes.flags();
        return nodes.remote_index();
      }();
      const auto restore = [&] {
        shortcut = original;
        if (nodes.has_field(name)) nodes.field(name) = original;
        else nodes.add(original);
      };
      attack([&] { nodes.field(name) = replacement; }, restore);
      attack([&] { nodes.remove_field(name); }, restore);
      attack([&] { nodes.remove_field(name); nodes.add(replacement); }, restore);
      attack([&] { shortcut = replacement; }, restore);
      attack([&] { shortcut = replacement; nodes.field(name) = replacement; }, restore);
      // Edit through each side of the separated map/shortcut relationship.
      for (auto field : {original, replacement}) {
        if (field.rank() == 2) {
          auto values = atlas::array::make_view<double, 2>(field);
          const double value = values(0, 0);
          attack([&] { nodes.field(name) = replacement; values(0, 0) += .01; },
                 [&] { values(0, 0) = value; restore(); });
        } else if (name == "glb_idx") {
          auto values = atlas::array::make_view<atlas::gidx_t, 1>(field);
          const auto value = values(0);
          attack([&] { nodes.field(name) = replacement; ++values(0); },
                 [&] { values(0) = value; restore(); });
        } else {
          auto values = atlas::array::make_view<int, 1>(field);
          const auto value = values(0);
          attack([&] { nodes.field(name) = replacement; ++values(0); },
                 [&] { values(0) = value; restore(); });
        }
      }
    }
  }
  const int rbMutations = mutations - beforeCompoundMutations;
  const int rbRejections = rejections - beforeCompoundRejections;
  if (rbMutations != 112 || rbRejections != 784) {
    throw std::runtime_error("shortcut alias attacks were omitted");
  }
  // Fill the explicit cache bound, prove rejection without hidden eviction,
  // and verify the first operator still has exactly its original identity.
  for (size_t i = 1; i < 64; ++i) {
    (void)geometry.pointOperator({0.}, {static_cast<double>(i)}, compiler);
  }
  bool bounded = false;
  try {
    (void)geometry.pointOperator({0.}, {64.}, compiler);
  } catch (const std::invalid_argument &error) {
    bounded = std::string(error.what()).find("exceeds 64") != std::string::npos;
  }
  if (!bounded || geometry.pointOperator({0.}, {0.}, compiler) != point) {
    throw std::runtime_error("Geometry-lifetime cache evicted or exceeded its declared bound");
  }
  std::shared_ptr<const ijedi::MpasAtlasPointOperator> surviving;
  {
    ijedi::MpasAtlasGeometry temporary(snapshot);
    surviving = temporary.pointOperator({0.}, {0.}, compiler);
  }
  if (surviving->apply(pointValues, 1) != point->apply(pointValues, 1)) {
    throw std::runtime_error("Atlas operator did not retain its source after Geometry destruction");
  }
  return {{"probe", probe},
          {"storage_mutations", originalMutations},
          {"rejections", originalRejections},
          {"geometry_handle_mutations", handleMutations},
          {"geometry_handle_rejections", handleRejections},
          {"operator_storage_mutations", beforeCompoundMutations - originalMutations - handleMutations},
          {"operator_storage_rejections", beforeCompoundRejections - originalRejections - handleRejections},
          {"compound_storage_mutations", rbMutations}, {"compound_storage_rejections", rbRejections},
          {"cache_capacity", 64},
          {"cache_bound_enforced_without_eviction", bounded},
          {"operator_survives_geometry", true}};
}

struct CacheRun {
  std::string outputPrefix, inputPrefix;
  Json trusted;
  bool restoring() const { return !inputPrefix.empty(); }
  std::string read(const std::string &name) const {
    std::ifstream input(inputPrefix + "." + name + ".cache.json");
    if (!input) {
      throw std::runtime_error("fresh-process Atlas cache input is absent");
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }
  void write(const std::string &name, const std::string &payload) const {
    std::ofstream output(outputPrefix + "." + name + ".cache.json");
    output << payload;
    if (!output) {
      throw std::runtime_error("cannot persist Atlas cache fixture");
    }
  }
};

Json meshManifest(const atlas::Mesh &mesh) {
  Json out;
  const auto coordinates = atlas::array::make_view<double, 2>(mesh.nodes().lonlat());
  const auto nodeIds = atlas::array::make_view<atlas::gidx_t, 1>(mesh.nodes().global_index());
  const auto cellIds = atlas::array::make_view<atlas::gidx_t, 1>(mesh.cells().global_index());
  const auto &connectivity = mesh.cells().node_connectivity();
  out["node_ids"] = Json::array();
  out["coordinates"] = Json::array();
  out["cell_ids"] = Json::array();
  out["faces"] = Json::array();
  for (atlas::idx_t i = 0; i < mesh.nodes().size(); ++i) {
    out["node_ids"].push_back(nodeIds(i));
    out["coordinates"].push_back({coordinates(i, 0), coordinates(i, 1)});
  }
  for (atlas::idx_t i = 0; i < mesh.cells().size(); ++i) {
    out["cell_ids"].push_back(cellIds(i));
    Json row = Json::array();
    for (atlas::idx_t j = 0; j < connectivity.cols(i); ++j) {
      row.push_back(connectivity(i, j));
    }
    out["faces"].push_back(row);
  }
  return out;
}

Json pointChecks(const ijedi::MpasAtlasGeometry &geometry,
                 const ijedi::MpasHorizontalSnapshot &snapshot, const CacheRun &cacheRun) {
  std::vector<double> lat, lon;
  for (double latitude : {-90., -89.9, -30., 0., 30., 89.9, 90.}) {
    for (double longitude : {-180., -0.001, 0., 90., 179.999, 180.}) {
      lat.push_back(latitude);
      lon.push_back(longitude);
    }
  }
  const auto start = std::chrono::steady_clock::now();
  const std::string compiler = atlas::Library::instance().gitsha1(40);
  (void)atlas::Library::instance().gitsha1(7);
  if (compiler.size() != 40 || atlas::Library::instance().gitsha1(40) != compiler) {
    throw std::runtime_error(
        "Atlas full compiler identity was absent or truncated by an abbreviated query");
  }
  auto owned = cacheRun.restoring()
                   ? std::make_unique<ijedi::MpasAtlasPointOperator>(
                         geometry, lat, lon, compiler, cacheRun.read("point"),
                         cacheRun.trusted.at("point").get<std::string>())
                   : std::make_unique<ijedi::MpasAtlasPointOperator>(geometry, lat, lon, compiler);
  const auto &op = *owned;
  cacheRun.write("point", op.serializeCache());
  ijedi::MpasAtlasPointOperator restored(geometry, lat, lon, compiler, op.serializeCache(),
                                         op.cacheReceipt());
  double maximumScalar = 0, maximumVector = 0, maximumWeighted = 0;
  for (size_t levels : {1, 55, 56}) {
    for (int seed = 0; seed < 4; ++seed) {
      std::mt19937_64 generator(20260930 + seed);
      std::normal_distribution<double> normal;
      std::vector<double> x(op.sourceSize() * levels), v(x.size());
      std::vector<double> y(op.targetSize() * levels), z(y.size());
      for (auto *values : {&x, &v, &y, &z}) {
        for (double &value : *values) {
          value = normal(generator);
        }
      }
      const auto wx = op.apply(x, levels), wty = op.applyTranspose(y, levels);
      if (wx != restored.apply(x, levels) || wty != restored.applyTranspose(y, levels)) {
        throw std::runtime_error("restored scalar Atlas execution differs elementwise");
      }
      const auto vector = op.applyVector(x, v, levels);
      const auto transpose = op.applyVectorTranspose(y, z, levels);
      if (vector != restored.applyVector(x, v, levels) ||
          transpose != restored.applyVectorTranspose(y, z, levels)) {
        throw std::runtime_error("restored vector Atlas execution differs elementwise");
      }
      long double lhs = 0, rhs = 0, norm = 0, vlhs = 0, vrhs = 0, vnorm = 0;
      for (size_t i = 0; i < y.size(); ++i) {
        lhs += static_cast<long double>(wx[i]) * y[i];
        norm += std::abs(static_cast<long double>(wx[i]) * y[i]);
        vlhs += static_cast<long double>(vector[0][i]) * y[i] +
                static_cast<long double>(vector[1][i]) * z[i];
        vnorm += std::abs(static_cast<long double>(vector[0][i]) * y[i]) +
                 std::abs(static_cast<long double>(vector[1][i]) * z[i]);
      }
      for (size_t i = 0; i < x.size(); ++i) {
        rhs += static_cast<long double>(x[i]) * wty[i];
        vrhs += static_cast<long double>(x[i]) * transpose[0][i] +
                static_cast<long double>(v[i]) * transpose[1][i];
      }
      const double scalarError = std::abs(lhs - rhs) / norm,
                   vectorError = std::abs(vlhs - vrhs) / vnorm;
      if (!std::isfinite(scalarError) || !std::isfinite(vectorError) || scalarError > 2.e-14 ||
          vectorError > 2.e-14) {
        throw std::runtime_error("Atlas transpose identity exceeds frozen bar");
      }
      maximumScalar = std::max(maximumScalar, scalarError);
      maximumVector = std::max(maximumVector, vectorError);
      const auto &ms = op.sourceMeasures();
      const auto adjoint = op.applyWeightedAdjoint(y, levels);
      long double weightedRhs = 0;
      for (size_t i = 0; i < x.size(); ++i) {
        weightedRhs += static_cast<long double>(x[i]) * adjoint[i] * ms[i / levels];
      }
      const double weightedError = std::abs(lhs - weightedRhs) / norm;
      if (!std::isfinite(weightedError) || weightedError > 2.e-14) {
        throw std::runtime_error("Atlas weighted adjoint exceeds frozen bar");
      }
      maximumWeighted = std::max(maximumWeighted, weightedError);
      const auto vectorAdjoint = op.applyVectorWeightedAdjoint(y, z, levels);
      long double weightedVectorRhs = 0;
      for (size_t i = 0; i < x.size(); ++i) {
        weightedVectorRhs += (static_cast<long double>(x[i]) * vectorAdjoint[0][i] +
                              static_cast<long double>(v[i]) * vectorAdjoint[1][i]) *
                             ms[i / levels];
      }
      const double weightedVectorError = std::abs(vlhs - weightedVectorRhs) / vnorm;
      if (!std::isfinite(weightedVectorError) || weightedVectorError > 2.e-14) {
        throw std::runtime_error("Atlas receipt-owned vector weighted adjoint exceeds frozen bar");
      }
      maximumWeighted = std::max(maximumWeighted, weightedVectorError);
      std::fill(x.begin(), x.end(), 1.0);
      for (double value : op.apply(x, levels)) {
        if (!std::isfinite(value) || std::abs(value - 1.0) > 1.e-13) {
          throw std::runtime_error("Atlas point interpolation fails constant preservation");
        }
      }
    }
  }
  int attacks = 0;
  const auto reject = [&](const std::string &payload, const std::vector<double> &targetLat,
                          const std::string &identity) {
    try {
      ijedi::MpasAtlasPointOperator bad(geometry, targetLat, lon, identity, payload,
                                        op.cacheReceipt());
    } catch (const std::exception &) {
      ++attacks;
      return;
    }
    throw std::runtime_error("corrupt or rebound Atlas cache was accepted");
  };
  auto bad = Json::parse(op.serializeCache());
  auto &entries = bad["entries"];
  bool perturbed = false;
  for (size_t i = 0; i + 1 < entries.size(); ++i) {
    if (entries[i][0] == entries[i + 1][0]) {
      entries[i][2] = entries[i][2].get<double>() + 0.001;
      entries[i + 1][2] = entries[i + 1][2].get<double>() - 0.001;
      perturbed = true;
      break;
    }
  }
  if (!perturbed) {
    throw std::runtime_error("cache attack lacks a two-entry row");
  }
  reject(bad.dump(), lat, compiler);
  auto moved = lat;
  std::swap(moved[0], moved[12]);
  reject(op.serializeCache(), moved, compiler);
  reject(op.serializeCache(), lat, compiler + "-wrong");
  bad = Json::parse(op.serializeCache());
  bad["columns"] = op.sourceSize() - 1;
  reject(bad.dump(), lat, compiler);
  bad = Json::parse(op.serializeCache());
  bad["vector_entries"][0][3] = bad["vector_entries"][0][3].get<double>() + 0.01;
  reject(bad.dump(), lat, compiler);
  // A quoted horizontal identity alone must not authorize a different metric.
  // Keep topology and that identity unchanged, but substitute a positive area.
  auto alteredMetric = snapshot;
  alteredMetric.reals.at("areaCell")[0] *= 1.01;
  const ijedi::MpasAtlasGeometry differentMetric(alteredMetric);
  if (geometry.cellNodeMeasureReceipt() == differentMetric.cellNodeMeasureReceipt()) {
    throw std::runtime_error("native cell-area receipt did not detect a substituted metric");
  }
  bool metricRejected = false;
  try {
    ijedi::MpasAtlasPointOperator wrongMetric(differentMetric, lat, lon, compiler,
                                              op.serializeCache(), op.cacheReceipt());
  } catch (const std::exception &) {
    metricRejected = true;
  }
  if (!metricRejected) {
    throw std::runtime_error("Atlas cache accepted a substituted source metric");
  }
  ++attacks;
  return {{"nnz", op.nonzeros()},
          {"targets", op.targetSize()},
          {"receipt", op.cacheReceipt()},
          {"scalar_adjoint_max", maximumScalar},
          {"vector_adjoint_max", maximumVector},
          {"weighted_adjoint_max", maximumWeighted},
          {"levels", {1, 55, 56}},
          {"seeds", 4},
          {"cache_negative_controls", attacks},
          {"wall_seconds",
           std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()}};
}

Json conservativeChecks(const ijedi::AtlasCellMeanEndpoint &sourceGeometry,
                        const ijedi::AtlasCellMeanEndpoint &targetGeometry,
                        const CacheRun &cacheRun, const std::string &name,
                        bool measureRuntime = false) {
  const std::string compiler = atlas::Library::instance().gitsha1(40);
  const auto setupStart = std::chrono::steady_clock::now();
  auto owned = cacheRun.restoring()
                   ? std::make_unique<ijedi::MpasAtlasConservativeOperator>(
                         sourceGeometry, targetGeometry, compiler, cacheRun.read(name),
                         cacheRun.trusted.at(name).get<std::string>())
                   : std::make_unique<ijedi::MpasAtlasConservativeOperator>(
                         sourceGeometry, targetGeometry, compiler);
  const auto &op = *owned;
  const double setupSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - setupStart).count();
  cacheRun.write(name, op.serializeCache());
  ijedi::MpasAtlasConservativeOperator restored(sourceGeometry, targetGeometry, compiler,
                                                op.serializeCache(), op.cacheReceipt());
  const auto &ms = op.sourceMeasures(), &mt = op.targetMeasures();
  double adjointError = 0;
  Json timings = Json::array();
  // Independent analytic source fields. Check their discrete physical
  // integrals, not equality to a self-produced interpolation reference.
  const auto lonlat = atlas::array::make_view<double, 2>(sourceGeometry.space().lonlat());
  constexpr size_t analyticFields = 8;
  std::vector<double> analytic(ms.size() * analyticFields);
  for (size_t i = 0; i < ms.size(); ++i) {
    const double lon = lonlat(i, 0) * std::acos(-1.) / 180.;
    const double lat = lonlat(i, 1) * std::acos(-1.) / 180.;
    const double x = std::cos(lat) * std::cos(lon), y = std::cos(lat) * std::sin(lon);
    const double z = std::sin(lat);
    const double fields[analyticFields] = {
        x, y, z, x * y, x * z, y * z, x * x - y * y, 3 * z * z - 1};
    std::copy(fields, fields + analyticFields, analytic.begin() + i * analyticFields);
  }
  const auto mappedAnalytic = op.apply(analytic, analyticFields);
  double analyticIntegralError = 0;
  for (size_t k = 0; k < analyticFields; ++k) {
    long double sourceIntegral = 0, targetIntegral = 0, norm = 0;
    for (size_t i = 0; i < ms.size(); ++i) {
      const long double term = static_cast<long double>(analytic[i * analyticFields + k]) * ms[i];
      sourceIntegral += term;
      norm += std::abs(term);
    }
    for (size_t i = 0; i < mt.size(); ++i) {
      targetIntegral += static_cast<long double>(mappedAnalytic[i * analyticFields + k]) * mt[i];
    }
    const double error = std::abs(sourceIntegral - targetIntegral) / norm;
    if (!std::isfinite(error) || norm <= 0 || error > 2.e-14) {
      throw std::runtime_error("analytic cell-mean integral exceeds frozen conservative bar");
    }
    analyticIntegralError = std::max(analyticIntegralError, error);
  }
  for (size_t levels : {1, 55, 56}) {
    for (int seed = 0; seed < 4; ++seed) {
      std::mt19937_64 generator(20260930 + seed);
      std::normal_distribution<double> normal;
      std::vector<double> x(ms.size() * levels), y(mt.size() * levels);
      for (auto *values : {&x, &y}) {
        for (double &value : *values) {
          value = normal(generator);
        }
      }
      const auto wx = op.apply(x, levels), adjoint = op.applyWeightedAdjoint(y, levels);
      if (measureRuntime) {
        Json repeats = Json::array();
        for (int repeat = 0; repeat < 5; ++repeat) {
          const auto start = std::chrono::steady_clock::now();
          const auto result = op.apply(x, levels);
          repeats.push_back(
              std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
          if (result != wx) {
            throw std::runtime_error("cached conservative repeat changed bytes");
          }
        }
        timings.push_back({{"levels", levels}, {"seed", seed}, {"apply_seconds", repeats}});
      }
      if (wx != restored.apply(x, levels) || adjoint != restored.applyWeightedAdjoint(y, levels) ||
          op.applyTranspose(y, levels) != restored.applyTranspose(y, levels)) {
        throw std::runtime_error(
            "cached conservative primal/transpose/weighted execution differs elementwise");
      }
      long double lhs = 0, rhs = 0, norm = 0, sourceIntegral = 0, targetIntegral = 0,
                  integralNorm = 0;
      for (size_t i = 0; i < mt.size(); ++i) {
        for (size_t k = 0; k < levels; ++k) {
          const size_t j = i * levels + k;
          lhs += static_cast<long double>(wx[j]) * y[j] * mt[i];
          norm += std::abs(static_cast<long double>(wx[j]) * y[j] * mt[i]);
          targetIntegral += static_cast<long double>(wx[j]) * mt[i];
        }
      }
      for (size_t i = 0; i < ms.size(); ++i) {
        for (size_t k = 0; k < levels; ++k) {
          const size_t j = i * levels + k;
          rhs += static_cast<long double>(x[j]) * adjoint[j] * ms[i];
          sourceIntegral += static_cast<long double>(x[j]) * ms[i];
          integralNorm += std::abs(static_cast<long double>(x[j]) * ms[i]);
        }
      }
      const double residual = std::abs(lhs - rhs) / norm;
      adjointError = std::max(adjointError, residual);
      if (!std::isfinite(residual) || !std::isfinite(sourceIntegral) ||
          !std::isfinite(targetIntegral) || !std::isfinite(integralNorm) || integralNorm <= 0 ||
          residual > 2.e-14 || std::abs(sourceIntegral - targetIntegral) / integralNorm > 2.e-14) {
        throw std::runtime_error("real conservative integral/adjoint exceeds frozen bar");
      }
    }
    const auto constant = op.apply(std::vector<double>(ms.size() * levels, 1.), levels);
    for (double value : constant) {
      if (!std::isfinite(value) || std::abs(value - 1) > 2.e-14) {
        throw std::runtime_error("real conservative constant preservation failed");
      }
    }
  }
  int rejected = 0;
  const auto reject = [&](const std::string &payload, const std::string &identity) {
    try {
      ijedi::MpasAtlasConservativeOperator bad(sourceGeometry, targetGeometry, identity, payload,
                                               op.cacheReceipt());
    } catch (const std::exception &) {
      ++rejected;
      return;
    }
    throw std::runtime_error("corrupt or rebound conservative cache was accepted");
  };
  auto bad = Json::parse(op.serializeCache());
  bad["entries"][0][2] = bad["entries"][0][2].get<double>() + 1.e-4;
  reject(bad.dump(), compiler);
  bad = Json::parse(op.serializeCache());
  bad["columns"] = op.sourceSize() + 1;
  reject(bad.dump(), compiler);
  bad = Json::parse(op.serializeCache());
  bad["entries"][0][0] = op.targetSize();
  reject(bad.dump(), compiler);
  bad = Json::parse(op.serializeCache());
  bad["entries"][0][2] = -1.;
  reject(bad.dump(), compiler);
  bad = Json::parse(op.serializeCache());
  bad["atlas_uid"] = "wrong";
  reject(bad.dump(), compiler);
  reject(op.serializeCache(), std::string(40, '0'));
  Json result{{"nnz", op.nonzeros()},
              {"row_max", op.rowResidual()},
              {"column_max", op.columnResidual()},
              {"coefficient_receipt", op.cacheReceipt()},
              {"cache_key", op.cacheKey()},
              {"cache_negative_controls", rejected},
              {"source_cells", op.sourceSize()},
              {"target_cells", op.targetSize()},
              {"analytic_fields", analyticFields},
              {"analytic_integral_max", analyticIntegralError},
              {"weighted_adjoint_max", adjointError},
              {"seeds", 4},
              {"levels", {1, 55, 56}}};
  if (measureRuntime) {
    result["setup_seconds"] = setupSeconds;
    result["apply_trials"] = timings;
  }
  return result;
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 3 && argc != 4 && argc != 5 && argc != 6) {
    std::cerr << "usage: ijedi_mpas_atlas_topology SNAPSHOT_JSON OUTPUT_JSON [CACHE_PREFIX "
                 "TRUSTED_RECEIPTS_JSON] | SNAPSHOT OUTPUT --conservative-scaling "
                 "[CACHE_PREFIX TRUSTED_RECEIPTS_JSON]\n";
    return 2;
  }
  try {
    atlas::initialize(argc, argv);
    const bool scaling =
        (argc == 4 || argc == 6) && std::string(argv[3]) == "--conservative-scaling";
    const CacheRun cacheRun{
        argv[2], argc == 6 ? argv[4] : (argc == 5 ? argv[3] : ""),
        argc == 6 ? Json::parse(argv[5]) : (argc == 5 ? Json::parse(argv[4]) : Json::object())};
    std::ifstream input(argv[1]);
    Json document;
    input >> document;
    ijedi::MpasHorizontalSnapshot snapshot;
    snapshot.cells = document.at("cells");
    snapshot.edges = document.at("edges");
    snapshot.vertices = document.at("vertices");
    snapshot.cellWidth = document.at("cell_width");
    snapshot.receipt = document.at("receipt");
    snapshot.sphereRadiusMetres = document.at("sphere_radius_metres");
    snapshot.angleUnits = document.at("angle_units");
    snapshot.lengthUnits = document.at("length_units");
    snapshot.areaUnits = document.at("area_units");
    snapshot.reals = document.at("reals").get<decltype(snapshot.reals)>();
    snapshot.integers = document.at("integers").get<decltype(snapshot.integers)>();
    if (argc == 4 && !scaling) {
      if (std::string(argv[3]) == "--storage-negative-controls") {
        const auto checks = atlasHandleChecks(snapshot);
        if (checks.at("storage_mutations") != 13 || checks.at("rejections") != 65) {
          throw std::runtime_error("geometry mutation gate omitted a protected storage/action");
        }
        std::ofstream output(argv[2]);
        output << Json{{"atlas_compiler_identity", atlas::Library::instance().gitsha1(40)},
                       {"atlas_handle_checks", checks}}
                      .dump(2)
               << '\n';
        if (!output) {
          throw std::runtime_error("cannot retain geometry negative controls");
        }
        return 0;
      }
      if (std::string(argv[3]) != "--integrity-probe") {
        throw std::invalid_argument("unknown Atlas integrity probe argument");
      }
      std::ofstream output(argv[2]);
      output << atlasHandleProbe(snapshot).dump(2) << '\n';
      if (!output) {
        throw std::runtime_error("cannot write Atlas integrity probe");
      }
      return 0;
    }
    if (argc == 6 && !scaling) {
      throw std::invalid_argument("unknown Atlas scaling argument");
    }
    ijedi::MpasAtlasGeometry geometry(snapshot);
    if (scaling) {
      if (snapshot.cells != 10242 && snapshot.cells != 40962 && snapshot.cells != 163842) {
        throw std::invalid_argument("conservative scaling requires a declared native resolution");
      }
      const auto native = ijedi::AtlasCellMeanEndpoint::fromMpas(geometry);
      const auto analysis = ijedi::AtlasCellMeanEndpoint::fromAtlasGrid(atlas::Grid("O16"));
      Json result{
          {"scope", "serial_native_Atlas_conservative_scaling_not_model_forecast"},
          {"source_cells", snapshot.cells},
          {"geometry_receipt", geometry.receipt()},
          {"atlas_compiler_identity", atlas::Library::instance().gitsha1(40)},
          {"restoring", cacheRun.restoring()},
          {"forward", conservativeChecks(native, analysis, cacheRun, "analysis-forward", true)},
          {"reverse", conservativeChecks(analysis, native, cacheRun, "analysis-reverse", true)}};
      rusage usage;
      if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error("cannot measure conservative process RSS");
      }
#ifdef __APPLE__
      result["peak_rss_bytes"] = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
      result["peak_rss_bytes"] = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
      std::ofstream file(argv[2]);
      file << result.dump(2) << '\n';
      if (!file) {
        throw std::runtime_error("cannot retain conservative scaling result");
      }
      return 0;
    }
    Json output;
    output["atlas_compiler_identity"] = atlas::Library::instance().gitsha1(40);
    output["atlas_handle_checks"] = atlasHandleChecks(snapshot);
    output["receipt"] = geometry.receipt();
    output["point_checks"] = pointChecks(geometry, snapshot, cacheRun);
    const auto nativeMeans = ijedi::AtlasCellMeanEndpoint::fromMpas(geometry);
    output["conservative_identity"] =
        conservativeChecks(nativeMeans, nativeMeans, cacheRun, "identity");
    auto rotatedSnapshot = snapshot;
    const double angle = 0.23;
    for (const std::string suffix : {"Cell", "Vertex", "Edge"}) {
      for (double &longitude : rotatedSnapshot.reals["lon" + suffix]) {
        longitude += angle;
      }
    }
    rotatedSnapshot.receipt = std::string(64, 'f');  // synthetic global rigid-rotation fixture
    for (size_t e = 0; e < snapshot.edges; ++e) {
      const double x = snapshot.reals.at("edgeNormalVectors")[3 * e];
      const double y = snapshot.reals.at("edgeNormalVectors")[3 * e + 1];
      rotatedSnapshot.reals["edgeNormalVectors"][3 * e] = std::cos(angle) * x - std::sin(angle) * y;
      rotatedSnapshot.reals["edgeNormalVectors"][3 * e + 1] =
          std::sin(angle) * x + std::cos(angle) * y;
    }
    ijedi::MpasAtlasGeometry rotated(rotatedSnapshot);
    const auto rotatedMeans = ijedi::AtlasCellMeanEndpoint::fromMpas(rotated);
    output["conservative_rotated_forward"] =
        conservativeChecks(nativeMeans, rotatedMeans, cacheRun, "forward");
    output["conservative_rotated_reverse"] =
        conservativeChecks(rotatedMeans, nativeMeans, cacheRun, "reverse");
    const auto analysisMeans = ijedi::AtlasCellMeanEndpoint::fromAtlasGrid(atlas::Grid("O16"));
    output["conservative_analysis_forward"] =
        conservativeChecks(nativeMeans, analysisMeans, cacheRun, "analysis-forward");
    output["conservative_analysis_reverse"] =
        conservativeChecks(analysisMeans, nativeMeans, cacheRun, "analysis-reverse");
    auto physicalSnapshot = snapshot;
    physicalSnapshot.sphereRadiusMetres = 6371220.;
    physicalSnapshot.receipt =
        std::string(64, 'b');  // dimensional-conversion fixture, not another IC
    const double scale = physicalSnapshot.sphereRadiusMetres * physicalSnapshot.sphereRadiusMetres;
    for (double &area : physicalSnapshot.reals.at("areaCell")) {
      area *= scale;
    }
    const ijedi::MpasAtlasGeometry physicalGeometry(physicalSnapshot);
    const auto physicalMeans = ijedi::AtlasCellMeanEndpoint::fromMpas(physicalGeometry);
    for (size_t i = 0; i < nativeMeans.measures().size(); ++i) {
      if (std::abs(physicalMeans.measures()[i] / nativeMeans.measures()[i] - 1.) > 2.e-14) {
        throw std::runtime_error(
            "physical MPAS square-metre areas did not convert to owned steradians");
      }
    }
    output["conservative_physical_radius_forward"] =
        conservativeChecks(physicalMeans, analysisMeans, cacheRun, "physical-forward");
    output["conservative_physical_radius_reverse"] =
        conservativeChecks(analysisMeans, physicalMeans, cacheRun, "physical-reverse");
    output["primal"] = meshManifest(geometry.primalMesh());
    output["dual"] = meshManifest(geometry.dualMesh());
    output["atlas_to_native_cells"] = geometry.atlasToNativeCells();
    output["native_to_atlas_cells"] = geometry.nativeToAtlasCells();
    output["atlas_to_native_edges"] = geometry.atlasToNativeEdges();
    output["native_to_atlas_edges"] = geometry.nativeToAtlasEdges();
    output["edge_cell_orientation"] = geometry.edgeCellOrientation();
    output["cell_mean_measures"] = geometry.cellMeanMeasures();
    output["space_sizes"] = {geometry.cellNodes().size(), geometry.cellMeans().size(),
                             geometry.edgeNormals().size(), geometry.vertexNodes().size()};
    const auto &mesh = geometry.primalMesh();
    const auto &endpoints = mesh.edges().node_connectivity();
    const auto &adjacency = mesh.edges().cell_connectivity();
    const auto ids = atlas::array::make_view<atlas::gidx_t, 1>(mesh.edges().global_index());
    output["edge_ids"] = Json::array();
    output["edge_vertices"] = Json::array();
    output["edge_cells"] = Json::array();
    output["edge_coordinates"] = Json::array();
    output["cell_mean_coordinates"] = Json::array();
    output["edge_canonical_normals"] = Json::array();
    const auto ell = atlas::array::make_view<double, 2>(geometry.edgeNormals().lonlat());
    const auto cll = atlas::array::make_view<double, 2>(geometry.cellMeans().lonlat());
    const auto env =
        atlas::array::make_view<double, 2>(mesh.edges().field("mpas_canonical_edge_normal"));
    for (atlas::idx_t i = 0; i < geometry.cellMeans().size(); ++i) {
      output["cell_mean_coordinates"].push_back({cll(i, 0), cll(i, 1)});
    }
    for (atlas::idx_t i = 0; i < mesh.edges().size(); ++i) {
      output["edge_ids"].push_back(ids(i));
      output["edge_vertices"].push_back({endpoints(i, 0), endpoints(i, 1)});
      output["edge_cells"].push_back({adjacency(i, 0), adjacency(i, 1)});
      output["edge_coordinates"].push_back({ell(i, 0), ell(i, 1)});
      output["edge_canonical_normals"].push_back({env(i, 0), env(i, 1), env(i, 2)});
    }
    // Reject before exposing any invalid geometry. The valid snapshot remains
    // unchanged and can be constructed again after every failed attempt.
    int attacks = 0;
    const auto reject = [&](const ijedi::MpasHorizontalSnapshot &wrong) {
      try {
        ijedi::MpasAtlasGeometry bad(wrong);
      } catch (const std::exception &) {
        ++attacks;
        return;
      }
      throw std::runtime_error("malformed snapshot was accepted");
    };
    auto wrong = snapshot;
    wrong.integers["verticesOnCell"][0] = snapshot.vertices;
    reject(wrong);
    wrong = snapshot;
    wrong.integers["indexToCellID"][1] = wrong.integers["indexToCellID"][0];
    reject(wrong);
    wrong = snapshot;
    wrong.integers["verticesOnEdge"][1] = wrong.integers["verticesOnEdge"][0];
    reject(wrong);
    wrong = snapshot;
    wrong.integers["cellsOnEdge"][1] = wrong.integers["cellsOnEdge"][0];
    reject(wrong);
    wrong = snapshot;
    wrong.reals["areaCell"][0] = -1;
    reject(wrong);
    wrong = snapshot;
    wrong.receipt.clear();
    reject(wrong);
    wrong = snapshot;
    wrong.receipt = "not-a-sha256-receipt";
    reject(wrong);
    wrong = snapshot;
    wrong.angleUnits = "degrees";
    reject(wrong);
    wrong = snapshot;
    wrong.lengthUnits = "km";
    reject(wrong);
    wrong = snapshot;
    wrong.areaUnits = "km^2";
    reject(wrong);
    wrong = snapshot;
    wrong.sphereRadiusMetres = std::numeric_limits<double>::quiet_NaN();
    reject(wrong);
    wrong = snapshot;
    wrong.sphereRadiusMetres *= 2.;
    bool wrongRadiusRejected = false;
    try {
      const ijedi::MpasAtlasGeometry wrongRadius(wrong);
      (void)ijedi::AtlasCellMeanEndpoint::fromMpas(wrongRadius);
    } catch (const std::exception &) {
      wrongRadiusRejected = true;
    }
    if (!wrongRadiusRejected) {
      throw std::runtime_error("declared areas accepted the wrong sphere radius");
    }
    ++attacks;
    wrong = snapshot;
    for (size_t k = 0; k < 3; ++k) {
      wrong.reals["edgeNormalVectors"][k] *= -1;
    }
    reject(wrong);
    wrong = snapshot;
    wrong.reals["edgeNormalVectors"][0] = std::numeric_limits<double>::quiet_NaN();
    reject(wrong);
    wrong = snapshot;
    std::reverse(wrong.integers["verticesOnCell"].begin(),
                 wrong.integers["verticesOnCell"].begin() + wrong.integers["nEdgesOnCell"][0]);
    reject(wrong);
    // A valid rigidly rotated geometry with the original quoted receipt must
    // not be allowed to reuse its original point or conservative coefficients.
    rotatedSnapshot.receipt = snapshot.receipt;
    const ijedi::MpasAtlasGeometry forgedIdentity(rotatedSnapshot);
    if (forgedIdentity.atlasInputReceipt() == geometry.atlasInputReceipt()) {
      throw std::runtime_error(
          "Atlas input content receipt missed a source-coordinate substitution");
    }
    const std::string compiler = atlas::Library::instance().gitsha1(40);
    const ijedi::MpasAtlasPointOperator originalPoint(geometry, {0., 90.}, {0., 0.}, compiler);
    bool rejectedPoint = false, rejectedConservative = false;
    try {
      ijedi::MpasAtlasPointOperator bad(forgedIdentity, {0., 90.}, {0., 0.}, compiler,
                                        originalPoint.serializeCache(),
                                        originalPoint.cacheReceipt());
    } catch (const std::exception &) {
      rejectedPoint = true;
    }
    const ijedi::MpasAtlasConservativeOperator originalConservative(geometry, geometry, compiler);
    try {
      ijedi::MpasAtlasConservativeOperator bad(forgedIdentity, geometry, compiler,
                                               originalConservative.serializeCache(),
                                               originalConservative.cacheReceipt());
    } catch (const std::exception &) {
      rejectedConservative = true;
    }
    if (!rejectedPoint || !rejectedConservative) {
      throw std::runtime_error(
          "quoted identity authorized substituted source geometry in an Atlas cache");
    }
    attacks += 2;
    ijedi::MpasAtlasGeometry again(snapshot);
    if (again.atlasToNativeEdges() != geometry.atlasToNativeEdges()) {
      throw std::runtime_error("construction after negative controls changed ordering");
    }
    output["negative_controls"] = attacks;
    std::ofstream stream(argv[2]);
    stream << output.dump(2) << '\n';
    if (!stream) {
      throw std::runtime_error("cannot write Atlas topology manifest");
    }
    std::cout << "native Atlas topology constructed; " << attacks
              << " malformed snapshots rejected\n";
    atlas::finalize();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
