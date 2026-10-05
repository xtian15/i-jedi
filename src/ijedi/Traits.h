#pragma once

#include <string>

#include "ijedi/Geometry/Geometry.h"
#include "ijedi/Increment/Increment.h"
#include "ijedi/Interpolation/LocalInterpolator.h"
#include "ijedi/LinearModel/LinearModel.h"
#include "ijedi/LinearVariableChange/LinearVariableChange.h"
#include "ijedi/Model/Model.h"
#include "ijedi/State/State.h"
#include "ijedi/VariableChange/VariableChange.h"
#include "ijedi/Covariance/ErrorCovariance.h"
#include "ijedi/Geometry/GeometryIterator.h"
#include "ijedi/ModelAux/ModelAuxControl.h"
#include "ijedi/ModelAux/ModelAuxCovariance.h"
#include "ijedi/ModelAux/ModelAuxIncrement.h"
#include "ijedi/ModelData/ModelData.h"

namespace ufo {
template<typename ITERATOR> class ObsLocalization;
}

namespace ijedi
{

  struct Traits
  {
    static std::string name() { return "ijedi"; }
    static std::string nameCovar() { return "ijediError"; }

    typedef ijedi::Geometry                  Geometry;
    typedef ijedi::GeometryIterator          GeometryIterator;
    typedef ijedi::State                     State;
    typedef ijedi::Increment                 Increment;
    typedef ijedi::ModelData                 ModelData;
    typedef ijedi::VariableChange            VariableChange;
    typedef ijedi::LinearVariableChange      LinearVariableChange;
    typedef ijedi::Model                     Model;
    typedef ijedi::LinearModel               LinearModel;
    typedef ijedi::ErrorCovariance           Covariance;
    typedef ijedi::ModelAuxControl           ModelAuxControl;
    typedef ijedi::ModelAuxIncrement         ModelAuxIncrement;
    typedef ijedi::ModelAuxCovariance        ModelAuxCovariance;
    typedef ijedi::LocalInterpolator          LocalInterpolator;
    typedef ufo::ObsLocalization<ijedi::GeometryIterator> ObsLocalization;
  };

  struct TraitsAtm
  {
    // The name here is not meaningful because ijedi supports both atmosphere
    // and ocean models. This name is used in yamls for coupled applications, and
    // "atmosphere" and "ocean" are chosen solely for user-friendliness
    static std::string name() { return "atmosphere"; }
    static std::string nameCovar() { return "ijediError"; }

    typedef ijedi::Geometry                  Geometry;
    typedef ijedi::GeometryIterator          GeometryIterator;
    typedef ijedi::State                     State;
    typedef ijedi::Increment                 Increment;
    typedef ijedi::ModelData                 ModelData;
    typedef ijedi::VariableChange            VariableChange;
    typedef ijedi::LinearVariableChange      LinearVariableChange;
    typedef ijedi::Model                     Model;
    typedef ijedi::LinearModel               LinearModel;
    typedef ijedi::ErrorCovariance           Covariance;
    typedef ijedi::ModelAuxControl           ModelAuxControl;
    typedef ijedi::ModelAuxIncrement         ModelAuxIncrement;
    typedef ijedi::ModelAuxCovariance        ModelAuxCovariance;
    typedef ijedi::LocalInterpolator          LocalInterpolator;
    typedef ufo::ObsLocalization<ijedi::GeometryIterator> ObsLocalization;
  };

  struct TraitsOcn
  {
    static std::string name() { return "ocean"; }
    static std::string nameCovar() { return "ijediError"; }

    typedef ijedi::Geometry                  Geometry;
    typedef ijedi::GeometryIterator          GeometryIterator;
    typedef ijedi::State                     State;
    typedef ijedi::Increment                 Increment;
    typedef ijedi::ModelData                 ModelData;
    typedef ijedi::VariableChange            VariableChange;
    typedef ijedi::LinearVariableChange      LinearVariableChange;
    typedef ijedi::Model                     Model;
    typedef ijedi::LinearModel               LinearModel;
    typedef ijedi::ErrorCovariance           Covariance;
    typedef ijedi::ModelAuxControl           ModelAuxControl;
    typedef ijedi::ModelAuxIncrement         ModelAuxIncrement;
    typedef ijedi::ModelAuxCovariance        ModelAuxCovariance;
    typedef ijedi::LocalInterpolator          LocalInterpolator;
    typedef ufo::ObsLocalization<ijedi::GeometryIterator> ObsLocalization;
  };

}  // namespace ijedi
