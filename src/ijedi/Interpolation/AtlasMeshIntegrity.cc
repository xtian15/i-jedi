/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Interpolation/AtlasMeshIntegrity.h"

#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "atlas/array.h"
#include "atlas/mesh/HybridElements.h"
#include "atlas/mesh/Nodes.h"
#include "atlas/mesh/actions/BuildCellCentres.h"
#include "atlas/mesh/actions/BuildXYZField.h"
#include "atlas/mesh/actions/BuildEdges.h"
#include "atlas/mesh/actions/BuildNode2CellConnectivity.h"
#include "atlas/util/Config.h"

namespace ijedi {
namespace {

using Checks = std::vector<std::function<void()>>;

[[noreturn]] void changed(const std::string &name) {
  throw std::invalid_argument(
      "Atlas geometry storage changed after authenticated construction: " + name);
}

// Bind logical elements only: padding may be uninitialized. Check layout before
// any byte access. Exact bytes also bind signed zero and NaN payloads; neither
// floating-point equality nor an array version counter has that property.
void bindField(Checks &checks, const std::string &name,
               std::function<atlas::Field()> get) {
  const auto field = get();
  if (field.rank() != 1 && field.rank() != 2) {
    throw std::invalid_argument("Atlas geometry field has unsupported rank: " + name);
  }
  const auto shape = field.array().shape();
  const auto strides = field.array().strides();
  const auto datatype = field.datatype();
  const auto fieldName = field.name();
  const size_t width = datatype.size();
  const size_t rows = shape[0];
  const size_t columns = field.rank() == 1 ? 1 : shape[1];
  std::vector<unsigned char> bytes(rows * columns * width);
  const auto *data = static_cast<const unsigned char *>(field.array().data());
  for (size_t row = 0; row < rows; ++row) {
    for (size_t col = 0; col < columns; ++col) {
      const size_t offset = row * strides[0] + (field.rank() == 1 ? 0 : col * strides[1]);
      std::memcpy(bytes.data() + (row * columns + col) * width, data + offset * width, width);
    }
  }
  checks.emplace_back([get, name, shape, strides, datatype, fieldName, width, rows, columns,
                        bytes = std::move(bytes)] {
    const auto current = get();
    if (current.name() != fieldName || current.datatype() != datatype ||
        current.array().shape() != shape || current.array().strides() != strides) {
      changed(name + "/layout");
    }
    const auto *now = static_cast<const unsigned char *>(current.array().data());
    if (current.array().contiguous()) {
      if (!bytes.empty() && std::memcmp(now, bytes.data(), bytes.size()) != 0) changed(name);
    } else {
      for (size_t row = 0; row < rows; ++row) {
        if (current.rank() == 1 || strides[1] == 1) {
          if (std::memcmp(now + row * strides[0] * width,
                          bytes.data() + row * columns * width, columns * width) != 0) {
            changed(name);
          }
        } else {
          for (size_t col = 0; col < columns; ++col) {
            if (std::memcmp(now + (row * strides[0] + col * strides[1]) * width,
                            bytes.data() + (row * columns + col) * width, width) != 0) {
              changed(name);
            }
          }
        }
      }
    }
  });
}

template <typename Get>
void bindValue(Checks &checks, const std::string &name, Get get) {
  const auto value = get();
  checks.emplace_back([get, value, name] {
    if (get() != value) changed(name);
  });
}

// Compare Atlas' native blocks. Reacquire them each time: retained data pointers
// would miss a connectivity replacement and could become dangling.
void bindElements(Checks &checks, const std::string &name,
                  std::function<const atlas::mesh::HybridElements &()> get) {
  bindValue(checks, name + "/size", [get] { return get().size(); });
  for (const std::string field : {"glb_idx", "partition", "halo", "flags", "remote_idx"}) {
    bindField(checks, name + "/" + field, [get, field] { return get().field(field); });
  }
  for (int relation : {0, 1, 2}) {
    const auto connectivity = [get, relation]() -> const atlas::mesh::MultiBlockConnectivity & {
      if (relation == 1) return get().cell_connectivity();
      if (relation == 2) return get().edge_connectivity();
      return get().node_connectivity();
    };
    const auto blocks = connectivity().blocks();
    bindValue(checks, name + "/relation-name", [connectivity] { return connectivity().name(); });
    bindValue(checks, name + "/missing", [connectivity] {
      return connectivity().missing_value();
    });
    bindValue(checks, name + "/size", [connectivity] { return connectivity().size(); });
    bindValue(checks, name + "/blocks", [connectivity] { return connectivity().blocks(); });
    bindValue(checks, name + "/rows", [connectivity] { return connectivity().rows(); });
    for (atlas::idx_t block = 0; block < blocks; ++block) {
      const auto rows = connectivity().block(block).rows();
      const auto cols = connectivity().block(block).cols();
      const auto *data = connectivity().block(block).data();
      const std::vector<atlas::idx_t> values(data, data + rows * cols);
      checks.emplace_back([connectivity, block, rows, cols, values, name] {
        const auto &current = connectivity().block(block);
        if (current.rows() != rows || current.cols() != cols ||
            (!values.empty() &&
             std::memcmp(current.data(), values.data(),
                          values.size() * sizeof(atlas::idx_t)) != 0)) {
          changed(name + "/connectivity");
        }
      });
    }
  }
  for (const std::string field : {"lonlat", "centre ", "mpas_canonical_edge_normal"}) {
    const bool exists = get().has_field(field);
    bindValue(checks, name + "/has-" + field, [get, field] { return get().has_field(field); });
    if (exists) {
      bindField(checks, name + "/" + field, [get, field] { return get().field(field); });
    }
  }
  if (get().has_field("mpas_canonical_edge_normal")) {
    for (const std::string key : {"component_basis", "horizontal_geometry_receipt"}) {
      bindValue(checks, name + "/" + key, [get, key] {
        return get().field("mpas_canonical_edge_normal").metadata().getString(key);
      });
    }
  }
}

void bindNodeConnectivity(Checks &checks, const std::string &name,
                          std::function<const atlas::mesh::IrregularConnectivity &()> get) {
  const auto rows = get().rows();
  const auto connectivityName = get().name();
  std::vector<atlas::idx_t> counts, displacements, values;
  for (atlas::idx_t row = 0; row < rows; ++row) {
    counts.push_back(get().cols(row));
    displacements.push_back(get().displs(row));
    for (atlas::idx_t col = 0; col < counts.back(); ++col) values.push_back(get()(row, col));
  }
  checks.emplace_back([get, name, rows, connectivityName, counts, displacements, values] {
    const auto &current = get();
    if (current.rows() != rows || current.name() != connectivityName) changed(name + "/layout");
    size_t offset = 0;
    for (atlas::idx_t row = 0; row < rows; ++row) {
      if (current.cols(row) != counts[row] || current.displs(row) != displacements[row]) {
        changed(name + "/layout");
      }
      for (atlas::idx_t col = 0; col < counts[row]; ++col) {
        if (current(row, col) != values[offset++]) changed(name);
      }
    }
  });
}

std::string selectedMetadata(const eckit::Configuration &metadata,
                              const std::vector<std::string> &keys) {
  // Encode presence separately. Absence is an input too (e.g. FE fallback).
  std::ostringstream out;
  out.precision(17);
  for (const auto &key : keys) {
    out << key << ':' << metadata.has(key) << ':';
    if (metadata.has(key)) out << metadata.getDouble(key);
    out << ';';
  }
  return out.str();
}

}  // namespace

AtlasMeshIntegrity::AtlasMeshIntegrity(std::vector<atlas::Mesh> meshes,
                                       std::vector<atlas::FunctionSpace> spaces)
    : meshes_(std::move(meshes)), spaces_(std::move(spaces)) {
  for (const auto &space : spaces_) (void)space.lonlat();
  for (auto mesh : meshes_) {
    // Complete the native Atlas adjacency before sealing. Later callers must
    // not materialize an unbound relation under the same topology receipt.
    atlas::mesh::actions::build_node_to_cell_connectivity(mesh);
    atlas::mesh::actions::build_node_to_edge_connectivity(mesh);
    (void)atlas::mesh::actions::BuildXYZField("xyz")(mesh);
    atlas::util::Config config;
    config.set("name", "centre ");
    config.set("flatten_virtual_elements", false);
    (void)atlas::mesh::actions::BuildCellCentres(config)(mesh);
  }
  bindStorage();
}

void AtlasMeshIntegrity::bindStorage() {
  size_t index = 0;
  for (auto mesh : meshes_) {
    const auto prefix = "mesh-" + std::to_string(index++);
    bindValue(checks_, prefix + "/nodes/size", [mesh] { return mesh.nodes().size(); });
    bindNodeConnectivity(checks_, prefix + "/nodes/cells", [mesh]() ->
                         const atlas::mesh::IrregularConnectivity & {
      return mesh.nodes().cell_connectivity();
    });
    bindNodeConnectivity(checks_, prefix + "/nodes/edges", [mesh]() ->
                         const atlas::mesh::IrregularConnectivity & {
      return mesh.nodes().edge_connectivity();
    });
    for (const std::string field : {"lonlat", "xy", "xyz", "glb_idx", "partition",
                                    "ghost", "halo", "flags", "remote_idx"}) {
      // Nodes keeps cached accessors independently of its mutable field map.
      // Authenticate their alias relationship, not just the map's bytes.
      if (field != "xyz") {
        const atlas::Field sealed = mesh.nodes().field(field);
        checks_.emplace_back([mesh, field, prefix, sealed] {
          const auto &nodes = mesh.nodes();
          atlas::Field shortcut;
          if (field == "lonlat") shortcut = nodes.lonlat();
          else if (field == "xy") shortcut = nodes.xy();
          else if (field == "glb_idx") shortcut = nodes.global_index();
          else if (field == "partition") shortcut = nodes.partition();
          else if (field == "ghost") shortcut = nodes.ghost();
          else if (field == "halo") shortcut = nodes.halo();
          else if (field == "flags") shortcut = nodes.flags();
          else
            shortcut = nodes.remote_index();
          if (!nodes.has_field(field) || nodes.field(field).get() != shortcut.get() ||
              shortcut.get() != sealed.get()) {
            changed(prefix + "/nodes/" + field + "/shortcut-alias");
          }
        });
      }
      bindField(checks_, prefix + "/nodes/" + field,
                [mesh, field] { return mesh.nodes().field(field); });
    }
    bindElements(checks_, prefix + "/cells", [mesh]() -> const atlas::mesh::HybridElements & {
      return mesh.cells();
    });
    bindElements(checks_, prefix + "/edges", [mesh]() -> const atlas::mesh::HybridElements & {
      return mesh.edges();
    });
    bindValue(checks_, prefix + "/compiler-metadata", [mesh] {
      return selectedMetadata(mesh.metadata(), {"cell_maximum_diagonal_on_unit_sphere", "halo"});
    });
    bindValue(checks_, prefix + "/node-metadata", [mesh] {
      return selectedMetadata(mesh.nodes().metadata(), {"NbRealPts"});
    });
  }
  index = 0;
  for (const auto &space : spaces_) {
    const auto prefix = "space-" + std::to_string(index++);
    bindValue(checks_, prefix + "/type", [space] { return space.type(); });
    bindValue(checks_, prefix + "/size", [space] { return space.size(); });
    bindField(checks_, prefix + "/lonlat", [space] { return space.lonlat(); });
  }
}

void AtlasMeshIntegrity::validate() const {
  for (const auto &check : checks_) check();
}

}  // namespace ijedi
