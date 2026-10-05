"""Scoped scientific reference repair must not permit generic rebaselining."""
import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "test/mpas"))
from validate_installed_contract_tests import check_retained_sources


class ReferenceCorrectionTests(unittest.TestCase):
    def test_original_and_corrected_sources_are_both_required(self):
        names = ["dycore-torch/mpas-pytorch/tests/"+name for name in (
            "test_assimilation_variables.py", "test_ijedi_contracts_real.py",
            "test_variable_registry.py", "test_analysis_coordinates_real.py")]
        original_hashes = {name: "1"*64 for name in names}
        original_hashes["unchanged.py"] = "3"*64
        original = dict(tests=[str(i) for i in range(56)], skips=0,
                        test_sources=original_hashes)
        retained = dict(original, tests=[str(i) for i in range(71)], source_commit="a"*40)
        hashes = dict(original_hashes, **{name: "2"*64 for name in names})
        correction = dict(schema_version=1, original_source_commit="a"*40,
            vader_source_commit="cb75e639ca09da1b132a777a81f6fe56209f64dd",
            test_sources={name: dict(original_sha256="1"*64, corrected_sha256="2"*64)
                          for name in names})
        check_retained_sources(original, retained, hashes, correction)
        with self.assertRaises(RuntimeError): check_retained_sources(original, retained, hashes)
        for attack in ("old", "new", "extra", "omitted", "vader", "ancestor", "unchanged"):
            c, h = copy.deepcopy(correction), dict(hashes)
            if attack == "old": c["test_sources"][names[0]]["original_sha256"] = "4"*64
            elif attack == "new": c["test_sources"][names[0]]["corrected_sha256"] = "4"*64
            elif attack == "extra": c["test_sources"]["other.py"] = c["test_sources"][names[0]]
            elif attack == "omitted": c["test_sources"].pop(names[0])
            elif attack == "vader": c["vader_source_commit"] = "b"*40
            elif attack == "ancestor": c["original_source_commit"] = "b"*40
            else: h["unchanged.py"] = "4"*64
            with self.assertRaises(RuntimeError, msg=attack):
                check_retained_sources(original, retained, h, c)


if __name__ == "__main__": unittest.main()
