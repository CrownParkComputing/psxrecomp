"""Tests for the per-sector notification diff.

The transfers are understood; what is not is what the guest is TOLD. These
pin the flag summarisation and the one interpretation that matters: native
announcing immediately where the oracle deliberately withheld.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import notify_diff as nd  # noqa: E402


def nat(lba, data=1, dma=0, pended=0, lost=0):
    return {"lba": lba, "data": data, "dma": dma, "pended": pended,
            "lost": lost}


def orc(lba, **kw):
    row = {"lba": lba, "data": 1, "dropped": 0, "queued": 0, "delivered": 0,
           "redelivered": 0, "drained": 0}
    row.update(kw)
    return row


class SummariseTest(unittest.TestCase):
    def test_native_flags_accumulate_per_lba(self):
        r = nd.native_rows([nat(1, pended=1), nat(1, lost=1)])
        self.assertEqual(r[1]["records"], 2)
        self.assertEqual(r[1]["pended"], 1)
        self.assertEqual(r[1]["lost"], 1)

    def test_oracle_flags_accumulate_per_lba(self):
        r = nd.oracle_rows([orc(5, dropped=1), orc(5, redelivered=1)])
        self.assertEqual(r[5]["dropped"], 1)
        self.assertEqual(r[5]["redelivered"], 1)

    def test_unknown_lba_skipped(self):
        self.assertEqual(nd.native_rows([{"data": 1}]), {})

    def test_marks_string(self):
        r = nd.oracle_rows([orc(5, dropped=1, drained=1)])
        self.assertEqual(nd.marks(r[5], nd.ORC_KEYS), "DD---D")


class InterpretTest(unittest.TestCase):
    def test_immediate_vs_withheld_is_flagged(self):
        n = nd.native_rows([nat(110)])
        o = nd.oracle_rows([orc(110, dropped=1, redelivered=1, drained=1)])
        notes = nd.interpret(n, o, 100, 120)
        self.assertEqual(len(notes), 1)
        self.assertIn("immediate INT1", notes[0])
        self.assertIn("re-announced", notes[0])

    def test_both_pending_is_not_flagged(self):
        n = nd.native_rows([nat(110, pended=1)])
        o = nd.oracle_rows([orc(110, queued=1, delivered=1, drained=1)])
        self.assertEqual(nd.interpret(n, o, 100, 120), [])

    def test_both_immediate_is_not_flagged(self):
        n = nd.native_rows([nat(110)])
        o = nd.oracle_rows([orc(110, delivered=1, drained=1)])
        self.assertEqual(nd.interpret(n, o, 100, 120), [])

    def test_sector_missing_on_one_side_is_not_interpreted(self):
        n = nd.native_rows([nat(110)])
        self.assertEqual(nd.interpret(n, {}, 100, 120), [])


class DegradedSessionTest(unittest.TestCase):
    """A contaminated native session must not be diffed.

    The first live run compared a stale disc_speed=2x session (lost
    notifications, sectors read 2-7 times) against a healthy oracle and
    concluded the notification patterns agreed.
    """

    def test_lost_flag_marks_degraded(self):
        r = nd.native_rows([nat(110, lost=1)])
        self.assertTrue(r[110]["lost"])

    def test_repeated_reads_mark_degraded(self):
        r = nd.native_rows([nat(110), nat(110)])
        self.assertGreater(r[110]["records"], 1)

    def test_clean_session_has_single_records_and_no_loss(self):
        r = nd.native_rows([nat(110), nat(111, pended=1)])
        self.assertTrue(all(v["records"] == 1 and not v["lost"]
                            for v in r.values()))
