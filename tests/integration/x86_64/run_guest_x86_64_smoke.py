#!/usr/bin/env python3
"""Boot the pure x86-64 smoke rootfs on the 4 KiB x86-64 runtime."""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import pty
import select
import signal
import subprocess
import sys
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import (  # noqa: E402
    emulator_arguments,
    remove_emulator_run_directories,
)


SUCCESS = b"x86_64 gadgets rootfs: syscall tls signal xstate ok"


def terminate(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is None:
        try:
            process.send_signal(signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()
    remove_emulator_run_directories(process.pid)


def boot(
    emulator: pathlib.Path, rootfs: pathlib.Path, engine: str, timeout: int
) -> bytes:
    master, slave = pty.openpty()
    os.set_blocking(master, False)
    try:
        process = subprocess.Popen(
            emulator_arguments(
                emulator,
                rootfs,
                "512M",
                kernel_arguments=(
                    "console=tty0",
                    "console=ttyS0",
                    "panic=1",
                ),
                init_path="/init",
                engine=engine,
            ),
            stdin=slave,
            stdout=slave,
            stderr=slave,
            close_fds=True,
        )
    finally:
        os.close(slave)

    transcript = bytearray()
    deadline = time.monotonic() + timeout
    try:
        while process.poll() is None:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(
                    f"x86-64 smoke guest did not stop within {timeout} seconds"
                )
            readable, _, _ = select.select([master], [], [], remaining)
            if not readable:
                continue
            try:
                transcript.extend(os.read(master, 65536))
            except BlockingIOError:
                pass
            except OSError:
                if process.poll() is None:
                    raise
                break

        while True:
            try:
                chunk = os.read(master, 65536)
            except (BlockingIOError, OSError):
                break
            if not chunk:
                break
            transcript.extend(chunk)
        if process.returncode != 0:
            raise RuntimeError(
                f"x86-64 smoke guest exited with status {process.returncode}"
            )
        return bytes(transcript)
    except BaseException:
        terminate(process)
        if transcript:
            print(transcript.decode(errors="replace"), file=sys.stderr)
        raise
    finally:
        os.close(master)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("gadgets", "jit"), default="gadgets")
    parser.add_argument("--timeout", type=int, default=180)
    arguments = parser.parse_args()

    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        parser.error(f"emulator is not executable: {emulator}")
    if not rootfs.is_file():
        parser.error(f"rootfs is not a regular file: {rootfs}")
    if arguments.timeout <= 0:
        parser.error("timeout must be positive")

    try:
        transcript = boot(
            emulator, rootfs, arguments.engine, arguments.timeout
        )
        if SUCCESS not in transcript:
            raise RuntimeError("x86-64 smoke success marker is missing")
    except BaseException:
        if "transcript" in locals():
            print(transcript.decode(errors="replace"), file=sys.stderr)
        raise

    print(
        f"PASS engine={arguments.engine} userspace-slice=x86_64 "
        "kernel-mode=arm64 page-size=4k"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
