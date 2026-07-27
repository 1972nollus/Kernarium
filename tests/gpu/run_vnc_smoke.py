#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import time


def unused_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--client", type=Path, required=True)
    options = parser.parse_args()
    port = unused_port()
    password = "umltest"

    with tempfile.TemporaryDirectory(prefix="uml-gpu-vnc-") as directory:
        vhost_socket = Path(directory) / "gpu.sock"
        server = subprocess.Popen(
            [
                str(options.server),
                "--socket",
                str(vhost_socket),
                "--vnc-port",
                str(port),
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
            text=True,
        )
        try:
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if server.poll() is not None:
                    output, _ = server.communicate()
                    raise RuntimeError(f"VNC server exited early:\n{output}")
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
