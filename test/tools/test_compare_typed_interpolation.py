"""Negative controls for the result checker, not a scientific-model oracle."""
import copy
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np

from compare_typed_interpolation import FIELDS, equal_payload_bits


class TypedResultCheckerTest(unittest.TestCase):
    def test_numeric_equality_is_not_payload_equality(self):
        actual, expected = np.asarray([-0., 1.]), np.asarray([0., 1.])
        self.assertTrue(np.array_equal(actual, expected))
        self.assertEqual(equal_payload_bits(actual, expected).tolist(), [False, True])

    def test_positive_and_corruption_controls(self):
        shapes = {name: [256, 1 if name.endswith("at_surface") else
                         56 if name.endswith("levels") else 55] for name in FIELDS}
        count = 3 * sum(rows * levels for rows, levels in shapes.values())
        self.assertEqual(count, 425472)
        values = np.arange(count, dtype="<f8")
        baseline = dict(schema_version=1, states=3, field_order=list(FIELDS), field_shapes=shapes,
                        source_size=10242, target_count=256, nonzeros=768, value_count=count,
                        independent_cartesian_vector_basis_checked=True,
                        wrong_moisture_denominator_rejected=True,
                        independent_numpy_values_checked=3*10242*554,
                        independent_numpy_rtol=2.e-14, independent_numpy_atol=2.e-12,
                        independent_numpy_equation_checks_passed=True)
        with tempfile.TemporaryDirectory(prefix="typed-result-checker-") as directory:
            root = Path(directory)
            expected_path, actual_path, manifest_path = (root / name for name in
                                                       ("direct.bin", "oops.bin", "manifest.json"))
            for attack in ("none", "signed-zero", "one-bit", "missing-values", "field-order", "level-shape"):
                manifest = copy.deepcopy(baseline)
                actual, expected = values.copy(), values.copy()
                if attack == "signed-zero": actual[0] = -0.
                elif attack == "one-bit": actual[1] = np.nextafter(actual[1], np.inf)
                elif attack == "missing-values":
                    actual, expected = actual[:1], expected[:1]
                    manifest["value_count"] = 1
                elif attack == "field-order": manifest["field_order"].reverse()
                elif attack == "level-shape": manifest["field_shapes"]["air_pressure"] = [256, 56]
                expected.tofile(expected_path)
                actual.tofile(actual_path)
                manifest["values_sha256"] = hashlib.sha256(expected.tobytes()).hexdigest()
                manifest_path.write_text(json.dumps(manifest))
                result = subprocess.run([sys.executable, str(Path(__file__).with_name("compare_typed_interpolation.py")),
                                         "--oops", str(actual_path), "--direct", str(expected_path),
                                         "--manifest", str(manifest_path)], capture_output=True, text=True)
                with self.subTest(attack=attack):
                    if attack == "none":
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertEqual(json.loads(result.stdout)["values"], count)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        reason = ("not elementwise bitwise equal" if attack in ("signed-zero", "one-bit") else
                                  "independent-oracle contract" if attack == "field-order" else
                                  "incomplete variable/location/level inventory")
                        self.assertIn(reason, result.stderr)


if __name__ == "__main__":
    unittest.main()
