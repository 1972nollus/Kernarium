#!/usr/bin/env python3
"""Exercise authenticated rootfs provisioning with a small GPT fixture."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from unittest import mock

ROOTFS_TOOL_ROOT = Path(__file__).resolve().parents[3] / "tools/rootfs"
sys.path.insert(0, str(ROOTFS_TOOL_ROOT))

import provision
from verify import verify


LINUX_FILESYSTEM_GUID = "0fc63daf-8483-4772-8e79-3d69d8477de4"


def make_fixture(path: Path) -> dict[str, object]:
    sector_size = 512
    sector_count = 4096
    image = bytearray(sector_size * sector_count)
    mbr = memoryview(image)[:sector_size]
    mbr[446 + 4] = 0xEE
    struct.pack_into("<II", mbr, 446 + 8, 1, sector_count - 1)
    mbr[510:512] = b"\x55\xaa"

    header = memoryview(image)[sector_size : 2 * sector_size]
    header[:8] = b"EFI PART"
    struct.pack_into("<I", header, 8, 0x0001_0000)
    struct.pack_into("<I", header, 12, 92)
    struct.pack_into("<QQ", header, 24, 1, sector_count - 1)
    struct.pack_into("<QQ", header, 40, 34, sector_count - 34)
    header[56:72] = uuid.UUID("12345678-1234-5678-9abc-def012345678").bytes_le
    struct.pack_into("<QII", header, 72, 2, 128, 128)

    first_lba = 2048
    last_lba = 3000
    entry = memoryview(image)[2 * sector_size : 2 * sector_size + 128]
    entry[:16] = uuid.UUID(LINUX_FILESYSTEM_GUID).bytes_le
    entry[16:32] = uuid.UUID("87654321-4321-8765-cba9-876543210fed").bytes_le
    struct.pack_into("<QQ", entry, 32, first_lba, last_lba)

    path.write_bytes(image)
    os.chmod(path, 0o444)
    digest = hashlib.sha256(image).hexdigest()
    return {
        "schema_version": 2,
        "image": "rootfs.img",
        "sha256": digest,
        "size": len(image),
        "frozen_mode": "0444",
        "partition_table": {
            "scheme": "gpt",
            "sector_size": sector_size,
            "protective_mbr": {
                "type": "0xee",
                "first_lba": 1,
                "sector_count": sector_count - 1,
            },
            "gpt": {
                "header_lba": 1,
                "first_usable_lba": 34,
                "last_usable_lba": sector_count - 34,
                "partition_entry_lba": 2,
                "partition_entry_count": 128,
                "partition_entry_size": 128,
            },
            "partitions": [
                {
                    "number": 1,
                    "type_guid": LINUX_FILESYSTEM_GUID,
                    "first_lba": first_lba,
                    "last_lba": last_lba,
                }
            ],
        },
    }


def run(arguments: list[str]) -> int:
    with mock.patch.object(sys, "argv", ["provision.py", *arguments]):
        return provision.main()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="uml-rootfs-provision-test-") as temporary:
        root = Path(temporary)
        source = root / "published-rootfs.img"
        manifest_value = make_fixture(source)
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps(manifest_value, sort_keys=True), encoding="utf-8")

        statement = root / "release-statement.json"
        statement.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "artifact": manifest_value["image"],
                    "sha256": manifest_value["sha256"],
                    "size": manifest_value["size"],
                },
                sort_keys=True,
                separators=(",", ":"),
            )
            + "\n",
            encoding="utf-8",
        )
        private_key = root / "release-private.pem"
        public_key = root / "release-public.pem"
        signature = root / "release-statement.sig"
        subprocess.run(
            ["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048", "-out", private_key],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["openssl", "pkey", "-in", private_key, "-pubout", "-out", public_key],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", private_key, "-out", signature, statement],
            check=True,
            capture_output=True,
        )

        provision.verify_signature(statement, signature, public_key)
        provision.verify_release_statement(statement, manifest_value)

        local_output = root / "empty-local-workspace" / "rootfs.img"
        if run(["--source", str(source), "--output", str(local_output), "--manifest", str(manifest)]) != 0:
            raise RuntimeError("local empty-workspace provisioning failed")
        verify(local_output, manifest)

        release_output = root / "empty-release-workspace" / "rootfs.img"
        downloads = {
            "https://release.invalid/rootfs.img": source,
            "https://release.invalid/release-statement.json": statement,
            "https://release.invalid/release-statement.sig": signature,
        }

        def copy_fixture(url: str, destination: Path) -> None:
            shutil.copyfile(downloads[url], destination)

        with mock.patch.object(provision, "copy_from_url", side_effect=copy_fixture):
            result = run(
                [
                    "--url", "https://release.invalid/rootfs.img",
                    "--statement-url", "https://release.invalid/release-statement.json",
                    "--signature-url", "https://release.invalid/release-statement.sig",
                    "--public-key", str(public_key),
                    "--allow-network",
                    "--output", str(release_output),
                    "--manifest", str(manifest),
                ]
            )
        if result != 0:
            raise RuntimeError("authenticated empty-workspace provisioning failed")
        verify(release_output, manifest)

        tampered = root / "tampered-statement.json"
        tampered.write_text(statement.read_text(encoding="utf-8") + " ", encoding="utf-8")
        try:
            provision.verify_signature(tampered, signature, public_key)
        except provision.VerificationError:
            pass
        else:
            raise RuntimeError("tampered release statement was accepted")

    print("rootfs provisioning tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
