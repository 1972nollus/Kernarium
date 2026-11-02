#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A Virtio sound stream starts afresh after a system suspend.

The ALSA core restarts a stream that was suspended at offset 0, and the
Virtio sound driver's prepare must then start from nothing too: no position,
no queued period and no pending message of the stream before the suspend. A
suspend the driver kept the position across counted it as played, so the
stream's period completions moved the position two or three periods each and
the application wrote twice as fast as the sound plays. A stream paused
across a suspend could neither be recovered nor, after it was closed, be
opened again, because the driver still counted the periods the device had
held. The driver's freeze marks every substream suspended and forgets its
messages, and its prepare resets each message's fill level.

The guest boots with the null audio output and two vCPUs and suspends itself
with an RTC alarm two seconds ahead. virtio_sound_suspend_probe.py checks the
stream position after a suspend, a prepare after a half-filled period, a
stream paused across a suspend, and a reopen after one, with exact
assertions and no rates; its header describes each case. Every case must
pass, and the guest must print no kernel warning, oops or host fault report.
The contract is the suspend paragraph of kernel/mainline-6.18/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 2
BOOT_SECONDS = 180
PROBE_SECONDS = 240
PROBE = "/opt/uml-test/virtio_sound_suspend_probe.py"
SUSPEND_EXIT = "PM: suspend exit"
# The probe suspends once in each of these cases.
SUSPENDING_CASES = 3
FAILURE = re.compile(
    r"WARNING:|\bBUG\b|Oops|Call Trace:|Kernel panic|Darwin UML panic|"
    r"Darwin POSIX|Kernel mode fault|rcu: INFO"
)
CASE_FAILURE = re.compile(r"^VSND (\w+) FAIL: (.*)$", re.M)
DRIVER_COMPLAINT = re.compile(r"^.*virtio_snd.*$", re.M)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument(
        "--case", action="append", help="a probe case to run (default: all)"
    )
    arguments = parser.parse_args()

    probe = (REPOSITORY / "tests/integration/kernel"
             / "virtio_sound_suspend_probe.py").read_bytes()
    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        (f"ncpus={VCPUS}", "--audio-output=null"),
        overlay_shell=True,
        engine=arguments.engine,
        payloads={PROBE: GuestFile(probe, 0o755)},
    ) as guest:
        guest.boot_to_shell(BOOT_SECONDS)
        request = len(guest.output())
        command = " ".join(["python3", "-B", PROBE, *(arguments.case or [])])
        result = guest.run_command(command, PROBE_SECONDS)
        tail = guest.output()[request:]
        problems = [
            f"{case}: {reason}" for case, reason in CASE_FAILURE.findall(result.output)
        ]
        if result.return_code != 0 and not problems:
            problems.append(f"the probe exited with status {result.return_code}")
        failure = FAILURE.search(tail)
        if failure is not None:
            problems.append(f"the guest printed {failure[0]!r}")
        if (
            not problems
            and arguments.case is None
            and tail.count(SUSPEND_EXIT) < SUSPENDING_CASES
        ):
            problems.append(
                f"the guest suspended {tail.count(SUSPEND_EXIT)} times, "
                f"expected {SUSPENDING_CASES}"
            )
        if problems:
            complaints = DRIVER_COMPLAINT.findall(tail)
            if complaints:
                print("driver messages:\n" + "\n".join(complaints))
            raise RuntimeError("; ".join(problems))
        guest.check_command("echo alive", 20)
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off")
    print("every stream restarted afresh after the suspend")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
