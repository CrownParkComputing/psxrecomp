#!/usr/bin/env python3
"""Waiting for the effect instead of racing it.

Every tool here needs the effect RUNNING — a write trace over its packets, a
block probe at one of its instructions, a walk of the list it builds. Requiring
it to be on screen at the instant a button is clicked means racing a transient
animation, and losing that race produces a different-looking failure in each
tool: "no writes recorded", "no candidate fired", "no table found", "the block
was not reached". Four symptoms, one cause, none of them naming it.
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


GF = _load("psx_gpu_frame")


class Appears:
    """class_on_screen stand-in: absent for `after` calls, then present."""

    def __init__(self, after=2, drawing=None):
        self.calls = 0
        self.after = after
        self.drawing = drawing or {"PolyFT4": 584}

    def __call__(self, conn, op):
        self.calls += 1
        if self.calls > self.after:
            return True, dict(self.drawing, **{op: 64})
        return False, self.drawing


class TestWaitForClass(unittest.TestCase):
    def setUp(self):
        self._real = GF.class_on_screen

    def tearDown(self):
        GF.class_on_screen = self._real

    def test_it_returns_as_soon_as_the_class_appears(self):
        GF.class_on_screen = Appears(after=2)
        on, drawing = GF.wait_for_class(None, "PolyG4+semi", timeout=5, poll=0)
        self.assertTrue(on)
        self.assertEqual(drawing["PolyG4+semi"], 64)

    def test_it_gives_up_and_reports_what_WAS_drawing(self):
        # The failure has to name what was on screen instead, or the reader
        # cannot tell "wrong scene" from "tool is broken".
        GF.class_on_screen = Appears(after=10**6)
        on, drawing = GF.wait_for_class(None, "PolyG4+semi", timeout=0.05, poll=0.01)
        self.assertFalse(on)
        self.assertIn("PolyFT4", drawing)

    def test_an_already_present_class_does_not_wait(self):
        GF.class_on_screen = Appears(after=0)
        on, _ = GF.wait_for_class(None, "PolyG4+semi", timeout=5, poll=99)
        self.assertTrue(on, "should not have slept at all")

    def test_a_transient_debug_error_does_not_abort_the_wait(self):
        # The emulator can be mid-pause while polling; one failed poll is not a
        # reason to stop waiting for an effect that has not happened yet.
        calls = {"n": 0}

        def flaky(conn, op):
            calls["n"] += 1
            if calls["n"] == 1:
                raise GF.DebugError("read_ram: timed out")
            return True, {op: 64}

        GF.class_on_screen = flaky
        on, _ = GF.wait_for_class(None, "PolyG4+semi", timeout=5, poll=0)
        self.assertTrue(on)

    def test_it_tells_the_operator_to_trigger_the_effect(self):
        GF.class_on_screen = Appears(after=3)
        buf = io.StringIO()
        GF.wait_for_class(None, "PolyG4+semi", timeout=5, poll=0, out=buf)
        self.assertIn("trigger the effect", buf.getvalue())

    def test_the_prompt_is_printed_once_not_per_poll(self):
        GF.class_on_screen = Appears(after=5)
        buf = io.StringIO()
        GF.wait_for_class(None, "PolyG4+semi", timeout=5, poll=0, out=buf)
        self.assertEqual(buf.getvalue().count("trigger the effect"), 1)


if __name__ == "__main__":
    unittest.main()
