/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "atlas/field.h"
#include "oops/base/FieldSet3D.h"
#include "oops/base/Variables.h"
#include "oops/util/DateTime.h"
#include "oops/util/Duration.h"

#include "ijedi/Python/MpasBackendContext.h"

namespace ijedi {

class Geometry;
class MpasBackendContext;
class MpasStateHandle;

/// One authoritative storage backend for an I-JEDI State.
///
/// The interface is deliberately the narrow surface required by OOPS.  An
/// implementation may expose Atlas fields as views, but it remains responsible
/// for synchronization and there is never a second authoritative copy.
class StateBackend {
 public:
  virtual ~StateBackend() = default;

  virtual std::unique_ptr<StateBackend> clone() const = 0;
  virtual std::unique_ptr<StateBackend> clone(const oops::Variables &) const = 0;
  virtual std::unique_ptr<StateBackend> clone(const Geometry &) const = 0;
  virtual void assign(const StateBackend &) = 0;

  virtual const util::DateTime validTime() const = 0;
  virtual void updateTime(const util::Duration &) = 0;
  virtual const oops::Variables &variables() const = 0;

  virtual void zero() = 0;
  virtual void accumul(double, const StateBackend &) = 0;
  virtual double norm() const = 0;

  virtual bool hasFieldSet() const = 0;
  virtual bool isMpas() const { return false; }
  virtual void advanceModel(const util::Duration &);
  virtual std::string regressionManifest() const;
  virtual std::string continuationManifest() const;
  virtual std::string typedTransformManifest() const;
  virtual std::vector<MpasTypedField> materializeTypedFields(const oops::Variables &) const;
  virtual atlas::FieldSet &fieldSet() = 0;
  virtual const atlas::FieldSet &fieldSet() const = 0;
  virtual void toFieldSet(atlas::FieldSet &) const = 0;
  virtual void fromFieldSet(const atlas::FieldSet &) = 0;

  virtual size_t serialSize() const = 0;
  virtual void serialize(std::vector<double> &) const = 0;
  virtual void deserialize(const std::vector<double> &, size_t &) = 0;
};

/// Atlas-native backend used by the existing FV3, MOM6, GSI-BEC and Atlas
/// geometry paths.
class AtlasStateBackend final : public StateBackend {
 public:
  AtlasStateBackend(const Geometry &, const oops::Variables &, const util::DateTime &,
                    bool initToZero);
  AtlasStateBackend(const Geometry &, const AtlasStateBackend &);
  AtlasStateBackend(const oops::Variables &, const AtlasStateBackend &);
  AtlasStateBackend(const AtlasStateBackend &);

  std::unique_ptr<StateBackend> clone() const override;
  std::unique_ptr<StateBackend> clone(const oops::Variables &) const override;
  std::unique_ptr<StateBackend> clone(const Geometry &) const override;
  void assign(const StateBackend &) override;

  const util::DateTime validTime() const override;
  void updateTime(const util::Duration &) override;
  const oops::Variables &variables() const override;

  void zero() override;
  void accumul(double, const StateBackend &) override;
  double norm() const override;

  bool hasFieldSet() const override { return true; }

  atlas::FieldSet &fieldSet() override;
  const atlas::FieldSet &fieldSet() const override;
  void toFieldSet(atlas::FieldSet &) const override;
  void fromFieldSet(const atlas::FieldSet &) override;

  size_t serialSize() const override;
  void serialize(std::vector<double> &) const override;
  void deserialize(const std::vector<double> &, size_t &) override;

 private:
  static const AtlasStateBackend &checked(const StateBackend &);
  const Geometry &geom_;
  oops::FieldSet3D fields_;
};

/// MPAS-PyTorch backend.  The opaque Python state is the sole numerical
/// authority; no Atlas sidecar is allocated or synchronized.
class MpasStateBackend final : public StateBackend {
 public:
  MpasStateBackend(const Geometry &, const oops::Variables &, const util::DateTime &);
  MpasStateBackend(const MpasStateBackend &);
  MpasStateBackend(const Geometry &, const MpasStateBackend &);
  ~MpasStateBackend() override;

  std::unique_ptr<StateBackend> clone() const override;
  std::unique_ptr<StateBackend> clone(const oops::Variables &) const override;
  std::unique_ptr<StateBackend> clone(const Geometry &) const override;
  void assign(const StateBackend &) override;

  const util::DateTime validTime() const override;
  void updateTime(const util::Duration &) override;
  const oops::Variables &variables() const override;

  void zero() override;
  void accumul(double, const StateBackend &) override;
  double norm() const override;

  bool hasFieldSet() const override { return true; }
  bool isMpas() const override { return true; }
  void advanceModel(const util::Duration &) override;
  std::string regressionManifest() const override;
  std::string continuationManifest() const override;
  std::string typedTransformManifest() const override;
  std::vector<MpasTypedField> materializeTypedFields(const oops::Variables &) const override;
  atlas::FieldSet &fieldSet() override;
  const atlas::FieldSet &fieldSet() const override;
  void toFieldSet(atlas::FieldSet &) const override;
  void fromFieldSet(const atlas::FieldSet &) override;

  size_t serialSize() const override;
  void serialize(std::vector<double> &) const override;
  void deserialize(const std::vector<double> &, size_t &) override;

 private:
  static const MpasStateBackend &checked(const StateBackend &);
  static void requireSameVariables(const oops::Variables &, const oops::Variables &);
  void invalidateViews();
  std::vector<MpasTypedField> stateFields(const oops::Variables &) const;
  void validateViews() const;

  const Geometry &geom_;
  std::shared_ptr<MpasBackendContext> context_;
  std::shared_ptr<MpasStateHandle> state_;
  oops::Variables variables_;
  util::DateTime time_;
  // Generation-bound copies, never numerical authority. Descriptors retain
  // hashes and shapes only; their transferred tensor values are discarded.
  mutable atlas::FieldSet views_;
  mutable std::vector<MpasTypedField> viewDescriptors_;
};

}  // namespace ijedi
