#!/usr/bin/env python3
"""Maxima over many captures, not a single snapshot.

A single display-list capture from each side is confounded by phase: the two
are never at the same moment, so a class-count difference could mean "this
emulator does not draw that" or "it was not drawing it just then". That
ambiguity has cost this investigation several wrong turns, including dismissing
this very difference as phase.

Maxima are not symmetric that way. Phase can hide a primitive in one frame; it
cannot hide it in forty. If psx-runtime never once reaches a count the oracle
routinely shows, "not just then" stops being available as an explanation.
"""

import importlib.util
import io
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / f"{name}.py")
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


CC = _load("class_census")


class TestSummary(unittest.TestCase):
    def run_it(self, nat, orc):
        buf = io.StringIO()
        rows = CC.summarise(nat, orc, out=buf)
        return {r["key"]: r for r in rows}, buf.getvalue()

    def test_a_class_never_reached_is_flagged(self):
        # The measured case: 32 against 290 additive gouraud triangles.
        rows, txt = self.run_it({"PolyG3+semi|B+F": [30, 32, 28, 31]},
                                {"PolyG3+semi|B+F": [288, 290, 286, 290]})
        self.assertEqual(rows["PolyG3+semi|B+F"]["native_max"], 32)
        self.assertEqual(rows["PolyG3+semi|B+F"]["oracle_max"], 290)
        self.assertIn("never draws these", txt)

    def test_a_class_that_catches_up_at_some_point_is_not_flagged(self):
        # This is what phase looks like: low in some captures, comparable in
        # others. Flagging it would repeat the mistake this tool exists to fix.
        rows, txt = self.run_it({"PolyG3+semi|B+F": [30, 290, 12]},
                                {"PolyG3+semi|B+F": [288, 40, 290]})
        self.assertNotIn("never draws these", txt)

    def test_small_classes_are_not_flagged_on_noise(self):
        # A class peaking at 6 against 4 is not evidence of anything; the floor
        # keeps single-digit jitter out of the finding.
        _, txt = self.run_it({"PolyFT3|opaque": [4]}, {"PolyFT3|opaque": [6]})
        self.assertNotIn("never draws", txt)

    def test_the_reverse_asymmetry_is_reported_distinctly(self):
        # psx-runtime drawing something the oracle does not is a different
        # finding and must not read as the expected one.
        _, txt = self.run_it({"PolyG4+semi|B+F": [300]},
                             {"PolyG4+semi|B+F": [20]})
        self.assertIn("the ORACLE never draws these", txt)

    def test_matching_classes_show_no_gap(self):
        rows, _ = self.run_it({"PolyFT4|opaque": [584, 584]},
                              {"PolyFT4|opaque": [584, 584]})
        r = rows["PolyFT4|opaque"]
        self.assertEqual(r["native_max"], r["oracle_max"])

    def test_a_class_only_one_side_has_at_all_still_appears(self):
        rows, _ = self.run_it({}, {"PolyFT4+semi|B+F": [30]})
        self.assertEqual(rows["PolyFT4+semi|B+F"]["native_max"], 0)
        self.assertEqual(rows["PolyFT4+semi|B+F"]["oracle_max"], 30)


if __name__ == "__main__":
    unittest.main()
