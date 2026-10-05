/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ijedi {
class MpasAtlasGeometry;

// Atlas is both the compiler and the execution engine. No independent CSR
// multiply is retained here. Cache restoration authenticates the exact ordered
// coefficients against a receipt supplied by the owner, before using Atlas.
class MpasAtlasPointOperator {
 public:
  MpasAtlasPointOperator(const MpasAtlasGeometry &, const std::vector<double> &latitudes,
                         const std::vector<double> &longitudes,
                         const std::string &compilerIdentity);
  MpasAtlasPointOperator(const MpasAtlasGeometry &, const std::vector<double> &latitudes,
                         const std::vector<double> &longitudes, const std::string &compilerIdentity,
                         const std::string &cachePayload, const std::string &trustedReceipt);
  ~MpasAtlasPointOperator();
  static std::string keyFor(const MpasAtlasGeometry &, const std::vector<double> &latitudes,
                            const std::vector<double> &longitudes,
                            const std::string &compilerIdentity);
  MpasAtlasPointOperator(const MpasAtlasPointOperator &) = delete;
  MpasAtlasPointOperator &operator=(const MpasAtlasPointOperator &) = delete;

  size_t sourceSize() const;
  size_t targetSize() const;
  size_t nonzeros() const;
  const std::string &horizontalReceipt() const;
  const std::string &cacheKey() const;
  const std::string &cacheReceipt() const;
  std::string serializeCache() const;
  std::vector<double> apply(const std::vector<double> &, size_t levels) const;
  std::vector<double> applyTranspose(const std::vector<double> &, size_t levels) const;
  // One synchronous integrity check for a composed multi-field action. No
  // unchecked executor or mutable validation token escapes this class.
  using ScalarFields = std::vector<std::reference_wrapper<const std::vector<double>>>;
  std::vector<std::vector<double>> applyBatch(const ScalarFields &,
                                               const std::vector<size_t> &levels) const;
  std::vector<std::vector<double>> applyBatchTranspose(const ScalarFields &,
                                                        const std::vector<size_t> &levels) const;
  // The model's receipt-owned cell areas define the source metric, with unit
  // index-level measure. Observations use the identity metric (R is a later
  // gate). Runtime callers cannot substitute a different mass vector.
  const std::vector<double> &sourceMeasures() const;
  std::vector<double> applyWeightedAdjoint(const std::vector<double> &, size_t levels) const;
  std::array<std::vector<double>, 2> applyVector(const std::vector<double> &east,
                                                 const std::vector<double> &north,
                                                 size_t levels) const;
  std::array<std::vector<double>, 2> applyVectorTranspose(const std::vector<double> &east,
                                                          const std::vector<double> &north,
                                                          size_t levels) const;
  std::array<std::vector<double>, 2> applyVectorWeightedAdjoint(const std::vector<double> &east,
                                                                const std::vector<double> &north,
                                                                size_t levels) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace ijedi
