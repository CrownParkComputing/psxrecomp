"""Tests for the within-frame fade-scale check.

This is the one comparison in this investigation that needs no oracle and no
phase matching: the fade is one level per frame, so $s6 constant within a
frame is a property psx-runtime can be checked against on its own.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import scale_within_frame as swf  # noqa: E402


def sample(v):
    return {"regs": {"s6": f"0x{v:X}"}}


class DistinctTest(unittest.TestCase):
    def test_counts_distinct_values(self):
        c = swf.distinct_scales([sample(128), sample(128), sample(54)])
        self.assertEqual(len(c), 2)
        self.assertEqual(c[128], 2)

    def test_ignores_samples_without_regs(self):
        c = swf.distinct_scales([{"regs": {}}, {}, sample(80)])
        self.assertEqual(len(c), 1)

    def test_other_register_selectable(self):
        s = [{"regs": {"s4": "0x1F800234"}}]
        self.assertEqual(len(swf.distinct_scales(s, "s4")), 1)


class VerdictTest(unittest.TestCase):
    def test_constant_is_reported_and_redirects(self):
        v, why = swf.verdict_of([swf.distinct_scales([sample(128)] * 20)])
        self.assertEqual(v, "constant-within-frame")
        self.assertIn("source bytes", why)

    def test_varying_within_one_frame_is_the_finding(self):
        frame = swf.distinct_scales([sample(x) for x in (128, 96, 54, 12)])
        v, why = swf.verdict_of([frame])
        self.assertEqual(v, "varies-within-frame")
        self.assertIn("4 DIFFERENT", why)

    def test_worst_frame_drives_the_verdict(self):
        good = swf.distinct_scales([sample(128)] * 10)
        bad = swf.distinct_scales([sample(x) for x in (128, 96, 54)])
        v, _ = swf.verdict_of([good, bad])
        self.assertEqual(v, "varies-within-frame")

    def test_no_samples_refuses(self):
        v, why = swf.verdict_of([])
        self.assertEqual(v, "no-samples")
        self.assertIn("did not fire", why)

    def test_empty_frames_ignored_not_counted_as_constant(self):
        v, _ = swf.verdict_of([swf.distinct_scales([])])
        self.assertEqual(v, "no-samples")
