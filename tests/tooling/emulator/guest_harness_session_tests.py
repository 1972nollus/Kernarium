#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prove the guest harness cannot leave an emulator running after it dies."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import os
import pathlib
import signal
import subprocess
import sys
import tempfile
import time
import unittest

SUPPORT = pathlib.Path(__file__).resolve().parents[2] / "integration/support"
sys.path.insert(0, str(SUPPORT))
import guest as guest_support  # noqa: E402


# A stand-in for uml-darwin. Guest only builds an argument vector and starts the
# process, so a payload that ignores its arguments and waits exercises the real
# construction path without a kernel, a rootfs, or a boot.
STAND_IN = "#!/bin/sh\nexec sleep 3600\n"

# A stand-in for a runner, so the test can kill a harness the way a shell job,
# a CTest timeout or a Ctrl-C does: by signalling its whole process group.
HARNESS = "\n".join(
    (
        "import pathlib, sys, time",
        f"sys.path.insert(0, {str(SUPPORT)!r})",
        "import guest as guest_support",
        "emulator = pathlib.Path(sys.argv[1])",
        "guest = guest_support.Guest(",
        "    emulator=emulator,",
        "    rootfs=pathlib.Path('/nonexistent/rootfs.erofs'),",
        "    memory='4G',",
        "    workspace='tmpfs',",
        "    init_shell=True,",
        ")",
        "print(guest.process_id, flush=True)",
        "time.sleep(3600)",
    )
)


def process_alive(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def wait_until_gone(pid: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not process_alive(pid):
            return True
        time.sleep(0.05)
    return not process_alive(pid)


class AttachedGuestSessionTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory(prefix="uml-guest-harness-")
        self.addCleanup(directory.cleanup)
        self.emulator = pathlib.Path(directory.name, "emulator")
        self.emulator.write_text(STAND_IN, encoding="utf-8")
        self.emulator.chmod(0o755)

    def test_the_emulator_joins_the_harness_process_group(self) -> None:
        # This is the whole mechanism: sharing the group is what makes a group
        # kill reach the guest. Started in a session of its own it would be
        # unreachable, and a harness killed outright would leave it running.
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)
        self.assertEqual(os.getpgid(guest.process_id), os.getpgid(0))

    def test_return_code_reports_the_attached_process_status(self) -> None:
        self.emulator.write_text("#!/bin/sh\nexit 23\n", encoding="utf-8")
        guest = guest_support.Guest(
            emulator=self.emulator,
            rootfs=pathlib.Path("/nonexistent/rootfs.erofs"),
            memory="4G",
            workspace="tmpfs",
            init_shell=True,
        )
        self.addCleanup(guest.close)

        deadline = time.monotonic() + 5
        while guest.return_code is None and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(guest.return_code, 23)
        with self.assertRaises(AttributeError):
            guest.return_code = 0

    def test_a_killed_harness_group_takes_its_guest_with_it(self) -> None:
        harness = subprocess.Popen(
            [sys.executable, "-c", HARNESS, str(self.emulator)],
            stdout=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        self.addCleanup(self.reap, harness)
        assert harness.stdout is not None
        emulator_pid = int(harness.stdout.readline().strip())
        self.addCleanup(self.reap_emulator, emulator_pid)
        self.assertTrue(process_alive(emulator_pid))

        os.killpg(harness.pid, signal.SIGKILL)
        harness.wait()
        self.assertTrue(
            wait_until_gone(emulator_pid, 30),
            "the guest outlived its harness group; an orphan boots to idle and "
            "still burns host cores, quietly oversubscribing later benchmarks",
        )

    def reap(self, harness: subprocess.Popen[str]) -> None:
        if harness.poll() is None:
            harness.kill()
        harness.wait()
        if harness.stdout is not None:
            harness.stdout.close()

    def reap_emulator(self, emulator_pid: int) -> None:
        try:
            os.kill(emulator_pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass


if __name__ == "__main__":
    unittest.main()
