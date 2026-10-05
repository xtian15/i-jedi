#!/usr/bin/env python3
"""Require the complete OOPS/independent spatial TLAD inventory and exact bytes."""
import argparse
from decimal import Decimal, localcontext
import json
import math


CONTROL = {"eastward_wind": [10242, 55], "northward_wind": [10242, 55],
           "upward_air_velocity": [10242, 56], "moist_potential_temperature": [10242, 55],
           "jacobian_dry_air_density": [10242, 55], "moisture_tracers": [10242, 55, 6]}
FIELDS = ["air_pressure", "air_pressure_levels", "air_pressure_at_surface", "air_temperature",
    "dry_air_density", "water_vapor_mixing_ratio_wrt_dry_air", "water_vapor_mixing_ratio_wrt_moist_air",
    "eastward_wind", "northward_wind", "height_above_mean_sea_level",
    "height_above_mean_sea_level_levels", "height_above_mean_sea_level_at_surface"]
GEOVAL = {name: ([10242] if name.endswith("_at_surface") else
    [10242, 56 if name in ("air_pressure_levels", "height_above_mean_sea_level_levels") else 55]) for name in FIELDS}
OBSERVATION = {name: [256, shape[1] if len(shape) > 1 else 1] for name, shape in GEOVAL.items()}


def compare(actual, expected):
    if len(actual["cases"]) != 24 or len(expected["cases"]) != 24 or len(actual["adjoints"]) != 312 or len(expected["adjoints"]) != 312:
        raise RuntimeError("OOPS spatial TLAD requires every generation/seed/mask/response")
    if actual.get("zero_response_trials") != 72:
        raise RuntimeError("OOPS spatial TLAD omitted the static-height zero responses")
    checked = 0
    def tensor_group(a, b, shapes):
        nonlocal checked
        if a != b or {name: item["shape"] for name, item in b.items()} != shapes:
            raise RuntimeError("OOPS spatial TLAD inventory, shape or bytes differ from the independent owner/matrix oracle")
        checked += len(shapes)
    for i, (a, b) in enumerate(zip(actual["cases"], expected["cases"])):
        identity = dict(generation=i//8, seed=(i%8)//2, mask=i%2)
        if any(type(a[key]) is not int or type(b[key]) is not int or a[key] != value or b[key] != value for key, value in identity.items()):
            raise RuntimeError("OOPS spatial TLAD case ordering differs")
        for group, shapes in (("control", CONTROL), ("geoval_tangent", GEOVAL), ("observation_tangent", OBSERVATION)):
            tensor_group(a[group], b[group], shapes)
    for i, (a, b) in enumerate(zip(actual["adjoints"], expected["adjoints"])):
        case, response = divmod(i, 13)
        identity = dict(generation=case//8, seed=(case%8)//2, mask=case%2, response=response-1)
        if any(type(a[key]) is not int or type(b[key]) is not int or a[key] != value or b[key] != value for key, value in identity.items()):
            raise RuntimeError("OOPS spatial TLAD response ordering differs")
        tensor_group(a["control_adjoint"], b["control_adjoint"], CONTROL)
        residual = a["relative_residual"]
        if not math.isfinite(residual) or residual < 0 or residual > 2.e-12:
            raise RuntimeError("OOPS spatial TLAD fails its frozen adjoint bar")
        with localcontext() as arithmetic:
            arithmetic.prec = 80
            lhs, rhs, scale = (Decimal(a[key]) for key in ("lhs", "rhs", "absolute_product_sum"))
            if not all(value.is_finite() for value in (lhs, rhs, scale)) or scale < 0:
                raise RuntimeError("OOPS spatial TLAD full-precision sides/scale are invalid")
            if scale == 0:
                if lhs != 0 or rhs != 0 or residual != 0 or identity["response"] not in (9, 10, 11):
                    raise RuntimeError("OOPS spatial TLAD falsely labels an absent response")
            else:
                measured = float(abs(lhs-rhs)/scale)
                if measured > 2.e-12 or abs(measured-residual) > max(1.e-28, residual*1.e-12):
                    raise RuntimeError("OOPS spatial TLAD residual disagrees with its full-precision sides")
    if checked != 2592:
        raise RuntimeError("OOPS spatial TLAD omitted a mandatory tensor comparison")
    return checked


def compare_single_winds(actual, expected):
    trials = actual.get("single_wind_cases", [])
    if len(trials) != 48:
        raise RuntimeError("OOPS spatial TLAD requires both single-wind requests at every state/seed/mask")
    checked = 0
    for index, trial in enumerate(trials):
        case, wind = divmod(index, 2)
        response = 7+wind
        identity = dict(generation=case//8, seed=(case%8)//2, mask=case%2, response=response)
        if any(type(trial[key]) is not int or trial[key] != value for key, value in identity.items()):
            raise RuntimeError("OOPS single-wind case ordering differs")
        name = FIELDS[response]
        forward = {name: expected["cases"][case]["observation_tangent"][name]}
        reverse = expected["adjoints"][case*13+response+1]["control_adjoint"]
        if trial["observation_tangent"] != forward or trial["control_adjoint"] != reverse:
            raise RuntimeError("OOPS single-wind TL/AD differs from the independent complete isolated response")
        checked += 1+len(CONTROL)
    if checked != 336:
        raise RuntimeError("OOPS single-wind proof omitted a required tensor comparison")
    return checked


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--oops", required=True); parser.add_argument("--direct", required=True)
    args = parser.parse_args()
    with open(args.oops) as file: actual = json.load(file)
    with open(args.direct) as file: expected = json.load(file)
    print(json.dumps(dict(exact_tensor_comparisons=compare(actual, expected), adjoint_trials=312,
        single_wind_tensor_comparisons=compare_single_winds(actual, expected),
        single_wind_trials=48, states=3, seeds=4, masks=2, output_families=12)))


if __name__ == "__main__": main()
