#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for diagnostic JIT link-profile ranking."""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

import analyze_jit_gpr_link_profile as profile


CAPTURE = """\
noise before the diagnostic rows
darwin-jit-gpr-link-summary executor=0x1 successful=13 unattributed=2 outer=2
darwin-jit-gpr-link-edge executor=0x1 kind=direct source=0x1000 target=0x2000 dirty=0x3 livein=0x6 count=7
darwin-jit-gpr-link-edge executor=0x1 kind=pic source=0x1004 target=0x3000 dirty=0x4 livein=0x1 count=4
darwin-jit-gpr-link-reg executor=0x1 reg=x0 livein=4 dirty=7 overlap=0 saved=11
darwin-jit-gpr-link-summary executor=0x2 successful=5 unattributed=0 outer=1
darwin-jit-gpr-link-edge executor=0x2 kind=direct source=0x4000 target=0x5000 dirty=0x2 livein=0x2 count=5
darwin-jit-gpr-link-edge executor=0x2 kind=direct source=0x4004 target=0x5004 dirty=0 livein=0 count=0
"""

SP_CAPTURE = """\
darwin-jit-gpr-link-summary executor=0x3 successful=3 unattributed=0 outer=2
darwin-jit-gpr-link-edge executor=0x3 kind=direct source=0x6000 target=0x7000 dirty=0x80000000 livein=0x40000000 count=3
"""


class LinkProfileTests(unittest.TestCase):
    def write_capture(self, contents: str = CAPTURE) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "profile.log"
        path.write_text(contents, encoding="utf-8")
        return path

    def test_read_profile_aggregates_all_executors(self) -> None:
        parsed = profile.read_profile([self.write_capture()])
        self.assertEqual(parsed.summaries, 2)
        self.assertEqual(parsed.successful_links, 18)
        self.assertEqual(parsed.unattributed_links, 2)
        self.assertEqual(parsed.attributed_links, 16)
        self.assertEqual(parsed.outer_invocations, 3)

    def test_saved_operations_use_dirty_and_livein_once_each(self) -> None:
        parsed = profile.read_profile([self.write_capture()])
        saved = parsed.saved_operations()
        self.assertEqual(saved[0], 11)
        self.assertEqual(saved[1], 24)
        self.assertEqual(saved[2], 11)
        self.assertTrue(all(value == 0 for value in saved[3:]))

    def test_sp_is_a_distinct_candidate(self) -> None:
        parsed = profile.read_profile([self.write_capture(SP_CAPTURE)])
        saved = parsed.saved_operations()
        self.assertEqual(len(saved), 32)
        self.assertEqual(saved[30], 3)
        self.assertEqual(saved[31], 3)
        score = next(
            score
            for score in profile.rank_pairs(parsed)
            if (score.first, score.second) == (30, 31)
        )
        self.assertEqual(profile.pair_name(score), "x30+sp")

    def test_pair_ranking_charges_layout_aware_boundary_cost(self) -> None:
        parsed = profile.read_profile([self.write_capture()])
        scores = profile.rank_pairs(parsed)
        self.assertEqual((scores[0].first, scores[0].second), (0, 1))
        self.assertEqual(scores[0].raw_saved, 35)
        self.assertEqual(scores[0].boundary_words, 4)
        self.assertEqual(scores[0].boundary_cost, 12)
        self.assertEqual(scores[0].net_saved, 23)
        self.assertEqual(len(scores), 496)

        by_pair = {
            (score.first, score.second): score for score in scores
        }
        self.assertEqual(by_pair[(0, 2)].boundary_words, 6)
        self.assertEqual(by_pair[(0, 2)].boundary_cost, 18)
        self.assertEqual(by_pair[(30, 31)].boundary_words, 4)

    def test_explicit_boundary_cost_overrides_layout_model(self) -> None:
        parsed = profile.read_profile([self.write_capture()])
        scores = profile.rank_pairs(parsed, pair_boundary_cost=5)
        self.assertTrue(all(score.boundary_words == 5 for score in scores))
        self.assertTrue(all(score.boundary_cost == 15 for score in scores))

    def test_negative_cost_is_rejected(self) -> None:
        parsed = profile.read_profile([self.write_capture()])
        with self.assertRaises(ValueError):
            profile.rank_pairs(parsed, pair_boundary_cost=-1)

    def test_profile_output_reports_complete_coverage(self) -> None:
        parsed = profile.read_profile([self.write_capture()])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            profile.print_profile(
                "capture", parsed, profile.rank_pairs(parsed), top=1
            )
        rendered = output.getvalue()
        self.assertIn("coverage\t0.888889", rendered)
        self.assertNotIn("edge-count-mismatch", rendered)


if __name__ == "__main__":
    unittest.main()
