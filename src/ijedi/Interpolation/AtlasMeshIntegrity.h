/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <string>
#include <functional>
#include <vector>

#include "atlas/functionspace.h"
#include "atlas/mesh.h"

namespace ijedi {

// Atlas const handles still permit mutable field views. Bind the actual
// compiler/executor inputs, not merely the snapshot used to construct them.
// Shares ownership with operators so validation survives Geometry destruction.
// Concurrent mutation through external Atlas handles is unsupported.
class AtlasMeshIntegrity {
 public:
  AtlasMeshIntegrity(std::vector<atlas::Mesh>, std::vector<atlas::FunctionSpace>);
  void validate() const;

 private:
  void bindStorage();
  const std::vector<atlas::Mesh> meshes_;
  const std::vector<atlas::FunctionSpace> spaces_;
  // Private exact byte snapshots, not mutable aliases or probabilistic hashes.
  // Each check reacquires the current field/connectivity before comparing.
  std::vector<std::function<void()>> checks_;
};

}  // namespace ijedi
