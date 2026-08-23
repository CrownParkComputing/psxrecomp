#!/usr/bin/env python3
"""Choosing the enclosing block, and being honest about what was captured.

psx-runtime's probe fires at basic-block LEADERS, so an arbitrary mid-block
address never matches one and the block containing it has to be found by
arming a spread and seeing what fires. Two things follow, and both are easy to
get quietly wrong:

The leader must be at or BELOW the target. A leader after it belongs to the
next block, whose registers describe what happens afterwards — real values,
answering a different question.

And the values are read at block ENTRY. For callee-saved registers set outside
the loop that is the same value as at the target; for one the block computes it
is not. Reporting both identically would hand back a number that is real,
plausible, and taken from the wrong instant.
"""

import importlib.util
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


PR = _load("probe_regs")


class TestCandidates(unittest.TestCase):
    def test_the_target_itself_is_tried_first(self):
        c = PR.candidates(0x8006844C, 0x100, 0x10)
        self.assertEqual(c[0], 0x8006844C)

    def test_candidates_go_BACKWARDS_from_the_target(self):
        # A block leader is at or before the instruction it contains. Searching
        # forward would find the next block entirely.
        c = PR.candidates(0x8006844C, 0x100, 0x10)
        self.assertTrue(all(a <= 0x8006844C for a in c))
        self.assertEqual(c[1], 0x8006843C)

    def test_the_probe_slot_limit_is_respected(self):
        # PC_PROBE_MAX_PCS is 16 in the engine; asking for more silently drops
        # the excess there, so it is capped here where it is visible.
        c = PR.candidates(0x8006844C, 0x10000, 0x4)
        self.assertLessEqual(len(c), PR.MAX_PCS)

    def test_the_range_is_not_exceeded(self):
        c = PR.candidates(0x80068400, 0x40, 0x10)
        self.assertTrue(all(a > 0x80068400 - 0x40 for a in c))


class TestSavedRegisterClassification(unittest.TestCase):
    def test_callee_saved_registers_are_known(self):
        # $s4 and $s6 carry the colour routine's source pointer and scale, are
        # set outside the loop, and are therefore trustworthy at block entry.
        for r in ("s4", "s6", "sp", "ra"):
            self.assertIn(r, PR.SAVED)

    def test_temporaries_are_not_treated_as_stable(self):
        # $v0/$t6/$a0 are recomputed inside the colour block; their entry value
        # is not their value at the store.
        for r in ("v0", "t6", "a0", "at"):
            self.assertNotIn(r, PR.SAVED)


if __name__ == "__main__":
    unittest.main()
