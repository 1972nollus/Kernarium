#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
from pathlib import Path
import re
import select
import subprocess
import tempfile
import time


VNC_PORT = re.compile(rb"^VNC display: [^:]+:(\d+) ")


def output_text(output: bytes | bytearray) -> str:
    return bytes(output).decode("utf-8", errors="replace")


def wait_for_vnc_port(server: subprocess.Popen[bytes], timeout: float) -> int:
    assert server.stdout is not None
    deadline = time.monotonic() + timeout
    output = bytearray()
    pending = bytearray()
    descriptor = server.stdout.fileno()
    while time.monotonic() < deadline:
        readable, _, _ = select.select([descriptor], [], [], 0.05)
        if readable:
            chunk = os.read(descriptor, 4096)
            if chunk:
                output.extend(chunk)
                pending.extend(chunk)
                while b"\n" in pending:
                    line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    match = VNC_PORT.match(line)
                    if match is not None:
                        port = int(match.group(1))
                        if port != 0:
                            return port
        if server.poll() is not None:
            remaining = server.stdout.read()
            if remaining:
                output.extend(remaining)
            raise RuntimeError(
                "VNC server exited early:\n" + output_text(output))
    raise TimeoutError(
        "VNC server did not announce a dynamically bound port:\n" +
        output_text(output))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    options = parser.parse_args()
    password = "umltest"

    with tempfile.TemporaryDirectory(prefix="uml-gpu-vnc-") as directory:
        vhost_socket = Path(directory) / "gpu.sock"
        server = subprocess.Popen(
            [
                str(options.server),
                "--socket",
                str(vhost_socket),
                "--vnc-port",
                "0",
                "--vnc-password",
                password,
                "--width",
                "64",
                "--height",
                "48",
                "--self-test-pattern",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            bufsize=0,
        )
        try:
            port = wait_for_vnc_port(server, 5)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    output, _ = server.communicate()
                    raise RuntimeError(
                        "VNC server exited early:\n" + output_text(output))
                if vhost_socket.exists():
                    break
                time.sleep(0.05)
            else:
                raise RuntimeError("VNC server did not become ready")

            rejected = subprocess.run(
                [
                    str(options.client),
                    "--password",
                    "wrong",
                    str(port),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=10,
            )
            if rejected.returncode == 0:
                raise RuntimeError("VNC server accepted an incorrect password")

            subprocess.run(
                [
                    str(options.client),
                    "--password",
                    password,
                    str(port),
                ],
                check=True,
                timeout=10,
            )
            subprocess.run(
                [
                    str(options.client),
                    "--password",
                    password,
                    "--input",
                    str(port),
                ],
                check=True,
                timeout=10,
            )
        finally:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
