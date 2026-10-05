/*
 * (C) Copyright 2026 UCAR
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0
 * which can be obtained at http://www.apache.org/licenses/LICENSE-2.0.
 */

#include "ijedi/VariableChange/VariableChange.h"

#include "eckit/config/Configuration.h"
#include "eckit/config/LocalConfiguration.h"
#include "ijedi/VariableChange/VaderCookbook.h"
#include "ijedi/Geometry/Geometry.h"
#include "ijedi/State/State.h"
#include "ijedi/ModelData/ModelData.h"
#include "oops/base/Variables.h"
#include "oops/util/Logger.h"
#include "vader/vader.h"

namespace ijedi {

VariableChange::VariableChange(const eckit::Configuration &varchangeConfig,
                               const Geometry &geometry)
    : geom_(geometry) {
  oops::Log::trace() << "ijedi::VariableChange::VariableChange starting" << std::endl;
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
  varchange_ = std::make_unique<vader::Vader>(parameters, config);
  oops::Log::trace() << "ijedi::VariableChange::VariableChange done" << std::endl;
}

void VariableChange::changeVar(State &xx, const oops::Variables &vars) const {
  oops::Log::trace() << "ijedi::VariableChange::changeVar starting" << std::endl;
  if (geom_.isMpas()) {
    if (!xx.isMpas() ||
        xx.geometry().mpasContext()->geometryReceipt() != geom_.mpasContext()->geometryReceipt() ||
        xx.geometry().mpasContext()->configurationReceipt() !=
            geom_.mpasContext()->configurationReceipt() ||
        xx.geometry().mpasContext()->stateSchemaDigest() !=
            geom_.mpasContext()->stateSchemaDigest()) {
      throw eckit::BadParameter("MPAS variable request belongs to different owner/support", Here());
    }
    // Select versioned views while retaining the one exact native authority.
    State selected(vars, xx);
    xx = selected;
    return;
  }

  // Several Vader recipes (e.g. SeaWaterTemperature_A) require geometry-sourced
  // ingredient fields (latitude, longitude, sea_area_fraction) that are not
  // state variables. Inject them transiently into the working FieldSet;
  // The I-JEDI Vader wrapper filters its output back down to the requested
  // variables, so they do not persist in xx. Skip injection when no transform
  // is needed (the identity path returns early, which would otherwise leave the
  // fields in the state).
  if (!(vars == xx.variables())) {
    geom_.addVaderIngredients(xx.fieldSet());
  }

  if (vars <= xx.variables()) {
    State selected(vars, xx);
    xx = selected;
  } else {
    oops::Variables missing(vars);
    missing -= xx.variables();
    atlas::FieldSet fields;
    xx.toFieldSet(fields);
    const oops::Variables populated = varchange_->changeVar(fields, missing);
    oops::Variables available(xx.variables());
    available += populated;
    if (!(vars <= available)) {
      throw eckit::BadValue("Vader could not produce all requested I-JEDI variables", Here());
    }
    atlas::FieldSet selected;
    for (const auto &var : vars) {
      selected.add(fields.field(var.name()));
    }
    xx.fromFieldSet(selected);
  }
  xx.setAtlasFieldMetadata();
  oops::Log::trace() << "ijedi::VariableChange::changeVar done" << std::endl;
}

void VariableChange::changeVarInverse(State &xx, const oops::Variables &vars) const {
  throw eckit::NotImplemented("ijedi::VariableChange::changeVarInverse is not implemented", Here());
}

void VariableChange::print(std::ostream &os) const {}

}  // namespace ijedi
