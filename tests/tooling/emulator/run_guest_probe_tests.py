#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin what the shared probe runner asks of a guest and when it fails.

tests/integration/support/run_guest_probe.py boots a guest, runs one probe and
fails on the probe's exit status. This test replaces the guest with a recorder
and checks the files the runner installs, the commands it sends in order, and
which outcomes raise, so no emulator starts.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock

SUPPORT = pathlib.Path(__file__).resolve().parents[2] / "integration/support"
sys.path.insert(0, str(SUPPORT))
SPEC = importlib.util.spec_from_file_location(
    "run_guest_probe", SUPPORT / "run_guest_probe.py"
)
assert SPEC is not None and SPEC.loader is not None
run_guest_probe = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_guest_probe)

GUEST_DIRECTORY = "/usr/local/libexec/uml-test"
MOUNT_TMP = ("check", "mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
ALIVE = ("check", "echo alive", 20)


class FakeGuest:
    """Records the session; the probe's outcome is fixed by the test."""

    def __init__(self, return_code: int, console: str) -> None:
        self.return_code = return_code
        self.console = console
        self.events: list[tuple] = []
        self.constructed: dict = {}

    def __call__(self, *positional, **keywords) -> FakeGuest:
        self.constructed = {"positional": positional, **keywords}
        return self

    def __enter__(self) -> FakeGuest:
        return self

    def __exit__(self, *exception: object) -> None:
        pass

    def boot_to_shell(self, timeout: int) -> None:
        self.events.append(("boot", timeout))

    def check_command(self, command: str, timeout: int) -> None:
        self.events.append(("check", command, timeout))

    def run_command(self, command: str, timeout: int):
        self.events.append(("run", command, timeout))
        return mock.Mock(return_code=self.return_code, output="probe output\n")

    def output(self) -> str:
        return self.console


class ProbeRunnerTests(unittest.TestCase):
    def run_probe(
        self,
        probe: str,
        *options: str,
        return_code: int = 0,
        console: str = "",
    ) -> tuple[FakeGuest, str, Exception | None]:
        guest = FakeGuest(return_code, console)
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / probe
            source.write_bytes(b"probe source")
            arguments = [
                "run_guest_probe.py",
                "--emulator", "/emulator/uml-darwin",
                "--rootfs", "/images/rootfs.erofs",
                "--probe", str(source),
                "--ncpus", "2",
                "--probe-timeout", "120",
                *options,
            ]
            output = io.StringIO()
            failure = None
            with (
                mock.patch.object(sys, "argv", arguments),
                mock.patch.object(run_guest_probe, "Guest", guest),
                contextlib.redirect_stdout(output),
            ):
                try:
                    self.assertEqual(run_guest_probe.main(), 0)
                except RuntimeError as error:
                    failure = error
        return guest, output.getvalue(), failure

    def test_python_probe_runs_under_python_and_a_final_command(self) -> None:
        guest, output, failure = self.run_probe("probe_a.py")
        self.assertIsNone(failure)
        self.assertEqual(output, "probe output\n")
        positional = guest.constructed["positional"]
        self.assertEqual(
            positional[2:5], ("1G", "tmpfs", ("ncpus=2",)))
        self.assertTrue(guest.constructed["init_shell"])
        self.assertEqual(guest.constructed["engine"], "jit")
        installed = guest.constructed["payloads"]
        self.assertEqual(list(installed), [f"{GUEST_DIRECTORY}/probe.py"])
        self.assertEqual(installed[f"{GUEST_DIRECTORY}/probe.py"].contents,
                         b"probe source")
        self.assertEqual(guest.events, [
            ("boot", 180),
            MOUNT_TMP,
            ("run", f"python3 -B {GUEST_DIRECTORY}/probe.py", 120),
            ALIVE,
        ])

    def test_a_failing_exit_status_fails_before_the_final_command(self) -> None:
        guest, output, failure = self.run_probe("probe_a.py", return_code=3)
        self.assertIn("probe_a.py reported 3 failure(s)", str(failure))
        self.assertEqual(output, "probe output\n")
        self.assertNotIn(ALIVE, guest.events)


if __name__ == "__main__":
    unittest.main()
