/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace ijedi {
class MpasAtlasGeometry;
class AtlasCellMeanEndpoint;

// A physical source -> target cell-mean map. A separately compiled reverse
// physical map is not its adjoint. All execution is through the Atlas matrix.
class MpasAtlasConservativeOperator {
 public:
  MpasAtlasConservativeOperator(const AtlasCellMeanEndpoint &source,
                                const AtlasCellMeanEndpoint &target,
                                const std::string &compilerIdentity);
  MpasAtlasConservativeOperator(const AtlasCellMeanEndpoint &source,
                                const AtlasCellMeanEndpoint &target,
                                const std::string &compilerIdentity, const std::string &payload,
                                const std::string &trustedReceipt);
  MpasAtlasConservativeOperator(const MpasAtlasGeometry &source, const MpasAtlasGeometry &target,
                                const std::string &compilerIdentity);
  MpasAtlasConservativeOperator(const MpasAtlasGeometry &source, const MpasAtlasGeometry &target,
                                const std::string &compilerIdentity, const std::string &payload,
                                const std::string &trustedReceipt);
  ~MpasAtlasConservativeOperator();
  MpasAtlasConservativeOperator(const MpasAtlasConservativeOperator &) = delete;
  MpasAtlasConservativeOperator &operator=(const MpasAtlasConservativeOperator &) = delete;
  size_t sourceSize() const;
  size_t targetSize() const;
  size_t nonzeros() const;
  const std::string &cacheKey() const;
  const std::string &cacheReceipt() const;
  const std::vector<double> &sourceMeasures() const;
  const std::vector<double> &targetMeasures() const;
  double rowResidual() const;
  double columnResidual() const;
  std::string serializeCache() const;
  std::vector<double> apply(const std::vector<double> &, size_t levels) const;
  std::vector<double> applyTranspose(const std::vector<double> &, size_t levels) const;
  std::vector<double> applyWeightedAdjoint(const std::vector<double> &, size_t levels) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace ijedi
