#!/usr/bin/env python3
"""Exercise writable-rootfs safety guards without launching a guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import hashlib
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
from unittest import mock

import maintenance


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def test_authorization_is_required(root: Path) -> None:
    try:
        maintenance.main(["--recovery-artifact", str(root / "recovery.img")])
    except RuntimeError as error:
        require("--authorize-rootfs-write" in str(error), "wrong authorization failure")
    else:
        raise RuntimeError("writable maintenance accepted no authorization")


def test_schema_two_lock_and_mode_guards(root: Path) -> None:
    emulator = root / "emulator"
    emulator.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    emulator.chmod(0o755)
    rootfs = root / "rootfs.img"
    rootfs.write_bytes(b"frozen-rootfs-fixture")
    rootfs.chmod(0o444)
    recovery = root / "recovery.img"
    recovery.write_bytes(rootfs.read_bytes())
    recovery.chmod(0o444)
    digest = hashlib.sha256(rootfs.read_bytes()).hexdigest()
    manifest = root / "manifest.json"
    manifest.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "sha256": digest,
                "size": rootfs.stat().st_size,
            }
        ),
        encoding="utf-8",
    )
    loaded = maintenance.load_baseline(manifest)
    require(loaded["schema_version"] == 2, "schema 2 baseline was rejected")

    def fake_snapshot(path: Path) -> dict[str, object]:
        value = path.read_bytes()
        metadata = path.stat()
        return {
            "path": str(path),
            "sha256": hashlib.sha256(value).hexdigest(),
            "size": len(value),
            "inode": metadata.st_ino,
            "mtime_ns": metadata.st_mtime_ns,
            "mode": stat.S_IMODE(metadata.st_mode),
        }

    def fake_run(
        _command: list[str], _input: bytes, _log: Path, _timeout: float
    ) -> tuple[int, bool, bool]:
        require(
            stat.S_IMODE(rootfs.stat().st_mode) == maintenance.MAINTENANCE_MODE,
            "rootfs was not writable during the authorized callback",
        )
        probe = subprocess.run(
            [
                sys.executable,
                "-c",
                (
                    "from pathlib import Path; from image_lock import image_lock; "
                    "import sys; "
                    "p=Path(sys.argv[1]); "
                    "\ntry:\n"
                    "  with image_lock(p, exclusive=False, blocking=False): sys.exit(2)\n"
                    "except BlockingIOError: sys.exit(0)\n"
                ),
                str(rootfs),
            ],
            check=False,
            env={**os.environ, "PYTHONPATH": str(Path(__file__).parent)},
        )
        require(probe.returncode == 0, "exclusive maintenance lock was released early")
        return 0, True, False

    with mock.patch.object(maintenance, "validated_recovery", return_value=fake_snapshot(recovery)), \
         mock.patch.object(maintenance, "snapshot", side_effect=fake_snapshot), \
         mock.patch.object(maintenance, "run", side_effect=fake_run):
        result = maintenance.main(
            [
                "--authorize-rootfs-write",
                "--emulator", str(emulator),
                "--rootfs", str(rootfs),
                "--baseline", str(manifest),
                "--recovery-artifact", str(recovery),
                "--artifact-root", str(root / "artifacts"),
            ]
        )
    require(result == 0, "guarded maintenance fixture failed")
    require(
        stat.S_IMODE(rootfs.stat().st_mode) == maintenance.FROZEN_MODE,
        "rootfs mode was not refrozen",
    )


def test_process_group_cleanup() -> None:
    process = subprocess.Popen(
        ["/bin/sh", "-c", "sleep 30 & wait"],
        start_new_session=True,
    )
    maintenance.terminate_process_group(process)
    require(process.poll() is not None, "maintenance process group survived cleanup")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="uml-rootfs-maintenance-test-") as value:
        root = Path(value)
        test_authorization_is_required(root)
        test_schema_two_lock_and_mode_guards(root)
    test_process_group_cleanup()
    print("rootfs maintenance safety tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
