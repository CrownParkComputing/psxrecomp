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
    def test_takes_maxima_not_means(self):
        sigs = [{"quads": 4, "distinct_colours": 10, "saturated_colours": 1,
                 "y_span": 100},
                {"quads": 64, "distinct_colours": 151, "saturated_colours": 68,
                 "y_span": 599}]
        m = ep.merge(sigs)
        self.assertEqual(m["quads"], 64)
        self.assertEqual(m["distinct_colours"], 151)
        self.assertEqual(m["y_span"], 599)

    def test_samples_without_quads_ignored_but_counted(self):
        sigs = [{"quads": 0, "distinct_colours": 0, "saturated_colours": 0,
                 "y_span": 0},
                {"quads": 8, "distinct_colours": 5, "saturated_colours": 0,
                 "y_span": 20}]
        m = ep.merge(sigs)
        self.assertEqual(m["samples_with_quads"], 1)
        self.assertEqual(m["samples"], 2)


class VerdictTest(unittest.TestCase):
    def _sig(self, colours, span, n=1):
        return {"distinct_colours": colours, "y_span": span,
                "saturated_colours": 0, "quads": 10, "samples_with_quads": n,
                "samples": n}

    def test_blowup_is_named_upstream(self):
        v, _ = ep.verdict(self._sig(151, 599), self._sig(5, 155))
        self.assertEqual(v, "native-builds-different-geometry")

    def test_agreement_points_at_rasterisation(self):
        v, why = ep.verdict(self._sig(6, 160), self._sig(5, 155))
        self.assertEqual(v, "signatures-agree")
        self.assertIn("RASTERISED", why)

    def test_no_oracle_samples_is_not_evidence_of_clean(self):
        v, why = ep.verdict(self._sig(151, 599), self._sig(0, 0, n=0))
        self.assertEqual(v, "no-oracle-samples")
        self.assertIn("not evidence", why)

    def test_no_native_samples_refuses(self):
        v, _ = ep.verdict(self._sig(0, 0, n=0), self._sig(5, 155))
        self.assertEqual(v, "no-native-samples")

    def test_reverse_blowup_flagged_as_suspect(self):
        v, why = ep.verdict(self._sig(5, 155), self._sig(151, 599))
        self.assertEqual(v, "oracle-builds-more")
        self.assertIn("suspect", why)


if __name__ == "__main__":
    unittest.main()
