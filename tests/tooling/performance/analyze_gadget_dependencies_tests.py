#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Contract tests for execution-weighted scalar and SIMD dependency coverage."""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import csv
import importlib.util
from pathlib import Path
import tempfile
import threading
import types
import unittest

path = Path(__file__).resolve().parents[2] / "perf" / "analyze_gadget_dependencies.py"
spec = importlib.util.spec_from_file_location("gadget_dependencies", path)
assert spec and spec.loader
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


def values():
    result = {"entered_guest_words": 1000, "unsupported_groups": 700}
    for family in ("move", "multiply", "load"):
        result.update({f"{family}_s{sources}_m{mask}": 0
                       for sources in range(4) for mask in range(8)})
    return result


class DependencyAnalysisTests(unittest.TestCase):
    def test_single_iteration_preview_completion_events(self):
        runner_path = path.with_name("run_guest_geekbench.py")
        runner_spec = importlib.util.spec_from_file_location("dependency_geekbench_runner", runner_path)
        assert runner_spec and runner_spec.loader
        runner = importlib.util.module_from_spec(runner_spec)
        runner_spec.loader.exec_module(runner)
        transcript = (b"GB_BEGIN\r\nSingle-Core\r\n"
                      b"File Compression            123        22.0 MB/sec\r\n"
                      b"Navigation                  456        10.0 routes/sec\r\n"
                      b"Multi-Core\r\n"
                      b"File Compression            321        66.0 MB/sec\r\n"
                      b"Navigation                  654        30.0 routes/sec\r\nGB_END\r\n")
        guest = types.SimpleNamespace(_transcript=bytearray(transcript))
        stop, ready = threading.Event(), threading.Event()
        stop.set()
        with tempfile.TemporaryDirectory() as directory:
            output, events = Path(directory) / "console.log", Path(directory) / "events.tsv"
            runner.mirror_transcript(guest, output, ready, stop, events)
            self.assertTrue(ready.is_set())
            self.assertEqual(output.read_bytes(), transcript)
            with events.open() as source:
                rows = list(csv.DictReader(source, delimiter="\t"))
            self.assertEqual([row["phase"] for row in rows],
                             ["benchmark", "Single-Core", "Single-Core-completed",
                              "Single-Core-completed", "Multi-Core",
                              "Multi-Core-completed", "Multi-Core-completed", "complete"])
            self.assertEqual([row["workload"] for row in rows[2:4]],
                             ["File Compression", "Navigation"])
            self.assertEqual([row["workload"] for row in rows[5:7]],
                             ["File Compression", "Navigation"])

    def test_totals_are_execution_weighted(self):
        counters = values()
        counters.update(move_s0_m0=50, multiply_s3_m5=100,
                        load_s1_m0=100, load_s1_m1=50)
        result = analysis.summarize(counters)
        self.assertEqual(result["entered_decode_groups"], 1000)
        self.assertEqual(result["eligible_groups"], 300)
        self.assertEqual(result["forwardable_groups"], 150)
        self.assertEqual(result["eligible_source_reads"], 450)
        self.assertEqual(result["forwardable_source_reads"], 250)
        self.assertEqual(result["forwardable_word_percent"], 15)
        self.assertAlmostEqual(result["forwardable_source_percent"], 25000 / 450)

    def test_legacy_schema_remains_readable(self):
        result = analysis.summarize({
            "entered_guest_words": 100, "unsupported_groups": 70,
            "add_imm_x_f0_m1": 10, "eor_x_f0_m3": 20})
        self.assertEqual(result["forwardable_source_reads"], 50)
        self.assertEqual(result["forwardable_word_percent"], 30)
        self.assertIsNone(result["domains"]["simd_fp"])

    def test_scalar_and_simd_totals_do_not_double_count(self):
        counters = values()
        counters.update({f"simd_s{sources}_m{mask}": 0
                         for sources in range(4) for mask in range(8)})
        counters.update(unsupported_groups=500, move_s0_m0=50,
                        multiply_s3_m5=100, load_s1_m0=100, load_s1_m1=50,
                        simd_s2_m3=150, simd_s1_m0=50)
        result = analysis.summarize(counters)
        scalar, vector = result["domains"]["scalar"], result["domains"]["simd_fp"]
        self.assertEqual(result["entered_decode_groups"], 1000)
        self.assertEqual(result["eligible_groups"], 500)
        self.assertEqual(result["forwardable_groups"], 300)
        self.assertEqual(result["eligible_source_reads"], 800)
        self.assertEqual(result["forwardable_source_reads"], 550)
        self.assertEqual(result["forwardable_word_percent"], 30)
        self.assertEqual(result["forwardable_source_percent"], 68.75)
        self.assertEqual(scalar["forwardable_word_percent"], 15)
        self.assertEqual(vector["forwardable_word_percent"], 15)
        self.assertEqual(vector["eligible_source_reads"], 350)
        self.assertEqual(vector["forwardable_source_reads"], 300)

    def test_observed_zero_simd_is_not_unavailable(self):
        counters = values()
        counters.update({f"simd_s{sources}_m{mask}": 0
                         for sources in range(4) for mask in range(8)})
        result = analysis.summarize(counters)
        self.assertEqual(result["domains"]["simd_fp"]["forwardable_word_percent"], 0)

    def test_single_and_multi_core_sections(self):
        counters = values()
        counters["load_s1_m1"] = 300
        rows = [{"monotonic_ns": timestamp,
                 **{key: value * timestamp // 100 for key, value in counters.items()}}
                for timestamp in range(0, 601, 100)]
        events = [
            dict(monotonic_ns="100", phase="Single-Core", workload="between-workloads"),
            dict(monotonic_ns="200", phase="Single-Core-completed", workload="File"),
            dict(monotonic_ns="250", phase="Single-Core", workload="Navigation"),
            dict(monotonic_ns="300", phase="Single-Core-completed", workload="Navigation"),
            dict(monotonic_ns="400", phase="Multi-Core", workload="between-workloads"),
            dict(monotonic_ns="500", phase="Multi-Core-completed", workload="File"),
            dict(monotonic_ns="600", phase="complete", workload="complete"),
        ]
        result = analysis.geekbench_windows(rows, events)
        self.assertEqual(result["summary"]["entered_guest_words"], 5000)
        self.assertEqual([item["phase"] for item in result["sections"]],
                         ["Single-Core", "Multi-Core"])
        self.assertEqual([item["summary"]["entered_guest_words"] for item in result["sections"]],
                         [3000, 2000])
        self.assertEqual([item["phase"] for item in result["completed_workloads"]],
                         ["Single-Core", "Single-Core", "Multi-Core"])
        single = analysis.geekbench_windows(rows, events[:4] + [events[-1]])
        self.assertEqual(len(single["sections"]), 1)
        with self.assertRaisesRegex(ValueError, "completed"):
            analysis.geekbench_windows(rows, events[:-1])
        with self.assertRaisesRegex(ValueError, "outside"):
            analysis.geekbench_windows(rows, events[:3] + [dict(
                monotonic_ns="300", phase="Multi-Core-completed", workload="Early")] + events[3:])

    def test_combined_schema_round_trip(self):
        counters = values()
        counters.update({f"simd_s{sources}_m{mask}": 0
                         for sources in range(4) for mask in range(8)})
        counters.update(simd_s2_m3=100)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "combined.csv"
            with output.open("w") as stream:
                writer = csv.DictWriter(stream, ["monotonic_ns", *counters])
                writer.writeheader()
                writer.writerow({"monotonic_ns": 1, **dict.fromkeys(counters, 0)})
                writer.writerow({"monotonic_ns": 101, **counters})
            rows = analysis.read_counts(output)
            self.assertEqual(analysis.interval(rows, 1, 101)["domains"]["simd_fp"][
                "forwardable_source_reads"], 200)

    def test_inconsistent_masks_are_rejected(self):
        counters = values()
        counters["load_s1_m3"] = 1
        with self.assertRaisesRegex(ValueError, "source count"):
            analysis.summarize(counters)

    def test_snapshot_schema_and_monotonicity(self):
        counters = values()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "counters.csv"
            with path.open("w") as output:
                writer = csv.DictWriter(output, ["monotonic_ns", *counters])
                writer.writeheader()
                writer.writerow({"monotonic_ns": 1, **{key: 0 for key in counters}})
                writer.writerow({"monotonic_ns": 101, **counters})
            rows = analysis.read_counts(path)
            self.assertEqual(analysis.interval(rows, 1, 101)["entered_guest_words"], 1000)
            with path.open("a") as output:
                writer = csv.DictWriter(output, ["monotonic_ns", *counters])
                writer.writerow({"monotonic_ns": 201, **{key: 0 for key in counters}})
            with self.assertRaisesRegex(ValueError, "backwards"):
                analysis.read_counts(path)

    def test_empty_window_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "empty"):
            analysis.interval([], 100, 100)


if __name__ == "__main__":
    unittest.main()
