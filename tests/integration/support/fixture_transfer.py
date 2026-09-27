# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a static guest probe fetched over the SLiRP gateway.

A runner hands over one payload: the probe itself, or an archive holding
``probe`` and its fixtures. The host serves it on its loopback, which the
guest reaches as the SLiRP gateway 10.0.2.2. A disposable overlay-shell guest
fetches and checksums the payload, runs the probe, and the run fails on a
missing PASS marker, an unclean shutdown, or a kernel failure in the
transcript.
"""

from __future__ import annotations
# Imported support must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import contextlib
import functools
import hashlib
import http.server
import pathlib
import secrets
import shlex
import struct
import tempfile
import threading
from collections.abc import Iterator, Sequence

from guest import Guest


BOOT_TIMEOUT_SECONDS = 120
SHUTDOWN_TIMEOUT_SECONDS = 45
REPORT_TIMEOUT_SECONDS = 20
PROBE_SIZE_LIMIT = 8 * 1024 * 1024
# ELF e_machine values of the x86_64 and AArch64 guest ABIs.
GUEST_MACHINES = (62, 183)
SLIRP_GATEWAY = "10.0.2.2"
WORKSPACE = "/tmp/uml-probe"
RESULT_LOG = "/tmp/uml-probe-result.log"
KERNEL_FAILURE_MARKERS = (
    "Kernel panic",
    "BUG:",
    "Oops:",
    "Illegal instruction",
    "detected stalls on CPUs",
)


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *_args: object) -> None:
        pass


def parse_arguments(description: str | None) -> argparse.Namespace:
    """Parse a probe runner's command line and check that the probe is one."""
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument(
        "--page-size", required=True, type=int, choices=(4096, 16384)
    )
    parser.add_argument("--log", type=pathlib.Path)
    arguments = parser.parse_args()
    binary = arguments.probe.read_bytes()
    if (
        not 64 <= len(binary) <= PROBE_SIZE_LIMIT
        or binary[:6] != b"\x7fELF\x02\x01"
    ):
        parser.error("probe must be a bounded little-endian ELF64 executable")
    if struct.unpack_from("<H", binary, 18)[0] not in GUEST_MACHINES:
        parser.error("probe must use the x86_64 or AArch64 guest ABI")
    return arguments


@contextlib.contextmanager
def serve_payload(payload: bytes) -> Iterator[str]:
    """Serve payload on the host loopback and yield the guest's URL for it."""
    with tempfile.TemporaryDirectory(prefix="uml-probe-payload-") as directory:
        name = secrets.token_hex(16)
        pathlib.Path(directory, name).write_bytes(payload)
        server = http.server.ThreadingHTTPServer(
            ("127.0.0.1", 0), functools.partial(QuietHandler, directory=directory)
        )
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            yield f"http://{SLIRP_GATEWAY}:{server.server_port}/{name}"
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)
            if thread.is_alive():
                raise RuntimeError("fixture server did not stop")


def fetch_command(url: str, destination: str, digest: str) -> str:
    """Fetch url to destination with curl or wget and verify its SHA-256."""
    source = shlex.quote(url)
    target = shlex.quote(destination)
    return (
        "if command -v curl >/dev/null; then "
        f"curl -fsS --connect-timeout 5 --max-time 30 {source} -o {target}; "
        f"else wget -q -T 30 -O {target} {source}; fi; "
        f"echo '{digest}  {destination}' | sha256sum -c - >/dev/null"
    )


def run_probe(
    arguments: argparse.Namespace,
    *,
    subject: str,
    payload: bytes,
    payload_name: str,
    kernel_arguments: Sequence[str],
    probe_timeout_seconds: int,
    command_timeout_seconds: int,
    pass_marker: str,
    probe_arguments: Sequence[str] = (),
    precondition: str | None = None,
    unpack: str | None = None,
    engine: str = "jit",
) -> None:
    """Boot a guest, fetch payload as payload_name, and run ``./probe``.

    precondition runs before the fetch and unpack after it; the probe's
    output is kept in RESULT_LOG and must contain pass_marker."""
    digest = hashlib.sha256(payload).hexdigest()
    probe = shlex.join(
        ("timeout", str(probe_timeout_seconds), "./probe", *probe_arguments)
    )
    with serve_payload(payload) as url, Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "2G",
        "tmpfs",
        kernel_arguments=kernel_arguments,
        overlay_shell=True,
        engine=engine,
    ) as guest:
        try:
            guest.boot_and_login(BOOT_TIMEOUT_SECONDS)
            steps = ["set -e"]
            if precondition:
                steps.append(precondition)
            steps += [
                f'test "$(getconf PAGESIZE)" = {arguments.page_size}',
                f"mkdir -p {WORKSPACE}",
                f"cd {WORKSPACE}",
                fetch_command(url, payload_name, digest),
            ]
            if unpack:
                steps.append(unpack)
            steps += ["chmod 700 probe", f"{probe} >{RESULT_LOG} 2>&1"]
            result = guest.run_command("; ".join(steps), command_timeout_seconds)
            report = guest.run_command(f"cat {RESULT_LOG}", REPORT_TIMEOUT_SECONDS)
            print(report.output, flush=True)
            if (
                result.return_code != 0
                or report.return_code != 0
                or pass_marker not in report.output
            ):
                raise RuntimeError(f"guest {subject} failed")
            if (
                not guest.shutdown(SHUTDOWN_TIMEOUT_SECONDS)
                or guest.return_code != 0
            ):
                raise RuntimeError(f"{subject} guest did not shut down cleanly")
            transcript = guest.output()
            if any(marker in transcript for marker in KERNEL_FAILURE_MARKERS):
                raise RuntimeError(f"{subject} guest reported a kernel failure")
        finally:
            if arguments.log:
                arguments.log.parent.mkdir(parents=True, exist_ok=True)
                arguments.log.write_text(guest.output())
