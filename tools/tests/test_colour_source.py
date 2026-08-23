"""Tests for locating and decoding the effect's colour source table."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import colour_source as cs  # noqa: E402


def sample(v, reg="s4"):
    return {"regs": {reg: f"0x{v:08X}"}}


class PointerSpanTest(unittest.TestCase):
    def test_span_widened_backwards(self):
        """The code reads $s4-12..$s4-1, so the table starts before $s4."""
        (lo, hi), vals = cs.pointer_span([sample(0x80100010),
                                          sample(0x80100020)])
        self.assertEqual(lo, 0x80100010 - cs.SRC_BACK)
        self.assertEqual(hi, 0x80100020)
        self.assertEqual(vals, [0x80100010, 0x80100020])

    def test_no_samples_returns_none(self):
        self.assertIsNone(cs.pointer_span([{"regs": {}}, {}]))

    def test_duplicate_pointers_collapsed(self):
        _, vals = cs.pointer_span([sample(0x80100010)] * 5)
        self.assertEqual(vals, [0x80100010])


class DecodeTest(unittest.TestCase):
    def test_decodes_little_endian_colour_words(self):
        blob = bytes([0x93, 0x50, 0x04, 0x00,      # (147, 80, 4)
                      0x93, 0x93, 0x68, 0x00])     # (147, 147, 104)
        cols = cs.colours_in(blob, 0x80100000, 0x80100000, 0x80100004)
        self.assertEqual(cols[(147, 80, 4)], 1)
        self.assertEqual(cols[(147, 147, 104)], 1)

    def test_command_byte_ignored(self):
        """The top byte is the GP0 command, not colour."""
        blob = bytes([0x93, 0x50, 0x04, 0x38])
        cols = cs.colours_in(blob, 0x80100000, 0x80100000, 0x80100000)
        self.assertEqual(list(cols), [(147, 80, 4)])

    def test_out_of_range_offsets_skipped(self):
        cols = cs.colours_in(b"\x01\x02\x03\x04", 0x80100000,
                             0x80100000, 0x80100100)
        self.assertEqual(sum(cols.values()), 1)


class VerdictTest(unittest.TestCase):
    def test_uniform_table_points_downstream(self):
        v, why = cs.verdict_of(3)
        self.assertEqual(v, "source-is-uniform")
        self.assertIn("not in the table", why)

    def test_varied_table_points_at_its_filler(self):
        v, why = cs.verdict_of(153)
        self.assertEqual(v, "source-is-varied")
        self.assertIn("FILLS this table", why)

    def test_empty_refuses(self):
        v, _ = cs.verdict_of(0)
        self.assertEqual(v, "no-source")

    def test_boundary_is_inclusive(self):
        self.assertEqual(cs.verdict_of(8)[0], "source-is-uniform")
        self.assertEqual(cs.verdict_of(9)[0], "source-is-varied")
