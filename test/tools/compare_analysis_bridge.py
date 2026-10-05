#!/usr/bin/env python3
"""Fail closed on omitted embedded/direct analysis arrays, shapes or bytes."""
import argparse
import json
import math
from decimal import Decimal, localcontext


def compare(embedded, direct):
    required_updates = [dict(generation=i, exact_absolute_assignment=True,
                            identity_continuation=True, zero_increment_continuation=True,
                            atomic_rejections=4) for i in range(3)]
    if embedded.get("native_update_checks") != required_updates:
        raise RuntimeError("analysis bridge omitted exact updates or atomic rejection checks")
    if len(embedded["cases"]) != 12 or len(direct["cases"]) != 12:
        raise RuntimeError("analysis bridge requires all three boundaries and four seeds")
    groups = {"native_values": 5, "measures": 4, "control": 6, "native_directions": 5,
              "geoval_tangents": 12, "native_covectors": 5, "control_adjoint": 6}
    native_shapes = {"u": [30720, 55], "w": [10242, 56], "theta_m": [10242, 55],
                     "rho_zz": [10242, 55], "scalars": [10242, 55, 6]}
    control_shapes = {"eastward_wind": [10242, 55], "northward_wind": [10242, 55],
                      "upward_air_velocity": [10242, 56], "moist_potential_temperature": [10242, 55],
                      "jacobian_dry_air_density": [10242, 55], "moisture_tracers": [10242, 55, 6]}
    geoval_shapes = {name: [10242, 55] for name in (
        "air_pressure", "air_temperature", "dry_air_density", "eastward_wind", "northward_wind",
        "water_vapor_mixing_ratio_wrt_dry_air", "water_vapor_mixing_ratio_wrt_moist_air",
        "height_above_mean_sea_level")}
    geoval_shapes.update(air_pressure_levels=[10242, 56],
        height_above_mean_sea_level_levels=[10242, 56], air_pressure_at_surface=[10242],
        height_above_mean_sea_level_at_surface=[10242])
    shapes = {"native_values": native_shapes, "native_directions": native_shapes,
              "native_covectors": native_shapes, "control": control_shapes, "control_adjoint": control_shapes,
              "geoval_tangents": geoval_shapes, "measures": {"cell_layer": [10242, 55],
              "cell_interface": [10242, 56], "cell_surface": [10242], "edge_layer": [30720, 55]}}
    checked = 0
    for index, (actual, expected) in enumerate(zip(embedded["cases"], direct["cases"])):
        if (actual["generation"], actual["seed"], expected["generation"], expected["seed"]) != (
                index//4, index%4, index//4, index%4):
            raise RuntimeError("analysis bridge boundary/seed ordering differs")
        for group, count in groups.items():
            if len(expected[group]) != count or actual[group] != expected[group]:
                raise RuntimeError(f"embedded analysis shape/bytes/inventory differs: {index}: {group}")
            if {name: field["shape"] for name, field in expected[group].items()} != shapes[group]:
                raise RuntimeError("analysis bridge lost the exact x1.10242 native/control/GeoVaL inventory")
            checked += count
        residual = actual["adjoint_relative_residual"]
        if not math.isfinite(residual) or residual < 0 or residual > 2.e-12:
            raise RuntimeError("embedded weighted adjoint fails its frozen numerical bar")
        with localcontext() as arithmetic:
            arithmetic.prec = 80
            lhs, rhs, scale = (Decimal(actual[key]) for key in
                               ("lhs", "rhs", "absolute_product_sum"))
            if not all(value.is_finite() for value in (lhs, rhs, scale)) or scale <= 0:
                raise RuntimeError("embedded adjoint sides/normalizer are nonfinite or empty")
            recomputed = float(abs(lhs - rhs) / scale)
            if recomputed > 2.e-12 or abs(recomputed - residual) > max(1.e-28, residual*1.e-12):
                raise RuntimeError("embedded adjoint residual disagrees with its full-precision sides")
    if checked != 516:
        raise RuntimeError("analysis bridge omitted a mandatory tensor comparison")
    return checked


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--embedded", required=True)
    parser.add_argument("--direct", required=True)
    args = parser.parse_args()
    with open(args.embedded) as stream: embedded = json.load(stream)
    with open(args.direct) as stream: direct = json.load(stream)
    count = compare(embedded, direct)
    print(json.dumps({"exact_tensor_comparisons": count, "boundaries": 3, "seeds": 4}))


if __name__ == "__main__":
    main()
