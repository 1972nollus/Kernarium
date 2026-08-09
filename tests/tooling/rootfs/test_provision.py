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

import provision
from image_lock import image_lock
from verify import VerificationError, verify


def make_fixture(path: Path) -> dict[str, object]:
    block_size = 4096
    blocks = 2
    image = bytearray(block_size * blocks)
    superblock = memoryview(image)[1024 : 1024 + 144]
    struct.pack_into("<III", superblock, 0, 0xE0F5E1E2, 0x12345678, 0x5)
    superblock[12] = 12
    struct.pack_into("<H", superblock, 14, 37)
    struct.pack_into("<Q", superblock, 16, 3)
    struct.pack_into("<I", superblock, 36, blocks)
    fixture_uuid = uuid.UUID("12345678-1234-5678-9abc-def012345678")
    superblock[48:64] = fixture_uuid.bytes
    superblock[64:80] = b"fixture\0" + b"\0" * 8
    struct.pack_into("<I", superblock, 80, 0x3)

    path.write_bytes(image)
    digest = hashlib.sha256(image).hexdigest()
    return {
        "schema_version": 3,
        "image": "rootfs.erofs",
        "sha256": digest,
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


def manifested_copy(
    root: Path,
    name: str,
    image_value: bytes,
    manifest_value: dict[str, object],
) -> tuple[Path, Path]:
    image = root / f"{name}.erofs"
    image.write_bytes(image_value)
    value = json.loads(json.dumps(manifest_value))
    value["size"] = len(image_value)
    value["sha256"] = hashlib.sha256(image_value).hexdigest()
    manifest = root / f"{name}.json"
    manifest.write_text(json.dumps(value, sort_keys=True), encoding="utf-8")
    return image, manifest


def require_rejected(image: Path, manifest: Path, diagnostic: str) -> None:
    try:
        verify(image, manifest)
    except VerificationError as error:
        if diagnostic not in str(error):
            raise RuntimeError(
                f"unexpected EROFS rejection for {image.name}: {error}"
            ) from error
    else:
        raise RuntimeError(f"invalid EROFS fixture was accepted: {image.name}")


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="uml-rootfs-provision-test-") as temporary:
        root = Path(temporary)
        source = root / "published-rootfs.erofs"
        manifest_value = make_fixture(source)
        manifest = root / "manifest.json"
        manifest.write_text(json.dumps(manifest_value, sort_keys=True), encoding="utf-8")
        # Host permission bits are not part of the EROFS contract.
        source.chmod(0o666)
        verify(source, manifest)

        non_object_manifest = root / "non-object.json"
        non_object_manifest.write_text("[]\n", encoding="utf-8")
        require_rejected(source, non_object_manifest, "must be a JSON object")

        wrong_schema = json.loads(json.dumps(manifest_value))
        wrong_schema["schema_version"] = 2
        wrong_schema_path = root / "wrong-schema.json"
        wrong_schema_path.write_text(
            json.dumps(wrong_schema, sort_keys=True), encoding="utf-8"
        )
        require_rejected(source, wrong_schema_path, "unsupported schema")

        invalid_magic = bytearray(source.read_bytes())
        struct.pack_into("<I", invalid_magic, 1024, 0)
        invalid_magic_image, invalid_magic_manifest = manifested_copy(
            root, "invalid-magic", invalid_magic, manifest_value
        )
        require_rejected(
            invalid_magic_image, invalid_magic_manifest, "does not have an EROFS"
        )

        invalid_blocks = bytearray(source.read_bytes())
        struct.pack_into("<I", invalid_blocks, 1024 + 36, 3)
        invalid_blocks_image, invalid_blocks_manifest = manifested_copy(
            root, "invalid-blocks", invalid_blocks, manifest_value
        )
        require_rejected(
            invalid_blocks_image,
            invalid_blocks_manifest,
            "block count does not cover",
        )

        invalid_compression = bytearray(source.read_bytes())
        struct.pack_into("<H", invalid_compression, 1024 + 84, 1)
        compression_image, compression_manifest = manifested_copy(
            root, "invalid-compression", invalid_compression, manifest_value
        )
        require_rejected(
            compression_image,
            compression_manifest,
            "available_compr_algs does not match",
        )

        external_device = bytearray(source.read_bytes())
        struct.pack_into("<H", external_device, 1024 + 86, 1)
        external_image, external_manifest = manifested_copy(
            root, "external-device", external_device, manifest_value
        )
        require_rejected(
            external_image,
            external_manifest,
            "extra_devices does not match",
        )

        truncated_image, truncated_manifest = manifested_copy(
            root, "truncated", source.read_bytes()[:1100], manifest_value
        )
        require_rejected(truncated_image, truncated_manifest, "superblock is truncated")

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

        local_output = root / "empty-local-workspace" / "rootfs.erofs"
        if run(["--source", str(source), "--output", str(local_output), "--manifest", str(manifest)]) != 0:
            raise RuntimeError("local empty-workspace provisioning failed")
        verify(local_output, manifest)

        raced_output = root / "raced-workspace" / "rootfs.erofs"

        class RacingLock:
            def __enter__(self):
                raced_output.write_bytes(b"concurrent publisher")
                return self

            def __exit__(self, _type, _value, _traceback):
                return False

        with mock.patch.object(
            provision, "image_lock", return_value=RacingLock()
        ):
            if run(
                [
                    "--source",
                    str(source),
                    "--output",
                    str(raced_output),
                    "--manifest",
                    str(manifest),
                ]
            ) == 0:
                raise RuntimeError(
                    "provisioning replaced an image published while waiting for its lock"
                )
        if raced_output.read_bytes() != b"concurrent publisher":
            raise RuntimeError("provisioning overwrote the concurrently published image")

        with image_lock(local_output, exclusive=False):
            if run(
                [
                    "--source",
                    str(source),
                    "--output",
                    str(local_output),
                    "--manifest",
                    str(manifest),
                    "--replace",
                ]
            ) == 0:
                raise RuntimeError("provisioning ignored the active campaign lock")

        mutable = root / "post-check.erofs"
        shutil.copyfile(source, mutable)
        post_check = subprocess.run(
            [
                sys.executable,
                str(ROOTFS_TOOL_ROOT / "run_verified.py"),
                "--image",
                str(mutable),
                "--manifest",
                str(manifest),
                "--",
                sys.executable,
                "-c",
                (
                    "from pathlib import Path; import sys; "
                    "p=Path(sys.argv[1]); b=bytearray(p.read_bytes()); "
                    "b[-1] ^= 1; p.write_bytes(b)"
                ),
                str(mutable),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if post_check.returncode == 0 or "SHA-256" not in post_check.stderr:
            raise RuntimeError(
                "post-run rootfs mutation was not rejected:\n"
                f"stdout:\n{post_check.stdout}\nstderr:\n{post_check.stderr}"
            )

        release_output = root / "empty-release-workspace" / "rootfs.erofs"
        downloads = {
            "https://release.invalid/rootfs.erofs": source,
            "https://release.invalid/release-statement.json": statement,
            "https://release.invalid/release-statement.sig": signature,
        }

        def copy_fixture(url: str, destination: Path) -> None:
            shutil.copyfile(downloads[url], destination)

        with mock.patch.object(provision, "copy_from_url", side_effect=copy_fixture):
            result = run(
                [
                    "--url", "https://release.invalid/rootfs.erofs",
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
