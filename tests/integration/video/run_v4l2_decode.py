#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode every advertised V4L2 profile in a disposable Linux guest."""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

import argparse
import functools
import hashlib
import http.server
from pathlib import Path
import secrets
import shlex
import struct
import sys
import tarfile
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/integration/support"))
from guest import Guest  # noqa: E402


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args: object) -> None:
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=Path)
    parser.add_argument("--rootfs", required=True, type=Path)
    parser.add_argument("--probe", required=True, type=Path)
    parser.add_argument("--page-size", required=True, type=int, choices=(4096, 16384))
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    binary = args.probe.read_bytes()
    if not 64 <= len(binary) <= 8 * 1024 * 1024 or binary[:6] != b"\x7fELF\x02\x01":
        parser.error("probe must be a bounded little-endian ELF64 executable")
    if struct.unpack_from("<H", binary, 18)[0] not in (62, 183):
        parser.error("probe must use the x86_64 or AArch64 guest ABI")
    with tempfile.TemporaryDirectory(prefix="uml-v4l2-fixtures-") as temporary:
        directory = Path(temporary)
        archive = directory / (secrets.token_hex(16) + ".tar.gz")
        with tarfile.open(archive, "w:gz") as output:
            output.add(args.probe, arcname="probe")
            for fixture in sorted((ROOT / "tests/support/video").iterdir()):
                if fixture.suffix in (".bin", ".raw"):
                    output.add(fixture, arcname="video/" + fixture.name)
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0),
            functools.partial(Handler, directory=temporary))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        guest = None
        try:
            guest = Guest(args.emulator.resolve(), args.rootfs.resolve(), "2G", "tmpfs",
                kernel_arguments=("--video", "ncpus=2", "panic=1"), overlay_shell=True)
            guest.boot_and_login(120)
            url = f"http://10.0.2.2:{server.server_port}/{archive.name}"
            command = (
                "set -e; test -c /dev/video0; "
                f"test \"$(getconf PAGESIZE)\" = {args.page_size}; "
                "ip link set eth0 up; ip addr replace 10.0.2.15/24 dev eth0; "
                "mkdir -p /tmp/uml-v4l2; cd /tmp/uml-v4l2; "
                "if command -v curl >/dev/null; then "
                f"curl -fsS --connect-timeout 5 --max-time 30 {shlex.quote(url)} -o payload.tar.gz; "
                f"else wget -q -T 30 -O payload.tar.gz {shlex.quote(url)}; fi; "
                f"echo '{digest}  payload.tar.gz' | sha256sum -c - >/dev/null; "
                "tar -xzf payload.tar.gz; chmod 700 probe; "
                "timeout 300 ./probe video >/tmp/uml-v4l2-result.log 2>&1"
            )
            result = guest.run_command(command, 360)
            report = guest.run_command("cat /tmp/uml-v4l2-result.log", 20)
            print(report.output, flush=True)
            if result.return_code != 0 or report.return_code != 0 or "UML_V4L2_DECODE: PASS" not in report.output:
                raise RuntimeError("guest V4L2 decoder regression failed")
            if not guest.shutdown(45) or guest.return_code != 0:
                raise RuntimeError("V4L2 guest did not shut down cleanly")
            transcript = guest.output()
            if any(marker in transcript for marker in (
                "Kernel panic", "BUG:", "Oops:", "Illegal instruction",
                "detected stalls on CPUs",
            )):
                raise RuntimeError("V4L2 guest reported a kernel failure")
            return 0
        finally:
            if guest is not None:
                if args.log:
                    args.log.parent.mkdir(parents=True, exist_ok=True)
                    args.log.write_text(guest.output())
                guest.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
            if thread.is_alive():
                raise RuntimeError("fixture server did not stop")


if __name__ == "__main__":
    raise SystemExit(main())
