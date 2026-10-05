/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <string>
#include <array>
#include <vector>
#include <map>
#include <memory>

#include "atlas/functionspace.h"
#include "atlas/field.h"
#include "atlas/mesh.h"
#include "eckit/thread/Mutex.h"

namespace ijedi {

struct MpasHorizontalSnapshot;
struct MpasStaticVerticalSnapshot;
class MpasAtlasPointOperator;
class AtlasMeshIntegrity;

// Native serial-global Atlas topology. Native MPAS order is retained by dual
// nodes and primal nodes; mixed primal faces and Atlas-built edges have explicit
// bijections. No retriangulation of the primal cells is performed.
class MpasAtlasGeometry {
 public:
  explicit MpasAtlasGeometry(const MpasHorizontalSnapshot &);
  const atlas::Mesh &primalMesh() const { return primal_; }
  const atlas::Mesh &dualMesh() const { return dual_; }
  const atlas::FunctionSpace &cellNodes() const { return cellNodes_; }
  const atlas::FunctionSpace &cellMeans() const { return cellMeans_; }
  const atlas::FunctionSpace &edgeNormals() const { return edgeNormals_; }
  const atlas::FunctionSpace &vertexNodes() const { return vertexNodes_; }
  const std::vector<atlas::idx_t> &nativeToAtlasCells() const { return nativeToCells_; }
  const std::vector<atlas::idx_t> &atlasToNativeCells() const { return cellsToNative_; }
  const std::vector<atlas::idx_t> &nativeToAtlasEdges() const { return nativeToEdges_; }
  const std::vector<atlas::idx_t> &atlasToNativeEdges() const { return edgesToNative_; }
  // Atlas edge-cell side order relative to the MPAS cellsOnEdge[0] -> [1]
  // normal convention. This is not an endpoint-tangent sign.
  const std::vector<int> &edgeCellOrientation() const { return edgeCellOrientation_; }
  const std::vector<double> &cellMeanMeasures() const { return cellMeanMeasures_; }
  double sphereRadiusMetres() const { return sphereRadiusMetres_; }
  const std::vector<double> &cellNodeMeasures() const { return cellNodeMeasures_; }
  const std::string &cellNodeMeasureReceipt() const { return cellNodeMeasureReceipt_; }
  const std::string &receipt() const { return receipt_; }
  // Content binding of the exact snapshot fields consumed by this Atlas
  // adapter, in addition to the model owner's broader horizontal receipt.
  const std::string &atlasInputReceipt() const { return atlasInputReceipt_; }
  const std::shared_ptr<const AtlasMeshIntegrity> &storageIntegrity() const { return integrity_; }
  void validateStorage() const;
  // A traversal validates all storage at entry/completion, then checks each
  // consumed coordinate row in O(1). Never return edited live coordinates.
  std::array<double, 2> authenticatedCellCoordinates(size_t) const;
  // Owned copies on the cell-node space. Editing a view cannot mutate model
  // support or the independent horizontal authority.
  atlas::FieldSet staticVerticalFields(const MpasStaticVerticalSnapshot &) const;
  std::shared_ptr<const MpasAtlasPointOperator> pointOperator(
      const std::vector<double> &latitudes, const std::vector<double> &longitudes,
      const std::string &compilerIdentity) const;

 private:
  atlas::Mesh primal_, dual_;
  atlas::FunctionSpace cellNodes_, cellMeans_, edgeNormals_, vertexNodes_;
  std::vector<atlas::idx_t> nativeToCells_, cellsToNative_, nativeToEdges_, edgesToNative_;
  std::vector<int> edgeCellOrientation_;
  std::vector<double> cellMeanMeasures_;
  double sphereRadiusMetres_ = 0;
  std::vector<double> cellNodeMeasures_;
  std::string cellNodeMeasureReceipt_;
  std::string receipt_;
  std::string atlasInputReceipt_;
  std::shared_ptr<const AtlasMeshIntegrity> integrity_;
  atlas::Field traversalCoordinates_;
  std::vector<double> traversalCoordinateBytes_;
  mutable eckit::Mutex pointCacheMutex_;
  // Geometry-lifetime cache, explicitly bounded without hidden eviction.
  static constexpr size_t pointCacheCapacity_ = 64;
  mutable std::map<std::string, std::shared_ptr<const MpasAtlasPointOperator>> pointCache_;
};

}  // namespace ijedi
