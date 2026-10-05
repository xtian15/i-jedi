/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include "atlas/field/FieldSet.h"

namespace ijedi {
// Give the allocation its own collection handle, sharing only the existing
// numerical fields. The pinned Atlas FieldSet tracks all observer registrations,
// including members inserted and displaced after an alias is returned.
inline atlas::FieldSet ownMpasFieldSet(const atlas::FieldSet &source) {
  atlas::FieldSet result;
  result.name() = source.name();
  result.metadata() = source.metadata();
  result.add(source);
  return result;
}

}  // namespace ijedi
