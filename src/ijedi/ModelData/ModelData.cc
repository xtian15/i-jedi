/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/ModelData/ModelData.h"

#include "ijedi/Geometry/Geometry.h"

namespace ijedi {

ModelData::ModelData(const Geometry &geometry) : data_(geometry.modelData()) {}

void ModelData::print(std::ostream &os) const { os << data_; }

}  // namespace ijedi
