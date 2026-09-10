#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host-only proof that a CTS shard survives one hang and one host abort."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import base64
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

INTEGRATION_GPU = Path(__file__).resolve().parents[2] / "integration/gpu"
sys.path.insert(0, str(INTEGRATION_GPU))
import run_vk_gl_cts as gl  # noqa: E402
import run_vk_cts as vk  # noqa: E402

CASES = tuple(f"KHR-GL46.fixture.case{index:02d}" for index in range(7))


def qpa_result(case: str, status: str = "Pass") -> str:
    return (
        f"#beginTestCaseResult {case}\n"
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        f'<TestCaseResult Version="0.3.4" CasePath="{case}" '
        'CaseType="SelfValidate">\n'
        ' <Text>virtio-gpu venus</Text>\n'
        f' <Result StatusCode="{status}">result</Result>\n'
        "</TestCaseResult>\n"
        "#endTestCaseResult\n"
    )


def qpa_session(cases, *, ended: bool = True, in_flight: str | None = None) -> str:
    body = "".join(qpa_result(case) for case in cases)
    text = "#sessionInfo releaseName 1.4.5.5\n#beginSession\n" + body
    if in_flight is not None:
        text += f"#beginTestCaseResult {in_flight}\n<?xml version"
    if ended:
        text += "#endSession\n"
    return text


def framed(name: str, payload: bytes) -> str:
    compressed = gzip.compress(payload, mtime=0)
    digest = hashlib.sha256(compressed).hexdigest()
    encoded = base64.b64encode(compressed).decode("ascii")
    return (
        f"UML_CTS_ARTIFACT_BEGIN|{name}|gzip+base64|{len(compressed)}|{digest}\n"
        f"UML_CTS_ARTIFACT_CHUNK|{name}|0|{encoded}\n"
        f"UML_CTS_ARTIFACT_END|{name}|1\n"
    )


def segmented(payload: bytes, parts: int) -> dict[str, bytes]:
    step = max(1, -(-len(payload) // parts))
    return {
        f"results.qpa.{index:04d}": payload[offset : offset + step]
        for index, offset in enumerate(range(0, max(len(payload), 1), step))
    }


class Step:
    """One scripted guest session."""

    def __init__(
        self,
        *,
        outcome: str,
        cases,
        ended: bool,
        in_flight: str | None = None,
        summary: bytes | None = None,
        selected=None,
        console: str | None = None,
        return_code: int = 0,
        drop_trailer: bool = False,
        no_qpa: bool = False,
        shutdown_stall: bool = False,
    ) -> None:
        self.outcome = outcome
        self.return_code = return_code
        self.shutdown_stall = shutdown_stall
        self.console = console or (
            "Darwin UML stopped: result=0 exitcode=0\n"
            if outcome == "completed"
            else "guest went quiet\n"
        )
        wire: dict[str, bytes] = {}
        if not no_qpa:
            wire.update(
                segmented(
                    qpa_session(cases, ended=ended, in_flight=in_flight).encode(), 2
                )
            )
        wire["renderer.txt"] = (
            b"device=/dev/dri/card0\ndevice=/dev/dri/renderD128\n"
            b"x11_server=xwayland\nguest_page_size=16384\n"
            b"zink over Virtio-GPU Venus\n"
        )
        if selected is not None:
            wire["selected.txt"] = "".join(f"{case}\n" for case in selected).encode()
        if summary is not None:
            wire["summary.txt"] = summary
        self.frames = "".join(framed(name, payload) for name, payload in wire.items())
        if drop_trailer:
            self.frames = "\n".join(self.frames.splitlines()[:-1]) + "\n"


class ScriptedGuests:
    def __init__(self, steps) -> None:
        self.steps = list(steps)
        self.calls: list[dict] = []

    def __call__(
        self,
        command,
        console_path,
        transport_path,
        socket_path,
        *,
        guest_timeout,
        console_stall_timeout=0,
        environment=None,
    ):
        step = self.steps[len(self.calls)]
        self.calls.append(
            {"command": list(command), "console": console_path, "environment": environment}
        )
        console_path.parent.mkdir(parents=True, exist_ok=True)
        console_path.write_text(step.console)
        transport_path.write_text(step.frames)
        artifacts = gl.decode_artifacts(
            step.frames, partial=step.outcome != "completed"
        )
        return gl.RawSession(
            step.console,
            step.frames,
            step.return_code,
            step.outcome,
            None,
            artifacts,
            step.shutdown_stall,
        )


def drive(directory: Path, steps, *, max_resumptions: int = 4):
    guests = ScriptedGuests(steps)
    original = gl.run_guest_session
    gl.run_guest_session = guests
    try:
        sessions, expected, merged = gl.run_shard_sessions(
            directory,
            build_command=lambda resume, guest: ["fixture", str(resume), str(guest)],
            resolve_expected=lambda first: tuple(
                gl.parse_case_list(first["selected.txt"].decode())
            ),
            check_session=lambda index, resume, found: None,
            guest_timeout=60,
            console_stall_timeout=30,
            max_resumptions=max_resumptions,
        )
    finally:
        gl.run_guest_session = original
    return guests, sessions, expected, merged


def provenance_metadata(sessions, root: Path) -> dict:
    return {
        "session_count": len(sessions),
        "max_resumptions": 4,
        "artifacts_provenance": gl.shard_provenance(sessions),
        "sessions": gl.session_records(sessions, root),
        "host_recorded_results": gl.host_recorded_results(sessions),
    }


class QpaTransportTests(unittest.TestCase):
    def test_checkpoint_segments_assemble_into_one_artifact(self) -> None:
        payload = qpa_session(CASES).encode()
        transcript = "".join(
            framed(name, chunk) for name, chunk in segmented(payload, 3).items()
        )
        self.assertEqual(gl.decode_artifacts(transcript), {"results.qpa": payload})

    def test_segment_gaps_duplicates_and_mixtures_fail_closed(self) -> None:
        payload = qpa_session(CASES).encode()
        parts = segmented(payload, 3)
        names = sorted(parts)
        gap = "".join(
            framed(name, parts[name]) for name in names if name != names[1]
        )
        with self.assertRaisesRegex(gl.CampaignError, "gapless prefix"):
            gl.decode_artifacts(gap)
        mixed = framed(names[0], parts[names[0]]) + framed("results.qpa", payload)
        with self.assertRaisesRegex(gl.CampaignError, "whole-file QPA"):
            gl.decode_artifacts(mixed)
        with self.assertRaisesRegex(gl.CampaignError, "unsafe or unexpected"):
            gl.decode_artifacts(framed("results.qpa.99999", b"x"))

    def test_partial_decode_keeps_complete_segments_only(self) -> None:
        payload = qpa_session(CASES[:4], ended=False).encode()
        parts = segmented(payload, 2)
        names = sorted(parts)
        transcript = framed(names[0], parts[names[0]])
        truncated = framed(names[1], parts[names[1]]).splitlines()[:-1]
        transcript += "\n".join(truncated) + "\n"
        with self.assertRaisesRegex(gl.CampaignError, "missing its trailer"):
            gl.decode_artifacts(transcript)
        self.assertEqual(
            gl.decode_artifacts(transcript, partial=True),
            {"results.qpa": parts[names[0]]},
        )


class QpaPrefixTests(unittest.TestCase):
    def test_prefix_scan_separates_complete_results_from_the_trailer(self) -> None:
        text = qpa_session(CASES[:3], ended=False, in_flight=CASES[3])
        prefix = gl.scan_qpa(text)
        self.assertEqual(list(prefix.results), list(CASES[:3]))
        self.assertEqual(prefix.in_flight_case, CASES[3])
        self.assertFalse(prefix.ended_session)
        with self.assertRaisesRegex(gl.CampaignError, "truncated"):
            gl.parse_qpa(text)

    def test_complete_session_parses_identically_through_both_paths(self) -> None:
        text = qpa_session(CASES)
        self.assertEqual(gl.parse_qpa(text), gl.scan_qpa(text).results)
        self.assertTrue(gl.scan_qpa(text).ended_session)


class QpaMergeTests(unittest.TestCase):
    def evidence(self, index, cases, *, ended, terminated, status, outcome, resume):
        return gl.SessionEvidence(
            index=index,
            outcome=outcome,
            resume_after=resume,
            qpa_text=qpa_session(cases, ended=ended),
            completed_cases=tuple(cases),
            terminated_case=terminated,
            terminated_status=status,
            console="console.log",
            artifact_transport=gl.ARTIFACT_TRANSPORT_LOG,
            artifacts_directory="artifacts",
            return_code=0,
        )

    def test_single_complete_session_is_returned_byte_for_byte(self) -> None:
        session = self.evidence(
            0, CASES, ended=True, terminated=None, status=None,
            outcome="completed", resume=0,
        )
        self.assertEqual(gl.merge_qpa_sessions([session]), session.qpa_text)

    def test_merged_sessions_record_the_offending_case_explicitly(self) -> None:
        first = self.evidence(
            0, CASES[:3], ended=False, terminated=CASES[3], status=gl.STALL_STATUS,
            outcome="stalled", resume=0,
        )
        second = self.evidence(
            1, CASES[4:], ended=True, terminated=None, status=None,
            outcome="completed", resume=4,
        )
        merged = gl.merge_qpa_sessions([first, second])
        results = gl.parse_qpa(merged)
        self.assertEqual(list(results), list(CASES))
        self.assertEqual(results[CASES[3]], "Timeout")
        self.assertEqual(merged.count("#beginSession"), 1)
        self.assertEqual(merged.count("#endSession"), 1)
        self.assertIn(f"{gl.QPA_BOUNDARY}session 1 resumes at index 4", merged)
        self.assertIn(f"{gl.QPA_BOUNDARY}host-recorded Timeout reason=stalled", merged)

    def test_host_recorded_blocks_reject_invalid_names_and_statuses(self) -> None:
        with self.assertRaisesRegex(gl.CampaignError, "invalid host-recorded status"):
            gl.host_recorded_qpa_block(CASES[0], "Pass", "stalled")
        with self.assertRaisesRegex(gl.CampaignError, "invalid host-recorded case"):
            gl.host_recorded_qpa_block("a b", gl.STALL_STATUS, "stalled")
        with self.assertRaisesRegex(gl.CampaignError, "invalid host-recorded reason"):
            gl.host_recorded_qpa_block(CASES[0], gl.ABORT_STATUS, "Metal abort")


class ShardResumptionTests(unittest.TestCase):
    def summary(self) -> bytes:
        return b"cts_rc=0\n"

    def hang_then_finish(self, root: Path, *, max_resumptions: int = 4):
        steps = [
            Step(
                outcome="stalled",
                cases=CASES[:3],
                ended=False,
                in_flight=CASES[3],
                selected=CASES,
            ),
            Step(
                outcome="completed",
                cases=CASES[4:],
                ended=True,
                selected=CASES[4:],
                summary=self.summary(),
            ),
        ]
        return drive(root, steps, max_resumptions=max_resumptions)

    def test_hang_preserves_results_records_timeout_and_continues(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guests, sessions, expected, merged = self.hang_then_finish(root)
            self.assertEqual(expected, CASES)
            self.assertEqual(list(merged), list(CASES))
            self.assertEqual(merged[CASES[3]], "Timeout")
            self.assertEqual(
                [status for case, status in merged.items() if case != CASES[3]],
                ["Pass"] * 6,
            )
            self.assertEqual(len(sessions), 2)
            self.assertEqual([row.resume_after for row in sessions], [0, 4])
            self.assertEqual(sessions[0].outcome, "stalled")
            self.assertEqual(sessions[1].outcome, "completed")
            self.assertEqual(sessions[0].terminated_case, CASES[3])
            self.assertIsNone(sessions[1].terminated_case)
            # The offending case is never retried: the fresh guest is asked
            # for the cases after it, in the same traversal order.
            self.assertEqual(guests.calls[1]["command"][1], "4")
            self.assertEqual(
                sessions[0].artifacts_directory, "sessions/00/artifacts"
            )
            self.assertTrue((root / "sessions/01/console.log").is_file())
            self.assertTrue((root / "console.log").is_file())

    def test_host_abort_records_a_crash_for_the_case_in_flight(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            steps = [
                Step(
                    outcome="aborted",
                    cases=CASES[:2],
                    ended=False,
                    selected=CASES,
                    return_code=134,
                    console="Darwin UML host signal 6 at pc=0x0\n",
                ),
                Step(
                    outcome="completed",
                    cases=CASES[3:],
                    ended=True,
                    selected=CASES[3:],
                    summary=self.summary(),
                ),
            ]
            _, sessions, _, merged = drive(Path(temporary), steps)
            self.assertEqual(merged[CASES[2]], "Crash")
            self.assertEqual(sessions[0].terminated_status, gl.ABORT_STATUS)
            self.assertEqual(sessions[0].return_code, 134)
            self.assertEqual(list(merged), list(CASES))

    def test_resumptions_are_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            steps = [
                Step(outcome="stalled", cases=CASES[:1], ended=False, selected=CASES),
                Step(outcome="stalled", cases=CASES[2:3], ended=False, selected=CASES[2:]),
                Step(outcome="completed", cases=CASES[4:], ended=True,
                     selected=CASES[4:], summary=self.summary()),
            ]
            with self.assertRaisesRegex(gl.CampaignError, "exhausted its 1 resumptions"):
                drive(Path(temporary), steps, max_resumptions=1)

    def test_zero_bound_keeps_the_historical_single_session_shard(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="stalled", cases=CASES[:1], ended=False, selected=CASES),
            ]
            with self.assertRaisesRegex(gl.CampaignError, "exhausted its 0 resumptions"):
                drive(root, steps, max_resumptions=0)

    def test_a_clean_single_session_shard_keeps_its_layout(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="completed", cases=CASES, ended=True,
                     selected=CASES, summary=self.summary()),
            ]
            _, sessions, _, merged = drive(root, steps)
            self.assertEqual(len(sessions), 1)
            self.assertEqual(sessions[0].artifacts_directory, "artifacts")
            self.assertFalse((root / gl.SESSION_DIRECTORY).exists())
            self.assertEqual(list(merged), list(CASES))

    def test_a_hang_on_the_last_case_needs_no_second_guest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="stalled", cases=CASES[:-1], ended=False,
                     in_flight=CASES[-1], selected=CASES, summary=b"cts_rc=124\n"),
            ]
            guests, sessions, expected, merged = drive(root, steps)
            self.assertEqual(len(guests.calls), 1)
            self.assertEqual(sessions[0].terminated_case, CASES[-1])
            self.assertEqual(merged[CASES[-1]], "Timeout")
            self.assertEqual(list(merged), list(CASES))
            # One session, but the shard's own QPA and summary are the host's.
            self.assertEqual(gl.shard_provenance(sessions), "host-merged")
            self.assertEqual(sessions[0].artifacts_directory, "sessions/00/artifacts")
            artifacts = gl.merged_shard_artifacts(sessions, root, len(expected))
            gl.write_artifacts(artifacts, root / "artifacts")
            gl.validate_session_provenance(
                root, provenance_metadata(sessions, root), artifacts, len(expected)
            )
            self.assertEqual(
                gl.parse_qpa(artifacts["results.qpa"].decode())[CASES[-1]], "Timeout"
            )

    def test_a_dirty_stop_with_nothing_to_record_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="stalled", cases=CASES, ended=False,
                     selected=CASES, summary=b"cts_rc=0\n"),
            ]
            _, sessions, expected, _ = drive(root, steps)
            # Every case has a result, so there is no case to blame; a guest
            # that never stopped cleanly must not become an ordinary shard.
            self.assertIsNone(sessions[0].terminated_case)
            self.assertEqual(gl.shard_provenance(sessions), "single-session-guest")
            artifacts = {
                path.name: path.read_bytes()
                for path in sorted((root / "artifacts").iterdir())
            }
            with self.assertRaisesRegex(gl.CampaignError, "must record its case"):
                gl.validate_session_provenance(
                    root, provenance_metadata(sessions, root), artifacts, len(expected)
                )

    def test_an_abort_before_the_first_checkpoint_still_merges(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            steps = [
                Step(outcome="aborted", cases=(), ended=False, selected=CASES,
                     no_qpa=True, return_code=134,
                     console="Darwin UML host signal 6 at pc=0x0\n"),
                Step(outcome="completed", cases=CASES[1:], ended=True,
                     selected=CASES[1:], summary=b"cts_rc=0\n"),
            ]
            _, sessions, expected, merged = drive(root, steps)
            # The first guest delivered its enumeration but no results at all;
            # the case it was starting is still named, not dropped.
            self.assertEqual(sessions[0].completed_cases, ())
            self.assertEqual(merged[CASES[0]], "Crash")
            self.assertEqual(list(merged), list(CASES))
            artifacts = gl.merged_shard_artifacts(sessions, root, len(expected))
            gl.write_artifacts(artifacts, root / "artifacts")
            gl.validate_session_provenance(
                root, provenance_metadata(sessions, root), artifacts, len(expected)
            )
            self.assertEqual(
                list(gl.parse_qpa(artifacts["results.qpa"].decode())), list(CASES)
            )

    def test_out_of_order_guest_results_fail_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            steps = [
                Step(outcome="stalled", cases=CASES[:2], ended=False, selected=CASES),
                Step(outcome="completed", cases=CASES[5:], ended=True,
                     selected=CASES[5:], summary=self.summary()),
            ]
            with self.assertRaisesRegex(gl.CampaignError, "traversal order"):
                drive(Path(temporary), steps)

    def test_continuation_guests_get_their_own_ownership_journal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            token = "a" * 32
            os.environ[gl.guest_process.OWNERSHIP_ENV] = json.dumps(
                {"path": str(root / gl.guest_process.JOURNAL_NAME), "token": token}
            )
            try:
                guests, sessions, _, _ = self.hang_then_finish(root)
            finally:
                del os.environ[gl.guest_process.OWNERSHIP_ENV]
            self.assertIsNone(guests.calls[0]["environment"])
            setting = json.loads(
                guests.calls[1]["environment"][gl.guest_process.OWNERSHIP_ENV]
            )
            self.assertEqual(setting["token"], token)
            self.assertEqual(
                Path(setting["path"]),
                root / "sessions/01" / gl.guest_process.JOURNAL_NAME,
            )
            self.assertIsNone(sessions[0].ownership_journal)
            self.assertEqual(
                sessions[1].ownership_journal,
                f"sessions/01/{gl.guest_process.JOURNAL_NAME}",
            )


class MergedProvenanceTests(unittest.TestCase):
    def build(self, root: Path):
        steps = [
            Step(outcome="stalled", cases=CASES[:3], ended=False,
                 in_flight=CASES[3], selected=CASES),
            Step(outcome="completed", cases=CASES[4:], ended=True,
                 selected=CASES[4:], summary=b"cts_rc=0\n"),
        ]
        _, sessions, expected, merged = drive(root, steps)
        artifacts = gl.merged_shard_artifacts(sessions, root, len(expected))
        gl.write_artifacts(artifacts, root / "artifacts")
        return provenance_metadata(sessions, root), artifacts, expected

    def test_merged_artifacts_are_reproducible_from_session_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            metadata, artifacts, expected = self.build(root)
            gl.validate_session_provenance(root, metadata, artifacts, len(expected))
            summary = artifacts["summary.txt"].decode()
            self.assertIn("cts_rc=1", summary)
            self.assertIn("provenance=host-merged", summary)
            self.assertIn("sessions=2", summary)
            self.assertIn(f"host_recorded_0={CASES[3]}=Timeout", summary)
            self.assertIn(f"expected_case_count={len(CASES)}", summary)
            self.assertEqual(
                metadata["host_recorded_results"], {CASES[3]: "Timeout"}
            )

    def test_tampered_merge_or_dropped_record_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            metadata, artifacts, expected = self.build(root)
            forged = dict(artifacts)
            forged["results.qpa"] = artifacts["results.qpa"].replace(
                b"#terminateTestCaseResult Timeout",
                b'<Result StatusCode="Pass">result</Result>',
            )
            with self.assertRaisesRegex(gl.CampaignError, "not reproducible"):
                gl.validate_session_provenance(root, metadata, forged, len(expected))
            dropped = dict(metadata, host_recorded_results={})
            with self.assertRaisesRegex(gl.CampaignError, "host-recorded shard results"):
                gl.validate_session_provenance(root, dropped, artifacts, len(expected))
            relabelled = dict(metadata, artifacts_provenance="single-session-guest")
            with self.assertRaisesRegex(gl.CampaignError, "contradicts its sessions"):
                gl.validate_session_provenance(root, relabelled, artifacts, len(expected))
            over_bound = dict(metadata, max_resumptions=0)
            with self.assertRaisesRegex(gl.CampaignError, "recorded resumption bound"):
                gl.validate_session_provenance(root, over_bound, artifacts, len(expected))

    def test_replaced_session_evidence_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            metadata, artifacts, expected = self.build(root)
            (root / "sessions/01/console.log").write_text("replaced\n")
            with self.assertRaisesRegex(gl.CampaignError, "session evidence changed"):
                gl.validate_session_provenance(root, metadata, artifacts, len(expected))


class ShutdownBoundTests(unittest.TestCase):
    """A guest that delivered its evidence must still stop within a bound.

    An unkillable wedged case can hold the poweroff open while the guest
    kernel keeps printing stall reports, so a bound derived from console
    silence never fires and the shard would burn its whole guest timeout.
    """

    def wait(self, *, deliver: bool, grace: int, timeout: int):
        frames = [
            "UML_CTS_ARTIFACT_BEGIN|summary.txt|gzip+base64|10|" + "0" * 64 + "\n",
            "UML_CTS_ARTIFACT_CHUNK|summary.txt|0|AAAA\n",
        ]
        if deliver:
            frames.append(f"{gl.FINAL_ARTIFACT_FRAME}1\n")

        class Transport:
            connection = None

            def __init__(self, path: Path) -> None:
                self.lines: list[str] = []
                self.pending = list(frames)

            def service(self, timeout: float = 0.0) -> None:
                time.sleep(0.02)
                if self.pending:
                    self.lines.append(self.pending.pop(0))

            def finish(self) -> str:
                return "".join(self.lines)

            def close(self) -> None:
                pass

        class Process:
            returncode = None

            def poll(self) -> None:
                return None

            def wait(self, timeout: float | None = None) -> None:
                return None

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            console = root / "console.log"
            console.write_text("[  900.0] rcu: kthread starved\n")
            with mock.patch.object(gl, "AcknowledgedArtifactSocket", Transport):
                gl.wait_for_guest(
                    Process(), console, timeout, root / "socket",
                    console_stall_timeout=0, shutdown_grace=grace,
                )

    def test_a_delivered_guest_that_never_stops_is_bounded(self) -> None:
        with self.assertRaises(gl.GuestSessionInterrupted) as caught:
            self.wait(deliver=True, grace=1, timeout=60)
        self.assertEqual(caught.exception.reason, "stalled")
        self.assertIn("did not stop within 1 seconds", str(caught.exception))
        # The partial evidence travels with the interruption, which is what a
        # resumption is rebuilt from.
        self.assertIn(gl.FINAL_ARTIFACT_FRAME, caught.exception.artifact_transcript)
        self.assertIn("rcu:", caught.exception.transcript)
        # A shard that already holds every acknowledged frame lost no coverage.
        self.assertTrue(caught.exception.delivered)

    def test_the_bound_is_off_before_the_final_frame_and_when_disabled(self) -> None:
        for deliver, grace in ((False, 1), (True, 0)):
            with self.assertRaises(gl.GuestSessionInterrupted) as caught:
                self.wait(deliver=deliver, grace=grace, timeout=2)
            self.assertIn("exceeded 2 seconds", str(caught.exception))
            self.assertEqual(caught.exception.delivered, deliver)

    def test_a_delivered_stall_completes_its_shard_and_stays_recorded(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            guests, sessions, expected, merged = drive(root, [
                Step(
                    outcome="stalled",
                    cases=CASES,
                    ended=True,
                    selected=CASES,
                    summary=b"cts_rc=0\n",
                    console="guest delivered its evidence and never stopped\n",
                    return_code=-15,
                    shutdown_stall=True,
                ),
            ])
            # One guest, no resumption, and every selected case kept.
            self.assertEqual(len(guests.calls), 1)
            self.assertEqual(expected, CASES)
            self.assertEqual(tuple(merged), CASES)
            self.assertEqual(sessions[0].outcome, "completed")
            self.assertIsNone(sessions[0].terminated_case)
            self.assertTrue(sessions[0].shutdown_stall)
            self.assertTrue(sessions[0].record(root)["shutdown_stall"])


class VulkanResumptionTests(unittest.TestCase):
    def test_resumed_launcher_skips_only_the_accounted_prefix(self) -> None:
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "current-must-pass", [], None, vk.Fraction(0, 8)
        )
        first = vk.build_receipt_guest_launcher(
            receipt, selection, page_size=16384, timeout=900, progress_timeout=600
        )
        resumed = vk.build_receipt_guest_launcher(
            receipt, selection, page_size=16384, timeout=900,
            progress_timeout=600, resume_after=1234,
        )
        self.assertIn("--deqp-fraction=0,8", first)
        self.assertIn("--deqp-fraction-mandatory-caselist-file=", first)
        self.assertIn('tail -n +1235 "$expected" > "$selected"', resumed)
        # The resumed guest re-enumerates the identical fraction and then runs
        # the remainder as an explicit list, so the filter must not run twice.
        run_block = resumed.rsplit("emit_prerun ||", 1)[1]
        self.assertNotIn("--deqp-fraction=", run_block)
        self.assertIn("results.qpa.%04d", resumed)
        self.assertIn("emit_prerun", resumed)

    def test_receipt_summary_requires_every_host_recorded_case(self) -> None:
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "exact-family", ["dEQP-VK.info.build"], None, vk.Fraction(0, 1)
        )
        with self.assertRaisesRegex(vk.CampaignError, "must record every terminated"):
            vk.validate_receipt_attempt(
                receipt, selection, "", {}, host_recorded={}, session_count=2
            )
        # One session that recorded its last case is a legitimate host merge,
        # so it reaches the ordinary artifact contract instead of this guard.
        with self.assertRaisesRegex(vk.CampaignError, "artifact set is incomplete"):
            vk.validate_receipt_attempt(
                receipt,
                selection,
                "",
                {},
                host_recorded={"dEQP-VK.info.build": "Timeout"},
                session_count=1,
            )


class GuestCheckpointProgramTests(unittest.TestCase):
    """Run the generated guest checkpoint function in a real POSIX shell."""

    def shell_function(self, launcher: str, name: str) -> str:
        match = re.search(rf"^{name}\(\)\n\{{\n.*?^\}}\n", launcher, re.M | re.S)
        self.assertIsNotNone(match, f"{name} is not a top-level shell function")
        return match.group(0)

    def test_checkpoint_segments_reconstruct_the_guest_file(self) -> None:
        shell = shutil.which("sh")
        self.assertIsNotNone(shell)
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "current-must-pass", [], None, vk.Fraction(0, 8)
        )
        launcher = vk.build_receipt_guest_launcher(
            receipt, selection, page_size=16384, timeout=900, progress_timeout=600
        )
        flush = self.shell_function(launcher, "flush_qpa")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = qpa_session(CASES[:4], ended=False).encode()
            harness = f"""set -eu
output={root}
qpa="$output/results.qpa"
qpa_offset=0
qpa_segments=0
emit_artifact()
{{
    cp "$2" "$output/sent-$1"
    printf '%s\\n' "$1" >> "$output/order"
}}
{flush}
head -c 40 "$output/source" > "$qpa"
flush_qpa 0
cat "$output/source" > "$qpa"
flush_qpa 0
flush_qpa 1
flush_qpa 1
printf '%s %s\\n' "$qpa_offset" "$qpa_segments" > "$output/state"
"""
            (root / "source").write_bytes(payload)
            (root / "harness.sh").write_text(harness)
            subprocess.run([shell, str(root / "harness.sh")], check=True)
            order = (root / "order").read_text().split()
            self.assertEqual(order, ["results.qpa.0000", "results.qpa.0001"])
            joined = b"".join(
                (root / f"sent-{name}").read_bytes() for name in order
            )
            self.assertEqual(joined, payload)
            self.assertEqual(
                (root / "state").read_text().split(), [str(len(payload)), "2"]
            )
            transcript = "".join(
                framed(name, (root / f"sent-{name}").read_bytes()) for name in order
            )
            self.assertEqual(
                gl.decode_artifacts(transcript), {"results.qpa": payload}
            )

    def test_empty_guest_qpa_still_transports_one_segment(self) -> None:
        shell = shutil.which("sh")
        receipt = vk.receipt_contract.validate_receipt(vk.RECEIPT_PATH)
        selection = vk.resolve_receipt_selection(
            "smoke", [], None, vk.Fraction(0, 1)
        )
        launcher = vk.build_receipt_guest_launcher(
            receipt, selection, page_size=16384, timeout=900
        )
        flush = self.shell_function(launcher, "flush_qpa")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            harness = f"""set -eu
output={root}
qpa="$output/results.qpa"
qpa_offset=0
qpa_segments=0
emit_artifact()
{{
    cp "$2" "$output/sent-$1"
    printf '%s\\n' "$1" >> "$output/order"
}}
{flush}
: > "$qpa"
flush_qpa 0
flush_qpa 1
"""
            (root / "harness.sh").write_text(harness)
            subprocess.run([shell, str(root / "harness.sh")], check=True)
            self.assertEqual(
                (root / "order").read_text().split(), ["results.qpa.0000"]
            )
            self.assertEqual((root / "sent-results.qpa.0000").read_bytes(), b"")


if __name__ == "__main__":
    unittest.main()
