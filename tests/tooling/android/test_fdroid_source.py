#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the pinned F-Droid source contract and its materialization."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import functools
import hashlib
import http.server
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

CLIENT = "org.fdroid.fdroid_2000051.apk"
EXTENSION = "org.fdroid.fdroid.privileged_2140.apk"
PAYLOADS = {CLIENT: b"client payload", EXTENSION: b"extension payload"}


def fail(message: str) -> None:
    raise SystemExit(f"F-Droid source fixture: {message}")


def metadata_for(payloads: dict[str, bytes], base_url: str) -> str:
    return "\n".join(
        [
            "FDROID_SCHEMA=1",
            f"FDROID_BASE_URL='{base_url}'",
            "FDROID_DESTINATION='vendor/kernarium/fdroid'",
            "FDROID_PGP_KEY_FINGERPRINT='37D2C98789D8311948394E3E41E7044E1DBA2E89'",
            f"FDROID_SIGNING_CERT_SHA256='{'ab' * 32}'",
            f"FDROID_CLIENT_FILE='{CLIENT}'",
            f"FDROID_CLIENT_SIZE={len(payloads[CLIENT])}",
            f"FDROID_CLIENT_SHA256='{hashlib.sha256(payloads[CLIENT]).hexdigest()}'",
            f"FDROID_EXTENSION_FILE='{EXTENSION}'",
            f"FDROID_EXTENSION_SIZE={len(payloads[EXTENSION])}",
            f"FDROID_EXTENSION_SHA256='{hashlib.sha256(payloads[EXTENSION]).hexdigest()}'",
            "",
        ]
    )


def run(tool: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, "-B", str(tool), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def expect_ok(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode != 0:
        fail(f"{label} failed: {result.stdout}")


def expect_refused(result: subprocess.CompletedProcess[str], label: str) -> None:
    if result.returncode == 0:
        fail(f"{label} was accepted")
    if "Traceback" in result.stdout:
        fail(f"{label} raised instead of reporting: {result.stdout}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    real_root = args.source_root.resolve() / "android/lineage-24"
    tool = real_root / "tools/fdroid_source.py"

    # The tracked pins describe the real files and pass their own checks.
    expect_ok(run(tool, "verify", "--android-root", str(real_root)), "tracked pins")

    with tempfile.TemporaryDirectory(prefix="fdroid-source-") as temporary:
        root = Path(temporary)
        android_root = root / "android"
        pins = android_root / "third_party/fdroid"
        pins.mkdir(parents=True)
        for name in ("Android.bp", "privapp-permissions-org.fdroid.fdroid.privileged.xml"):
            shutil.copyfile(real_root / "third_party/fdroid" / name, pins / name)
        served = root / "served"
        served.mkdir()
        for name, payload in PAYLOADS.items():
            (served / name).write_bytes(payload)
        cache = root / "cache"
        cache.mkdir()
        for name, payload in PAYLOADS.items():
            (cache / name).write_bytes(payload)

        class QuietHandler(http.server.SimpleHTTPRequestHandler):
            def log_message(self, *arguments: object) -> None:
                pass

        handler = functools.partial(QuietHandler, directory=str(served))
        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base_url = f"http://127.0.0.1:{server.server_address[1]}"
        try:
            (pins / "metadata.env").write_text(
                metadata_for(PAYLOADS, "https://f-droid.example/repo")
            )

            # Materialize from the local cache.
            workspace = root / "workspace-cache"
            workspace.mkdir()
            expect_ok(
                run(tool, "prepare", "--android-root", str(android_root),
                    "--aosp-source", str(workspace), "--cache", str(cache)),
                "preparation from the cache",
            )
            destination = workspace / "vendor/kernarium/fdroid"
            expect_ok(
                run(tool, "verify", "--android-root", str(android_root),
                    "--aosp-source", str(workspace)),
                "verification of the materialized sources",
            )
            expect_ok(
                run(tool, "prepare", "--android-root", str(android_root),
                    "--aosp-source", str(workspace), "--cache", str(cache)),
                "a repeated preparation",
            )

            # Materialize by download.
            workspace = root / "workspace-download"
            workspace.mkdir()
            expect_ok(
                run(tool, "prepare", "--android-root", str(android_root),
                    "--aosp-source", str(workspace), "--base-url", base_url),
                "preparation by download",
            )

            # The pinned URL must be https.
            (pins / "metadata.env").write_text(
                metadata_for(PAYLOADS, "http://f-droid.example/repo")
            )
            expect_refused(
                run(tool, "prepare", "--android-root", str(android_root),
                    "--aosp-source", str(root / "workspace-http")),
                "a plain-http pinned URL",
            )
            (pins / "metadata.env").write_text(
                metadata_for(PAYLOADS, "https://f-droid.example/repo")
            )

            # A file of the wrong size or digest is refused from either source.
            (cache / CLIENT).write_bytes(b"client payload!")
            expect_refused(
                run(tool, "prepare", "--android-root", str(android_root),
                    "--aosp-source", str(root / "workspace-bad-cache"),
                    "--cache", str(cache)),
                "an oversized cached file",
            )
            (cache / CLIENT).write_bytes(b"client PAYLOAD")
            expect_refused(
                run(tool, "prepare", "--android-root", str(android_root),
                    "--aosp-source", str(root / "workspace-bad-digest"),
                    "--cache", str(cache)),
                "a cached file with a different digest",
            )
            (served / EXTENSION).write_bytes(b"extension payload plus")
            expect_refused(
                run(tool, "prepare", "--android-root", str(android_root),
                    "--aosp-source", str(root / "workspace-bad-download"),
                    "--base-url", base_url),
                "an oversized download",
            )
            (served / EXTENSION).write_bytes(PAYLOADS[EXTENSION])

            # Tampering with a materialized workspace is detected.
            (destination / CLIENT).write_bytes(b"client PAYLOAD")
            expect_refused(
                run(tool, "verify", "--android-root", str(android_root),
                    "--aosp-source", str(workspace.parent / "workspace-cache")),
                "a tampered APK",
            )
            (destination / CLIENT).write_bytes(PAYLOADS[CLIENT])
            (destination / "stray.apk").write_bytes(b"stray")
            expect_refused(
                run(tool, "verify", "--android-root", str(android_root),
                    "--aosp-source", str(workspace.parent / "workspace-cache")),
                "an extra file",
            )
            (destination / "stray.apk").unlink()
            (destination / "Android.bp").write_text("// changed\n")
            expect_refused(
                run(tool, "verify", "--android-root", str(android_root),
                    "--aosp-source", str(workspace.parent / "workspace-cache")),
                "a changed module file",
            )

            # The privileged permission allowlist is exactly the two it needs.
            permissions = pins / "privapp-permissions-org.fdroid.fdroid.privileged.xml"
            permissions.write_text(
                permissions.read_text().replace(
                    "</privapp-permissions>",
                    '    <permission name="android.permission.WRITE_SECURE_SETTINGS"/>\n'
                    "    </privapp-permissions>",
                )
            )
            expect_refused(
                run(tool, "verify", "--android-root", str(android_root)),
                "an allowlist with an extra permission",
            )
        finally:
            server.shutdown()

    print("Android 17 F-Droid source contract passed")


if __name__ == "__main__":
    main()
