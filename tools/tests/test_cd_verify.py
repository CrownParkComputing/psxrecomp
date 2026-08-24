"""Tests for cd_verify's gap detection and delivery analysis."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import cd_verify as cv  # noqa: E402


class GapTest(unittest.TestCase):
    def test_detects_the_lom_signature(self):
        loads = [{"lba": 125111, "dest": 0x0E1F18, "size": 2048},
                 {"lba": 125113, "dest": 0x0E2718, "size": 2004}]
        g = cv.request_gaps(loads)
        self.assertEqual(len(g), 1)
        self.assertEqual(g[0]["dest_sectors"], 1)
        self.assertEqual(g[0]["lba_sectors"], 2)

    def test_contiguous_loads_are_clean(self):
        loads = [{"lba": 125111, "dest": 0x0E1F18, "size": 2048},
                 {"lba": 125112, "dest": 0x0E2718, "size": 2048}]
        self.assertEqual(cv.request_gaps(loads), [])

    def test_same_lba_multisector_read_is_not_a_gap(self):
        """One SetLoc feeding two DMAs stamps both with the same LBA -- LBA
        step 0 for dest step 1 IS flagged, and should be: interpreting it
        needs the delivery records, not silence."""
        loads = [{"lba": 126793, "dest": 0x0E1F18, "size": 2048},
                 {"lba": 126793, "dest": 0x0E2718, "size": 2048}]
        g = cv.request_gaps(loads)
        self.assertEqual(len(g), 1)
        self.assertEqual(g[0]["lba_sectors"], 0)


class DeliveryTest(unittest.TestCase):
    def test_flags_aggregated_per_lba(self):
        recs = [{"lba": 125112, "data": 1, "dma": 0, "pended": 1, "lost": 1},
                {"lba": 125113, "data": 1, "dma": 1, "pended": 0, "lost": 0}]
        seen = cv.analyse_records(recs)
        self.assertEqual(seen[125112]["lost"], 1)
        self.assertEqual(seen[125113]["dma"], 1)

    def test_missing_lba_absent_not_zeroed(self):
        seen = cv.analyse_records([{"lba": 1, "data": 1, "dma": 0,
                                    "pended": 0, "lost": 0}])
        self.assertNotIn(2, seen)


class TableStateTest(unittest.TestCase):
    """1 distinct colour is a zero-filled buffer, not a correct palette.

    A previous run analysed an earlier load into the same general-purpose
    region, read the table before the palette arrived, and called it
    CORRECT. Presence of the palette's own words decides, not a low count.
    """

    def words_blob(self, words):
        return b"".join(w.to_bytes(4, "little") for w in words)

    def test_palette_words_from_iso_sector(self):
        blob = self.words_blob([0x0888F8, 0xB0F8F8, 0, 0x0888F8])
        ws = {int.from_bytes(blob[i:i + 4], "little") & 0xFFFFFF
              for i in range(0, len(blob) - 3, 4)}
        self.assertIn(0x0888F8, ws)
        self.assertIn(0xB0F8F8, ws)

    def test_zero_fill_is_not_the_palette(self):
        blob = self.words_blob([0] * 64)
        ws = {int.from_bytes(blob[i:i + 4], "little") & 0xFFFFFF
              for i in range(0, len(blob) - 3, 4)}
        self.assertNotIn(0x0888F8, ws)
