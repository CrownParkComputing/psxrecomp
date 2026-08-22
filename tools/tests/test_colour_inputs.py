#!/usr/bin/env python3
"""Pacing and cleanup around the oracle's PC breakpoint.

Both of the things tested here already went wrong once, and neither failed in a
way that pointed at its cause.

A DuckStation breakpoint PAUSES the emulator when it fires, and a paused oracle
pumps its debug socket only from a Qt idle timer — 1 Hz with no gamepad
attached. Polling it at 100 ms stacks up connections it cannot accept, and the
result surfaces as "server closed without replying": a network-shaped error
with a pacing-shaped cause.

And a tool that parks the oracle then exits without resuming leaves it crippled
for every later run. The first attempt fails for one reason; every attempt after
it fails for a different one, which is a miserable thing to debug.
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


CI = _load("colour_inputs")
# Reach the library THROUGH the module under test. Loading psx_gpu_frame a
# second time here would create a distinct DebugError class, and colour_inputs'
# `except DebugError` would not catch the one this file raises — the test would
# fail for a reason that exists only in the test.
GF = sys.modules[CI.DebugError.__module__]


class FakeOracle:
    """Answers pc_hit_last as invalid until `hit_after` polls have gone by."""

    def __init__(self, hit_after=2, drop_once=False):
        self.calls = []
        self.hit_after = hit_after
        self.polls = 0
        self.drop_once = drop_once
        self.running = True

    def cmd(self, name, **kw):
        self.calls.append(name)
        if name == "continue":
            self.running = True
            return {"ok": True}
        if name == "pc_break":
            return {"ok": True}
        if name == "pc_hit_last":
            self.polls += 1
            if self.drop_once and self.polls == 1:
                raise GF.DebugError("pc_hit_last: server closed without replying")
            if self.polls >= self.hit_after:
                self.running = False        # a hit parks the emulator
                return {"ok": True, "valid": True,
                        "regs": {"s4": "0x0011506C", "s6": "0x00000060"}}
            return {"ok": True, "valid": False}
        if name == "pc_hit_clear":
            return {"ok": True}
        raise GF.DebugError(f"unknown {name}")


class TestWaitForHit(unittest.TestCase):
    def test_resumes_before_arming(self):
        # Recovery from a previous run that left the oracle parked. Without
        # this the tool can never succeed again without a manual restart.
        f = FakeOracle()
        CI.wait_for_hit(f, 0x8006844C, timeout=5, poll=0)
        self.assertEqual(f.calls[0], "continue",
                         f"first call was {f.calls[0]}, not a resume")

    def test_returns_the_registers_on_hit(self):
        f = FakeOracle(hit_after=2)
        rep = CI.wait_for_hit(f, 0x8006844C, timeout=5, poll=0)
        self.assertIsNotNone(rep)
        self.assertEqual(rep["regs"]["s4"], "0x0011506C")

    def test_a_dropped_reply_is_survived_not_fatal(self):
        # The hit itself parks the oracle mid-exchange, so losing one reply
        # around that moment is expected rather than a failure.
        f = FakeOracle(hit_after=3, drop_once=True)
        self.assertIsNotNone(CI.wait_for_hit(f, 0x8006844C, timeout=5, poll=0))

    def test_gives_up_and_returns_none_rather_than_hanging(self):
        f = FakeOracle(hit_after=10**9)
        self.assertIsNone(CI.wait_for_hit(f, 0x8006844C, timeout=0.2, poll=0.05))

    def test_default_poll_matches_a_paused_oracle(self):
        # The idle timer is 1 Hz without a gamepad. Polling faster than that is
        # what produced "server closed without replying".
        self.assertGreaterEqual(GF.ORACLE_PAUSED_POLL_S, 1.0)


class TestOracleResume(unittest.TestCase):
    def test_reports_success(self):
        f = FakeOracle()
        self.assertTrue(GF.oracle_resume(f))
        self.assertIn("continue", f.calls)

    def test_a_failure_is_reported_not_raised(self):
        class Dead:
            def cmd(self, *a, **k):
                raise GF.DebugError("no debug server")
        self.assertFalse(GF.oracle_resume(Dead()))


if __name__ == "__main__":
    unittest.main()
