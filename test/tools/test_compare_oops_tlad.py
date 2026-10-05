"""Synthetic checker controls only; these do not qualify any model science."""
import copy
import unittest
from compare_oops_tlad import CONTROL, GEOVAL, OBSERVATION, compare, compare_single_winds


def fixture():
    def arrays(shapes): return {name: dict(shape=list(shape), sha256="a"*64) for name, shape in shapes.items()}
    cases = [dict(generation=i//8, seed=(i%8)//2, mask=i%2,
        control=arrays(CONTROL), geoval_tangent=arrays(GEOVAL), observation_tangent=arrays(OBSERVATION)) for i in range(24)]
    adjoints = []
    for i in range(312):
        case, response = divmod(i, 13)
        adjoints.append(dict(generation=case//8, seed=(case%8)//2, mask=case%2, response=response-1,
            control_adjoint=arrays(CONTROL), lhs="0" if response >= 10 else "1",
            rhs="0" if response >= 10 else "1", absolute_product_sum="0" if response >= 10 else "2", relative_residual=0.))
    single_winds = []
    for index in range(48):
        case, wind = divmod(index, 2)
        response = 7+wind
        name = ("eastward_wind", "northward_wind")[wind]
        single_winds.append(dict(generation=case//8, seed=(case%8)//2, mask=case%2, response=response,
            observation_tangent={name: copy.deepcopy(cases[case]["observation_tangent"][name])},
            control_adjoint=copy.deepcopy(adjoints[case*13+response+1]["control_adjoint"])))
    return dict(cases=cases, adjoints=adjoints, single_wind_cases=single_winds, zero_response_trials=72)


class Checker(unittest.TestCase):
    def test_complete(self): self.assertEqual(compare(fixture(), fixture()), 2592)
    def test_single_winds_complete(self): self.assertEqual(compare_single_winds(fixture(), fixture()), 336)
    def test_single_wind_omission_order_and_numeric_forgeries_fail(self):
        for kind in ("omit", "bool", "order", "forward", "reverse"):
            a, b = fixture(), fixture()
            if kind == "omit": a["single_wind_cases"].pop()
            elif kind == "bool": a["single_wind_cases"][0]["seed"] = False
            elif kind == "order": a["single_wind_cases"][0]["response"] = 8
            elif kind == "forward": a["single_wind_cases"][0]["observation_tangent"]["eastward_wind"]["sha256"] = "b"*64
            else: a["single_wind_cases"][0]["control_adjoint"]["northward_wind"]["sha256"] = "b"*64
            with self.subTest(kind=kind), self.assertRaises(RuntimeError): compare_single_winds(a, b)
    def test_omitted_forged_truncated_and_weakened_results_fail(self):
        for kind in ("omit", "bool_seed", "response", "bytes", "both_tracer", "both_location", "nan", "fake_green", "static"):
            a, b = fixture(), fixture()
            if kind == "omit": a["adjoints"].pop()
            elif kind == "bool_seed": a["cases"][0]["seed"] = False
            elif kind == "response": a["adjoints"][3]["response"] = 3
            elif kind == "bytes": a["adjoints"][0]["control_adjoint"]["eastward_wind"]["sha256"] = "b"*64
            elif kind == "both_tracer":
                for report in (a, b): report["adjoints"][0]["control_adjoint"]["moisture_tracers"]["shape"][-1] = 5
            elif kind == "both_location":
                for report in (a, b): report["cases"][0]["observation_tangent"]["eastward_wind"]["shape"][0] = 255
            elif kind == "nan": a["adjoints"][0]["relative_residual"] = float("nan")
            elif kind == "fake_green": a["adjoints"][0]["rhs"] = "1.01"
            else: a["adjoints"][10]["rhs"] = "1"
            with self.subTest(kind=kind), self.assertRaises(RuntimeError): compare(a, b)


if __name__ == "__main__": unittest.main()
