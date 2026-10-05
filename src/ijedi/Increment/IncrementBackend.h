/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <memory>

#include "oops/base/FieldSet3D.h"

namespace ijedi {
class MpasIncrementBackend;

// Exactly one storage authority per Increment. The established Atlas backend
// keeps its existing operations; MPAS has a location- and namespace-typed carrier.
class IncrementBackend {
 public:
  virtual ~IncrementBackend() = default;
  virtual bool isMpas() const = 0;
  virtual oops::FieldSet3D &atlasFields();
  virtual const oops::FieldSet3D &atlasFields() const;
  virtual MpasIncrementBackend &typed();
  virtual const MpasIncrementBackend &typed() const;
};

class AtlasIncrementBackend final : public IncrementBackend {
 public:
  AtlasIncrementBackend(const util::DateTime &time, const eckit::mpi::Comm &comm)
      : fields_(time, comm) {}
  bool isMpas() const override { return false; }
  oops::FieldSet3D &atlasFields() override { return fields_; }
  const oops::FieldSet3D &atlasFields() const override { return fields_; }

 private:
  oops::FieldSet3D fields_;
};
}  // namespace ijedi
