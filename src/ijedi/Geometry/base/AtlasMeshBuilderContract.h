/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */
#pragma once

#include "atlas/array.h"
#include "atlas/mesh.h"
#include "eckit/exception/Exceptions.h"

namespace ijedi {

// MeshBuilder populates ghost() but Atlas 0.46 NodeColumns reductions use
// the GHOST topology bit. Bind both to the supplied ownership before halo setup.
inline void bindMeshBuilderOwnership(atlas::Mesh &mesh) {
  const auto ghost = atlas::array::make_view<int, 1>(mesh.nodes().ghost());
  auto flags = atlas::array::make_view<int, 1>(mesh.nodes().flags());
  for (atlas::idx_t node = 0; node < mesh.nodes().size(); ++node) {
    if (ghost(node) != 0 && ghost(node) != 1) {
      throw eckit::BadValue("MeshBuilder ownership must be zero or one", Here());
    }
    const int bit = atlas::mesh::Nodes::Topology::GHOST;
    flags(node) = ghost(node) ? flags(node) | bit : flags(node) & ~bit;
  }
}

}  // namespace ijedi
