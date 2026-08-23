"""Tests for wedge_watch's trigger decision.

The predicate must stay quiet on healthy frames of this very scene (which
legitimately draw a handful of 150-200 px vignette triangles) and fire on a
wedge fan (dozens of giant untextured polygons at once).
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import wedge_watch  # noqa: E402


def tri(verts, semi=True, gouraud=True, textured=False):
    return {"op_name": "PolyG3+semi", "kind": "poly", "semi": semi,
            "gouraud": gouraud, "textured": textured, "stp": 1,
            "verts": verts, "colors": [[64, 64, 64]] * len(verts)}


def healthy_frame():
    """A few big vignette triangles plus a fine glow mesh, like bad.json."""
    prims = [
        tri([[13, 205], [39, 224], [3, 238]]),
        tri([[205, 238], [3, 238], [39, 224]]),
        tri([[39, 224], [96, 204], [205, 238]]),
        tri([[3, 238], [3, 160], [13, 205]]),
    ]
    for i in range(200):   # the glow mesh: tiny triangles
        x = 140 + (i % 20)
        prims.append(tri([[x, 70], [x + 8, 74], [x + 3, 78]]))
    return {"frame": 100, "prims": prims}


def wedge_frame():
    """A fan of giant triangles radiating from the screen centre."""
    prims = []
    pts = [(-232, -2), (60, -2), (200, -2), (408, 60), (408, 238),
           (200, 238), (-100, 238), (-232, 120), (-232, 60), (408, 120),
           (0, -2), (320, 238)]
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        prims.append(tri([[160, 120], list(a), list(b)]))
    return {"frame": 200, "prims": prims}


class EvaluateTest(unittest.TestCase):
    def test_healthy_frame_does_not_trigger(self):
        ev = wedge_watch.evaluate(healthy_frame())
        self.assertFalse(ev["trigger"])
        self.assertLess(ev["wedge_count"], 10)

    def test_wedge_fan_triggers(self):
        ev = wedge_watch.evaluate(wedge_frame())
        self.assertTrue(ev["trigger"])
        self.assertGreaterEqual(ev["wedge_count"], 10)
        self.assertGreater(ev["max_span"], 300)

    def test_opaque_wedges_still_trigger(self):
        """'Hard-edged solid polygons' may not carry the semi flag at all."""
        d = wedge_frame()
        for p in d["prims"]:
            p["semi"] = False
        self.assertTrue(wedge_watch.evaluate(d)["trigger"])

    def test_flat_wedges_still_trigger(self):
        d = wedge_frame()
        for p in d["prims"]:
            p["gouraud"] = False
            p["op_name"] = "PolyF3"
        self.assertTrue(wedge_watch.evaluate(d)["trigger"])

    def test_textured_background_never_counts(self):
        """Full-screen textured tiles are legitimate at any size."""
        prims = [tri([[-232, 9], [-72, 9], [-232, 121]], textured=True)
                 for _ in range(40)]
        ev = wedge_watch.evaluate({"frame": 1, "prims": prims})
        self.assertFalse(ev["trigger"])
        self.assertEqual(ev["wedge_count"], 0)

    def test_real_captures_do_not_trigger(self):
        """The actual bad/good captures of this scene stay quiet."""
        import json
        base = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__))))), "analysis", "frames")
        for name in ("bad.json", "good.json"):
            path = os.path.join(base, name)
            if not os.path.exists(path):
                self.skipTest(f"{name} not on disk")
            with open(path) as f:
                d = json.load(f)
            self.assertFalse(wedge_watch.evaluate(d)["trigger"], name)


class SweepTest(unittest.TestCase):
    def test_first_poll_inspects_newest_only_span(self):
        frames = wedge_watch.sweep_frames(None, {"newest": 500, "oldest": 100}, 3)
        self.assertEqual(frames[0], 500)
        self.assertEqual(frames, [500])

    def test_covers_gap_since_last_poll(self):
        frames = wedge_watch.sweep_frames(490, {"newest": 500, "oldest": 100}, 3)
        self.assertEqual(frames, [500, 497, 494, 491])

    def test_clamps_to_ring_oldest(self):
        frames = wedge_watch.sweep_frames(0, {"newest": 110, "oldest": 100}, 5)
        self.assertTrue(all(f >= 100 for f in frames))
        self.assertEqual(frames[0], 110)


if __name__ == "__main__":
    unittest.main()
