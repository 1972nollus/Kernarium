#!/usr/bin/env python3
"""Exercise authenticated rootfs provisioning with a small EROFS fixture."""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import hashlib
import json
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

import provision  # noqa: E402
from verify import VerificationError, verify  # noqa: E402


def make_fixture(path: Path) -> dict[str, object]:
    block_size = 4096
    blocks = 2
    image = bytearray(block_size * blocks)
    superblock = memoryview(image)[1024 : 1024 + 144]
    struct.pack_into("<III", superblock, 0, 0xE0F5E1E2, 0x12345678, 0x5)
    superblock[12] = 12
    struct.pack_into("<Q", superblock, 16, 3)
    struct.pack_into("<I", superblock, 36, blocks)
    fixture_uuid = uuid.UUID("12345678-1234-5678-9abc-def012345678")
    superblock[48:64] = fixture_uuid.bytes
    superblock[64:80] = b"fixture\0" + b"\0" * 8
    struct.pack_into("<I", superblock, 80, 0x3)

    path.write_bytes(image)
    return {
        "schema_version": 3,
        "image": "rootfs.erofs",
        "sha256": hashlib.sha256(image).hexdigest(),
        "size": len(image),
        "filesystem": {
            "format": "erofs",
            "superblock_offset": 1024,
            "magic": "0xe0f5e1e2",
            "checksum": "0x12345678",
            "feature_compat": "0x00000005",
            "feature_incompat": "0x00000003",
            "available_compr_algs": "0x0000",
            "extra_devices": 0,
            "block_size": block_size,
            "blocks": blocks,
            "inodes": 3,
            "uuid": str(fixture_uuid),
            "volume_name": "fixture",
        },
    }


def run(arguments: list[str]) -> int:
    with mock.patch.object(sys, "argv", ["provision.py", *arguments]):
        return provision.main()


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="uml-rootfs-provision-test-") as name:
        root = Path(name)
        source = root / "published-rootfs.erofs"
        manifest_value = make_fixture(source)
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps(manifest_value), encoding="utf-8")
        verify(source, manifest)

        invalid = root / "invalid.erofs"
        invalid_bytes = bytearray(source.read_bytes())
        struct.pack_into("<I", invalid_bytes, 1024, 0)
        invalid.write_bytes(invalid_bytes)
        invalid_manifest_value = dict(manifest_value)
        invalid_manifest_value["sha256"] = hashlib.sha256(invalid_bytes).hexdigest()
        invalid_manifest = root / "invalid.json"
        invalid_manifest.write_text(
            json.dumps(invalid_manifest_value), encoding="utf-8"
        )
        try:
            verify(invalid, invalid_manifest)
        except VerificationError as error:
            if "does not have an EROFS" not in str(error):
                raise
        else:
            raise RuntimeError("invalid EROFS fixture was accepted")

        output = root / "local" / "rootfs.erofs"
        if run(
            [
                "--source",
                str(source),
                "--output",
                str(output),
                "--manifest",
                str(manifest),
            ]
        ) != 0:
            raise RuntimeError("local provisioning failed")
        verify(output, manifest)
        if run(
            [
                "--source",
                str(source),
                "--output",
                str(output),
                "--manifest",
                str(manifest),
            ]
        ) == 0:
            raise RuntimeError("provisioning replaced an existing image")
        if run(
            [
                "--source",
                str(source),
                "--output",
                str(output),
                "--manifest",
                str(manifest),
                "--replace",
            ]
        ) != 0:
            raise RuntimeError("explicit rootfs replacement failed")
        verify(output, manifest)

        raced_output = root / "raced" / "rootfs.erofs"
        raced_output.parent.mkdir()
        real_verify = provision.verify

        def publish_during_verification(image: Path, manifest_path: Path):
            result = real_verify(image, manifest_path)
            raced_output.write_bytes(b"concurrent publisher")
            return result

        with mock.patch.object(
            provision, "verify", side_effect=publish_during_verification
        ):
            status = run(
                [
                    "--source",
                    str(source),
                    "--output",
                    str(raced_output),
                    "--manifest",
                    str(manifest),
                ]
            )
        if status == 0 or raced_output.read_bytes() != b"concurrent publisher":
            raise RuntimeError("no-replace provisioning lost a concurrent output")

        statement = root / "release-statement.json"
        statement.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "artifact": manifest_value["image"],
                    "sha256": manifest_value["sha256"],
                    "size": manifest_value["size"],
                }
            ),
            encoding="utf-8",
        )
        private_key = root / "release-private.pem"
        public_key = root / "release-public.pem"
        signature = root / "release-statement.sig"
        subprocess.run(
            [
                "openssl",
                "genpkey",
                "-algorithm",
                "RSA",
                "-pkeyopt",
                "rsa_keygen_bits:2048",
                "-out",
                private_key,
            ],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            ["openssl", "pkey", "-in", private_key, "-pubout", "-out", public_key],
            check=True,
            capture_output=True,
        )
        subprocess.run(
            [
                "openssl",
                "dgst",
                "-sha256",
                "-sign",
                private_key,
                "-out",
                signature,
                statement,
            ],
            check=True,
            capture_output=True,
        )

        downloads = {
            "https://release.invalid/rootfs.erofs": source,
            "https://release.invalid/release-statement.json": statement,
            "https://release.invalid/release-statement.sig": signature,
        }

        def copy_fixture(url: str, destination: Path) -> None:
            shutil.copyfile(downloads[url], destination)

        release_output = root / "release" / "rootfs.erofs"
        with mock.patch.object(provision, "copy_from_url", side_effect=copy_fixture):
            status = run(
                [
                    "--url",
                    "https://release.invalid/rootfs.erofs",
                    "--statement-url",
                    "https://release.invalid/release-statement.json",
                    "--signature-url",
                    "https://release.invalid/release-statement.sig",
                    "--public-key",
                    str(public_key),
                    "--allow-network",
                    "--output",
                    str(release_output),
                    "--manifest",
                    str(manifest),
                ]
            )
        if status != 0:
            raise RuntimeError("authenticated release provisioning failed")
        verify(release_output, manifest)

        tampered = root / "tampered-statement.json"
        tampered.write_text(
            statement.read_text(encoding="utf-8") + " ", encoding="utf-8"
        )
        try:
            provision.verify_signature(tampered, signature, public_key)
        except VerificationError:
            pass
        else:
            raise RuntimeError("tampered release statement was accepted")

    print("rootfs provisioning tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
