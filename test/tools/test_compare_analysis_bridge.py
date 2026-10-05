"""Checker-only attacks; synthetic receipts do not qualify model science."""
import copy
import unittest

from compare_analysis_bridge import compare


def fixture():
    native = {"u": [30720, 55], "w": [10242, 56], "theta_m": [10242, 55],
              "rho_zz": [10242, 55], "scalars": [10242, 55, 6]}
    control = {"eastward_wind": [10242, 55], "northward_wind": [10242, 55],
               "upward_air_velocity": [10242, 56], "moist_potential_temperature": [10242, 55],
               "jacobian_dry_air_density": [10242, 55], "moisture_tracers": [10242, 55, 6]}
    geovals = {name: [10242, 55] for name in (
        "air_pressure", "air_temperature", "dry_air_density", "eastward_wind", "northward_wind",
        "water_vapor_mixing_ratio_wrt_dry_air", "water_vapor_mixing_ratio_wrt_moist_air",
        "height_above_mean_sea_level")}
    geovals.update(air_pressure_levels=[10242, 56], height_above_mean_sea_level_levels=[10242, 56],
                  air_pressure_at_surface=[10242], height_above_mean_sea_level_at_surface=[10242])
    groups = {"native_values": native, "native_directions": native, "native_covectors": native,
              "control": control, "control_adjoint": control, "geoval_tangents": geovals,
              "measures": {"cell_layer": [10242, 55], "cell_interface": [10242, 56],
                           "cell_surface": [10242], "edge_layer": [30720, 55]}}
    return {"native_update_checks": [dict(generation=i, exact_absolute_assignment=True,
        identity_continuation=True, zero_increment_continuation=True, atomic_rejections=4)
        for i in range(3)], "cases": [dict(generation=i//4, seed=i%4, adjoint_relative_residual=0.,
        lhs="1", rhs="1", absolute_product_sum="2",
        **{group: {name: {"shape": shape, "sha256": "a"*64} for name, shape in fields.items()}
           for group, fields in groups.items()}) for i in range(12)]}


class Checker(unittest.TestCase):
    def test_complete_inventory(self):
        self.assertEqual(compare(fixture(), fixture()), 516)

    def test_missing_reordered_substituted_and_self_consistently_truncated_rejected(self):
        reference = fixture()
        for kind in ("bytes", "boundary", "omitted", "self_truncated", "self_alias", "nan", "threshold",
                     "update_inventory", "update_failure", "false_residual", "zero_normalizer"):
            actual, expected = copy.deepcopy(reference), copy.deepcopy(reference)
            if kind == "bytes": actual["cases"][0]["native_covectors"]["u"]["sha256"] = "b"*64
            elif kind == "boundary": actual["cases"][1]["generation"] = 1
            elif kind == "omitted": del actual["cases"][0]["geoval_tangents"]["air_pressure"]
            elif kind == "self_truncated":
                for report in (actual, expected): report["cases"][0]["control"]["moisture_tracers"]["shape"][-1] = 5
            elif kind == "self_alias":
                for report in (actual, expected):
                    report["cases"][0]["control"]["wrong_name"] = report["cases"][0]["control"].pop("eastward_wind")
            elif kind == "nan": actual["cases"][0]["adjoint_relative_residual"] = float("nan")
            elif kind == "update_inventory": actual["native_update_checks"].pop()
            elif kind == "update_failure": actual["native_update_checks"][0]["exact_absolute_assignment"] = False
            elif kind == "false_residual": actual["cases"][0]["rhs"] = "1.01"
            elif kind == "zero_normalizer": actual["cases"][0]["absolute_product_sum"] = "0"
            else: actual["cases"][0]["adjoint_relative_residual"] = 2.0001e-12
            with self.subTest(kind=kind), self.assertRaises(RuntimeError): compare(actual, expected)


if __name__ == "__main__":
    unittest.main()
