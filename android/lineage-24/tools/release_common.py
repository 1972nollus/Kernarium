#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

_ARTIFACTS = str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts")
if _ARTIFACTS not in sys.path:
    sys.path.insert(0, _ARTIFACTS)
from contract_checks import ContractChecks, parse_config, sha256_path  # noqa: E402

_CHECKS = ContractChecks("Android release verifier")
error = _CHECKS.error
fail = _CHECKS.fail
require_exact_keys = _CHECKS.require_exact_keys
require_string = _CHECKS.require_string
require_integer = _CHECKS.require_integer
require_hex = _CHECKS.require_hex
safe_relative = _CHECKS.safe_relative
parse_env = _CHECKS.parse_env

SCHEMA_VERSION = 3
PRODUCT_FAMILY = "android-17-ack-6.18-4k"
DIRECTORY_PREFIX = "android17-ack6.18-4k-"
RELEASE_ID_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")

# The Android build variants a release may be built as, with the build
# properties each one fixes. LINEAGE24_BUILD_VARIANT in metadata/lineage.env
# selects the one the tracked tree builds, and every variant-dependent check
# follows that setting.
BUILD_VARIANTS = {
    "user": {"ro.build.type": "user", "ro.debuggable": "0"},
    "userdebug": {"ro.build.type": "userdebug", "ro.debuggable": "1"},
}


def configured_build_variant(android_root: Path) -> str:
    variant = parse_env(android_root / "metadata/lineage.env")["LINEAGE24_BUILD_VARIANT"]
    if variant not in BUILD_VARIANTS:
        fail(f"tracked Android build variant is not supported: {variant}")
    return variant


PAYLOAD_SPECS = {
    "boot/kernel-command-line.txt": ("kernel-command-line", "utf8-text"),
    "guest/android-17-uml-arm64.img.zst": ("guest-disk-archive", "zstd-raw-gpt"),
    "guest/partition-layout.json": ("partition-layout", "json"),
    "kernel/System.map": ("kernel-symbol-map", "utf8-text"),
    "kernel/config": ("kernel-config", "kconfig"),
    "kernel/modules.builtin": ("built-in-module-list", "utf8-text"),
    "kernel/modules.builtin.modinfo": ("built-in-module-metadata", "binary-records"),
    "kernel/vmlinux": ("kernel-elf", "elf64-aarch64-et-dyn"),
    "provenance/aosp-repo-manifest.xml": ("aosp-resolved-manifest", "xml"),
    "provenance/orangefox-repo-manifest.xml": ("orangefox-resolved-manifest", "xml"),
    "provenance/build-receipt.tsv": ("build-receipt", "tsv"),
    "support/compatibility-matrix.xml": ("vendor-compatibility-matrix", "xml"),
    "support/fstab.uml": ("android-fstab", "utf8-text"),
    "support/vintf-manifest.xml": ("vendor-vintf-manifest", "xml"),
}

TOP_LEVEL_KEYS = {
    "schema_version",
    "release_id",
    "directory_name",
    "product_family",
    "acceptance",
    "published_at_utc",
    "android",
    "aosp",
    "jelly",
    "orangefox",
    "kernel",
    "outer",
    "producer",
    "inventory",
}
ANDROID_KEYS = {
    "major",
    "sdk",
    "build_id",
    "build_variant",
    "fingerprint",
    "vendor_api_level",
    "product",
    "release_config",
}
AOSP_KEYS = {
    "manifest_repository",
    "manifest_tag",
    "manifest_commit",
    "manifest_tree",
    "base_manifest_sha256",
    "projects_manifest_sha256",
    "routes",
}
ROUTE_KEYS = {
    "route_id",
    "manifest_project",
    "checkout_path",
    "base_commit",
    "base_tree",
    "final_tree",
    "patch_count",
    "series_manifest_sha256",
}
JELLY_KEYS = {
    "repository",
    "branch",
    "base_commit",
    "base_tree",
    "final_commit",
    "final_tree",
    "patch_count",
    "patch_manifest_sha256",
    "destination",
    "license",
}
ORANGEFOX_KEYS = {
    "sync_repository",
    "sync_branch",
    "sync_commit",
    "sync_tree",
    "sync_script_sha256",
    "manifest_repository",
    "manifest_branch",
    "manifest_commit",
    "manifest_tree",
    "base_manifest_sha256",
    "branch",
    "bootable_recovery_repository",
    "bootable_recovery_commit",
    "bootable_recovery_tree",
    "vendor_recovery_repository",
    "vendor_recovery_commit",
    "vendor_recovery_tree",
    "se_omapi_repository",
    "se_omapi_commit",
    "se_omapi_tree",
    "device_destination",
    "product",
    "release_config",
    "patch_count",
    "projects_manifest_sha256",
    "routes",
    "license",
}
KERNEL_KEYS = {
    "family",
    "repository",
    "branch",
    "tag",
    "base_commit",
    "base_tree",
    "final_tree",
    "patch_count",
    "patch_manifest_sha256",
    "release",
    "page_size",
    "abi_version",
    "abi_fingerprint",
    "elf_role",
}
OUTER_KEYS = {"revision", "dirty"}
PRODUCER_KEYS = {"entry_point", "functional_options", "builder"}
BUILDER_KEYS = {
    "os",
    "architecture",
    "jdk",
    "clang",
    "linker",
    "darwin_sdk",
    "android_build_tools",
}
INVENTORY_KEYS = {"path", "role", "format", "size"}

ACCEPTANCE_KEYS = {
    "LINEAGE24_ACCEPTED_SCHEMA",
    "LINEAGE24_ACCEPTED_RELEASE_ID",
    "LINEAGE24_ACCEPTED_DIRECTORY_NAME",
    "LINEAGE24_ACCEPTED_PRODUCT_FAMILY",
    "LINEAGE24_ACCEPTED_ANDROID_MAJOR",
    "LINEAGE24_ACCEPTED_BUILD_VARIANT",
    "LINEAGE24_ACCEPTED_AOSP_MANIFEST_COMMIT",
    "LINEAGE24_ACCEPTED_AOSP_BASE_MANIFEST_SHA256",
    "LINEAGE24_ACCEPTED_AOSP_PROJECTS_MANIFEST_SHA256",
    "LINEAGE24_ACCEPTED_JELLY_UPSTREAM_COMMIT",
    "LINEAGE24_ACCEPTED_JELLY_FINAL_TREE",
    "LINEAGE24_ACCEPTED_JELLY_PATCH_MANIFEST_SHA256",
    "LINEAGE24_ACCEPTED_ORANGEFOX_MANIFEST_COMMIT",
    "LINEAGE24_ACCEPTED_ORANGEFOX_BASE_MANIFEST_SHA256",
    "LINEAGE24_ACCEPTED_ORANGEFOX_PROJECTS_MANIFEST_SHA256",
    "LINEAGE24_ACCEPTED_ORANGEFOX_BOOTABLE_RECOVERY_COMMIT",
    "LINEAGE24_ACCEPTED_ORANGEFOX_VENDOR_RECOVERY_COMMIT",
    "LINEAGE24_ACCEPTED_ORANGEFOX_SE_OMAPI_COMMIT",
    "LINEAGE24_ACCEPTED_KERNEL_FINAL_TREE",
    "LINEAGE24_ACCEPTED_KERNEL_PATCH_MANIFEST_SHA256",
    "LINEAGE24_ACCEPTED_OUTER_REVISION",
    "LINEAGE24_ACCEPTED_PAGE_SIZE",
    "LINEAGE24_ACCEPTED_ABI_VERSION",
    "LINEAGE24_ACCEPTED_ABI_FINGERPRINT",
}


def load_manifest(release_root: Path) -> dict:
    path = release_root / "release-manifest.json"
    if not path.is_file() or path.is_symlink():
        fail("release-manifest.json is missing or unsafe")
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        fail(f"cannot read release manifest: {error}")
    return validate_manifest(value)


def validate_manifest(value: object) -> dict:
    manifest = require_exact_keys(value, TOP_LEVEL_KEYS, "release manifest")
    if manifest["schema_version"] != SCHEMA_VERSION:
        fail("release manifest schema version is unsupported")
    release_id = require_string(manifest["release_id"], "release ID")
    if RELEASE_ID_RE.fullmatch(release_id) is None:
        fail("release ID has an unsafe format")
    directory_name = require_string(manifest["directory_name"], "directory name")
    if directory_name != DIRECTORY_PREFIX + release_id:
        fail("release directory name does not match the release ID")
    if manifest["product_family"] != PRODUCT_FAMILY:
        fail("release product family is not Android 17 ACK 6.18 4 KiB")
    if manifest["acceptance"] != "accepted":
        fail("release is not marked accepted")
    require_string(manifest["published_at_utc"], "publication timestamp")

    android = require_exact_keys(manifest["android"], ANDROID_KEYS, "android")
    if require_integer(android["major"], "Android major") != 17:
        fail("Android major is not 17")
    if require_integer(android["sdk"], "Android SDK") < 37:
        fail("Android SDK is below 37")
    for key in ("build_id", "build_variant", "fingerprint", "product", "release_config"):
        require_string(android[key], f"android.{key}")
    if android["build_variant"] not in BUILD_VARIANTS:
        fail("Android build variant is not user or userdebug")
    require_integer(android["vendor_api_level"], "vendor API level", minimum=202604)

    aosp = require_exact_keys(manifest["aosp"], AOSP_KEYS, "aosp")
    for key in ("manifest_repository", "manifest_tag"):
        require_string(aosp[key], f"aosp.{key}")
    require_hex(aosp["manifest_commit"], 40, "AOSP manifest commit")
    require_hex(aosp["manifest_tree"], 40, "AOSP manifest tree")
    require_hex(aosp["base_manifest_sha256"], 64, "base manifest SHA-256")
    require_hex(aosp["projects_manifest_sha256"], 64, "projects manifest SHA-256")
    validate_routes(aosp["routes"], "AOSP")

    jelly = require_exact_keys(manifest["jelly"], JELLY_KEYS, "jelly")
    if jelly["repository"] != (
        "https://github.com/LineageOS/android_packages_apps_Jelly"
    ):
        fail("Jelly repository is not the reviewed LineageOS source")
    if jelly["branch"] != "lineage-23.2":
        fail("Jelly branch is not lineage-23.2")
    for key in ("base_commit", "base_tree", "final_commit", "final_tree"):
        require_hex(jelly[key], 40, f"jelly.{key}")
    require_integer(jelly["patch_count"], "Jelly patch count", minimum=1)
    require_hex(
        jelly["patch_manifest_sha256"], 64, "Jelly patch manifest SHA-256"
    )
    if safe_relative(jelly["destination"], "Jelly destination") != "packages/apps/Jelly":
        fail("Jelly destination is not packages/apps/Jelly")
    if jelly["license"] != "Apache-2.0":
        fail("Jelly license identity changed")

    orangefox = require_exact_keys(
        manifest["orangefox"], ORANGEFOX_KEYS, "orangefox"
    )
    expected_strings = {
        "sync_repository": "https://gitlab.com/OrangeFox/sync.git",
        "manifest_repository": (
            "https://github.com/nebrassy/platform_manifest_twrp_aosp.git"
        ),
        "branch": "fox_14.1",
        "bootable_recovery_repository": (
            "https://gitlab.com/OrangeFox/bootable/Recovery.git"
        ),
        "vendor_recovery_repository": (
            "https://gitlab.com/OrangeFox/vendor/recovery.git"
        ),
        "se_omapi_repository": "https://gitlab.com/OrangeFox/external/se_omapi.git",
        "device_destination": "device/kernarium/darwin",
        "product": "twrp_darwin",
        "release_config": "ap2a",
        "license": "GPL-3.0-or-later",
    }
    for key, expected in expected_strings.items():
        if orangefox[key] != expected:
            fail(f"orangefox.{key} is not the reviewed value")
    for key in ("sync_branch", "manifest_branch"):
        require_string(orangefox[key], f"orangefox.{key}")
    for key in (
        "sync_commit", "sync_tree", "manifest_commit", "manifest_tree",
        "bootable_recovery_commit", "bootable_recovery_tree",
        "vendor_recovery_commit", "vendor_recovery_tree",
        "se_omapi_commit", "se_omapi_tree",
    ):
        require_hex(orangefox[key], 40, f"orangefox.{key}")
    for key in (
        "sync_script_sha256", "base_manifest_sha256",
        "projects_manifest_sha256",
    ):
        require_hex(orangefox[key], 64, f"orangefox.{key}")
    require_integer(orangefox["patch_count"], "OrangeFox patch count", minimum=1)
    validate_routes(orangefox["routes"], "OrangeFox")

    kernel = require_exact_keys(manifest["kernel"], KERNEL_KEYS, "kernel")
    if kernel["family"] != "android-6.18":
        fail("kernel family is not android-6.18")
    for key in ("repository", "branch", "tag", "release"):
        require_string(kernel[key], f"kernel.{key}")
    for key in ("base_commit", "base_tree", "final_tree"):
        require_hex(kernel[key], 40, f"kernel.{key}")
    require_hex(kernel["patch_manifest_sha256"], 64, "kernel patch manifest SHA-256")
    require_integer(kernel["patch_count"], "kernel patch count", minimum=1)
    if require_integer(kernel["page_size"], "kernel page size") != 4096:
        fail("kernel page size is not 4096")
    require_integer(kernel["abi_version"], "kernel ABI version", minimum=1)
    abi_fingerprint = require_string(kernel["abi_fingerprint"], "kernel ABI fingerprint")
    if re.fullmatch(r"0x[0-9a-f]{16}", abi_fingerprint) is None:
        fail("kernel ABI fingerprint has an unsafe format")
    if kernel["elf_role"] != "darwin-uml":
        fail("kernel ELF role is not darwin-uml")

    outer = require_exact_keys(manifest["outer"], OUTER_KEYS, "outer")
    require_hex(outer["revision"], 40, "outer revision")
    if outer["dirty"] is not False:
        fail("accepted release records a dirty outer project")

    producer = require_exact_keys(manifest["producer"], PRODUCER_KEYS, "producer")
    if producer["entry_point"] != "android/lineage-24/build/publish-release.sh":
        fail("release records an unexpected producer entry point")
    options = producer["functional_options"]
    if not isinstance(options, list) or not options:
        fail("producer functional options must be a nonempty array")
    for index, option in enumerate(options):
        require_string(option, f"producer option {index}")
    builder = require_exact_keys(producer["builder"], BUILDER_KEYS, "producer.builder")
    for key in BUILDER_KEYS:
        value = require_string(builder[key], f"producer.builder.{key}")
        if re.search(r"(^|[ (])/[^ )]+", value):
            fail(f"producer.builder.{key} stores an absolute filesystem path")
    if builder["architecture"] != "x86_64":
        fail("release was not produced on an x86_64 builder")

    inventory = manifest["inventory"]
    if not isinstance(inventory, list):
        fail("release inventory must be an array")
    expected_paths = set(PAYLOAD_SPECS)
    seen_paths: set[str] = set()
    ordered_paths: list[str] = []
    for index, raw_entry in enumerate(inventory):
        entry = require_exact_keys(raw_entry, INVENTORY_KEYS, f"inventory entry {index}")
        path = safe_relative(entry["path"], f"inventory entry {index} path")
        if path in seen_paths:
            fail(f"duplicate inventory path: {path}")
        seen_paths.add(path)
        ordered_paths.append(path)
        if path not in PAYLOAD_SPECS:
            fail(f"undeclared release payload: {path}")
        role, file_format = PAYLOAD_SPECS[path]
        if entry["role"] != role or entry["format"] != file_format:
            fail(f"inventory role or format differs for {path}")
        require_integer(entry["size"], f"inventory size for {path}", minimum=1)
    if seen_paths != expected_paths:
        fail(f"release inventory differs from its fixed contract: {sorted(expected_paths - seen_paths)}")
    if ordered_paths != sorted(ordered_paths):
        fail("release inventory is not sorted by path")
    return manifest


def validate_routes(routes: object, label: str) -> None:
    """Check one route table of a release manifest."""
    if not isinstance(routes, list) or not routes:
        fail(f"{label} routes must be a nonempty array")
    seen_routes: set[str] = set()
    for index, raw_route in enumerate(routes):
        route = require_exact_keys(raw_route, ROUTE_KEYS, f"{label} route {index}")
        route_id = require_string(route["route_id"], f"{label} route {index} ID")
        if route_id in seen_routes:
            fail(f"duplicate {label} route ID: {route_id}")
        seen_routes.add(route_id)
        for key in ("manifest_project", "checkout_path"):
            safe_relative(route[key], f"{label} route {route_id} {key}")
        for key in ("base_commit", "base_tree", "final_tree"):
            require_hex(route[key], 40, f"{label} route {route_id} {key}")
        require_integer(
            route["patch_count"], f"{label} route {route_id} patch count", minimum=1
        )
        require_hex(
            route["series_manifest_sha256"], 64,
            f"{label} route {route_id} series manifest SHA-256",
        )


def kernel_series_provenance(outer_root: Path) -> tuple[int, str]:
    """Return the ACK line's patch count and ordered patch-content digest."""
    tooling = str(outer_root / "kernel/common/tooling")
    if tooling not in sys.path:
        sys.path.insert(0, tooling)
    import kernel_series  # noqa: PLC0415

    try:
        return kernel_series.provenance(
            kernel_series.load_line(outer_root / "kernel/android-6.18/patches/metadata.env")
        )
    except kernel_series.SeriesError as error:
        fail(f"ACK kernel series are invalid: {error}")


def series_provenance(outer_root: Path, *directories: Path) -> tuple[int, str]:
    """Return the patch count and ordered patch-content digest of generated series."""
    scripts = str(outer_root / "scripts")
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    import patch_series  # noqa: PLC0415

    try:
        return patch_series.series_provenance(list(directories))
    except patch_series.SeriesError as error:
        fail(f"generated patch series are invalid: {error}")


def route_series(android_root: Path, projects: Path) -> list[Path]:
    """Return the series directories a route table lists, in route order."""
    lines = [
        line for line in projects.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    header = lines[0].split("\t")
    column = header.index("series_dir")
    return [android_root / line.split("\t")[column] for line in lines[1:]]


def read_routes(projects: Path, android_root: Path, label: str) -> list[dict]:
    """Return a route table's release-manifest routes, with series provenance."""
    lines = [
        line for line in projects.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    routes = []
    for row in csv.DictReader(lines, delimiter="\t"):
        patch_count, series_digest = series_provenance(
            android_root.parents[1], android_root / row["series_dir"]
        )
        routes.append({
            "route_id": row["route_id"],
            "manifest_project": row["manifest_project"],
            "checkout_path": row["checkout_path"],
            "base_commit": row["base_commit"],
            "base_tree": row["base_tree"],
            "final_tree": row["final_tree"],
            "patch_count": patch_count,
            "series_manifest_sha256": series_digest,
        })
    if not routes:
        fail(f"{label} projects manifest has no routes")
    return routes


TRACKED_SECTIONS = {
    "android": "Android",
    "aosp": "AOSP",
    "jelly": "Jelly",
    "orangefox": "OrangeFox",
    "kernel": "kernel",
}


def tracked_identity(android_root: Path) -> dict[str, dict]:
    """Return the release-manifest fields the tracked tree determines.

    The publisher adds each section's per-build fields: the AOSP and
    OrangeFox routes, Jelly's final commit, and the kernel's release, page
    size and ELF role. The verifier compares an accepted release against
    these fields.
    """
    outer_root = android_root.parents[1]
    aosp = parse_env(android_root / "metadata/lineage.env")
    jelly = parse_env(android_root / "third_party/jelly/metadata.env")
    orangefox = parse_env(android_root / "third_party/orangefox/metadata.env")
    kernel = parse_env(outer_root / "kernel/android-6.18/patches/metadata.env")
    abi = parse_env(outer_root / "kernel/common/abi/identity.env")
    jelly_count, jelly_digest = series_provenance(
        outer_root, android_root / "third_party/jelly/series"
    )
    orangefox_projects = android_root / "third_party/orangefox/patches/projects.tsv"
    orangefox_count, _ = series_provenance(
        outer_root, *route_series(android_root, orangefox_projects)
    )
    kernel_count, kernel_digest = kernel_series_provenance(outer_root)
    return {
        "android": {"build_variant": configured_build_variant(android_root)},
        "aosp": {
            "manifest_repository": aosp["LINEAGE24_MANIFEST_REPOSITORY"],
            "manifest_tag": aosp["LINEAGE24_MANIFEST_TAG"],
            "manifest_commit": aosp["LINEAGE24_MANIFEST_COMMIT"],
            "manifest_tree": aosp["LINEAGE24_MANIFEST_TREE"],
            "base_manifest_sha256": aosp["LINEAGE24_BASE_MANIFEST_SHA256"],
            "projects_manifest_sha256": sha256_path(android_root / "patches/projects.tsv"),
        },
        "jelly": {
            "repository": jelly["JELLY_UPSTREAM_REPOSITORY"],
            "branch": jelly["JELLY_UPSTREAM_BRANCH"],
            "base_commit": jelly["JELLY_UPSTREAM_COMMIT"],
            "base_tree": jelly["JELLY_UPSTREAM_TREE"],
            "final_tree": jelly["JELLY_FINAL_TREE"],
            "patch_count": jelly_count,
            "patch_manifest_sha256": jelly_digest,
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
            "bootable_recovery_repository": orangefox["ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY"],
            "bootable_recovery_commit": orangefox["ORANGEFOX_BOOTABLE_RECOVERY_COMMIT"],
            "bootable_recovery_tree": orangefox["ORANGEFOX_BOOTABLE_RECOVERY_TREE"],
            "vendor_recovery_repository": orangefox["ORANGEFOX_VENDOR_RECOVERY_REPOSITORY"],
            "vendor_recovery_commit": orangefox["ORANGEFOX_VENDOR_RECOVERY_COMMIT"],
            "vendor_recovery_tree": orangefox["ORANGEFOX_VENDOR_RECOVERY_TREE"],
            "se_omapi_repository": orangefox["ORANGEFOX_SE_OMAPI_REPOSITORY"],
            "se_omapi_commit": orangefox["ORANGEFOX_SE_OMAPI_COMMIT"],
            "se_omapi_tree": orangefox["ORANGEFOX_SE_OMAPI_TREE"],
            "device_destination": orangefox["ORANGEFOX_DEVICE_OVERLAY_DESTINATION"],
            "product": orangefox["ORANGEFOX_PRODUCT"],
            "release_config": orangefox["ORANGEFOX_RELEASE_CONFIG"],
            "patch_count": orangefox_count,
            "projects_manifest_sha256": sha256_path(orangefox_projects),
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
            "patch_count": kernel_count,
            "patch_manifest_sha256": kernel_digest,
            "abi_version": int(abi["DARWIN_UML_ABI_VERSION"]),
            "abi_fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
        },
    }


def verify_tracked_identity(manifest: dict, android_root: Path) -> None:
    for section, expected in tracked_identity(android_root).items():
        for key, value in expected.items():
            if manifest[section][key] != value:
                fail(
                    f"release {TRACKED_SECTIONS[section]} identity differs "
                    f"from tracked {key}"
                )


def verify_acceptance(manifest: dict, acceptance_path: Path) -> None:
    acceptance = parse_env(acceptance_path)
    if set(acceptance) != ACCEPTANCE_KEYS:
        fail("accepted-release descriptor keys differ from the fixed contract")
    expected = {
        "LINEAGE24_ACCEPTED_SCHEMA": str(SCHEMA_VERSION),
        "LINEAGE24_ACCEPTED_RELEASE_ID": manifest["release_id"],
        "LINEAGE24_ACCEPTED_DIRECTORY_NAME": manifest["directory_name"],
        "LINEAGE24_ACCEPTED_PRODUCT_FAMILY": manifest["product_family"],
        "LINEAGE24_ACCEPTED_ANDROID_MAJOR": str(manifest["android"]["major"]),
        "LINEAGE24_ACCEPTED_BUILD_VARIANT": manifest["android"]["build_variant"],
        "LINEAGE24_ACCEPTED_AOSP_MANIFEST_COMMIT": manifest["aosp"]["manifest_commit"],
        "LINEAGE24_ACCEPTED_AOSP_BASE_MANIFEST_SHA256": manifest["aosp"]["base_manifest_sha256"],
        "LINEAGE24_ACCEPTED_AOSP_PROJECTS_MANIFEST_SHA256": manifest["aosp"]["projects_manifest_sha256"],
        "LINEAGE24_ACCEPTED_JELLY_UPSTREAM_COMMIT": manifest["jelly"]["base_commit"],
        "LINEAGE24_ACCEPTED_JELLY_FINAL_TREE": manifest["jelly"]["final_tree"],
        "LINEAGE24_ACCEPTED_JELLY_PATCH_MANIFEST_SHA256": manifest["jelly"]["patch_manifest_sha256"],
        "LINEAGE24_ACCEPTED_ORANGEFOX_MANIFEST_COMMIT": manifest["orangefox"]["manifest_commit"],
        "LINEAGE24_ACCEPTED_ORANGEFOX_BASE_MANIFEST_SHA256": manifest["orangefox"]["base_manifest_sha256"],
        "LINEAGE24_ACCEPTED_ORANGEFOX_PROJECTS_MANIFEST_SHA256": manifest["orangefox"]["projects_manifest_sha256"],
        "LINEAGE24_ACCEPTED_ORANGEFOX_BOOTABLE_RECOVERY_COMMIT": manifest["orangefox"]["bootable_recovery_commit"],
        "LINEAGE24_ACCEPTED_ORANGEFOX_VENDOR_RECOVERY_COMMIT": manifest["orangefox"]["vendor_recovery_commit"],
        "LINEAGE24_ACCEPTED_ORANGEFOX_SE_OMAPI_COMMIT": manifest["orangefox"]["se_omapi_commit"],
        "LINEAGE24_ACCEPTED_KERNEL_FINAL_TREE": manifest["kernel"]["final_tree"],
        "LINEAGE24_ACCEPTED_KERNEL_PATCH_MANIFEST_SHA256": manifest["kernel"]["patch_manifest_sha256"],
        "LINEAGE24_ACCEPTED_OUTER_REVISION": manifest["outer"]["revision"],
        "LINEAGE24_ACCEPTED_PAGE_SIZE": str(manifest["kernel"]["page_size"]),
        "LINEAGE24_ACCEPTED_ABI_VERSION": str(manifest["kernel"]["abi_version"]),
        "LINEAGE24_ACCEPTED_ABI_FINGERPRINT": manifest["kernel"]["abi_fingerprint"],
    }
    for key, value in expected.items():
        if acceptance.get(key) != value:
            fail(f"release differs from accepted descriptor field {key}")


def inventory_map(manifest: dict) -> dict[str, dict]:
    return {entry["path"]: entry for entry in manifest["inventory"]}


def verify_inventory(release_root: Path, manifest: dict, *, allow_expanded_disk: bool = False) -> None:
    expected_files = set(PAYLOAD_SPECS) | {"release-manifest.json"}
    if allow_expanded_disk:
        expected_files.add("guest/android-17-uml-arm64.img")
    expected_dirs = {"."}
    for relative in expected_files:
        parent = PurePosixPath(relative).parent
        while str(parent) != ".":
            expected_dirs.add(str(parent))
            parent = parent.parent

    actual_files: set[str] = set()
    actual_dirs: set[str] = {"."}
    for root, directories, files in os.walk(release_root, topdown=True, followlinks=False):
        root_path = Path(root)
        relative_root = root_path.relative_to(release_root)
        for name in directories:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISDIR(mode):
                fail(f"unsafe release directory: {path.relative_to(release_root)}")
            actual_dirs.add(str((relative_root / name).as_posix()))
        for name in files:
            path = root_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode) or not stat.S_ISREG(mode):
                fail(f"unsafe release payload: {path.relative_to(release_root)}")
            actual_files.add(str((relative_root / name).as_posix()))
    if actual_files != expected_files:
        fail(
            "release file set differs from its manifest "
            f"(missing={sorted(expected_files - actual_files)}, "
            f"extra={sorted(actual_files - expected_files)})"
        )
    if actual_dirs != expected_dirs:
        fail(
            "release directory set differs from its manifest "
            f"(missing={sorted(expected_dirs - actual_dirs)}, "
            f"extra={sorted(actual_dirs - expected_dirs)})"
        )

    entries = inventory_map(manifest)
    for relative in PAYLOAD_SPECS:
        observed = (release_root / relative).stat().st_size
        if observed != entries[relative]["size"]:
            fail(f"release payload size differs for {relative}")


def verify_kernel_config(
    release_root: Path,
    android_root: Path,
    *,
    enforce_tracked_policy: bool,
) -> None:
    config = parse_config(release_root / "kernel/config")
    if config.get("CONFIG_MODULES") != "n":
        fail("release kernel does not have CONFIG_MODULES=n")
    module_values = sorted(key for key, value in config.items() if value == "m")
    if module_values:
        fail(f"release kernel contains loadable-module values: {module_values[:8]}")
    if not enforce_tracked_policy:
        return
    # The ACK kernel line owns its configuration contract.
    line_config = android_root.parents[1] / "kernel/android-6.18/config"
    for path, expected in (
        (line_config / "required-builtins.txt", "y"),
        (line_config / "forbidden-options.txt", "n"),
    ):
        for raw in path.read_text(encoding="utf-8").splitlines():
            symbol = raw.strip()
            if not symbol or symbol.startswith("#"):
                continue
            observed = config.get(symbol, "n")
            if observed != expected:
                fail(f"release kernel config {symbol}={observed}, expected {expected}")


def run_checked(arguments: list[str], *, env: dict[str, str] | None = None) -> None:
    result = subprocess.run(arguments, env=env, check=False)
    if result.returncode != 0:
        fail(f"verification command failed ({result.returncode}): {arguments[0]}")


def run_checked_capture(arguments: list[str]) -> str:
    result = subprocess.run(
        arguments,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        fail(
            f"verification command failed ({result.returncode}): "
            f"{arguments[0]} {result.stderr.strip()}"
        )
    return result.stdout.strip()


def verify_disk_boot_payloads(
    disk: Path,
    layout: Path,
    android_root: Path,
    *,
    structural: bool,
) -> None:
    boot_tool = android_root / "tools/boot-image.py"
    for partition in ("init_boot", "recovery"):
        run_checked([
            str(boot_tool),
            "verify",
            "--layout",
            str(layout),
            "--disk",
            str(disk),
            "--partition",
            partition,
        ])
    raw_range = run_checked_capture([
        str(boot_tool),
        "range",
        "--layout",
        str(layout),
        "--disk",
        str(disk),
        "--partition",
        "init_boot",
    ])
    try:
        ramdisk = json.loads(raw_range)
        offset = ramdisk["ramdisk_offset"]
        size = ramdisk["ramdisk_size"]
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        fail(f"cannot parse verified init_boot ramdisk range: {error}")
    if (
        isinstance(offset, bool)
        or not isinstance(offset, int)
        or isinstance(size, bool)
        or not isinstance(size, int)
        or offset < 0
        or size <= 0
    ):
        fail("verified init_boot ramdisk range is invalid")
    arguments = [str(android_root / "tools/verify-initramfs.py")]
    if structural:
        arguments.append("--structural")
    arguments.extend(["--offset", str(offset), "--size", str(size), str(disk)])
    run_checked(arguments)
    run_checked([
        str(android_root / "tools/verify-orangefox-recovery.py"),
        "--android-root",
        str(android_root),
        "--layout",
        str(layout),
        "--disk",
        str(disk),
    ])


def resolve_zstd(value: Path | None) -> Path:
    if value is not None:
        candidate = value
    else:
        located = shutil.which("zstd")
        if located is None:
            fail("zstd is not available; pass --zstd with an absolute local path")
        candidate = Path(located)
    if not candidate.is_absolute():
        fail("zstd must be an absolute local executable")
    candidate = candidate.resolve()
    if not candidate.is_file():
        fail("zstd must be an absolute regular local executable")
    if not os.access(candidate, os.X_OK):
        fail("zstd is not executable")
    return candidate


def verify_semantics(
    release_root: Path,
    manifest: dict,
    android_root: Path,
    zstd: Path | None,
    expanded_disk: Path | None,
    *,
    verify_archive: bool,
    enforce_tracked_policy: bool,
) -> None:
    run_checked([
        str(android_root / "build/verify-kernel-elf.py"),
        str(release_root / "kernel/vmlinux"),
    ])
    verify_kernel_config(
        release_root,
        android_root,
        enforce_tracked_policy=enforce_tracked_policy,
    )
    if enforce_tracked_policy:
        if (release_root / "boot/kernel-command-line.txt").read_bytes() != (
            android_root / "support/boot/kernel-command-line.txt"
        ).read_bytes():
            fail("release kernel command line differs from tracked policy")
        if (release_root / "guest/partition-layout.json").read_bytes() != (
            android_root / "support/image/partition-layout.json"
        ).read_bytes():
            fail("release partition layout differs from tracked policy")

    fstab = (release_root / "support/fstab.uml").read_text(encoding="utf-8")
    for name in (
        "system",
        "system_ext",
        "product",
        "vendor",
        "odm",
        "metadata",
        "userdata",
        "misc",
    ):
        if f"/dev/block/by-name/{name}" not in fstab:
            fail(f"release fstab is missing fixed partition {name}")
    for forbidden in ("logical", "slotselect", "avb", "_dlkm"):
        if forbidden in fstab:
            fail(f"release fstab contains unsupported token: {forbidden}")

    for relative in (
        "support/vintf-manifest.xml",
        "support/compatibility-matrix.xml",
        "provenance/aosp-repo-manifest.xml",
        "provenance/orangefox-repo-manifest.xml",
    ):
        try:
            ET.parse(release_root / relative)
        except (OSError, ET.ParseError) as error:
            fail(f"invalid XML payload {relative}: {error}")

    receipt = (release_root / "provenance/build-receipt.tsv").read_text(
        encoding="utf-8"
    )
    for forbidden in ("/home/", "builder_host", "private_key", "\tssh\t", "\tscp\t"):
        if forbidden in receipt.lower():
            fail("build receipt contains builder endpoint, credential, or local path state")

    if verify_archive:
        zstd_path = resolve_zstd(zstd)
        archive = release_root / "guest/android-17-uml-arm64.img.zst"
        run_checked([str(zstd_path), "-q", "-t", str(archive)])
    if expanded_disk is not None:
        if not expanded_disk.is_file() or expanded_disk.is_symlink():
            fail("expanded guest disk is missing or unsafe")
        run_checked([
            str(android_root / "tools/ubd-disk.py"),
            "verify",
            "--layout",
            str(release_root / "guest/partition-layout.json"),
            "--disk",
            str(expanded_disk),
        ])
        verify_disk_boot_payloads(
            expanded_disk,
            release_root / "guest/partition-layout.json",
            android_root,
            structural=not enforce_tracked_policy,
        )

    if manifest["kernel"]["page_size"] != 4096:
        fail("semantic verification received a non-4-KiB kernel")
    schema = android_root / "support/release/release-manifest.schema.json"
    if not schema.is_file():
        fail("tracked release-manifest schema is missing")


def verify_release(
    release_root: Path,
    android_root: Path,
    *,
    acceptance_path: Path | None = None,
    zstd: Path | None = None,
    expanded_disk: Path | None = None,
    allow_expanded_disk: bool = False,
    semantic: bool = True,
    verify_archive: bool = True,
) -> dict:
    if not release_root.is_absolute():
        fail("release source must be an absolute local path")
    if not release_root.is_dir() or release_root.is_symlink():
        fail("release source must be a regular local directory")
    manifest = load_manifest(release_root)
    if release_root.name != manifest["directory_name"]:
        fail("release directory basename differs from its manifest")
    if acceptance_path is None:
        verify_tracked_identity(manifest, android_root)
    else:
        verify_acceptance(manifest, acceptance_path)
        # Schema 2 recorded the ACK that qualified this rootfs build. Keep that
        # identity as immutable historical provenance, but do not compare it to
        # the current host/kernel bridge. The independently accepted ACK
        # artifact owns that compatibility check now, allowing an old rootfs
        # bundle's writable disk to survive a kernel-only application update.
    verify_inventory(release_root, manifest, allow_expanded_disk=allow_expanded_disk)
    if semantic:
        verify_semantics(
            release_root,
            manifest,
            android_root,
            zstd,
            expanded_disk,
            verify_archive=verify_archive,
            enforce_tracked_policy=acceptance_path is None,
        )
    return manifest
