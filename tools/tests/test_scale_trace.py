#!/usr/bin/env python3
"""Does the colour scale animate on each side?

Every vertex colour in this effect is source_rgb * $s6 >> 7, so $s6 IS the
fade: 128 leaves the colour unchanged, smaller values darken it. A bright,
unfaded effect is what a scale pinned at 128 would produce.

This compares VARIATION rather than values, which is the point. Frame numbers,
buffer halves and animation phase have each produced a confident wrong answer
in this investigation; a register that sweeps on one side and sits still on the
other is a difference none of those can manufacture.
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


ST = _load("scale_trace")


class TestDescribe(unittest.TestCase):
    def d(self, vals):
        return ST.describe(vals, "x", out=io.StringIO())

    def test_a_constant_series_is_flagged_constant(self):
        d = self.d([128] * 10)
        self.assertTrue(d["constant"])
        self.assertEqual(d["distinct"], 1)

    def test_the_neutral_value_is_called_out_specifically(self):
        # 128 is not just "constant" — it is the value at which the multiply
        # does nothing, which is why a pinned 128 and a pinned 64 mean very
        # different things.
        self.assertTrue(self.d([128] * 5)["neutral_only"])
        self.assertFalse(self.d([64] * 5)["neutral_only"])

    def test_a_varying_series_is_not_constant(self):
        d = self.d([52, 64, 68, 76, 96, 126, 128])
        self.assertFalse(d["constant"])
        self.assertEqual(d["min"], 52)
        self.assertEqual(d["max"], 128)

    def test_no_samples_yields_nothing_rather_than_a_default(self):
        self.assertIsNone(self.d([]))


class TestVerdictLogic(unittest.TestCase):
    """The four outcomes point in four different directions."""

    @staticmethod
    def shape(vals):
        return ST.describe(vals, "x", out=io.StringIO())

    def test_native_pinned_and_oracle_varying_is_the_suspected_fault(self):
        a = self.shape([128] * 12)
        b = self.shape([52, 64, 76, 96])
        self.assertTrue(a["constant"] and not b["constant"])
        self.assertTrue(a["neutral_only"])

    def test_both_varying_means_the_fade_is_not_missing(self):
        a = self.shape([60, 90, 128])
        b = self.shape([52, 96, 128])
        self.assertFalse(a["constant"])
        self.assertFalse(b["constant"])

    def test_both_constant_is_inconclusive_not_a_finding(self):
        # Most likely the effect simply is not animating on either side right
        # now, which says nothing about the bug.
        a = self.shape([128] * 5)
        b = self.shape([128] * 5)
        self.assertTrue(a["constant"] and b["constant"])

    def test_the_reverse_asymmetry_is_distinguished(self):
        # Oracle pinned while ours varies is the opposite of the expectation and
        # should not be reported as if it confirmed it.
        a = self.shape([60, 90, 128])
        b = self.shape([128] * 5)
        self.assertFalse(a["constant"])
        self.assertTrue(b["constant"])


if __name__ == "__main__":
    unittest.main()


class TestOneSidedResults(unittest.TestCase):
    """A side that produced nothing must say WHY, and the other side still counts.

    The first real run came back with oracle: null and verdict "incomplete",
    which reports that there is no comparison without reporting whether the
    emulator was unreachable, never reached the PC, or refused the breakpoint.
    Those call for different responses and the reason is known at the point of
    failure and nowhere else.

    It also threw away the half that DID answer — and that half disproved a
    hypothesis outright: psx-runtime's $s6 read 66, 72 and 128 across three
    samples, so it is not pinned at the neutral value after all.
    """

    def d(self, vals):
        return ST.describe(vals, "x", out=io.StringIO())

    def test_a_varying_side_disproves_the_pinned_hypothesis(self):
        got = self.d([66, 72, 128])
        self.assertFalse(got["constant"])
        self.assertFalse(got["neutral_only"])
        self.assertEqual(got["distinct"], 3)

    def test_three_identical_samples_would_have_supported_it(self):
        # Which is what three separate single-sample runs looked like, and why
        # sampling repeatedly is the difference between a pattern and an
        # artefact of when each run happened to look.
        got = self.d([128, 128, 128])
        self.assertTrue(got["constant"])
        self.assertTrue(got["neutral_only"])

    def test_samplers_return_a_reason_alongside_the_values(self):
        # Both return (values, reason) so an empty result can explain itself.
        import inspect
        for fn in (ST.sample_native, ST.sample_oracle):
            src = inspect.getsource(fn)
            self.assertIn("return vals", src)
            self.assertIn("Returns (values", src,
                          f"{fn.__name__} does not document the reason it returns")


class TestGranularity(unittest.TestCase):
    """Both animating does not mean both animating the SAME WAY.

    Measured: the oracle stepped 122/124/126/128 — by 2, across a 6-wide band —
    while psx-runtime showed 28 and 128 with nothing between. A fade that sweeps
    smoothly and one that jumps between extremes produce very different pictures
    from identical geometry, and min/max alone cannot tell them apart.
    """

    def d(self, vals):
        return ST.describe(vals, "x", out=io.StringIO())

    def test_step_size_is_reported(self):
        d = self.d([122, 124, 126, 128])
        self.assertEqual(d["max_step"], 2)
        self.assertEqual(d["median_step"], 2)

    def test_a_jump_between_extremes_shows_a_large_step(self):
        self.assertEqual(self.d([28, 128])["max_step"], 100)

    def test_range_alone_would_not_separate_them(self):
        # Same min and max, completely different behaviour.
        smooth = self.d(list(range(28, 129, 4)))
        jumpy = self.d([28, 128])
        self.assertEqual((smooth["min"], smooth["max"]),
                         (jumpy["min"], jumpy["max"]))
        self.assertLess(smooth["max_step"], jumpy["max_step"])

    def test_a_single_value_has_no_steps(self):
        d = self.d([128] * 5)
        self.assertEqual(d["max_step"], 0)
        self.assertEqual(d["median_step"], 0)

    def test_two_distinct_of_three_samples_is_not_evidence(self):
        # Which is exactly what the first complete run produced. Sparse
        # sampling of a smooth ramp looks identical to a genuine jump.
        d = self.d([28, 128, 128])
        self.assertEqual(d["samples"], 3)
        self.assertEqual(d["distinct"], 2)
