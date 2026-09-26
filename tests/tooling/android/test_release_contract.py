#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import copy
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def fail(message: str) -> None:
    raise SystemExit(f"android release contract fixture: {message}")


def expect_failure(function, label: str) -> None:
    try:
        function()
    except SystemExit:
        return
    fail(f"release contract accepted {label}")


def read_routes(projects_path: Path) -> list[dict]:
    lines = [
        line for line in projects_path.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    reader = csv.DictReader(lines, delimiter="\t")
    routes = []
    for row in reader:
        routes.append({
            "route_id": row["route_id"],
            "manifest_project": row["manifest_project"],
            "checkout_path": row["checkout_path"],
            "base_commit": row["base_commit"],
            "base_tree": row["base_tree"],
            "final_tree": row["final_tree"],
            "patch_count": int(row["patch_count"]),
            "series_manifest_sha256": row["series_manifest_sha256"],
        })
    return routes


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()

    root = args.source_root.resolve()
    android_root = root / "android/aosp-17"
    sys.path.insert(0, str(android_root / "tools"))
    from release_common import (  # noqa: PLC0415
        DIRECTORY_PREFIX,
        PAYLOAD_SPECS,
        PRODUCT_FAMILY,
        SCHEMA_VERSION,
        ACCEPTANCE_KEYS,
        kernel_series_provenance,
        parse_env,
        sha256_path,
        verify_release,
    )

    schema = json.loads(
        (android_root / "support/release/release-manifest.schema.json").read_text()
    )
    if schema.get("properties", {}).get("product_family", {}).get("const") != PRODUCT_FAMILY:
        fail("release schema does not pin the Android product family")

    aosp = parse_env(android_root / "metadata/aosp.env")
    jelly = parse_env(android_root / "third_party/jelly/metadata.env")
    orangefox = parse_env(android_root / "third_party/orangefox/metadata.env")
    kernel_root = root / "kernel/android-6.18"
    kernel = parse_env(kernel_root / "patches/metadata.env")
    kernel_patch_count, kernel_patch_digest = kernel_series_provenance(root)
    abi = parse_env(root / "kernel/common/abi/identity.env")
    tracked_acceptance = parse_env(android_root / "metadata/accepted-release.env")
    if set(tracked_acceptance) != ACCEPTANCE_KEYS:
        fail("tracked acceptance descriptor keys differ from the release contract")
    tracked_expected = {
        "ANDROID17_ACCEPTED_SCHEMA": str(SCHEMA_VERSION),
        "ANDROID17_ACCEPTED_PRODUCT_FAMILY": PRODUCT_FAMILY,
        "ANDROID17_ACCEPTED_ANDROID_MAJOR": "17",
        "ANDROID17_ACCEPTED_PAGE_SIZE": "16384",
    }
    for key, value in tracked_expected.items():
        if tracked_acceptance.get(key) != value:
            fail(f"tracked acceptance descriptor differs at {key}")
    if tracked_acceptance["ANDROID17_ACCEPTED_DIRECTORY_NAME"] != (
        DIRECTORY_PREFIX + tracked_acceptance["ANDROID17_ACCEPTED_RELEASE_ID"]
    ):
        fail("tracked acceptance directory does not match its release ID")
    release_id = "fixture-1"
    directory_name = DIRECTORY_PREFIX + release_id

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "release_id": release_id,
        "directory_name": directory_name,
        "product_family": PRODUCT_FAMILY,
        "acceptance": "accepted",
        "published_at_utc": "2026-08-21T00:00:00Z",
        "android": {
            "major": 17,
            "sdk": 37,
            "build_id": "FIXTURE",
            "build_variant": aosp["ANDROID17_BUILD_VARIANT"],
            "fingerprint": "fixture/fingerprint",
            "vendor_api_level": 202704,
            "product": aosp["ANDROID17_PRODUCT"],
            "release_config": aosp["ANDROID17_RELEASE_CONFIG"],
        },
        "aosp": {
            "manifest_repository": aosp["ANDROID17_MANIFEST_REPOSITORY"],
            "manifest_tag": aosp["ANDROID17_MANIFEST_TAG"],
            "manifest_commit": aosp["ANDROID17_MANIFEST_COMMIT"],
            "manifest_tree": aosp["ANDROID17_MANIFEST_TREE"],
            "base_manifest_sha256": aosp["ANDROID17_BASE_MANIFEST_SHA256"],
            "projects_manifest_sha256": sha256_path(
                android_root / "patches/projects.tsv"
            ),
            "routes": read_routes(android_root / "patches/projects.tsv"),
        },
        "jelly": {
            "repository": jelly["JELLY_UPSTREAM_REPOSITORY"],
            "branch": jelly["JELLY_UPSTREAM_BRANCH"],
            "base_commit": jelly["JELLY_UPSTREAM_COMMIT"],
            "base_tree": jelly["JELLY_UPSTREAM_TREE"],
            "final_commit": jelly["JELLY_FINAL_COMMIT"],
            "final_tree": jelly["JELLY_FINAL_TREE"],
            "patch_count": int(jelly["JELLY_PATCH_COUNT"]),
            "patch_manifest_sha256": sha256_path(
                android_root / "third_party/jelly/series/manifest.tsv"
            ),
            "destination": jelly["JELLY_DESTINATION"],
            "license": jelly["JELLY_LICENSE"],
        },
        "orangefox": {
            "sync_repository": orangefox["ORANGEFOX_SYNC_REPOSITORY"],
            "sync_branch": orangefox["ORANGEFOX_SYNC_BRANCH"],
            "sync_commit": orangefox["ORANGEFOX_SYNC_COMMIT"],
            "sync_tree": orangefox["ORANGEFOX_SYNC_TREE"],
            "sync_script_sha256": orangefox["ORANGEFOX_SYNC_SCRIPT_SHA256"],
            "manifest_repository": orangefox["ORANGEFOX_MANIFEST_REPOSITORY"],
            "manifest_branch": orangefox["ORANGEFOX_MANIFEST_BRANCH"],
            "manifest_commit": orangefox["ORANGEFOX_MANIFEST_COMMIT"],
            "manifest_tree": orangefox["ORANGEFOX_MANIFEST_TREE"],
            "base_manifest_sha256": orangefox["ORANGEFOX_BASE_MANIFEST_SHA256"],
            "branch": orangefox["ORANGEFOX_BRANCH"],
            "bootable_recovery_repository": orangefox[
                "ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY"
            ],
            "bootable_recovery_commit": orangefox[
                "ORANGEFOX_BOOTABLE_RECOVERY_COMMIT"
            ],
            "bootable_recovery_tree": orangefox[
                "ORANGEFOX_BOOTABLE_RECOVERY_TREE"
            ],
            "vendor_recovery_repository": orangefox[
                "ORANGEFOX_VENDOR_RECOVERY_REPOSITORY"
            ],
            "vendor_recovery_commit": orangefox[
                "ORANGEFOX_VENDOR_RECOVERY_COMMIT"
            ],
            "vendor_recovery_tree": orangefox[
                "ORANGEFOX_VENDOR_RECOVERY_TREE"
            ],
            "se_omapi_repository": orangefox["ORANGEFOX_SE_OMAPI_REPOSITORY"],
            "se_omapi_commit": orangefox["ORANGEFOX_SE_OMAPI_COMMIT"],
            "se_omapi_tree": orangefox["ORANGEFOX_SE_OMAPI_TREE"],
            "device_destination": orangefox[
                "ORANGEFOX_DEVICE_OVERLAY_DESTINATION"
            ],
            "product": orangefox["ORANGEFOX_PRODUCT"],
            "release_config": orangefox["ORANGEFOX_RELEASE_CONFIG"],
            "patch_count": int(orangefox["ORANGEFOX_PATCH_COUNT"]),
            "projects_manifest_sha256": sha256_path(
                android_root / "third_party/orangefox/patches/projects.tsv"
            ),
            "routes": read_routes(
                android_root / "third_party/orangefox/patches/projects.tsv"
            ),
            "license": orangefox["ORANGEFOX_LICENSE"],
        },
        "kernel": {
            "family": kernel["UML_KERNEL_FAMILY"],
            "repository": kernel["UML_KERNEL_UPSTREAM_REPOSITORY"],
            "branch": kernel["UML_KERNEL_UPSTREAM_BRANCH"],
            "tag": kernel["UML_KERNEL_UPSTREAM_TAG"],
            "base_commit": kernel["UML_KERNEL_UPSTREAM_COMMIT"],
            "base_tree": kernel["UML_KERNEL_UPSTREAM_TREE"],
            "final_tree": kernel["UML_KERNEL_FINAL_TREE"],
            "patch_count": kernel_patch_count,
            "patch_manifest_sha256": kernel_patch_digest,
            "release": "6.18.21-fixture",
            "page_size": 16384,
            "abi_version": int(abi["DARWIN_UML_ABI_VERSION"]),
            "abi_fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
            "elf_role": "darwin-uml",
        },
        "outer": {"revision": "a" * 40, "dirty": False},
        "producer": {
            "entry_point": "android/aosp-17/build/publish-release.sh",
            "functional_options": ["pinned AOSP source", "verified ACK build"],
            "builder": {
                "os": "Linux fixture",
                "architecture": "x86_64",
                "jdk": "fixture JDK",
                "clang": "fixture clang",
                "linker": "fixture linker",
                "darwin_sdk": "fixture SDK",
                "android_build_tools": "fixture build tools",
            },
        },
        "inventory": [],
    }

    with tempfile.TemporaryDirectory(prefix="android-release-contract-") as temporary:
        temporary_root = Path(temporary)
        release_root = temporary_root / directory_name
        release_root.mkdir()
        for relative, (role, file_format) in sorted(PAYLOAD_SPECS.items()):
            path = release_root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"x")
            manifest["inventory"].append({
                "path": relative,
                "role": role,
                "format": file_format,
                "size": 1,
            })

        manifest_path = release_root / "release-manifest.json"
        manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
        acceptance = temporary_root / "accepted-release.env"
        acceptance.write_text(
            "\n".join((
                f"ANDROID17_ACCEPTED_SCHEMA={SCHEMA_VERSION}",
                f"ANDROID17_ACCEPTED_RELEASE_ID='{release_id}'",
                f"ANDROID17_ACCEPTED_DIRECTORY_NAME='{directory_name}'",
                f"ANDROID17_ACCEPTED_PRODUCT_FAMILY='{PRODUCT_FAMILY}'",
                "ANDROID17_ACCEPTED_ANDROID_MAJOR=17",
                f"ANDROID17_ACCEPTED_AOSP_MANIFEST_COMMIT='{manifest['aosp']['manifest_commit']}'",
                f"ANDROID17_ACCEPTED_AOSP_BASE_MANIFEST_SHA256='{manifest['aosp']['base_manifest_sha256']}'",
                f"ANDROID17_ACCEPTED_AOSP_PROJECTS_MANIFEST_SHA256='{manifest['aosp']['projects_manifest_sha256']}'",
                f"ANDROID17_ACCEPTED_JELLY_UPSTREAM_COMMIT='{manifest['jelly']['base_commit']}'",
                f"ANDROID17_ACCEPTED_JELLY_FINAL_TREE='{manifest['jelly']['final_tree']}'",
                f"ANDROID17_ACCEPTED_JELLY_PATCH_MANIFEST_SHA256='{manifest['jelly']['patch_manifest_sha256']}'",
                f"ANDROID17_ACCEPTED_ORANGEFOX_MANIFEST_COMMIT='{manifest['orangefox']['manifest_commit']}'",
                f"ANDROID17_ACCEPTED_ORANGEFOX_BASE_MANIFEST_SHA256='{manifest['orangefox']['base_manifest_sha256']}'",
                f"ANDROID17_ACCEPTED_ORANGEFOX_PROJECTS_MANIFEST_SHA256='{manifest['orangefox']['projects_manifest_sha256']}'",
                f"ANDROID17_ACCEPTED_ORANGEFOX_BOOTABLE_RECOVERY_COMMIT='{manifest['orangefox']['bootable_recovery_commit']}'",
                f"ANDROID17_ACCEPTED_ORANGEFOX_VENDOR_RECOVERY_COMMIT='{manifest['orangefox']['vendor_recovery_commit']}'",
                f"ANDROID17_ACCEPTED_ORANGEFOX_SE_OMAPI_COMMIT='{manifest['orangefox']['se_omapi_commit']}'",
                f"ANDROID17_ACCEPTED_KERNEL_FINAL_TREE='{manifest['kernel']['final_tree']}'",
                f"ANDROID17_ACCEPTED_KERNEL_PATCH_MANIFEST_SHA256='{manifest['kernel']['patch_manifest_sha256']}'",
                f"ANDROID17_ACCEPTED_OUTER_REVISION='{manifest['outer']['revision']}'",
                "ANDROID17_ACCEPTED_PAGE_SIZE=16384",
                f"ANDROID17_ACCEPTED_ABI_VERSION={manifest['kernel']['abi_version']}",
                f"ANDROID17_ACCEPTED_ABI_FINGERPRINT='{manifest['kernel']['abi_fingerprint']}'",
                "",
            ))
        )

        verify_release(
            release_root,
            android_root,
            acceptance_path=acceptance,
            semantic=False,
        )

        historical = copy.deepcopy(manifest)
        historical["aosp"]["projects_manifest_sha256"] = "b" * 64
        historical["kernel"]["final_tree"] = "c" * 40
        historical["kernel"]["patch_manifest_sha256"] = "d" * 64
        historical["kernel"]["abi_version"] = 7
        historical["kernel"]["abi_fingerprint"] = "0x0123456789abcdef"
        historical_manifest = temporary_root / "historical-release-manifest.json"
        historical_manifest.write_text(
            json.dumps(historical, indent=2, sort_keys=True) + "\n"
        )
        manifest_path.write_bytes(historical_manifest.read_bytes())
        historical_acceptance = temporary_root / "historical-accepted-release.env"
        historical_acceptance.write_text(
            acceptance.read_text()
            .replace(manifest["aosp"]["projects_manifest_sha256"], "b" * 64)
            .replace(manifest["kernel"]["final_tree"], "c" * 40)
            .replace(manifest["kernel"]["patch_manifest_sha256"], "d" * 64)
            .replace(
                f"ANDROID17_ACCEPTED_ABI_VERSION={manifest['kernel']['abi_version']}",
                "ANDROID17_ACCEPTED_ABI_VERSION=7",
            )
            .replace(
                manifest["kernel"]["abi_fingerprint"],
                "0x0123456789abcdef",
            )
        )
        verify_release(
            release_root,
            android_root,
            acceptance_path=historical_acceptance,
            semantic=False,
        )
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a historical release without its acceptance descriptor",
        )
        manifest_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        )

        extra = release_root / "support/undeclared"
        extra.write_bytes(b"x")
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "an undeclared payload",
        )
        extra.unlink()

        extra_dir = release_root / "undeclared-directory"
        extra_dir.mkdir()
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "an undeclared directory",
        )
        extra_dir.rmdir()

        payload = release_root / "kernel/System.map"
        payload.write_bytes(b"xx")
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a changed payload size",
        )
        payload.write_bytes(b"x")

        original = copy.deepcopy(manifest)
        manifest["kernel"]["page_size"] = 4096
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a 4 KiB Android kernel",
        )
        manifest = copy.deepcopy(original)
        manifest["jelly"]["patch_manifest_sha256"] = "e" * 64
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a Jelly patch series different from tracked provenance",
        )
        manifest = copy.deepcopy(original)
        manifest["product_family"] = "wrong-family"
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "the wrong product family",
        )
        manifest = copy.deepcopy(original)
        manifest["inventory"][0]["path"] = "../escape"
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "an inventory path traversal",
        )
        manifest_path.write_text(json.dumps(original, indent=2, sort_keys=True) + "\n")

        acceptance_text = acceptance.read_text()
        acceptance.write_text(acceptance_text.replace(release_id, "wrong", 1))
        expect_failure(
            lambda: verify_release(
                release_root,
                android_root,
                acceptance_path=acceptance,
                semantic=False,
            ),
            "a mismatched acceptance descriptor",
        )

        remote_syntax = subprocess.run(
            [
                sys.executable,
                "-B",
                str(android_root / "tools/install-release.py"),
                "--source",
                "builder.invalid:/incoming/release",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if remote_syntax.returncode == 0:
            fail("local installer accepted non-filesystem source syntax")

    print("Android 17 release contract passed")


if __name__ == "__main__":
    main()
