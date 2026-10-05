/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "atlas/array.h"
#include "atlas/library/Library.h"
#include "ijedi/Geometry/mpas/MpasAtlasGeometry.h"
#include "ijedi/Interpolation/AtlasOperatorReceipt.h"
#include "ijedi/Interpolation/MpasAtlasPointOperator.h"
#include "ijedi/Python/MpasBackendContext.h"

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: ijedi_mpas_atlas_execution_costs SNAPSHOT LOCATIONS OUTPUT\n";
    return 2;
  }
  try {
    atlas::initialize(argc, argv);
    Json document, locations;
    std::ifstream(argv[1]) >> document;
    std::ifstream(argv[2]) >> locations;
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
    const ijedi::MpasAtlasGeometry geometry(snapshot);
    const std::string compiler = atlas::Library::instance().gitsha1(40);
    Json report{{"cells", snapshot.cells}, {"atlas_commit", compiler},
                {"horizontal_receipt", snapshot.receipt},
                {"repeats", 5}, {"threads", 1}, {"precision", "float64"}};
    const auto measure = [&](const std::string &name, const std::function<std::vector<double>()> &run) {
      const auto reference = run();  // untimed warmup
      ijedi::AtlasOperatorReceipt digest;
      digest.integer(reference.size());
      for (double value : reference) {
        if (!std::isfinite(value)) throw std::runtime_error("benchmark has nonfinite output");
        digest.real(value);
      }
      report["checksums"][name] = digest.finish();
      for (int repeat = 0; repeat < 5; ++repeat) {
        const auto start = Clock::now();
        const auto result = run();
        const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        if (result != reference) throw std::runtime_error("benchmark output is not repeatable");
        report["seconds"][name].push_back(elapsed);
      }
    };
    measure("guard", [&] { geometry.validateStorage(); return std::vector<double>{}; });
    const auto canonical = atlas::array::make_view<double, 2>(geometry.cellNodes().lonlat());
    for (int repeat = 0; repeat < 5; ++repeat) {
      const auto start = Clock::now();
      geometry.validateStorage();
      ijedi::AtlasOperatorReceipt receipt;
      for (size_t row = 0; row < snapshot.cells; ++row) {
        const auto point = geometry.authenticatedCellCoordinates(row);
        if (point[0] != canonical(row, 0) || point[1] != canonical(row, 1)) {
          throw std::runtime_error("authenticated traversal differs from canonical Atlas coordinates");
        }
        receipt.real(point[0]); receipt.real(point[1]);
      }
      geometry.validateStorage();
      const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
      const std::string digest = receipt.finish();
      if (repeat && report["traversal"]["sha256"] != digest) {
        throw std::runtime_error("authenticated traversal is not repeatable");
      }
      report["traversal"]["sha256"] = digest;
      report["traversal"]["points"] = snapshot.cells;
      report["traversal"]["seconds"].push_back(elapsed);
    }
    for (size_t targets : {size_t{1}, size_t{256}}) {
      std::vector<double> lat, lon;
      if (targets == 1) { lat = {0.}; lon = {0.}; }
      else {
        lat = locations.at("latitude_degrees").get<std::vector<double>>();
        lon = locations.at("longitude_degrees").get<std::vector<double>>();
        if (lat.size() != targets) throw std::runtime_error("benchmark requires 256 ordered targets");
      }
      const auto op = geometry.pointOperator(lat, lon, compiler);
      const std::string prefix = std::to_string(targets) + "/";
      report["cache_receipts"][prefix] = op->cacheReceipt();
      measure(prefix + "cache_hit", [&] {
        if (geometry.pointOperator(lat, lon, compiler) != op) {
          throw std::runtime_error("benchmark cache hit substituted the operator");
        }
        return std::vector<double>{};
      });
      for (size_t levels : {size_t{1}, size_t{55}, size_t{56}}) {
        std::vector<double> x(snapshot.cells * levels), other(x.size()), y(targets * levels);
        for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(static_cast<double>(i) * 0.013);
        for (size_t i = 0; i < x.size(); ++i) other[i] = 2. * x[i] + 1.;
        for (size_t i = 0; i < y.size(); ++i) y[i] = std::cos(static_cast<double>(i) * 0.017);
        const auto label = prefix + std::to_string(levels) + "/";
        measure(label + "scalar", [&] { return op->apply(x, levels); });
        measure(label + "transpose", [&] { return op->applyTranspose(y, levels); });
        measure(label + "vector", [&] {
          const auto value = op->applyVector(x, x, levels);
          auto result = value[0];
          result.insert(result.end(), value[1].begin(), value[1].end());
          return result;
        });
        measure(label + "vector_transpose", [&] {
          const auto value = op->applyVectorTranspose(y, y, levels);
          auto result = value[0];
          result.insert(result.end(), value[1].begin(), value[1].end());
          return result;
        });
        std::vector<std::reference_wrapper<const std::vector<double>>> fields;
        for (size_t field = 0; field < 8; ++field) {
          fields.emplace_back(std::cref(field % 2 ? other : x));
        }
#ifndef IJEDI_AR_BASELINE
        const auto batch = op->applyBatch(fields, std::vector<size_t>(8, levels));
        for (size_t field = 0; field < fields.size(); ++field) {
          if (batch[field] != op->apply(fields[field].get(), levels)) {
            throw std::runtime_error("scalar batch changed Atlas execution");
          }
        }
        std::vector<double> secondSeed(y);
        for (double &value : secondSeed) value = 3. * value + 1.;
        const ijedi::MpasAtlasPointOperator::ScalarFields seeds{std::cref(y), std::cref(secondSeed)};
        const auto adjoints = op->applyBatchTranspose(seeds, {levels, levels});
        if (adjoints.size() != 2 || adjoints[0] != op->applyTranspose(y, levels) ||
            adjoints[1] != op->applyTranspose(secondSeed, levels)) {
          throw std::runtime_error("scalar transpose batch changed Atlas execution");
        }
#endif
        measure(label + "eight_scalars", [&] {
          std::vector<std::vector<double>> outputs;
#ifdef IJEDI_AR_BASELINE
          for (const auto &field : fields) outputs.push_back(op->apply(field.get(), levels));
#else
          outputs = op->applyBatch(fields, std::vector<size_t>(8, levels));
#endif
          std::vector<double> result;
          for (const auto &output : outputs) result.insert(result.end(), output.begin(), output.end());
          return result;
        });
      }
#ifndef IJEDI_AR_BASELINE
      const auto rejects = [](const std::function<void()> &action, const std::string &message) {
        try { action(); }
        catch (const std::invalid_argument &error) {
          if (std::string(error.what()).find(message) != std::string::npos) return;
          throw;
        }
        throw std::runtime_error("batch attack was accepted: " + message);
      };
      std::vector<double> finite(op->sourceSize(), 1.), invalid(finite);
      invalid[0] = std::numeric_limits<double>::quiet_NaN();
      rejects([&] { (void)op->applyBatch({std::cref(finite)}, {}); }, "inventory");
      rejects([&] { (void)op->applyBatch({std::cref(finite), std::cref(invalid)}, {1, 1}); },
              "nonfinite");
      auto coordinates = atlas::array::make_view<double, 2>(geometry.cellNodes().lonlat());
      const double original = coordinates(0, 0);
      coordinates(0, 0) += 0.01;
      try {
        rejects([&] { (void)op->applyBatch({std::cref(finite)}, {1}); }, "geometry storage changed");
        const std::vector<double> seed(op->targetSize(), 1.);
        rejects([&] { (void)op->applyBatchTranspose({std::cref(seed)}, {1}); }, "geometry storage changed");
      } catch (...) { coordinates(0, 0) = original; throw; }
      coordinates(0, 0) = original;
      geometry.validateStorage();
#endif
    }
    std::ofstream output(argv[3]);
    output << report.dump(2) << '\n';
    if (!output) throw std::runtime_error("cannot write execution-cost evidence");
    atlas::finalize();
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
