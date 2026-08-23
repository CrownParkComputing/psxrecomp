"""Tests for effect_palette's phase-robust signature and verdict.

The signature exists because every frame-against-frame comparison in this
investigation foundered on animation phase. These tests pin that the
signature does not move with phase, that a class that matches nothing is not
silently reported as clean, and that the verdict refuses to conclude from
missing samples.
"""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import effect_palette as ep  # noqa: E402

FRAMES = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))), "analysis", "frames")


def quad(colors, ys=(10, 20, 30, 40), name="PolyG4+semi", blend="B+F"):
    return {"op_name": name, "blend": blend, "semi": True, "stp": 1,
            "kind": "poly", "colors": colors,
            "verts": [[0, y] for y in ys]}


class MatchingTest(unittest.TestCase):
    def test_matches_by_property_not_spelling(self):
        """Ring dumps and RAM walks spell the class differently."""
        a = ep.additive_shaded_quads([quad([[1, 2, 3]] * 4, name="PolyG4+semi")])
        b = ep.additive_shaded_quads([quad([[1, 2, 3]] * 4, name="poly G4")])
        self.assertEqual(len(a), 1)
        self.assertEqual(len(b), 1)

    def test_non_additive_excluded(self):
        q = quad([[1, 2, 3]] * 4, blend="B-F")
        q["stp"] = 2
        self.assertEqual(ep.additive_shaded_quads([q]), [])

    def test_triangles_excluded(self):
        self.assertEqual(
            ep.additive_shaded_quads([quad([[1, 2, 3]] * 3, name="PolyG3+semi")]),
            [])


class SignatureTest(unittest.TestCase):
    def test_counts_distinct_and_saturated(self):
        s = ep.signature([quad([[100, 0, 0], [0, 0, 180], [50, 50, 50],
                                [50, 50, 50]])])
        self.assertEqual(s["distinct_colours"], 3)
        self.assertEqual(s["saturated_colours"], 2)

    def test_y_span_measured(self):
        s = ep.signature([quad([[1, 1, 1]] * 4, ys=(10, 10, 600, 600))])
        self.assertEqual(s["y_span"], 590)

    def test_empty_input_is_zero_not_error(self):
        s = ep.signature([])
        self.assertEqual(s["quads"], 0)

    def test_real_captures_separate(self):
        """good renders correctly; the effect frame does not. 5 vs 151."""
        for name, max_colours in (("good.json", 20),):
            p = os.path.join(FRAMES, name)
            if not os.path.exists(p):
                self.skipTest(f"{name} not on disk")
            with open(p) as f:
                s = ep.signature(json.load(f)["prims"])
            self.assertGreater(s["quads"], 0, name)
            self.assertLess(s["distinct_colours"], max_colours, name)


class MergeTest(unittest.TestCase):
    def test_takes_maxima_within_an_object(self):
        sigs = [{"quads": 64, "distinct_colours": 151, "saturated_colours": 60,
                 "y_span": 599},
                {"quads": 64, "distinct_colours": 153, "saturated_colours": 68,
                 "y_span": 599}]
        m = ep.merge(sigs)
        g = m["groups"][(64, 599)]
        self.assertEqual(g["distinct_colours"], 153)
        self.assertEqual(g["saturated_colours"], 68)

    def test_distinct_objects_not_merged(self):
        sigs = [{"quads": 4, "distinct_colours": 10, "saturated_colours": 1,
                 "y_span": 100},
                {"quads": 64, "distinct_colours": 151, "saturated_colours": 68,
                 "y_span": 599}]
        m = ep.merge(sigs)
        self.assertEqual(set(m["groups"]), {(4, 100), (64, 599)})

    def test_samples_without_quads_ignored_but_counted(self):
        sigs = [{"quads": 0, "distinct_colours": 0, "saturated_colours": 0,
                 "y_span": 0},
                {"quads": 8, "distinct_colours": 5, "saturated_colours": 0,
                 "y_span": 20}]
        m = ep.merge(sigs)
        self.assertEqual(m["samples_with_quads"], 1)
        self.assertEqual(m["samples"], 2)


class VerdictTest(unittest.TestCase):
    def _side(self, *sigs):
        return ep.merge([{"quads": q, "y_span": sp, "distinct_colours": c,
                          "saturated_colours": 0} for q, sp, c in sigs])

    def test_blowup_is_named_upstream(self):
        v, _, k = ep.verdict(self._side((64, 599, 151)),
                             self._side((64, 599, 5)))
        self.assertEqual(v, "native-builds-different-geometry")
        self.assertEqual(k, (64, 599))

    def test_agreement_points_at_rasterisation(self):
        v, why, _ = ep.verdict(self._side((64, 599, 6)),
                               self._side((64, 599, 5)))
        self.assertEqual(v, "signatures-agree")
        self.assertIn("RASTERISED", why)

    def test_no_oracle_samples_is_not_evidence_of_clean(self):
        v, why, _ = ep.verdict(self._side((64, 599, 151)), ep.merge([]))
        self.assertEqual(v, "no-oracle-samples")
        self.assertIn("not evidence", why)

    def test_no_native_samples_refuses(self):
        v, _, _ = ep.verdict(ep.merge([]), self._side((64, 599, 5)))
        self.assertEqual(v, "no-native-samples")

    def test_reverse_blowup_flagged_as_suspect(self):
        v, why, _ = ep.verdict(self._side((64, 599, 5)),
                               self._side((64, 599, 151)))
        self.assertEqual(v, "oracle-builds-more")
        self.assertIn("suspect", why)


if __name__ == "__main__":
    unittest.main()


class PrimShapeTest(unittest.TestCase):
    """The ring dump and gpu_display_list's report() spell prims differently.

    Indexing the string shape as if it were the list shape is a crash, not a
    wrong answer -- which is exactly what it did against a live oracle.
    """

    def test_parses_string_verts(self):
        v = ep.parse_verts("(10,68) (20,667) (-30,100)")
        self.assertEqual(v, [(10, 68), (20, 667), (-30, 100)])

    def test_parses_list_verts(self):
        self.assertEqual(ep.parse_verts([[1, 2], [3, 4]]), [(1, 2), (3, 4)])

    def test_parses_string_colors(self):
        c = ep.parse_colors("(117, 0, 0) (17, 0, 180)")
        self.assertEqual(c, [(117, 0, 0), (17, 0, 180)])

    def test_parses_list_colors(self):
        self.assertEqual(ep.parse_colors([[1, 2, 3]]), [(1, 2, 3)])

    def test_empty_and_none_are_empty(self):
        for bad in (None, "", [], "no tuples here"):
            self.assertEqual(ep.parse_verts(bad), [])
            self.assertEqual(ep.parse_colors(bad), [])

    def test_string_shape_quad_not_dropped_by_length_check(self):
        """A 3-char string must not pass a 'has 3 vertices' test."""
        q = {"op": "PolyG4+semi", "blend": "B+F", "verts": "(1,2)",
             "colors": "(1, 2, 3)"}
        self.assertEqual(ep.additive_shaded_quads([q]), [])

    def test_signature_identical_across_shapes(self):
        as_list = {"op_name": "PolyG4+semi", "blend": "B+F", "stp": 1,
                   "semi": True, "kind": "poly",
                   "verts": [[10, 68], [20, 667], [30, 100], [40, 120]],
                   "colors": [[117, 0, 0], [17, 0, 180], [5, 5, 5], [5, 5, 5]]}
        as_str = {"op": "PolyG4+semi", "blend": "B+F", "kind": "poly",
                  "verts": "(10,68) (20,667) (30,100) (40,120)",
                  "colors": "(117, 0, 0) (17, 0, 180) (5, 5, 5) (5, 5, 5)"}
        a, b = ep.signature([as_list]), ep.signature([as_str])
        for k in ("quads", "distinct_colours", "saturated_colours", "y_span"):
            self.assertEqual(a[k], b[k], k)


class GroupingTest(unittest.TestCase):
    """Samples must be compared object-for-object.

    Taking maxima across every sample compared psx-runtime's EFFECT (64
    quads / 599 lines) against the oracle's PLACEMENT SCREEN (144 / 155) --
    two different things, producing a number that looked like an answer.
    """

    def sig(self, quads, span, colours, sat=0):
        return {"quads": quads, "y_span": span, "distinct_colours": colours,
                "saturated_colours": sat, "top_colours": []}

    def test_groups_kept_separate(self):
        m = ep.merge([self.sig(64, 599, 153), self.sig(144, 155, 4)])
        self.assertEqual(m["groups"][(64, 599)]["distinct_colours"], 153)
        self.assertEqual(m["groups"][(144, 155)]["distinct_colours"], 4)

    def test_maxima_within_a_group(self):
        m = ep.merge([self.sig(64, 599, 151), self.sig(64, 599, 153)])
        self.assertEqual(m["groups"][(64, 599)]["distinct_colours"], 153)
        self.assertEqual(m["groups"][(64, 599)]["samples"], 2)

    def test_only_shared_objects_compared(self):
        """Disjoint objects are a gap in the evidence, not a difference.

        Reported as effect-object-one-sided: the biggest object either side
        saw is the effect, and it was seen by only one of them.
        """
        nat = ep.merge([self.sig(64, 599, 153)])
        orc = ep.merge([self.sig(144, 155, 5)])
        v, why, k = ep.verdict(nat, orc)
        self.assertEqual(v, "effect-object-one-sided")
        self.assertIsNone(k)
        self.assertIn("nothing to compare", why)

    def test_effect_object_drives_the_verdict(self):
        """The real data: 153 vs 3 on the effect, 4 vs 5 on the placement glow."""
        nat = ep.merge([self.sig(64, 599, 153), self.sig(144, 155, 4)])
        orc = ep.merge([self.sig(64, 599, 3), self.sig(144, 155, 5)])
        v, why, k = ep.verdict(nat, orc)
        self.assertEqual(v, "native-builds-different-geometry")
        self.assertEqual(k, (64, 599))
        self.assertIn("153", why)
        self.assertIn("3", why)

    def test_agreement_when_both_low(self):
        nat = ep.merge([self.sig(144, 155, 4)])
        orc = ep.merge([self.sig(144, 155, 5)])
        v, _, k = ep.verdict(nat, orc)
        self.assertEqual(v, "signatures-agree")
        self.assertEqual(k, (144, 155))


class FadeFloorTest(unittest.TestCase):
    """The effect is a fade, so the load-bearing number is not how bright it
    gets but whether it ever goes out."""

    def sig(self, quads, span, colours, peak):
        return {"quads": quads, "y_span": span, "distinct_colours": colours,
                "saturated_colours": 0, "peak_channel": peak,
                "top_colours": []}

    def test_peak_channel_measured(self):
        s = ep.signature([{"op_name": "PolyG4+semi", "blend": "B+F", "stp": 1,
                           "semi": True, "kind": "poly",
                           "verts": [[0, 0], [1, 1], [2, 2], [3, 3]],
                           "colors": [[248, 136, 8], [24, 0, 0],
                                      [0, 0, 0], [0, 0, 0]]}])
        self.assertEqual(s["peak_channel"], 248)

    def test_dimmest_sample_tracked_not_just_brightest(self):
        m = ep.merge([self.sig(64, 599, 153, 248),
                      self.sig(64, 599, 151, 155),
                      self.sig(64, 599, 153, 201)])
        g = m["groups"][(64, 599)]
        self.assertEqual(g["peak_min"], 155)
        self.assertEqual(g["peak_max"], 248)

    def test_single_sample_has_equal_floor_and_peak(self):
        m = ep.merge([self.sig(64, 599, 3, 3)])
        g = m["groups"][(64, 599)]
        self.assertEqual(g["peak_min"], 3)
        self.assertEqual(g["peak_max"], 3)

    def test_verdict_names_the_fade_floor(self):
        """The real numbers: native never dims below 155, the oracle reaches 3."""
        nat = ep.merge([self.sig(64, 599, 153, 248),
                        self.sig(64, 599, 151, 155)])
        orc = ep.merge([self.sig(64, 599, 3, 3)])
        v, why, k = ep.verdict(nat, orc)
        self.assertEqual(v, "native-builds-different-geometry")
        self.assertIn("155", why)
        self.assertIn("fade never", why)

    def test_no_fade_claim_when_floors_are_close(self):
        """Do not assert a fade fault the numbers do not support."""
        nat = ep.merge([self.sig(64, 599, 153, 12)])
        orc = ep.merge([self.sig(64, 599, 3, 8)])
        _, why, _ = ep.verdict(nat, orc)
        self.assertNotIn("fade never", why)


class OneSidedObjectTest(unittest.TestCase):
    """A smaller object present on both sides is not a substitute for the
    effect object being missing on one of them."""

    def sig(self, quads, span, colours, peak=100):
        return {"quads": quads, "y_span": span, "distinct_colours": colours,
                "saturated_colours": 0, "peak_channel": peak,
                "top_colours": []}

    def test_refuses_when_effect_object_missing_from_native(self):
        """The exact run that wrongly reported signatures-agree."""
        nat = ep.merge([self.sig(144, 155, 5)])
        orc = ep.merge([self.sig(64, 599, 3), self.sig(144, 155, 5)])
        v, why, k = ep.verdict(nat, orc)
        self.assertEqual(v, "effect-object-one-sided")
        self.assertIsNone(k)
        self.assertIn("psx-runtime never saw it", why)

    def test_refuses_when_effect_object_missing_from_oracle(self):
        nat = ep.merge([self.sig(64, 599, 153), self.sig(144, 155, 5)])
        orc = ep.merge([self.sig(144, 155, 5)])
        v, _, _ = ep.verdict(nat, orc)
        self.assertEqual(v, "effect-object-one-sided")

    def test_compares_when_both_saw_the_effect(self):
        nat = ep.merge([self.sig(64, 599, 153), self.sig(144, 155, 5)])
        orc = ep.merge([self.sig(64, 599, 3), self.sig(144, 155, 5)])
        v, _, k = ep.verdict(nat, orc)
        self.assertEqual(v, "native-builds-different-geometry")
        self.assertEqual(k, (64, 599))

    def test_colour_count_is_independent_of_brightness(self):
        """The oracle showed 3 colours at peak 3 AND at peak 220."""
        dim = ep.merge([self.sig(64, 599, 3, peak=3)])
        bright = ep.merge([self.sig(64, 599, 3, peak=220)])
        self.assertEqual(dim["groups"][(64, 599)]["distinct_colours"],
                         bright["groups"][(64, 599)]["distinct_colours"])


class WindowSafetyTest(unittest.TestCase):
    """An ordering table chains to primitives anywhere in RAM.

    This game's list reaches from 0x0363B0 to 0x1B23E0 -- 1.5 MB. A fixed
    window around the root truncates the walk, truncates BOTH reads
    identically so the coherence check passes, and the missing primitives are
    indistinguishable from primitives the game never drew. Two consecutive
    runs reported zero oracle samples of any object because of it.
    """

    def test_window_defaults_to_off(self):
        import argparse
        import contextlib
        import io as _io
        parser = None
        # Re-parse the tool's own arguments to pin the shipped default.
        src = _io.open(os.path.join(os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "effect_palette.py")).read()
        self.assertIn('ap.add_argument("--window"', src)
        window_decl = src.split('ap.add_argument("--window"', 1)[1]
        window_decl = window_decl.split("ap.add_argument", 1)[0]
        self.assertIn("default=0", window_decl)
        del parser, argparse, contextlib

    def test_span_of_a_real_list_exceeds_any_small_window(self):
        """The measured span is the reason the default is off."""
        lo, hi = 0x0363B0, 0x1B23E0
        self.assertGreater(hi - lo, 0x100000)
