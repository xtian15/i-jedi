/*
 * (C) Copyright 2026 IC Weather LLC
 *
 * This software is licensed under the terms of the Apache Licence Version 2.0.
 */

#pragma once

#include <cstddef>
#include <iterator>
#include <ostream>
#include <string>

#include "eckit/geometry/Point3.h"
#include "oops/util/ObjectCounter.h"
#include "oops/util/Printable.h"

namespace ijedi {

class Geometry;

class GeometryIterator : public util::Printable,
                         private util::ObjectCounter<GeometryIterator> {
 public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = eckit::geometry::Point3;
  using difference_type = std::ptrdiff_t;
  using reference = eckit::geometry::Point3;
  using pointer = eckit::geometry::Point3 *;

  static const std::string classname() { return "ijedi::GeometryIterator"; }

  GeometryIterator(const Geometry &, size_t node, size_t level);
  GeometryIterator(const GeometryIterator &) = default;

  bool operator==(const GeometryIterator &) const;
  bool operator!=(const GeometryIterator &other) const { return !(*this == other); }
  eckit::geometry::Point3 operator*() const;
  GeometryIterator &operator++();

  size_t nodeIndex() const { return node_; }
  size_t levelIndex() const { return level_; }

 private:
  void print(std::ostream &) const override;
  const Geometry &geom_;
  size_t node_;
  size_t level_;
};

}  // namespace ijedi
