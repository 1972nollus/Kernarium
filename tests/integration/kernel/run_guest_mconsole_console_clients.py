#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A management-console command survives other vCPUs printing.

``sysrq`` and ``stack`` send what the kernel prints while they run to the
client that asked, so the console's write() walks a list of those clients
while any vCPU may print. The host asks for ``sysrq s`` before it suspends a
guest and flushes its disks. Three tasks pinned to vCPUs 1 to 3 of a 4-vCPU
guest keep writing to /dev/kmsg, so a vCPU other than the one running the
command owns the console whenever the command ends, and the runner asks for
``sysrq s`` from a fresh client socket, as the host does, EXCHANGES times or
for EXCHANGE_SECONDS. Every exchange must end with the final reply, the guest
must run on, and the console must show no fault, oops or panic. A kernel that
walks the list unlocked faulted at address 0x10 in console_write() within the
first thousand exchanges.

The contract is the management-console paragraph of kernel/common/README.md.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shutil
import socket
import struct
import sys
import tempfile
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


CPUS = 4
EXCHANGES = 10000
EXCHANGE_SECONDS = 30.0
REPLY_SECONDS = 20.0
MCONSOLE_MAGIC = 0xCAFEBABE
MCONSOLE_VERSION = 2
MCONSOLE_DATA = 512
WRITERS = (
    "writers=; for c in 1 2 3; do taskset -c $c sh -c 'while :; do "
    "echo \"console clients $$ 0123456789abcdef0123456789abcdef\" "
    "> /dev/kmsg; done' </dev/null >/dev/null 2>&1 & writers=\"$writers $!\"; "
    "done; echo $writers > /tmp/console-writers"
)
STOP_WRITERS = (
    "kill $(cat /tmp/console-writers); wait $(cat /tmp/console-writers); "
    "echo stopped"
)
FAILURE = re.compile(
    r"WARNING:|BUG:|Oops|Kernel panic|Darwin UML panic|Kernel mode fault|"
    r"Kernel mode signal"
)


class GuestGone(RuntimeError):
    pass


def exchange(guest: Guest, directory: pathlib.Path, serial: int,
             command: str) -> None:
    """One request from a client socket of its own, read to its final
    reply."""
    reply = directory / f"mc-clients-{serial}"
    reply.unlink(missing_ok=True)
    endpoint = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    try:
        endpoint.bind(str(reply))
        data = command.encode()
        try:
            endpoint.sendto(
                struct.pack("<III", MCONSOLE_MAGIC, MCONSOLE_VERSION, len(data))
                + data.ljust(MCONSOLE_DATA, b"\0"),
                str(directory / "mconsole"),
            )
        except (FileNotFoundError, ConnectionRefusedError) as error:
            guest.wait_for_exit(10.0)
            raise GuestGone(f"exchange {serial}: the console is gone") from error
        endpoint.setblocking(False)
        deadline = time.monotonic() + REPLY_SECONDS
        while True:
            try:
                answer = endpoint.recv(12 + MCONSOLE_DATA)
            except BlockingIOError:
                if guest.wait_for_exit(0.002):
                    raise GuestGone(f"exchange {serial}: the guest exited")
                if time.monotonic() > deadline:
                    raise RuntimeError(
                        f"exchange {serial}: no final reply in {REPLY_SECONDS:g} s"
                    )
                continue
            error, more, _ = struct.unpack_from("<III", answer)
            if error:
                raise RuntimeError(f"exchange {serial}: {command} was refused")
            if not more:
                return
    finally:
        endpoint.close()
        reply.unlink(missing_ok=True)


def run(emulator: pathlib.Path, rootfs: pathlib.Path, engine: str,
        private: pathlib.Path) -> None:
    with Guest(emulator, rootfs, "1G", "tmpfs", (f"ncpus={CPUS}",),
               init_shell=True, engine=engine) as guest:
        guest.boot_to_shell(180)
        directory = private / f"uml-kern{guest.process_id}"
        guest.check_command(WRITERS, 30)
        started = time.monotonic()
        done = 0
        try:
            while done < EXCHANGES and time.monotonic() - started < EXCHANGE_SECONDS:
                exchange(guest, directory, done + 1, "sysrq s")
                done += 1
        except GuestGone as gone:
            output = guest.output() + "\n" + guest.host_output()
            failure = FAILURE.search(output)
            start = max(0, failure.start() - 2000) if failure else -6000
            raise RuntimeError(
                f"{gone} after {done} exchanges:\n" + output[start:][:6000]
            ) from None
        elapsed = time.monotonic() - started
        guest.run_command(STOP_WRITERS, 60)
        output = guest.output()
        failure = FAILURE.search(output)
        if failure is not None:
            raise RuntimeError(
                f"the guest printed {failure[0]!r}:\n"
                + output[max(0, failure.start() - 2000):][:6000]
            )
        print(f"{done} sysrq s exchanges in {elapsed:.1f} s while three vCPUs "
              "printed")
        if not guest.shutdown(60):
            raise RuntimeError("the guest did not power off")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    # Short, so the management console's directory fits sun_path inside it.
    private = pathlib.Path(tempfile.mkdtemp(prefix="uml-mcc-"))
    os.environ["TMPDIR"] = str(private)
    tempfile.tempdir = str(private)
    try:
        run(arguments.emulator.resolve(), arguments.rootfs.resolve(),
            arguments.engine, private)
    finally:
        shutil.rmtree(private, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
