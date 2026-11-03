#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Write to the Virtio console and a UML serial line with allocations traced.

A console write runs with a spinlock held and interrupts off, so it must not
allocate: hvc throws away output its put_chars() failed to take for want of
memory, and a UML line that could not allocate its output ring reported
room it lacked, so the line discipline counted characters that never
reached the host as written. The Virtio console sends from a buffer it
allocated with the port, and a line allocates its ring when it opens.

The runner serves hvc0 on a --virtio-console-socket it reads as output
arrives, and ttyS1 on a host pipe it leaves unread for a few seconds, so the
line's ring fills and its writer must wait. The guest traces slab and
non-sleeping page allocations with their stacks while it writes the same
numbered records to both (console_write_memory_probe.py). The trace must
lose nothing and show no allocation under put_chars() or line_write(), the
ttyS1 writer must have waited for the host, both channels must deliver
every byte in order, ttyS1's with each newline turned into a carriage
return and a newline, and the guest must print no kernel warning and power
off cleanly.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import socket
import sys
import tempfile
import threading
import time

REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


VCPUS = 2
RECORDS = 4096
RECORD = 64
# The ttyS1 channel stays unread this long after the probe starts: far
# longer than the 256 KiB the probe writes needs to fill the host pipe and
# the line's 4 KiB ring.
HOLD_SECONDS = 3.0
DELIVERY_SECONDS = 30
PROBE = "/opt/uml-test/console_write_memory_probe.py"
PROBE_SECONDS = 120
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Call Trace:|page allocation failure|"
    r"atomic allocation failed|Kernel panic|Darwin UML panic|Darwin POSIX|"
    r"Kernel mode fault"
)
RESULT = re.compile(r"^CONSOLE_(WRITES|ALLOCATIONS) (.*)$", re.MULTILINE)


def record(index: int) -> bytes:
    body = f"R{index:06d}:" + "abcdefghijklmnopqrstuvwxyz0123456789" * 2
    return (body[:RECORD - 1] + "\n").encode()


class Drain(threading.Thread):
    """Collect one host channel's output until told to stop."""

    def __init__(self, receive) -> None:
        super().__init__(daemon=True)
        self._receive = receive
        self.opened = threading.Event()
        self.stop = threading.Event()
        self.data = bytearray()
        self.failure: BaseException | None = None

    def run(self) -> None:
        try:
            while not self.stop.is_set():
                if not self.opened.wait(0.05):
                    continue
                chunk = self._receive()
                if chunk is None:
                    continue
                if not chunk:
                    return
                self.data += chunk
        except BaseException as error:  # reported by the main thread
            self.failure = error


def socket_receiver(path: str):
    """Connect to the emulator's console socket once it listens."""
    connection: socket.socket | None = None

    def receive() -> bytes | None:
        nonlocal connection
        if connection is None:
            candidate = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                candidate.connect(path)
            except OSError:
                candidate.close()
                time.sleep(0.1)
                return None
            candidate.settimeout(0.2)
            connection = candidate
        try:
            return connection.recv(65536)
        except socket.timeout:
            return None

    return receive


def pipe_receiver(descriptor: int):
    os.set_blocking(descriptor, False)

    def receive() -> bytes | None:
        try:
            return os.read(descriptor, 65536)
        except BlockingIOError:
            time.sleep(0.01)
            return None

    return receive


def parse(output: str) -> dict[str, dict[str, float]]:
    results: dict[str, dict[str, float]] = {}
    for match in RESULT.finditer(output):
        results[match[1]] = {
            key: float(value)
            for key, value in (field.split("=") for field in match[2].split())
        }
    return results


def await_bytes(drain: Drain, size: int) -> None:
    deadline = time.monotonic() + DELIVERY_SECONDS
    while len(drain.data) < size and time.monotonic() < deadline:
        if drain.failure is not None:
            raise RuntimeError(f"reading a console failed: {drain.failure}")
        time.sleep(0.1)


def check(emulator: pathlib.Path, rootfs: pathlib.Path, engine: str) -> None:
    probe = (
        REPOSITORY / "tests/integration/console/console_write_memory_probe.py"
    ).read_bytes()
    expected = b"".join(record(index) for index in range(RECORDS))
    with tempfile.TemporaryDirectory(prefix="uml-console-memory-") as private:
        console = os.path.join(private, "console.sock")
        hvc = Drain(socket_receiver(console))
        hvc.opened.set()
        line_read, line_write = os.pipe()
        line = Drain(pipe_receiver(line_read))
        hvc.start()
        line.start()
        try:
            try:
                guest = Guest(
                    emulator,
                    rootfs,
                    "512M",
                    "tmpfs",
                    (
                        f"ncpus={VCPUS}",
                        f"--virtio-console-socket={console}",
                        f"ssl1=null,fd:{line_write}",
                    ),
                    init_shell=True,
                    engine=engine,
                    pass_fds=(line_write,),
                    payloads={PROBE: GuestFile(probe, 0o755)},
                )
            finally:
                os.close(line_write)
            with guest:
                guest.boot_to_shell(180)
                mark = len(guest.output())
                release = threading.Timer(HOLD_SECONDS, line.opened.set)
                release.start()
                try:
                    output = guest.check_command(
                        f"python3 {PROBE} {RECORDS}", PROBE_SECONDS
                    )
                finally:
                    release.cancel()
                    line.opened.set()
                results = parse(output)
                for name in ("WRITES", "ALLOCATIONS"):
                    if name not in results:
                        raise RuntimeError(
                            f"the probe printed no CONSOLE_{name} line:\n"
                            + output[-4000:]
                        )
                writes, allocations = results["WRITES"], results["ALLOCATIONS"]
                for channel in ("hvc", "line"):
                    if writes[channel] != len(expected):
                        raise RuntimeError(
                            f"{channel} took {int(writes[channel])} of "
                            f"{len(expected)} bytes:\n" + output[-4000:]
                        )
                if allocations["lost"] != 0:
                    raise RuntimeError(
                        f"the trace lost {int(allocations['lost'])} events:\n"
                        + output[-4000:]
                    )
                if allocations["hvc"] or allocations["line"]:
                    raise RuntimeError(
                        f"console writes allocated memory: put_chars() "
                        f"{int(allocations['hvc'])} times, line_write() "
                        f"{int(allocations['line'])} times:\n" + output[-4000:]
                    )
                if writes["line_seconds"] < HOLD_SECONDS / 2:
                    raise RuntimeError(
                        f"the ttyS1 writer finished in {writes['line_seconds']} s "
                        "while the host held the line unread, so its ring "
                        "never held it back:\n" + output[-4000:]
                    )
                await_bytes(hvc, len(expected))
                line_expected = expected.replace(b"\n", b"\r\n")
                await_bytes(line, len(line_expected))
                for channel, drain, want in (
                    ("hvc0", hvc, expected),
                    ("ttyS1", line, line_expected),
                ):
                    got = bytes(drain.data)
                    if got != want:
                        same = next(
                            (i for i, (a, b) in enumerate(zip(got, want)) if a != b),
                            min(len(got), len(want)),
                        )
                        raise RuntimeError(
                            f"{channel} delivered {len(got)} of {len(want)} "
                            f"bytes, first difference at byte {same}"
                        )
                tail = guest.output()[mark:]
                failure = FAILURE.search(tail)
                if failure is not None:
                    raise RuntimeError(
                        f"the guest printed {failure[0]!r}:\n"
                        + tail[max(0, failure.start() - 2000):][:8000]
                    )
                if not guest.shutdown(60) or guest.return_code != 0:
                    raise RuntimeError("the guest did not power off cleanly")
                print(
                    f"hvc0 and ttyS1 delivered {len(expected)} bytes each; "
                    f"the ttyS1 writer waited {writes['line_seconds']:.1f} s; "
                    f"{int(allocations['events'])} allocations traced, none "
                    "by a console write"
                )
        finally:
            hvc.stop.set()
            line.stop.set()
            hvc.join(5)
            line.join(5)
            os.close(line_read)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()
    check(arguments.emulator.resolve(), arguments.rootfs.resolve(), arguments.engine)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
