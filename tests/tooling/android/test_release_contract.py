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


def read_routes(projects_path: Path, android_root: Path, provenance) -> list[dict]:
    lines = [
        line for line in projects_path.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    reader = csv.DictReader(lines, delimiter="\t")
    routes = []
    for row in reader:
        patch_count, series_digest = provenance(android_root / row["series_dir"])
        routes.append({
            "route_id": row["route_id"],
            "manifest_remote": row["manifest_remote"],
            "manifest_project": row["manifest_project"],
            "checkout_path": row["checkout_path"],
            "base_commit": row["base_commit"],
            "base_tree": row["base_tree"],
            "final_tree": row["final_tree"],
            "patch_count": patch_count,
            "series_manifest_sha256": series_digest,
        })
    return routes


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()

    root = args.source_root.resolve()
    android_root = root / "android/lineage-24"
    sys.path.insert(0, str(android_root / "tools"))
    from release_common import (  # noqa: PLC0415
        BUILD_VARIANTS,
        DIRECTORY_PREFIX,
        PAYLOAD_SPECS,
        PRODUCT_FAMILY,
        SCHEMA_VERSION,
        ACCEPTANCE_KEYS,
        kernel_series_provenance,
        parse_env,
        route_series,
        series_provenance,
        sha256_path,
        verify_release,
    )

    def provenance(*directories: Path) -> tuple[int, str]:
        return series_provenance(root, *directories)

    schema = json.loads(
        (android_root / "support/release/release-manifest.schema.json").read_text()
    )
    if schema.get("properties", {}).get("product_family", {}).get("const") != PRODUCT_FAMILY:
        fail("release schema does not pin the Android product family")

    aosp = parse_env(android_root / "metadata/lineage.env")
    orangefox = parse_env(android_root / "third_party/orangefox/metadata.env")
    orangefox_projects = android_root / "third_party/orangefox/patches/projects.tsv"
    kernel_root = root / "kernel/android-6.18"
    kernel = parse_env(kernel_root / "patches/metadata.env")
    kernel_patch_count, kernel_patch_digest = kernel_series_provenance(root)
    abi = parse_env(root / "kernel/common/abi/identity.env")
    tracked_acceptance = parse_env(android_root / "metadata/accepted-release.env")
    if set(tracked_acceptance) != ACCEPTANCE_KEYS:
        fail("tracked acceptance descriptor keys differ from the release contract")
    tracked_expected = {
        "LINEAGE24_ACCEPTED_SCHEMA": str(SCHEMA_VERSION),
        "LINEAGE24_ACCEPTED_PRODUCT_FAMILY": PRODUCT_FAMILY,
        "LINEAGE24_ACCEPTED_ANDROID_MAJOR": "17",
        "LINEAGE24_ACCEPTED_PAGE_SIZE": "4096",
    }
    if tracked_acceptance["LINEAGE24_ACCEPTED_BUILD_VARIANT"] not in BUILD_VARIANTS:
        fail("tracked acceptance descriptor names an unsupported build variant")
    for key, value in tracked_expected.items():
        if tracked_acceptance.get(key) != value:
            fail(f"tracked acceptance descriptor differs at {key}")
    if tracked_acceptance["LINEAGE24_ACCEPTED_DIRECTORY_NAME"] != (
        DIRECTORY_PREFIX + tracked_acceptance["LINEAGE24_ACCEPTED_RELEASE_ID"]
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
            "build_variant": aosp["LINEAGE24_BUILD_VARIANT"],
            "fingerprint": "fixture/fingerprint",
            "vendor_api_level": 202704,
            "product": aosp["LINEAGE24_PRODUCT"],
            "release_config": aosp["LINEAGE24_RELEASE_CONFIG"],
        },
        "source": {
            "manifest_repository": aosp["LINEAGE24_MANIFEST_REPOSITORY"],
            "manifest_revision": aosp["LINEAGE24_MANIFEST_REVISION"],
            "manifest_commit": aosp["LINEAGE24_MANIFEST_COMMIT"],
            "manifest_tree": aosp["LINEAGE24_MANIFEST_TREE"],
            "aosp_tag": aosp["LINEAGE24_AOSP_TAG"],
            "base_manifest_sha256": aosp["LINEAGE24_BASE_MANIFEST_SHA256"],
            "projects_manifest_sha256": sha256_path(
                android_root / "patches/projects.tsv"
            ),
            "routes": read_routes(
                android_root / "patches/projects.tsv", android_root, provenance
            ),
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
            "patch_count": provenance(*route_series(android_root, orangefox_projects))[0],
            "projects_manifest_sha256": sha256_path(
                android_root / "third_party/orangefox/patches/projects.tsv"
            ),
            "routes": read_routes(orangefox_projects, android_root, provenance),
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
            "page_size": 4096,
            "abi_version": int(abi["DARWIN_UML_ABI_VERSION"]),
            "abi_fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
            "elf_role": "darwin-uml",
        },
        "outer": {"revision": "a" * 40, "dirty": False},
        "producer": {
            "entry_point": "android/lineage-24/build/publish-release.sh",
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
                f"LINEAGE24_ACCEPTED_SCHEMA={SCHEMA_VERSION}",
                f"LINEAGE24_ACCEPTED_RELEASE_ID='{release_id}'",
                f"LINEAGE24_ACCEPTED_DIRECTORY_NAME='{directory_name}'",
                f"LINEAGE24_ACCEPTED_PRODUCT_FAMILY='{PRODUCT_FAMILY}'",
                "LINEAGE24_ACCEPTED_ANDROID_MAJOR=17",
                f"LINEAGE24_ACCEPTED_BUILD_VARIANT='{manifest['android']['build_variant']}'",
                f"LINEAGE24_ACCEPTED_SOURCE_MANIFEST_COMMIT='{manifest['source']['manifest_commit']}'",
                f"LINEAGE24_ACCEPTED_SOURCE_BASE_MANIFEST_SHA256='{manifest['source']['base_manifest_sha256']}'",
                f"LINEAGE24_ACCEPTED_SOURCE_PROJECTS_MANIFEST_SHA256='{manifest['source']['projects_manifest_sha256']}'",
                f"LINEAGE24_ACCEPTED_ORANGEFOX_MANIFEST_COMMIT='{manifest['orangefox']['manifest_commit']}'",
                f"LINEAGE24_ACCEPTED_ORANGEFOX_BASE_MANIFEST_SHA256='{manifest['orangefox']['base_manifest_sha256']}'",
                f"LINEAGE24_ACCEPTED_ORANGEFOX_PROJECTS_MANIFEST_SHA256='{manifest['orangefox']['projects_manifest_sha256']}'",
                f"LINEAGE24_ACCEPTED_ORANGEFOX_BOOTABLE_RECOVERY_COMMIT='{manifest['orangefox']['bootable_recovery_commit']}'",
                f"LINEAGE24_ACCEPTED_ORANGEFOX_VENDOR_RECOVERY_COMMIT='{manifest['orangefox']['vendor_recovery_commit']}'",
                f"LINEAGE24_ACCEPTED_ORANGEFOX_SE_OMAPI_COMMIT='{manifest['orangefox']['se_omapi_commit']}'",
                f"LINEAGE24_ACCEPTED_KERNEL_FINAL_TREE='{manifest['kernel']['final_tree']}'",
                f"LINEAGE24_ACCEPTED_KERNEL_PATCH_MANIFEST_SHA256='{manifest['kernel']['patch_manifest_sha256']}'",
                f"LINEAGE24_ACCEPTED_OUTER_REVISION='{manifest['outer']['revision']}'",
                "LINEAGE24_ACCEPTED_PAGE_SIZE=4096",
                f"LINEAGE24_ACCEPTED_ABI_VERSION={manifest['kernel']['abi_version']}",
                f"LINEAGE24_ACCEPTED_ABI_FINGERPRINT='{manifest['kernel']['abi_fingerprint']}'",
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
        historical["source"]["projects_manifest_sha256"] = "b" * 64
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
            .replace(manifest["source"]["projects_manifest_sha256"], "b" * 64)
            .replace(manifest["kernel"]["final_tree"], "c" * 40)
            .replace(manifest["kernel"]["patch_manifest_sha256"], "d" * 64)
            .replace(
                f"LINEAGE24_ACCEPTED_ABI_VERSION={manifest['kernel']['abi_version']}",
                "LINEAGE24_ACCEPTED_ABI_VERSION=7",
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
        manifest["kernel"]["page_size"] = 16384
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "a 16 KiB Android kernel",
        )
        tracked_variant = original["android"]["build_variant"]
        for variant, label in (
            (next(name for name in BUILD_VARIANTS if name != tracked_variant),
             "a build variant other than the tracked one"),
            ("eng", "an unsupported build variant"),
        ):
            manifest = copy.deepcopy(original)
            manifest["android"]["build_variant"] = variant
            manifest_path.write_text(json.dumps(manifest))
            expect_failure(
                lambda: verify_release(release_root, android_root, semantic=False),
                label,
            )
        manifest = copy.deepcopy(original)
        manifest["source"]["aosp_tag"] = "android-0.0.0_r0"
        manifest_path.write_text(json.dumps(manifest))
        expect_failure(
            lambda: verify_release(release_root, android_root, semantic=False),
            "an Android source release different from the tracked one",
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
