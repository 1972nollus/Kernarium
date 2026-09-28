#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode every advertised V4L2 profile in a disposable Linux guest."""
from __future__ import annotations
__import__("sys").dont_write_bytecode = True

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tests/integration/support"))
import fixture_transfer  # noqa: E402


def main() -> int:
    arguments = fixture_transfer.parse_arguments(__doc__)
    fixtures = {
        "video/" + fixture.name: fixture.read_bytes()
        for fixture in sorted((ROOT / "tests/support/video").iterdir())
        if fixture.suffix in (".bin", ".raw")
    }
    fixture_transfer.run_probe(
        arguments,
        subject="V4L2 decoder regression",
        fixtures=fixtures,
        kernel_arguments=("--video", "ncpus=2", "panic=1"),
        precondition="test -c /dev/video0",
        probe_arguments=("video",),
        probe_timeout_seconds=300,
        command_timeout_seconds=360,
        pass_marker="UML_V4L2_DECODE: PASS",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
