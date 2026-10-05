/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <string>
#include <memory>
#include <vector>

#include "atlas/functionspace.h"
#include "atlas/grid.h"

namespace ijedi {
class MpasAtlasGeometry;
class AtlasMeshIntegrity;

// An immutable, factory-owned cell-mean endpoint. Measures are in steradians
// on the unit sphere, ordered exactly as its Atlas CellColumns. There is no
// constructor accepting caller-supplied masses or quoted mesh identities.
class AtlasCellMeanEndpoint {
 public:
  static AtlasCellMeanEndpoint fromMpas(const MpasAtlasGeometry &);
  static AtlasCellMeanEndpoint fromAtlasGrid(const atlas::Grid &);
  const atlas::FunctionSpace &space() const { return space_; }
  const std::vector<double> &measures() const { return measures_; }
  const std::string &receipt() const { return receipt_; }
  const std::string &contentReceipt() const { return contentReceipt_; }
  const std::shared_ptr<const AtlasMeshIntegrity> &storageIntegrity() const { return integrity_; }

 private:
  AtlasCellMeanEndpoint(atlas::FunctionSpace, std::vector<double>, std::string, std::string,
                        std::shared_ptr<const AtlasMeshIntegrity>);
  atlas::FunctionSpace space_;
  std::vector<double> measures_;
  std::string receipt_, contentReceipt_;
  std::shared_ptr<const AtlasMeshIntegrity> integrity_;
};
}  // namespace ijedi
