/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#include "ijedi/Geometry/GeometryIterator.h"

#include "atlas/array.h"
#include "ijedi/Geometry/Geometry.h"

namespace ijedi {

GeometryIterator::GeometryIterator(const Geometry &geom, size_t node, size_t level)
    : geom_(geom), node_(node), level_(level) {
  if (node_ < geom_.ownedNodeIndices().size()) geom_.validateIterationBoundary();
}

bool GeometryIterator::operator==(const GeometryIterator &other) const {
  return &geom_ == &other.geom_ && node_ == other.node_ && level_ == other.level_;
}

eckit::geometry::Point3 GeometryIterator::operator*() const {
  return geom_.iteratorPoint(node_, level_);
}

GeometryIterator &GeometryIterator::operator++() {
  if (geom_.iteratorDimension() == 3 && level_ + 1 < geom_.iteratorLevels()) {
    ++level_;
  } else {
    ++node_;
    level_ = 0;
  }
  if (node_ == geom_.ownedNodeIndices().size()) geom_.validateIterationBoundary();
  return *this;
}

void GeometryIterator::print(std::ostream &os) const {
  if (node_ < geom_.ownedNodeIndices().size()) {
    os << **this;
  } else {
    os << "GeometryIterator(end)";
  }
}

}  // namespace ijedi
