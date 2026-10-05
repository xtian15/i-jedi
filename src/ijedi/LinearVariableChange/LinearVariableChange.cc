/*
 * (C) Copyright 2026 UCAR
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 */

#include "ijedi/LinearVariableChange/LinearVariableChange.h"

#include <vector>
#include <memory>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <nlohmann/json.hpp>

#include "atlas/array.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/Increment/MpasIncrementBackend.h"
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
    if (!fields.has(variable.name())) {
      missing.push_back(variable);
    }
  }
  if (missing.empty()) {
    return;
  }

  const oops::Variables missingVariables(missing);
  atlas::FieldSet zeros =
      util::createFieldSet(geometry.functionSpace(), geometry.variableSizes(missingVariables),
                           missingVariables.variables());
  for (auto &field : zeros) {
    atlas::array::make_view<double, 2>(field).assign(0.0);
    field.metadata().set("interp_type", "default");
    fields.add(field);
  }
}

}  // namespace

LinearVariableChange::LinearVariableChange(const Geometry &geometry,
                                           const eckit::Configuration &varchangeConfig)
    : geom_(geometry) {
  if (geom_.isMpas()) {
    if (varchangeConfig.has("vader custom cookbook")) {
      throw eckit::BadParameter("MPAS uses its model-owned transform graph, not Vader recipes",
                                Here());
    }
    return;
  }
  eckit::LocalConfiguration configCookbook{};
  // If config has "vader custom cookbook" then use that, else use default
  if (varchangeConfig.has("vader custom cookbook")) {
    configCookbook = varchangeConfig.getSubConfiguration("vader custom cookbook");
  } else {
    const auto cb = ijedi::vaderDefaultCookbook();
    for (const auto &[key, val] : cb) {
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

LinearVariableChange::~LinearVariableChange() = default;

void LinearVariableChange::changeVarTraj(const State &xx, const oops::Variables &vars) {
  if (geom_.isMpas()) {
    (void)geom_.variableSizes(vars);
    if (!xx.isMpas() ||
        xx.geometry().mpasContext()->geometryReceipt() != geom_.mpasContext()->geometryReceipt() ||
        xx.geometry().mpasContext()->configurationReceipt() !=
            geom_.mpasContext()->configurationReceipt() ||
        xx.geometry().mpasContext()->stateSchemaDigest() !=
            geom_.mpasContext()->stateSchemaDigest()) {
      throw eckit::BadParameter(
          "MPAS trajectory belongs to a different geometry/configuration/schema", Here());
    }
    const auto expanded =
        MpasIncrementBackend::expandVectorDependencies(*geom_.mpasContext(), vars);
    (void)xx.materializeTypedFields(expanded);
    mpasTrajectory_ = std::make_unique<State>(xx);
    return;
  }
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
  if (increment.isMpas()) {
    const auto values = increment.analysisArrays();
    const auto expanded =
        MpasIncrementBackend::expandVectorDependencies(*geom_.mpasContext(), vars);
    MpasAnalysisArrays selected;
    for (const auto &var : expanded) {
      const auto key =
          increment.analysisNamespace() == "control" ? var.name().substr(8) : var.name();
      selected.emplace(key, values.at(key));
    }
    increment.replaceAnalysis(vars, selected);
    return;
  }
  atlas::FieldSet selected;
  for (const auto &var : vars) {
    if (!increment.fieldSet().has(var.name())) {
      throw eckit::BadValue("Linear variable change did not produce '" + var.name() + "'", Here());
    }
    selected.add(increment.fieldSet().field(var.name()));
  }
  increment.fromFieldSet(selected);
}

void LinearVariableChange::changeVarTL(Increment &increment, const oops::Variables &vars) const {
  if (geom_.isMpas()) {
    requireMpasTrajectory(increment, vars);
    if (vars <= increment.variables()) {
      selectVariables(increment, vars);
      return;
    }
    const auto outputSpace = MpasIncrementBackend::namespaceFor(*geom_.mpasContext(), vars);
    const auto inputSpace = increment.analysisNamespace();
    if (outputSpace == inputSpace && inputSpace != "geoval") {
      const auto complete = increment.completeAnalysisArrays();
      MpasAnalysisArrays selected;
      for (const auto &name : vars.variables()) {
        const auto raw = inputSpace == "control" ? name.substr(8) : name;
        selected.emplace(raw, complete.at(raw));
      }
      increment.replaceAnalysis(vars, selected);
      return;
    }
    if (outputSpace == "control") {
      throw eckit::BadParameter("MPAS TL has no native-to-control inverse map", Here());
    }
    auto native = increment.completeAnalysisArrays();
    if (increment.analysisNamespace() == "control") {
      native = mpasTrajectory_->controlToNative(native);
    } else if (increment.analysisNamespace() != "native") {
      throw eckit::BadParameter("MPAS TL input must be native or control", Here());
    }
    const auto expanded =
        MpasIncrementBackend::expandVectorDependencies(*geom_.mpasContext(), vars);
    if (outputSpace == "native") {
      MpasAnalysisArrays selected;
      for (const auto &name : vars.variables()) {
        selected.emplace(name, native.at(name));
      }
      increment.replaceAnalysis(vars, selected);
    } else {
      const auto result = mpasTrajectory_->nativeGeovalJvp(native, expanded);
      increment.replaceAnalysis(vars, result);
    }
    return;
  }
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

void LinearVariableChange::changeVarAD(Increment &increment, const oops::Variables &vars) const {
  if (geom_.isMpas()) {
    requireMpasTrajectory(increment, vars);
    if (vars <= increment.variables()) {
      selectVariables(increment, vars);
      return;
    }
    const auto inputSpace = increment.analysisNamespace();
    const auto outputSpace = MpasIncrementBackend::namespaceFor(*geom_.mpasContext(), vars);
    if (outputSpace == inputSpace && inputSpace != "geoval") {
      const auto complete = increment.completeAnalysisArrays();
      MpasAnalysisArrays selected;
      for (const auto &name : vars.variables()) {
        const auto raw = inputSpace == "control" ? name.substr(8) : name;
        selected.emplace(raw, complete.at(raw));
      }
      increment.replaceAnalysis(vars, selected);
      return;
    }
    if ((inputSpace != "geoval" && inputSpace != "native") || outputSpace == "geoval" ||
        (inputSpace == "native" && outputSpace != "control")) {
      throw eckit::BadParameter("MPAS AD has no declared map for these spaces", Here());
    }
    const auto names = vars.variables();
    if (names.empty()) {
      throw eckit::BadParameter("MPAS AD output inventory cannot be empty", Here());
    }
    const bool controlOutput = names.front().rfind("control_", 0) == 0;
    std::map<std::string, std::string> outputMetrics;
    std::ostringstream date;
    date << increment.validTime();
    for (const auto &name : names) {
      if ((name.rfind("control_", 0) == 0) != controlOutput) {
        throw eckit::BadParameter("MPAS AD output cannot mix control/native namespaces", Here());
      }
      const auto binding = nlohmann::json::parse(geom_.mpasContext()->variableBinding(
          name, controlOutput ? "control" : "native", date.str()));
      const auto &descriptor = binding.at("descriptor");
      outputMetrics[name] =
          descriptor.at("horizontal_location") == "edge"
              ? "edge_layer"
              : (descriptor.at("vertical_stagger") == "interface" ? "cell_interface"
                                                                  : "cell_layer");
    }
    auto seeds = inputSpace == "native" ? increment.completeAnalysisArrays()
                                        : increment.analysisArrays();
    const auto masses = increment.analysisMeasures();
    for (const auto &[name, metric] : masses) {
      auto &array = seeds.at(name);
      const auto &mass = metric.values;
      if (mass.empty() || array.values.size() % mass.size() != 0) {
        throw eckit::BadValue("MPAS AD metric does not match the seed tensor", Here());
      }
      const size_t repeat = array.values.size() / mass.size();
      for (size_t i = 0; i < array.values.size(); ++i) {
        array.values[i] *= mass.at(i / repeat);
      }
    }
    auto native = inputSpace == "native"
                      ? std::move(seeds)
                      : mpasTrajectory_->nativeGeovalVjp(
                            seeds, increment.analysisCarriedVariables());
    if (controlOutput) {
      native = mpasTrajectory_->nativeCovectorsToControl(native);
    }
    MpasAnalysisArrays selected;
    const auto nativeMass = geom_.mpasContext()->analysisMeasures();
    for (const auto &name : names) {
      auto array = native.at(controlOutput ? name.substr(8) : name);
      if (!controlOutput) {
        const auto &mass = nativeMass.at(outputMetrics.at(name)).values;
        const size_t repeat = array.values.size() / mass.size();
        for (size_t i = 0; i < array.values.size(); ++i) {
          array.values[i] /= mass.at(i / repeat);
        }
      }
      selected.emplace(controlOutput ? name.substr(8) : name, std::move(array));
    }
    increment.replaceAnalysis(vars, selected);
    return;
  }
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
  if (geom_.isMpas()) {
    mpasInverseIdentity(increment, vars);
    return;
  }
  if (vars <= increment.variables()) {
    selectVariables(increment, vars);
    return;
  }
  throw eckit::NotImplemented("I-JEDI inverse TL variable change is not implemented", Here());
}

void LinearVariableChange::changeVarInverseAD(Increment &increment,
                                              const oops::Variables &vars) const {
  if (geom_.isMpas()) {
    mpasInverseIdentity(increment, vars);
    return;
  }
  if (vars <= increment.variables()) {
    selectVariables(increment, vars);
    return;
  }
  throw eckit::NotImplemented("I-JEDI inverse AD variable change is not implemented", Here());
}

void LinearVariableChange::requireMpasTrajectory(const Increment &increment,
                                                 const oops::Variables &vars) const {
  (void)geom_.variableSizes(vars);
  if (!mpasTrajectory_ || !increment.isMpas() ||
      increment.validTime() != mpasTrajectory_->validTime() ||
      increment.geometry().mpasContext()->geometryReceipt() !=
          geom_.mpasContext()->geometryReceipt() ||
      increment.geometry().mpasContext()->configurationReceipt() !=
          geom_.mpasContext()->configurationReceipt() ||
      increment.geometry().mpasContext()->stateSchemaDigest() !=
          geom_.mpasContext()->stateSchemaDigest()) {
    throw eckit::BadParameter("MPAS linear transform has no matching owned trajectory/time/support",
                              Here());
  }
}

void LinearVariableChange::mpasInverseIdentity(Increment &increment,
                                               const oops::Variables &vars) const {
  requireMpasTrajectory(increment, vars);
  // The only supported MPAS inverse is exact identity on the same visible,
  // ordered typed inventory. Selection is rectangular and is not an inverse.
  if (vars != increment.variables() || vars.variables() != increment.variables().variables()) {
    throw eckit::NotImplemented("MPAS inverse variable change supports exact typed identity only",
                                Here());
  }
  (void)increment.analysisArrays();
}

}  // namespace ijedi
