#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify pinned OrangeFox metadata, generated topics, and an optional tree."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import xml.etree.ElementTree as ET
from pathlib import Path

import pinned_source
# pinned_source puts scripts/ and the contract checks on sys.path.
from contract_checks import parse_assignments  # noqa: E402


HEX40 = pinned_source.HEX40
HEX64 = re.compile(r"[0-9a-f]{64}")
PROJECT_HEADER = (
    "order", "route_id", "manifest_remote", "manifest_project", "checkout_path",
    "manifest_upstream", "base_commit", "base_tree", "final_tree", "series_dir",
)
EXTERNALS = (
    (
        "bootable/recovery",
        "ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY",
        "ORANGEFOX_BOOTABLE_RECOVERY_COMMIT",
        "ORANGEFOX_BOOTABLE_RECOVERY_TREE",
    ),
    (
        "vendor/recovery",
        "ORANGEFOX_VENDOR_RECOVERY_REPOSITORY",
        "ORANGEFOX_VENDOR_RECOVERY_COMMIT",
        "ORANGEFOX_VENDOR_RECOVERY_TREE",
    ),
    (
        "external/se_omapi",
        "ORANGEFOX_SE_OMAPI_REPOSITORY",
        "ORANGEFOX_SE_OMAPI_COMMIT",
        "ORANGEFOX_SE_OMAPI_TREE",
    ),
)


class VerificationError(ValueError):
    pass


def run(*command: str, cwd: Path | None = None) -> str:
    return pinned_source.run(*command, cwd=cwd, error=VerificationError)


def digest(path: Path) -> str:
    return pinned_source.digest(path, VerificationError)


def manifest_projects(root: ET.Element) -> dict[str, ET.Element]:
    return pinned_source.manifest_projects(root, VerificationError)


def verify_metadata(root: Path) -> tuple[dict[str, str], dict[str, ET.Element]]:
    values = parse_assignments(root / "third_party/orangefox/metadata.env", VerificationError)
    required = {
        "ORANGEFOX_SOURCE_SCHEMA", "ORANGEFOX_SYNC_REPOSITORY",
        "ORANGEFOX_SYNC_BRANCH", "ORANGEFOX_SYNC_COMMIT", "ORANGEFOX_SYNC_TREE",
        "ORANGEFOX_SYNC_SCRIPT_SHA256", "ORANGEFOX_MANIFEST_REPOSITORY",
        "ORANGEFOX_MANIFEST_BRANCH", "ORANGEFOX_MANIFEST_COMMIT",
        "ORANGEFOX_MANIFEST_TREE", "ORANGEFOX_BASE_MANIFEST_SHA256",
        "ORANGEFOX_BRANCH", "ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY",
        "ORANGEFOX_BOOTABLE_RECOVERY_COMMIT", "ORANGEFOX_BOOTABLE_RECOVERY_TREE",
        "ORANGEFOX_VENDOR_RECOVERY_REPOSITORY", "ORANGEFOX_VENDOR_RECOVERY_COMMIT",
        "ORANGEFOX_VENDOR_RECOVERY_TREE", "ORANGEFOX_SE_OMAPI_REPOSITORY",
        "ORANGEFOX_SE_OMAPI_COMMIT", "ORANGEFOX_SE_OMAPI_TREE",
        "ORANGEFOX_DEVICE_OVERLAY_DESTINATION", "ORANGEFOX_PRODUCT",
        "ORANGEFOX_RELEASE_CONFIG", "ORANGEFOX_LICENSE",
    }
    if set(values) != required:
        raise VerificationError(
            f"metadata keys differ (missing={sorted(required - set(values))}, "
            f"extra={sorted(set(values) - required)})"
        )
    expected = {
        "ORANGEFOX_SOURCE_SCHEMA": "1",
        "ORANGEFOX_SYNC_REPOSITORY": "https://gitlab.com/OrangeFox/sync.git",
        "ORANGEFOX_MANIFEST_REPOSITORY": (
            "https://github.com/nebrassy/platform_manifest_twrp_aosp.git"
        ),
        "ORANGEFOX_BRANCH": "fox_14.1",
        "ORANGEFOX_DEVICE_OVERLAY_DESTINATION": "device/kernarium/darwin",
        "ORANGEFOX_PRODUCT": "twrp_darwin",
        "ORANGEFOX_RELEASE_CONFIG": "ap2a",
        "ORANGEFOX_LICENSE": "GPL-3.0-or-later",
    }
    for key, wanted in expected.items():
        if values[key] != wanted:
            raise VerificationError(f"{key} is not the reviewed value")
    for key, value in values.items():
        if key.endswith(("_COMMIT", "_TREE")) and HEX40.fullmatch(value) is None:
            raise VerificationError(f"{key} is not a full Git object ID")
        if key.endswith("_SHA256") and HEX64.fullmatch(value) is None:
            raise VerificationError(f"{key} is not a SHA-256 digest")

    manifest_path = root / "third_party/orangefox/base-manifest.xml"
    if digest(manifest_path) != values["ORANGEFOX_BASE_MANIFEST_SHA256"]:
        raise VerificationError("resolved base manifest digest differs from metadata")
    return values, manifest_projects(ET.parse(manifest_path).getroot())


def inspect_routes(
    root: Path, projects: dict[str, ET.Element]
) -> list[pinned_source.Route]:
    routes = pinned_source.read_route_table(
        root / "third_party/orangefox/patches/projects.tsv",
        PROJECT_HEADER,
        root,
        "third_party/orangefox/patches/projects",
        projects,
        VerificationError,
    )
    if any(route.route_id != f"p{route.order:04d}" for route in routes):
        raise VerificationError("OrangeFox route order or ID is invalid")
    return routes


def remote_urls(checkout: Path) -> set[str]:
    remotes = run("git", "remote", cwd=checkout).splitlines()
    return {run("git", "remote", "get-url", remote, cwd=checkout) for remote in remotes}


def verify_checkout(
    root: Path,
    source: Path,
    values: dict[str, str],
    base_projects: dict[str, ET.Element],
    routes: list[pinned_source.Route],
    expect: str,
    allow_kernel: bool,
) -> None:
    if not source.is_dir() or not (source / ".repo/manifests").is_dir():
        raise VerificationError("OrangeFox source is not a Repo workspace")
    manifest_git = source / ".repo/manifests"
    if run("git", "rev-parse", "HEAD", cwd=manifest_git) != values["ORANGEFOX_MANIFEST_COMMIT"]:
        raise VerificationError("OrangeFox manifest checkout moved from its pin")
    if run("git", "rev-parse", "HEAD^{tree}", cwd=manifest_git) != values["ORANGEFOX_MANIFEST_TREE"]:
        raise VerificationError("OrangeFox manifest tree moved from its pin")

    pinned_source.verify_repo_routes(
        source, base_projects, routes, expect, "OrangeFox", VerificationError
    )
    route_paths = {route.path for route in routes}
    for path, repository_key, commit_key, tree_key in EXTERNALS:
        checkout = source / path
        if not checkout.is_dir() or checkout.is_symlink():
            raise VerificationError(f"OrangeFox external checkout is missing: {path}")
        if run("git", "status", "--porcelain", "--untracked-files=all", cwd=checkout):
            raise VerificationError(f"OrangeFox external checkout is dirty: {path}")
        if values[repository_key] not in remote_urls(checkout):
            raise VerificationError(f"OrangeFox external origin differs: {path}")
        if path in route_paths:
            # The routed history below owns its base/final identity. The
            # external pin still owns this checkout's transport and base.
            continue
        if run("git", "rev-parse", "HEAD", cwd=checkout) != values[commit_key]:
            raise VerificationError(f"OrangeFox external commit differs: {path}")
        if run("git", "rev-parse", "HEAD^{tree}", cwd=checkout) != values[tree_key]:
            raise VerificationError(f"OrangeFox external tree differs: {path}")

    if expect == "final":
        options = ("--allow-prebuilt-kernel",) if allow_kernel else ()
        pinned_source.verify_device_tree(root, "orangefox", source, VerificationError, *options)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--orangefox-source", type=Path)
    parser.add_argument("--expect", choices=("base", "final"), default="final")
    parser.add_argument("--allow-prebuilt-kernel", action="store_true")
    args = parser.parse_args()
    try:
        root = args.android_root.resolve()
        values, projects = verify_metadata(root)
        routes = inspect_routes(root, projects)
        if args.allow_prebuilt_kernel and args.orangefox_source is None:
            raise VerificationError("--allow-prebuilt-kernel requires --orangefox-source")
        if args.orangefox_source is not None:
            verify_checkout(
                root, args.orangefox_source.resolve(), values, projects, routes,
                args.expect, args.allow_prebuilt_kernel,
            )
    except (OSError, UnicodeError, ET.ParseError, VerificationError) as error:
        raise SystemExit(f"verify-orangefox-source.py: {error}") from error
    suffix = " and checkout" if args.orangefox_source is not None else ""
    print(f"verified {len(routes)} OrangeFox project routes{suffix}")


if __name__ == "__main__":
    main()
