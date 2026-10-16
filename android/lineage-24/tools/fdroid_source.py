#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prepare and verify the pinned F-Droid client and Privileged Extension.

`third_party/fdroid/metadata.env` pins both APKs by size and SHA-256; both are
presigned by F-Droid and the build installs them unchanged. `prepare` is the
only step that may use the network: it materializes the APKs and the tracked
Soong module and permission files into the Android workspace, taking each APK
from the destination, then from an optional local cache, then from the pinned
download URL, and refusing any file whose size or digest differs. `verify`
checks the pins and, given a workspace, that the materialized files are exactly
the pinned ones.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
import re
import shutil
import sys
import tempfile
import urllib.request
from pathlib import Path

THIRD_PARTY = "third_party/fdroid"
TRACKED_FILES = ("Android.bp", "privapp-permissions-org.fdroid.fdroid.privileged.xml")
REQUIRED_KEYS = {
    "FDROID_SCHEMA",
    "FDROID_BASE_URL",
    "FDROID_DESTINATION",
    "FDROID_PGP_KEY_FINGERPRINT",
    "FDROID_SIGNING_CERT_SHA256",
    "FDROID_CLIENT_FILE",
    "FDROID_CLIENT_SIZE",
    "FDROID_CLIENT_SHA256",
    "FDROID_EXTENSION_FILE",
    "FDROID_EXTENSION_SIZE",
    "FDROID_EXTENSION_SHA256",
}
APK_NAME = re.compile(r"^org\.fdroid\.[a-z.]+_[0-9]+\.apk$")
DOWNLOAD_TIMEOUT_SECONDS = 300


def error(message: str) -> SystemExit:
    return SystemExit(f"F-Droid source: {message}")


def fail(message: str) -> None:
    raise error(message)


def read_metadata(android_root: Path) -> dict[str, str]:
    path = android_root / THIRD_PARTY / "metadata.env"
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator:
            fail(f"malformed metadata line: {raw!r}")
        values[key] = value.strip("'")
    if set(values) != REQUIRED_KEYS:
        fail("metadata keys differ from the fixed contract")
    if values["FDROID_SCHEMA"] != "1":
        fail("unsupported metadata schema")
    for key in ("FDROID_CLIENT_SHA256", "FDROID_EXTENSION_SHA256", "FDROID_SIGNING_CERT_SHA256"):
        if re.fullmatch(r"[0-9a-f]{64}", values[key]) is None:
            fail(f"{key} is not a SHA-256 digest")
    if re.fullmatch(r"[0-9A-F]{40}", values["FDROID_PGP_KEY_FINGERPRINT"]) is None:
        fail("the PGP key fingerprint is not 40 uppercase hex digits")
    for key in ("FDROID_CLIENT_SIZE", "FDROID_EXTENSION_SIZE"):
        if not values[key].isdigit() or int(values[key]) <= 0:
            fail(f"{key} is not a positive size")
    for key in ("FDROID_CLIENT_FILE", "FDROID_EXTENSION_FILE"):
        if APK_NAME.fullmatch(values[key]) is None:
            fail(f"{key} is not an F-Droid APK file name")
    destination = Path(values["FDROID_DESTINATION"])
    if destination.is_absolute() or ".." in destination.parts or not destination.parts:
        fail("the destination must be a relative workspace path")
    return values


def apks(values: dict[str, str]) -> list[tuple[str, int, str]]:
    return [
        (values["FDROID_CLIENT_FILE"], int(values["FDROID_CLIENT_SIZE"]),
         values["FDROID_CLIENT_SHA256"]),
        (values["FDROID_EXTENSION_FILE"], int(values["FDROID_EXTENSION_SIZE"]),
         values["FDROID_EXTENSION_SHA256"]),
    ]


def file_matches(path: Path, size: int, digest: str) -> bool:
    if path.is_symlink() or not path.is_file() or path.stat().st_size != size:
        return False
    sha = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1 << 20):
            sha.update(chunk)
    return sha.hexdigest() == digest


def verify_tracked(android_root: Path, values: dict[str, str]) -> None:
    source = android_root / THIRD_PARTY
    for name in TRACKED_FILES:
        if not (source / name).is_file():
            fail(f"tracked file is missing: {THIRD_PARTY}/{name}")
    module_text = (source / "Android.bp").read_text(encoding="utf-8")
    for name, _, _ in apks(values):
        if f'apk: "{name}"' not in module_text:
            fail(f"Android.bp does not import the pinned APK {name}")
    permissions = (source / TRACKED_FILES[1]).read_text(encoding="utf-8")
    names = sorted(re.findall(r'<permission name="([^"]+)"', permissions))
    if names != ["android.permission.DELETE_PACKAGES", "android.permission.INSTALL_PACKAGES"]:
        fail("the privileged permission list must be exactly INSTALL_PACKAGES and DELETE_PACKAGES")


def verify_workspace(android_root: Path, values: dict[str, str], aosp_source: Path) -> None:
    destination = aosp_source / values["FDROID_DESTINATION"]
    if not destination.is_dir() or destination.is_symlink():
        fail(f"the materialized directory is missing: {destination}")
    expected = {name for name, _, _ in apks(values)} | set(TRACKED_FILES)
    present = {entry.name for entry in destination.iterdir()}
    if present != expected:
        fail(f"the materialized directory differs from the pinned set: {sorted(present ^ expected)}")
    for name, size, digest in apks(values):
        if not file_matches(destination / name, size, digest):
            fail(f"{name} differs from its pinned size or digest")
    for name in TRACKED_FILES:
        if (destination / name).read_bytes() != (android_root / THIRD_PARTY / name).read_bytes():
            fail(f"{name} differs from the tracked copy")


def download(url: str, size: int, destination: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "kernarium-fdroid-source"})
    with urllib.request.urlopen(request, timeout=DOWNLOAD_TIMEOUT_SECONDS) as response:
        data = response.read(size + 1)
    if len(data) != size:
        fail(f"download of {url} is not {size} bytes")
    destination.write_bytes(data)


def prepare(android_root: Path, values: dict[str, str], aosp_source: Path,
            cache: Path | None, base_url: str | None) -> None:
    destination = aosp_source / values["FDROID_DESTINATION"]
    base = (base_url or values["FDROID_BASE_URL"]).rstrip("/")
    if base_url is None and not base.startswith("https://"):
        fail("the pinned download URL must be https")
    destination.mkdir(parents=True, exist_ok=True)
    for name in TRACKED_FILES:
        shutil.copyfile(android_root / THIRD_PARTY / name, destination / name)
    for name, size, digest in apks(values):
        target = destination / name
        if file_matches(target, size, digest):
            print(f"F-Droid source: {name} is already materialized")
            continue
        with tempfile.TemporaryDirectory(prefix="fdroid-", dir=destination) as scratch:
            candidate = Path(scratch) / name
            if cache is not None and (cache / name).is_file():
                shutil.copyfile(cache / name, candidate)
                origin = "local cache"
            else:
                download(f"{base}/{name}", size, candidate)
                origin = "download"
            if not file_matches(candidate, size, digest):
                fail(f"{name} from the {origin} differs from its pinned size or digest")
            os.replace(candidate, target)
        print(f"F-Droid source: materialized {name} from the {origin}")
    verify_workspace(android_root, values, aosp_source)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("verify", "prepare"):
        command = sub.add_parser(name)
        command.add_argument("--android-root", type=Path, required=True)
        command.add_argument("--aosp-source", type=Path, required=(name == "prepare"))
    sub.choices["verify"].set_defaults(aosp_source=None)
    sub.choices["prepare"].add_argument("--cache", type=Path)
    sub.choices["prepare"].add_argument(
        "--base-url", help="download from this URL instead of the pinned one (tests)")
    args = parser.parse_args()
    android_root = args.android_root.resolve()
    values = read_metadata(android_root)
    verify_tracked(android_root, values)
    if args.command == "prepare":
        prepare(android_root, values, args.aosp_source.resolve(), args.cache, args.base_url)
    elif args.aosp_source is not None:
        verify_workspace(android_root, values, args.aosp_source.resolve())
    print("verified the pinned F-Droid sources")


if __name__ == "__main__":
    try:
        main()
    except SystemExit:
        raise
    except Exception as exc:  # pragma: no cover - report, never a traceback for operators
        print(f"F-Droid source: {exc}", file=sys.stderr)
        raise SystemExit(1)
