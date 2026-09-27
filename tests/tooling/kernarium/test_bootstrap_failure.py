#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A failed step of a Kernarium console bootstrap stops where it should.

Kernarium types the bootstrap into the shell the kernel starts as init, through
the guest console: a terminal in canonical mode, which hands the shell one line
per read. This test feeds composed bootstraps to dash, the guest's /bin/sh, the
same way, through a pseudo-terminal with echo off so that only what the
commands print comes back. The host has no /dev/ubdb, so the first check of the
overlay root fails.

The desktop bootstrap then hands the terminal to its rescue shell. Nothing
after that check may run, neither the rest of the bootstrap nor a mode's
inserted setup, and the rescue shell must run the next command typed at it.

A measured workload reports its failure marker and powers the guest off, which
a shell function stands in for here. Its body must not run.

Both shells run in a sandbox that denies every file write and every other
executable, so a bootstrap that does keep going cannot change the host.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import fcntl
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import termios
import time

GUEST_SHELL = Path("/bin/dash")
SANDBOX = Path("/usr/bin/sandbox-exec")
FAILURE_PREFIX = b"Unable to construct the Kernarium ext4 overlay root: "
FIRST_FAILURE = FAILURE_PREFIX + b"overlay block device"
# The markers are computed by the shell, so they appear only when a command
# producing them actually runs, never as a copy of the input.
MODE_SETUP = "echo KERNARIUM_MODE_SETUP_$((6 * 7))"
MODE_SETUP_RAN = b"KERNARIUM_MODE_SETUP_42"
RESCUE_COMMAND = b"echo KERNARIUM_RESCUE_SHELL_$((6 * 7))\n"
RESCUE_RAN = b"KERNARIUM_RESCUE_SHELL_42"
# Lines the desktop bootstrap prints only after its first check has passed.
LATER_OUTPUT = (
    b"Kernarium shared ext4 state",
    b"Kernarium desktop setup is owned by the rootfs image",
    b"Darwin UML root overlay",
)
WORKLOAD_MARKER = "KERNARIUM_TEST_WORKLOAD_ERROR"
WORKLOAD_FAILURE = WORKLOAD_MARKER.encode() + b": "
WORKLOAD_FIRST_FAILURE = WORKLOAD_FAILURE + b"overlay block device"
WORKLOAD_BODY = "echo KERNARIUM_WORKLOAD_$((6 * 7))"
WORKLOAD_RAN = b"KERNARIUM_WORKLOAD_42"
# The halted guest reads no more of its console: the stand-in for poweroff
# swallows every line typed after it until the terminal ends.
POWEROFF_STAND_IN = (
    b"sync() { :; }\n"
    b"poweroff() { echo KERNARIUM_POWEROFF_$((6 * 7));"
    b" while read -r _; do :; done; exit 0; }\n"
)
POWEROFF_RAN = b"KERNARIUM_POWEROFF_42"
END_OF_INPUT = b"\x04"
SANDBOX_PROFILE = """(version 1)
(allow default)
(deny network*)
(deny file-write*)
(allow file-write-data
    (literal "/dev/null")
    (literal "/dev/tty")
    (regex #"^/dev/ttys[0-9]+$"))
(deny process-exec*)
(allow process-exec*
    (literal "/bin/dash")
    (literal "/bin/sh")
    (literal "/bin/bash")
    (literal "/bin/zsh"))
"""
DEADLINE_SECONDS = 30.0


class Terminal:
    """The host side of the pseudo-terminal the shells run on."""

    def __init__(self, master: int, deadline: float) -> None:
        self.master = master
        self.deadline = deadline
        self.output = bytearray()
        self.closed = False

    def _remaining(self) -> float:
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("the shell did not answer in time")
        return remaining

    def _drain(self, timeout: float) -> None:
        if self.closed:
            time.sleep(min(timeout, 0.05))
            return
        readable, _, _ = select.select([self.master], [], [], timeout)
        if not readable:
            return
        try:
            chunk = os.read(self.master, 65536)
        except OSError:
            chunk = b""
        if chunk:
            self.output.extend(chunk)
        else:
            self.closed = True

    def type(self, data: bytes) -> None:
        """Types data while collecting output, so neither side can stall."""
        view = memoryview(data)
        while view:
            self._remaining()
            readable, writable, _ = select.select(
                [] if self.closed else [self.master], [self.master], [], 0.1
            )
            if readable:
                self._drain(0)
            if writable:
                written = os.write(self.master, view[:512])
                view = view[written:]

    def wait_for(self, marker: bytes) -> None:
        while marker not in self.output:
            if self.closed:
                raise TimeoutError(
                    f"the terminal closed before {marker!r} appeared")
            self._drain(min(self._remaining(), 0.2))

    def settle(self, process: subprocess.Popen[bytes]) -> None:
        while process.poll() is None:
            self._drain(min(self._remaining(), 0.2))
        # Collect whatever the shell wrote just before it exited.
        end = time.monotonic() + 0.5
        while not self.closed and time.monotonic() < end:
            self._drain(0.1)


def controlling_terminal() -> None:
    os.setsid()
    fcntl.ioctl(0, termios.TIOCSCTTY, 0)


def converse(steps: list[tuple[bytes, bytes | None]]) -> tuple[bytes, str]:
    """Types each step's input into dash, then waits for its marker, if any."""
    master, slave = os.openpty()
    attributes = termios.tcgetattr(slave)
    # The guest console discipline: whole lines, no local echo.
    attributes[3] |= termios.ICANON
    attributes[3] &= ~(termios.ECHO | termios.ECHONL)
    termios.tcsetattr(slave, termios.TCSANOW, attributes)
    terminal = Terminal(master, time.monotonic() + DEADLINE_SECONDS)
    with tempfile.TemporaryDirectory(prefix="kernarium-bootstrap-") as home:
        environment = {
            "HOME": home,
            "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
            "PS1": "# ",
            "PS2": "> ",
            "TERM": "dumb",
        }
        process = subprocess.Popen(
            [str(SANDBOX), "-p", SANDBOX_PROFILE, str(GUEST_SHELL)],
            stdin=slave,
            stdout=slave,
            stderr=slave,
            env=environment,
            close_fds=True,
            preexec_fn=controlling_terminal,
        )
        os.close(slave)
        stalled = ""
        try:
            for typed, marker in steps:
                terminal.type(typed)
                if marker is not None:
                    terminal.wait_for(marker)
            terminal.settle(process)
        except TimeoutError as error:
            stalled = str(error)
        finally:
            if process.poll() is None:
                process.kill()
            process.wait(timeout=10)
            os.close(master)
    return bytes(terminal.output), stalled


def print_bootstrap(printer: Path, *arguments: str) -> bytes:
    return subprocess.run(
        [str(printer), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        timeout=30,
    ).stdout


def report(name: str, failures: list[str], output: bytes) -> list[str]:
    if failures:
        print(f"{name} bootstrap output:", file=sys.stderr)
        print(output.decode(errors="replace"), file=sys.stderr)
    return [f"{name}: {failure}" for failure in failures]


def check_desktop(printer: Path) -> list[str]:
    bootstrap = print_bootstrap(printer, "desktop", MODE_SETUP)
    # The relay ends the bootstrap with one more newline.
    output, stalled = converse([
        (bootstrap + b"\n", FIRST_FAILURE),
        (RESCUE_COMMAND, RESCUE_RAN),
        (b"exit\n", None),
    ])
    failures = [stalled] if stalled else []
    if output.count(FAILURE_PREFIX) != 1 or FIRST_FAILURE not in output:
        failures.append("the bootstrap did not stop at its first failed check")
    if MODE_SETUP_RAN in output:
        failures.append("the inserted mode setup ran after the failure")
    for line in LATER_OUTPUT:
        if line in output:
            failures.append(f"the bootstrap kept going: {line.decode()!r}")
    if RESCUE_RAN not in output:
        failures.append("the rescue shell did not run a typed command")
    return report("desktop", failures, output)


def check_workload(printer: Path) -> list[str]:
    bootstrap = print_bootstrap(
        printer, "workload", WORKLOAD_MARKER, WORKLOAD_BODY)
    output, stalled = converse([
        (POWEROFF_STAND_IN + bootstrap + b"\n", POWEROFF_RAN),
        (END_OF_INPUT, None),
    ])
    failures = [stalled] if stalled else []
    if (output.count(WORKLOAD_FAILURE) != 1 or
            WORKLOAD_FIRST_FAILURE not in output):
        failures.append("the bootstrap did not report its first failed check")
    if POWEROFF_RAN not in output:
        failures.append("the bootstrap did not power the guest off")
    if WORKLOAD_RAN in output:
        failures.append("the workload body ran after the failure")
    return report("workload", failures, output)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--printer",
        type=Path,
        required=True,
        help="executable that writes a composed console bootstrap",
    )
    options = parser.parse_args()
    for required in (GUEST_SHELL, SANDBOX):
        if not os.access(required, os.X_OK):
            print(f"FAIL: {required} is unavailable", file=sys.stderr)
            return 1
    failures = check_desktop(options.printer) + check_workload(options.printer)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    print("PASS: a failed desktop check stops at a usable rescue shell")
    print("PASS: a failed workload check reports and powers off")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
