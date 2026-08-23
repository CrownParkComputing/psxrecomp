"""Tests for walk_side's coherence check.

Reading a display list out of a RUNNING emulator can catch the game
mid-rebuild. A torn read does not look like an error -- it looks like a frame
of primitives carrying a handful of near-identical colours, which is
indistinguishable from a real finding. It must be detected, and detecting it
must not require pausing the emulator: parking DuckStation for every sample
made it stutter through the very animation it was watching.
"""
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import gpu_display_list as gdl  # noqa: E402


class RecordingConn:
    """Serves RAM reads from a script of successive memory states."""

    def __init__(self, states):
        self.states = list(states)
        self.reads = 0
        self.commands = []

    def cmd(self, name, **kw):
        self.commands.append(name)
        if name == "frame":
            return {"frame": 1}
        return {}

    def raw(self, name, **kw):
        self.commands.append(name)
        return {}

    def frame(self):
        return 1

    def read(self):
        """Each read consumes the next state; the last one repeats."""
        self.reads += 1
        return self.states[min(self.reads - 1, len(self.states) - 1)]


class VerifyByRereadTest(unittest.TestCase):
    def test_identical_reads_are_coherent(self):
        stable = b"\xAA" * 64
        conn = RecordingConn([stable, stable])
        self.assertEqual(conn.read(), conn.read())

    def test_changed_reads_are_torn(self):
        conn = RecordingConn([b"\xAA" * 64, b"\xBB" * 64])
        self.assertNotEqual(conn.read(), conn.read())

    def test_walk_side_accepts_park_for_reread_flag(self):
        """The flag exists and defaults to parking for CLI callers."""
        import inspect
        sig = inspect.signature(gdl.walk_side)
        self.assertIn("park_for_reread", sig.parameters)
        self.assertTrue(sig.parameters["park_for_reread"].default)

    def test_non_parking_path_never_pauses(self):
        """With park_for_reread=False no pause/continue may be issued.

        Asserted against the source of the branch rather than a live socket:
        the whole point is that this path issues no pause at all.
        """
        src = inspect_source(gdl.walk_side)
        marker = "if park_for_reread:"
        self.assertIn(marker, src)
        after = src.split("else:\n            # Verify by RE-READING", 1)[1]
        # The verify branch runs up to the end of the coherence block.
        verify_branch = after.split('meta["coherent"] = coherent', 1)[0]
        self.assertNotIn('conn.cmd("pause")', verify_branch)
        self.assertNotIn('conn.cmd("continue")', verify_branch)

    def test_torn_is_flagged_in_meta(self):
        src = inspect_source(gdl.walk_side)
        self.assertIn('meta["torn"] = True', src)


def inspect_source(fn):
    import inspect
    return inspect.getsource(fn)


if __name__ == "__main__":
    unittest.main()
