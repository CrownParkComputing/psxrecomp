"""Tests for phase_parity's phase selection.

The fade register rests at its neutral value for most of the scene, so
locking both emulators to it does not establish that they are at the same
moment. oracle_phase must hold out for a distinctive value.
"""
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import phase_parity  # noqa: E402


class FakeConn:
    """Replays a scripted sequence of pc_hit_last values."""

    def __init__(self, phases):
        self.phases = list(phases)
        self.resumes = 0
        self.breaks = 0

    def cmd(self, name, **kw):
        if name == "pc_break":
            self.breaks += 1
            return {}
        if name == "pc_hit_last":
            if not self.phases:
                return {"valid": False}
            v = self.phases.pop(0)
            if v is None:
                return {"valid": False}
            return {"valid": True, "regs": {"s6": f"0x{v:X}"}}
        return {}

    def raw(self, *a, **k):
        return {}


class OraclePhaseTest(unittest.TestCase):
    def setUp(self):
        self._sleep = phase_parity.time.sleep
        phase_parity.time.sleep = lambda *_: None
        self._resume = phase_parity.oracle_resume
        phase_parity.oracle_resume = self._count_resume

    def tearDown(self):
        phase_parity.time.sleep = self._sleep
        phase_parity.oracle_resume = self._resume

    def _count_resume(self, conn):
        conn.resumes += 1

    def test_skips_rejected_phase(self):
        """A neutral reading is resumed past, not accepted as the lock."""
        conn = FakeConn([128, 128, 96])
        got = oracle = phase_parity.oracle_phase(
            conn, 0x8006844C, "s6", reject={128}, out=io.StringIO())
        self.assertEqual(got, 96)
        # It had to resume and re-break to get past the two neutral frames.
        self.assertGreaterEqual(conn.resumes, 3)
        self.assertGreaterEqual(conn.breaks, 3)
        del oracle

    def test_all_rejected_returns_none(self):
        """If only the common value is ever seen, that is a failure, not a lock."""
        conn = FakeConn([128] * 8)
        got = phase_parity.oracle_phase(
            conn, 0x8006844C, "s6", tries=8, reject={128}, out=io.StringIO())
        self.assertIsNone(got)

    def test_accepts_when_nothing_rejected(self):
        conn = FakeConn([128])
        got = phase_parity.oracle_phase(
            conn, 0x8006844C, "s6", reject=set(), out=io.StringIO())
        self.assertEqual(got, 128)

    def test_tolerates_invalid_readings(self):
        conn = FakeConn([None, None, 64])
        got = phase_parity.oracle_phase(
            conn, 0x8006844C, "s6", reject={128}, out=io.StringIO())
        self.assertEqual(got, 64)


if __name__ == "__main__":
    unittest.main()
