#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The stand-in debugger that serves the JIT's request for executable memory.

``JitService`` decides each request from the three registers and writes the
blessing byte through a function it is given, so the decisions run here with
a fake memory: one byte per 16 KiB page of a region the process mapped, a
refusal for anything else, and a detach on command. The host side's parts
that need no device are checked too: the pid of the launch a debugger attaches
to (the one ``devicectl`` reports for it, never one found in a listing), the
lldb command line, and how the Android runner records a run's JIT.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import json
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))
sys.path.insert(0, str(REPOSITORY / "tests" / "perf"))
sys.path.insert(0, str(REPOSITORY / "tools"))
import andarium  # noqa: E402
import ios_device  # noqa: E402
import ios_jit_debugger as debugger  # noqa: E402
import run_ios_device_android as runner  # noqa: E402

REGION = 0x10_4000_0000
PAGE = debugger.PAGE_BYTES


def service(*, fail_at: int | None = None) -> tuple[debugger.JitService, list[int], list[str]]:
    writes: list[int] = []
    messages: list[str] = []

    def write_byte(address: int) -> bool:
        if fail_at is not None and len(writes) == fail_at:
            return False
        writes.append(address)
        return True

    return debugger.JitService(write_byte, messages.append), writes, messages


class ServiceTests(unittest.TestCase):
    def test_a_prepare_request_writes_one_byte_in_each_page(self) -> None:
        jit, writes, _ = service()
        reply = jit.handle(debugger.decode(debugger.PREPARE, REGION, 5 * PAGE))
        self.assertEqual(reply, debugger.Reply(x0=REGION))
        self.assertEqual(writes, [REGION + n * PAGE for n in range(5)])
        self.assertEqual(jit.prepared_pages, 5)
        self.assertFalse(jit.detached)

    def test_a_length_that_ends_inside_a_page_covers_that_page(self) -> None:
        jit, writes, _ = service()
        jit.handle(debugger.decode(debugger.PREPARE, REGION, 2 * PAGE + 1))
        self.assertEqual(len(writes), 3)

    def test_a_request_the_debugger_cannot_serve_is_refused_with_no_write(self) -> None:
        for address, length in (
            (0, PAGE),                       # the process must map the region
            (REGION + 8, PAGE),              # not page aligned
            (debugger.ADDRESS_LIMIT - PAGE, 2 * PAGE),  # past 40 bits
        ):
            with self.subTest(address=address):
                jit, writes, messages = service()
                reply = jit.handle(debugger.decode(debugger.PREPARE, address, length))
                self.assertEqual(reply, debugger.Reply(x0=0))
                self.assertEqual(writes, [])
                self.assertTrue(messages and "refused" in messages[0])

    def test_a_write_that_fails_refuses_the_whole_request(self) -> None:
        jit, writes, _ = service(fail_at=2)
        reply = jit.handle(debugger.decode(debugger.PREPARE, REGION, 4 * PAGE))
        self.assertEqual(reply, debugger.Reply(x0=0))
        self.assertEqual(len(writes), 2)

    def test_an_empty_region_needs_no_write(self) -> None:
        jit, writes, _ = service()
        self.assertEqual(
            jit.handle(debugger.decode(debugger.PREPARE, REGION, 0)),
            debugger.Reply(x0=REGION))
        self.assertEqual(writes, [])

    def test_detach_is_asked_for_once_and_remembered(self) -> None:
        jit, writes, _ = service()
        reply = jit.handle(debugger.decode(debugger.DETACH, 0, 0))
        self.assertEqual(reply.detach, True)
        self.assertTrue(jit.detached)
        self.assertEqual(writes, [])

    def test_a_command_it_does_not_know_is_resumed_past(self) -> None:
        jit, writes, messages = service()
        self.assertIsNone(jit.handle(debugger.decode(7, REGION, PAGE)))
        self.assertEqual(writes, [])
        self.assertIn("unknown", messages[0])

    def test_the_command_is_the_low_half_of_the_register(self) -> None:
        # w16 is what the process sets; the upper half is whatever x16 held.
        request = debugger.decode(0xDEAD_BEEF_0000_0001, REGION, PAGE)
        self.assertEqual(request.command, debugger.PREPARE)

    def test_the_request_is_brk_f00d(self) -> None:
        self.assertEqual(debugger.REQUEST_WORD, 0xD4200000 | (0xF00D << 5))


class HostSideTests(unittest.TestCase):
    """The launch names the process the debugger attaches to. The device's
    list of processes can still hold an older instance of the application, and
    attaching to that one would leave the new process stopped for ever."""

    REPORT = {"result": {"launchOptions": {"startStopped": True}, "process": {
        "executable": "file:///private/var/containers/Bundle/Application/X/Andarium.app/Andarium",
        "processIdentifier": 2345}}}

    def test_the_pid_is_the_one_the_launch_reports(self) -> None:
        self.assertEqual(ios_device.launched_pid(self.REPORT), 2345)

    def test_a_report_that_names_no_process_names_no_pid(self) -> None:
        for report in ({}, [], None, {"result": {}}, {"result": {"process": {}}},
                       {"result": {"process": {"processIdentifier": "1"}}},
                       {"result": {"process": {"processIdentifier": True}}}):
            with self.subTest(report=report):
                self.assertIsNone(ios_device.launched_pid(report))

    def test_the_wait_returns_the_pid_once_the_report_is_written(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            report = pathlib.Path(name) / "launch.json"
            launch = subprocess.Popen(["sleep", "30"])
            try:
                # A report half written is not a report yet.
                report.write_text('{"result": {"pro', encoding="utf-8")
                writer = threading.Timer(
                    0.6, lambda: report.write_text(json.dumps(self.REPORT), encoding="utf-8"))
                writer.start()
                self.assertEqual(ios_device.wait_for_launched_pid(report, launch, 20), 2345)
                writer.join()
            finally:
                launch.kill()
                launch.wait()

    def test_the_wait_gives_up_when_devicectl_ended_without_a_report(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            launch = subprocess.Popen(["true"])
            launch.wait()
            with self.assertRaises(ios_device.DeviceError):
                ios_device.wait_for_launched_pid(pathlib.Path(name) / "none.json", launch, 20)

    def test_the_wait_is_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            launch = subprocess.Popen(["sleep", "30"])
            try:
                with self.assertRaises(ios_device.DeviceError):
                    ios_device.wait_for_launched_pid(pathlib.Path(name) / "none.json", launch, 1)
            finally:
                launch.kill()
                launch.wait()

    def test_the_launch_is_stopped_and_writes_the_report_the_pid_is_read_from(self) -> None:
        command = debugger.launch_command("device", "com.example.app", pathlib.Path("/work/launch.json"))
        self.assertEqual(command[:4], ["xcrun", "devicectl", "device", "process"])
        for flag in ("--start-stopped", "--console", "--terminate-existing"):
            self.assertIn(flag, command)
        self.assertEqual(command[command.index("--json-output") + 1], "/work/launch.json")
        self.assertEqual(command[-1], "com.example.app")

    def test_the_runner_s_console_does_the_same_and_clears_an_old_report(self) -> None:
        application = ios_device.Application(
            bundle_id="com.example.app", setting_prefixes=(), runtime_result=re.compile("x"))
        with tempfile.TemporaryDirectory() as name:
            log = pathlib.Path(name) / "console.log"
            stopped = ios_device.Console("device", application, {}, log, start_stopped=True)
            self.assertIn("--start-stopped", stopped._command)
            self.assertEqual(
                stopped._command[stopped._command.index("--json-output") + 1], str(stopped.report))
            plain = ios_device.Console("device", application, {}, log)
            self.assertNotIn("--start-stopped", plain._command)
            self.assertNotIn("--json-output", plain._command)
            # The report of an earlier launch is gone before the next one.
            stopped.report.write_text(json.dumps(self.REPORT), encoding="utf-8")
            with mock.patch.object(ios_device.subprocess, "Popen") as popen:
                popen.return_value.poll.return_value = None
                with stopped:
                    self.assertFalse(stopped.report.exists())
                    self.assertIs(popen.call_args.kwargs["stdin"], subprocess.DEVNULL)
            self.assertFalse(stopped.report.exists())

    def test_lldb_attaches_to_the_pid_and_serves_with_a_log_and_a_bound(self) -> None:
        command = debugger.attach_command(
            "device", 77, pathlib.Path("/work/serve.log"), 900)
        self.assertEqual(command[:3], ["xcrun", "lldb", "-b"])
        script = " ".join(command)
        self.assertIn("device select device", script)
        self.assertIn("device process attach -p 77", script)
        self.assertIn("ios_jit_debugger.py", script)
        self.assertIn("jit_serve --log /work/serve.log --timeout 900", script)

    def test_serving_starts_lldb_on_the_given_pid_without_listing_processes(self) -> None:
        with mock.patch.object(debugger.subprocess, "Popen") as popen:
            debugger.start_serving("device", 4242, pathlib.Path("/work/serve.log"), serve_timeout=60)
        command = popen.call_args.args[0]
        self.assertIn("device process attach -p 4242", " ".join(command))
        self.assertFalse(hasattr(debugger, "wait_for_pid"))
        self.assertFalse(hasattr(debugger, "find_pid"))


class RunnerTests(unittest.TestCase):
    def test_a_jit_run_asks_for_the_application_s_jit_setting(self) -> None:
        settings = andarium.settings_for(
            cpus=2, memory_mib=1536, engine="jit", host_port=21100)
        self.assertEqual(settings["ANDARIUM_JIT"], "1")
        # The runtime's own selection stays automatic: the application's
        # setting, and the debugger, decide.
        self.assertEqual(settings["KERNARIUM_EXECUTION_ENGINE"], "auto")
        gadgets = andarium.settings_for(
            cpus=2, memory_mib=1536, engine="gadgets", host_port=21100)
        self.assertNotIn("ANDARIUM_JIT", gadgets)

    def test_the_record_says_whether_the_jit_ran_and_what_the_debugger_did(self) -> None:
        with tempfile.TemporaryDirectory() as name:
            log = pathlib.Path(name) / "jit-debugger.log"
            log.write_text(
                "10:00:00 request Request(command=1, address=4398046511104, length=100663296)\n"
                "10:00:41 prepared 0x10400000000+0x6000000: 6144 pages in 40.8 s\n")
            ran = runner.jit_record("Andarium: JIT ready\n", log)
            self.assertEqual(ran, {
                "ran": True, "fallback": None, "region_bytes": 0x6000000,
                "region_pages": 6144, "bless_seconds": 40.8})
            fell = runner.jit_record(
                "Andarium: Started without JIT: StikDebug is not connected.\n",
                pathlib.Path(name) / "absent.log")
            self.assertEqual(fell, {
                "ran": False,
                "fallback": "Started without JIT: StikDebug is not connected."})


if __name__ == "__main__":
    unittest.main()
