/*
 * (C) Copyright 2026 UCAR
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 */

#include "ijedi/LinearVariableChange/LinearVariableChange.h"

#include <vector>

#include "atlas/array.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/ModelData/ModelData.h"
#include "ijedi/State/State.h"
#include "ijedi/VariableChange/VaderCookbook.h"
#include "oops/base/Variables.h"
#include "oops/util/FieldSetHelpers.h"

namespace ijedi {
namespace {

void addMissingAdjointInputs(const Geometry &geometry, const oops::Variables &variables,
                             atlas::FieldSet &fields) {
  std::vector<oops::Variable> missing;
  for (const auto &variable : variables) {
    if (!fields.has(variable.name())) missing.push_back(variable);
  }
  if (missing.empty()) return;

  const oops::Variables missingVariables(missing);
  atlas::FieldSet zeros = util::createFieldSet(
      geometry.functionSpace(), geometry.variableSizes(missingVariables),
      missingVariables.variables());
  for (auto &field : zeros) {
    atlas::array::make_view<double, 2>(field).assign(0.0);
    field.metadata().set("interp_type", "default");
    fields.add(field);
  }
}

}  // namespace

LinearVariableChange::LinearVariableChange(const Geometry & geometry,
                                           const eckit::Configuration & varchangeConfig)
    : geom_(geometry) {
  if (geom_.isMpas()) {
    throw eckit::NotImplemented(
        "MPAS linear variable changes require the stacked variable-transform PR", Here());
  }
  eckit::LocalConfiguration configCookbook{};
  // If config has "vader custom cookbook" then use that, else use default
  if (varchangeConfig.has("vader custom cookbook")) {
    configCookbook = varchangeConfig.getSubConfiguration("vader custom cookbook");
  } else {
    const auto cb = ijedi::vaderDefaultCookbook();
    for (const auto & [key, val] : cb) {
      configCookbook.set(key, val);
    }
  }
  eckit::LocalConfiguration config{};
  config.set(vader::configCookbookKey, configCookbook);
  // Set up model data for cookbook
  auto configModelData = ModelData(geometry).modelData();
  config.set(vader::configModelVarsKey, configModelData);

  vader::VaderParameters parameters;
  vader_ = std::make_unique<vader::Vader>(parameters, config);
}

void LinearVariableChange::changeVarTraj(const State & xx, const oops::Variables & vars) {
  // Some Vader recipes need geometry-sourced ingredient fields that are not
  // state variables; inject them into a working copy of the trajectory before
  // setting the linearization point.
  State traj(xx);
  geom_.addVaderIngredients(traj.fieldSet());
  atlas::FieldSet fields;
  traj.toFieldSet(fields);
  vader_->changeVarTraj(fields, vars);
}

void LinearVariableChange::ensureLinearPlan(oops::Variables &ingredients) const {
  if (vader_->needsTLADInit()) {
    const oops::Variables original(ingredients);
    varsVaderPopulates_ = vader_->initTLAD(ingredients);
    varsVaderPopulates_ -= original;
  }
}

void LinearVariableChange::selectVariables(Increment &increment,
                                           const oops::Variables &vars) const {
  atlas::FieldSet selected;
  for (const auto &var : vars) {
    if (!increment.fieldSet().has(var.name())) {
      throw eckit::BadValue("Linear variable change did not produce '" + var.name() + "'",
                            Here());
    }
    selected.add(increment.fieldSet().field(var.name()));
  }
  increment.fromFieldSet(selected);
}

void LinearVariableChange::changeVarTL(Increment &increment,
                                       const oops::Variables &vars) const {
  if (vars <= increment.variables()) {
    selectVariables(increment, vars);
    return;
  }
  oops::Variables ingredients(increment.variables());
  ensureLinearPlan(ingredients);
  atlas::FieldSet fields;
  increment.toFieldSet(fields);
  vader_->changeVarTL(fields);
  increment.fromFieldSet(fields);
  selectVariables(increment, vars);
}

void LinearVariableChange::changeVarAD(Increment &increment,
                                       const oops::Variables &vars) const {
  if (vars <= increment.variables()) {
    selectVariables(increment, vars);
    return;
  }
  oops::Variables ingredients(vars);
  ensureLinearPlan(ingredients);
  atlas::FieldSet fields;
  increment.toFieldSet(fields);
  vader_->changeVarAD(fields);
  // Do not put requested ingredient fields into the output-seed FieldSet
  // before Vader runs.  Vader's AD contract discovers and creates the active
  // ingredients; pre-populating them changes the recipe execution on masked
  // grids.  Only after the reverse pass do we materialize requested inputs
  // absent from the recipe graph, whose adjoints are exactly zero.
  addMissingAdjointInputs(geom_, vars, fields);
  increment.fromFieldSet(fields);
  selectVariables(increment, vars);
}

void LinearVariableChange::changeVarInverseTL(Increment &increment,
                                              const oops::Variables &vars) const {
  if (vars <= increment.variables()) {
    selectVariables(increment, vars);
    return;
  }
  throw eckit::NotImplemented("I-JEDI inverse TL variable change is not implemented", Here());
}

void LinearVariableChange::changeVarInverseAD(Increment &increment,
                                              const oops::Variables &vars) const {
  if (vars <= increment.variables()) {
    selectVariables(increment, vars);
    return;
  }
  throw eckit::NotImplemented("I-JEDI inverse AD variable change is not implemented", Here());
}

}  // namespace ijedi
