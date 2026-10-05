#!/usr/bin/env python3
"""Require elementwise agreement between OOPS and independent typed point values."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

FIELDS = ("air_pressure", "air_pressure_levels", "air_pressure_at_surface",
          "air_temperature", "dry_air_density", "water_vapor_mixing_ratio_wrt_dry_air",
          "water_vapor_mixing_ratio_wrt_moist_air", "eastward_wind", "northward_wind",
          "height_above_mean_sea_level", "height_above_mean_sea_level_levels",
          "height_above_mean_sea_level_at_surface")


def equal_payload_bits(actual: np.ndarray, expected: np.ndarray) -> np.ndarray:
    # Numeric equality hides signed-zero changes. The declared operation-order
    # contract is an exact binary64 payload contract, including those bits.
    return actual.view("<u8") == expected.view("<u8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--oops", type=Path, required=True)
    parser.add_argument("--direct", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    direct_bytes = args.direct.read_bytes()
    if hashlib.sha256(direct_bytes).hexdigest() != manifest.get("values_sha256"):
        raise RuntimeError("typed direct binary does not match its manifest digest")
    if (
        manifest.get("schema_version") != 1
        or manifest.get("states") != 3
        or manifest.get("field_order") != list(FIELDS)
        or manifest.get("source_size") != 10242
        or manifest.get("target_count") != 256
        or not 0 < manifest.get("nonzeros", 0) <= 3 * manifest.get("target_count", 0)
        or manifest.get("independent_cartesian_vector_basis_checked") is not True
        or manifest.get("wrong_moisture_denominator_rejected") is not True
        or manifest.get("independent_numpy_values_checked", 0) <= 0
        or manifest.get("independent_numpy_rtol") != 2.0e-14
        or manifest.get("independent_numpy_atol") != 2.0e-12
        or manifest.get("independent_numpy_equation_checks_passed") is not True
    ):
        raise RuntimeError("typed direct manifest lacks the independent-oracle contract")
    shapes = {name: [256, 1 if name.endswith("at_surface") else
                     56 if name.endswith("levels") else 55] for name in FIELDS}
    expected_count = 3 * sum(rows * levels for rows, levels in shapes.values())
    if manifest.get("field_shapes") != shapes or manifest.get("value_count") != expected_count:
        raise RuntimeError("typed direct manifest has an incomplete variable/location/level inventory")
    expected = np.fromfile(args.direct, dtype="<f8")
    actual = np.fromfile(args.oops, dtype="<f8")
    if expected.size != manifest["value_count"] or actual.size != expected.size:
        raise RuntimeError(
            f"typed interpolation count mismatch: manifest={manifest['value_count']}, "
            f"direct={expected.size}, OOPS={actual.size}"
        )
    if not np.isfinite(actual).all() or not np.isfinite(expected).all():
        raise RuntimeError("typed interpolation output contains nonfinite values")
    equal = equal_payload_bits(actual, expected)
    if not bool(np.all(equal)):
        mismatch = np.flatnonzero(~equal)
        maximum = float(np.max(np.abs(actual - expected)))
        first = int(mismatch[0])
        raise RuntimeError(
            "typed OOPS/direct interpolation is not elementwise bitwise equal: "
            f"mismatches={mismatch.size}, max_abs={maximum:.17g}, first={first}, "
            f"oops={actual[first]:.17g}, direct={expected[first]:.17g}"
        )
    print(
        json.dumps(
            {
                "elementwise_bitwise_equal": True,
                "states": manifest["states"],
                "fields": len(manifest["field_order"]),
                "targets": manifest["target_count"],
                "values": int(actual.size),
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
