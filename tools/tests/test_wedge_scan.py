"""Tests for wedge_scan's frame profiling and ranking.

The point of this tool is that no threshold decides anything -- a wedge frame
has to sort above ordinary frames of the same scene. These tests pin the
ranking behaviour and the frame-selection arithmetic.
"""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import wedge_scan  # noqa: E402

FRAMES_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.dirname(os.path.abspath(__file__))))), "analysis", "frames")


def poly(verts, textured=False, colors=None):
    return {"op_name": "PolyG3+semi", "kind": "poly", "semi": True,
            "gouraud": True, "textured": textured, "stp": 1, "verts": verts,
            "colors": colors or [[64, 64, 64]] * len(verts)}


def ordinary_frame(n=50):
    """Small glow-mesh triangles plus a couple of big vignette pieces."""
    prims = [poly([[13, 205], [39, 224], [3, 238]]),
             poly([[205, 238], [3, 238], [39, 224]])]
    prims += [poly([[140 + i, 70], [148 + i, 74], [143 + i, 78]])
              for i in range(n)]
    return {"frame": 10, "prims": prims}


def fan_frame(spokes=16):
    """Big untextured triangles all sharing one hub vertex."""
    prims = []
    for i in range(spokes):
        a = [-232 + i * 40, -2]
        b = [-232 + (i + 1) * 40, 238]
        prims.append(poly([[160, 120], a, b]))
    return {"frame": 20, "prims": prims}


class MetricsTest(unittest.TestCase):
    def test_fan_hub_is_found(self):
        m = wedge_scan.frame_metrics(fan_frame())
        self.assertEqual(m["fan_hub"], [160, 120])
        self.assertEqual(m["fan_hub_count"], 16)

    def test_big_untextured_counted(self):
        m = wedge_scan.frame_metrics(fan_frame())
        self.assertEqual(m["big_untextured"], 16)
        self.assertEqual(m["big_textured"], 0)

    def test_textured_counted_separately_not_dropped(self):
        """A wrong guess about which class carries the wedges must stay visible."""
        d = fan_frame()
        for p in d["prims"]:
            p["textured"] = True
        m = wedge_scan.frame_metrics(d)
        self.assertEqual(m["big_untextured"], 0)
        self.assertEqual(m["big_textured"], 16)

    def test_extent_covers_offscreen(self):
        m = wedge_scan.frame_metrics(fan_frame())
        self.assertLess(m["extent"][0], 0)
        self.assertGreater(m["extent"][2], 320)

    def test_degenerate_prims_ignored(self):
        d = {"frame": 1, "prims": [poly([[5, 5], [5, 5]])]}
        self.assertEqual(wedge_scan.frame_metrics(d)["big_untextured"], 0)


class RankingTest(unittest.TestCase):
    def test_fan_outranks_ordinary(self):
        rows = [wedge_scan.frame_metrics(ordinary_frame()),
                wedge_scan.frame_metrics(fan_frame())]
        rows.sort(key=wedge_scan.rank_key, reverse=True)
        self.assertEqual(rows[0]["frame"], 20)

    def test_real_captures_rank_low_against_a_fan(self):
        """bad.json/good.json are ordinary lists; a fan must beat both."""
        rows = []
        for name in ("bad.json", "good.json"):
            p = os.path.join(FRAMES_DIR, name)
            if not os.path.exists(p):
                self.skipTest(f"{name} not on disk")
            with open(p) as f:
                rows.append(wedge_scan.frame_metrics(json.load(f)))
        rows.append(wedge_scan.frame_metrics(fan_frame()))
        rows.sort(key=wedge_scan.rank_key, reverse=True)
        self.assertEqual(rows[0]["frame"], 20)


class ScanFramesTest(unittest.TestCase):
    def test_newest_first(self):
        f = wedge_scan.scan_frames({"newest": 500, "oldest": 100}, 10, 1)
        self.assertEqual(f[0], 500)
        self.assertEqual(f[-1], 491)

    def test_stride_applied(self):
        f = wedge_scan.scan_frames({"newest": 500, "oldest": 100}, 10, 2)
        self.assertEqual(f, [500, 498, 496, 494, 492])

    def test_clamped_to_oldest(self):
        f = wedge_scan.scan_frames({"newest": 110, "oldest": 100}, 999, 1)
        self.assertEqual(min(f), 100)

    def test_last_zero_means_whole_ring(self):
        f = wedge_scan.scan_frames({"newest": 110, "oldest": 100}, 0, 1)
        self.assertEqual(len(f), 11)


if __name__ == "__main__":
    unittest.main()


class BlendMetricsTest(unittest.TestCase):
    """Additive count is what locates the effect; a window with none of it
    did not contain the animation, however the frames rank."""

    def test_additive_and_subtractive_counted(self):
        prims = [poly([[0, 0], [10, 0], [0, 10]]) for _ in range(5)]
        for p in prims[:2]:
            p["stp"] = 2
        m = wedge_scan.frame_metrics({"frame": 1, "prims": prims})
        self.assertEqual(m["additive"], 3)
        self.assertEqual(m["subtractive"], 2)

    def test_opaque_prims_not_counted_as_blended(self):
        prims = [poly([[0, 0], [10, 0], [0, 10]]) for _ in range(4)]
        for p in prims:
            p["semi"] = False
        m = wedge_scan.frame_metrics({"frame": 1, "prims": prims})
        self.assertEqual(m["additive"], 0)
        self.assertEqual(m["subtractive"], 0)

    def test_effect_frames_have_glow_and_captured_frame_does_not(self):
        """The real captures: bad/good are inside the effect, wedge.json is not."""
        cases = [("bad.json", True), ("good.json", True)]
        for name, expect_glow in cases:
            p = os.path.join(FRAMES_DIR, name)
            if not os.path.exists(p):
                self.skipTest(f"{name} not on disk")
            with open(p) as f:
                m = wedge_scan.frame_metrics(json.load(f))
            self.assertEqual(m["additive"] > 0, expect_glow, name)
