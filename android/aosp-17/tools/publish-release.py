#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import datetime
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from release_common import (  # noqa: E402
    DIRECTORY_PREFIX,
    PAYLOAD_SPECS,
    PRODUCT_FAMILY,
    RELEASE_ID_RE,
    fail,
    kernel_series_provenance,
    parse_env,
    series_provenance,
    resolve_zstd,
    sha256_path,
    verify_disk_boot_payloads,
    verify_kernel_config,
    verify_release,
)


def absolute_path(value: Path, label: str) -> Path:
    if not value.is_absolute():
        fail(f"{label} must be an absolute local path")
    if any(part in (".", "..") for part in value.parts):
        fail(f"{label} must be normalized")
    return value


def regular_file(value: Path, label: str, *, executable: bool = False) -> Path:
    absolute_path(value, label)
    if not value.is_file() or value.is_symlink():
        fail(f"{label} must be a regular non-symlink file")
    if executable and not os.access(value, os.X_OK):
        fail(f"{label} must be executable")
    return value


def executable_tool(value: Path, label: str) -> Path:
    absolute_path(value, label)
    target = value.resolve()
    if not target.is_file() or not os.access(target, os.X_OK):
        fail(f"{label} must resolve to a regular executable")
    return value


def regular_directory(value: Path, label: str) -> Path:
    absolute_path(value, label)
    if not value.is_dir() or value.is_symlink():
        fail(f"{label} must be a regular non-symlink directory")
    return value


def run(
    arguments: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    capture: bool = False,
) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip() if capture else ""
        fail(f"command failed ({result.returncode}): {arguments[0]} {detail}")
    return result.stdout.strip() if capture else ""


def first_version(arguments: list[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(
        arguments,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        check=False,
    )
    if result.returncode != 0 or not result.stdout.strip():
        fail(f"cannot record tool version: {arguments[0]}")
    line = result.stdout.splitlines()[0].strip().replace("\t", " ")
    return re.sub(r"(^|[ (])/[^ )]+", r"\1<tool-build-source>", line)


def read_properties(archive: zipfile.ZipFile, member: str) -> dict[str, str]:
    try:
        payload = archive.read(member).decode("utf-8", errors="strict")
    except (KeyError, UnicodeError) as error:
        fail(f"cannot read packaged Android properties {member}: {error}")
    values: dict[str, str] = {}
    for raw in payload.splitlines():
        key, separator, value = raw.partition("=")
        if separator and key not in values:
            values[key] = value
    return values


def property_value(values: dict[str, str], keys: tuple[str, ...], label: str) -> str:
    for key in keys:
        value = values.get(key)
        if value:
            return value
    fail(f"packaged Android properties omit {label}")


def read_routes(path: Path, android_root: Path) -> list[dict]:
    lines = [
        line for line in path.read_text(encoding="utf-8").splitlines()
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
        fail("AOSP projects manifest has no routes")
    return routes


def copy_regular(source: Path, destination: Path) -> None:
    regular_file(source, f"release input {source.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination, follow_symlinks=False)
    shutil.copymode(source, destination, follow_symlinks=False)


def extract_small(
    archive: zipfile.ZipFile, member: str, destination: Path, *, limit: int = 1 << 20
) -> None:
    try:
        info = archive.getinfo(member)
    except KeyError:
        fail(f"target files omit release support payload: {member}")
    if info.is_dir() or info.file_size <= 0 or info.file_size > limit:
        fail(f"target-files release support payload has an unsafe size: {member}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(archive.read(info))


def verify_clean_git(repository: Path, label: str, expected_tree: str | None = None) -> str:
    revision = run(["git", "rev-parse", "HEAD"], cwd=repository, capture=True)
    dirty = run(
        ["git", "status", "--porcelain", "--untracked-files=all"],
        cwd=repository,
        capture=True,
    )
    if dirty:
        fail(f"{label} has uncommitted or untracked changes")
    if expected_tree is not None:
        tree = run(["git", "rev-parse", "HEAD^{tree}"], cwd=repository, capture=True)
        if tree != expected_tree:
            fail(f"{label} tree differs from the tracked final identity")
    return revision


def sdk_identity(darwin_sdk: Path) -> str:
    settings = darwin_sdk / "SDKSettings.json"
    if settings.is_file():
        try:
            value = json.loads(settings.read_text(encoding="utf-8"))
            version = value.get("Version") or value.get("DefaultDeploymentTarget")
            if isinstance(version, str) and version:
                return f"MacOSX.sdk {version}"
        except (OSError, UnicodeError, json.JSONDecodeError):
            pass
    return "MacOSX.sdk (version unavailable)"


def write_receipt(path: Path, rows: list[tuple[str, object]]) -> None:
    lines = ["key\tvalue"]
    for key, raw_value in rows:
        value = str(raw_value).replace("\t", " ").replace("\n", " ").strip()
        if not value or "/home/" in value:
            fail(f"build receipt value is empty or stores a local path: {key}")
        lines.append(f"{key}\t{value}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--aosp-source", type=Path, required=True)
    parser.add_argument("--orangefox-source", type=Path, required=True)
    parser.add_argument(
        "--kernel-artifact",
        type=Path,
        required=True,
        help="published ACK kernel artifact of the tracked kernel source",
    )
    parser.add_argument("--target-files", type=Path, required=True)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--darwin-sdk", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--linker", type=Path, required=True)
    parser.add_argument("--vintf-checker", type=Path, required=True)
    parser.add_argument("--zstd", type=Path, required=True)
    parser.add_argument("--release-root", type=Path, required=True)
    parser.add_argument("--release-id", required=True)
    args = parser.parse_args()

    if RELEASE_ID_RE.fullmatch(args.release_id) is None:
        fail("release ID has an unsafe format")
    if platform.system() != "Linux" or platform.machine() != "x86_64":
        fail("release publisher requires an x86_64 Linux builder")

    android_root = Path(__file__).resolve().parents[1]
    outer_root = android_root.parents[1]
    aosp_source = regular_directory(args.aosp_source, "AOSP source")
    orangefox_source = regular_directory(args.orangefox_source, "OrangeFox source")
    kernel_artifact = regular_directory(args.kernel_artifact, "ACK kernel artifact")
    darwin_sdk = regular_directory(args.darwin_sdk, "Darwin SDK")
    release_root = regular_directory(args.release_root, "release root")
    target_files = regular_file(args.target_files, "target-files archive")
    disk = regular_file(args.disk, "raw GPT disk")
    clang = executable_tool(args.clang, "pinned Clang")
    linker = executable_tool(args.linker, "pinned linker")
    vintf_checker = regular_file(args.vintf_checker, "VINTF checker", executable=True)
    zstd = resolve_zstd(args.zstd)

    directory_name = DIRECTORY_PREFIX + args.release_id
    destination = release_root / directory_name
    if destination.exists() or destination.is_symlink():
        fail("release destination already exists")

    aosp_metadata = parse_env(android_root / "metadata/aosp.env")
    jelly_metadata = parse_env(android_root / "third_party/jelly/metadata.env")
    orangefox_metadata = parse_env(
        android_root / "third_party/orangefox/metadata.env"
    )
    kernel_root = outer_root / "kernel/android-6.18"
    kernel_metadata = parse_env(kernel_root / "patches/metadata.env")
    abi_metadata = parse_env(outer_root / "kernel/common/abi/identity.env")
    outer_revision = verify_clean_git(outer_root, "outer project")
    # The release ships the kernel as its published artifact: the tracked
    # source's runtime image, without the DWARF the build's vmlinux keeps.
    run([
        sys.executable,
        "-B",
        str(outer_root / "kernel/common/artifacts/verify-kernel-artifact.py"),
        "--artifact",
        str(kernel_artifact),
        "--common-root",
        str(outer_root / "kernel/common"),
        "--kernel-metadata",
        str(kernel_root / "patches/metadata.env"),
        "--abi-metadata",
        str(outer_root / "kernel/common/abi/identity.env"),
    ])

    run([
        str(android_root / "tools/verify-jelly-source.py"),
        "--android-root",
        str(android_root),
        "--aosp-source",
        str(aosp_source),
    ])
    run([
        str(android_root / "tools/verify-aosp-patches.py"),
        "--android-root",
        str(android_root),
        "--aosp-source",
        str(aosp_source),
        "--expect",
        "final",
    ])
    run([
        str(android_root / "tools/verify-orangefox-source.py"),
        "--android-root",
        str(android_root),
        "--orangefox-source",
        str(orangefox_source),
        "--expect",
        "final",
        "--allow-prebuilt-kernel",
    ])
    kernel_patch_count, kernel_manifest_sha = kernel_series_provenance(outer_root)

    # verify-kernel-artifact.py has checked the payload files and the ELF.
    kernel_payload = kernel_artifact / "kernel"
    kernel_elf = kernel_payload / "vmlinux"
    kernel_config = kernel_payload / "config"
    kernel_release_file = kernel_payload / "kernel.release"
    verify_kernel_config(kernel_artifact, android_root, enforce_tracked_policy=True)
    run([
        str(android_root / "tools/verify-target-files.py"),
        "--target-files",
        str(target_files),
        "--kernel-elf",
        str(kernel_elf),
        "--kernel-config",
        str(kernel_config),
        "--kernel-release-file",
        str(kernel_release_file),
    ])
    vintf_env = os.environ.copy()
    vintf_env["PATH"] = f"{vintf_checker.parent}:/usr/bin:/bin"
    run([str(vintf_checker), str(target_files)], env=vintf_env)
    run([
        str(android_root / "tools/ubd-disk.py"),
        "verify",
        "--layout",
        str(android_root / "support/image/partition-layout.json"),
        "--disk",
        str(disk),
    ])
    verify_disk_boot_payloads(
        disk,
        android_root / "support/image/partition-layout.json",
        android_root,
        structural=False,
    )

    java_candidates = (
        aosp_source / "prebuilts/jdk/jdk21/linux-x86/bin/java",
        aosp_source / "prebuilts/jdk/jdk17/linux-x86/bin/java",
    )
    java = next((path for path in java_candidates if path.is_file()), None)
    if java is None:
        fail("pinned AOSP JDK is missing")

    try:
        archive = zipfile.ZipFile(target_files)
    except (OSError, zipfile.BadZipFile) as error:
        fail(f"cannot open target-files archive: {error}")
    with archive:
        system_properties = read_properties(archive, "SYSTEM/build.prop")
        vendor_properties = read_properties(archive, "VENDOR/build.prop")
        build_id = property_value(
            system_properties, ("ro.build.id", "ro.system.build.id"), "build ID"
        )
        sdk = int(property_value(
            system_properties,
            ("ro.build.version.sdk", "ro.system.build.version.sdk"),
            "SDK level",
        ))
        fingerprint = property_value(
            system_properties,
            ("ro.build.fingerprint", "ro.system.build.fingerprint"),
            "build fingerprint",
        )
        vendor_api_level = int(property_value(
            vendor_properties,
            ("ro.board.api_level", "ro.vendor.api_level"),
            "vendor API level",
        ))

        temporary_parent = Path(tempfile.mkdtemp(
            prefix=f".{directory_name}.", dir=release_root
        ))
        staging = temporary_parent / directory_name
        staging.mkdir()
        try:
            for name, source in (
                ("vmlinux", kernel_elf),
                ("config", kernel_config),
                ("System.map", kernel_payload / "System.map"),
                ("modules.builtin", kernel_payload / "modules.builtin"),
                ("modules.builtin.modinfo", kernel_payload / "modules.builtin.modinfo"),
            ):
                copy_regular(source, staging / "kernel" / name)
            copy_regular(
                android_root / "support/boot/kernel-command-line.txt",
                staging / "boot/kernel-command-line.txt",
            )
            copy_regular(
                android_root / "support/image/partition-layout.json",
                staging / "guest/partition-layout.json",
            )
            extract_small(archive, "VENDOR/etc/fstab.uml", staging / "support/fstab.uml")
            extract_small(
                archive,
                "VENDOR/etc/vintf/manifest.xml",
                staging / "support/vintf-manifest.xml",
            )
            extract_small(
                archive,
                "VENDOR/etc/vintf/compatibility_matrix.xml",
                staging / "support/compatibility-matrix.xml",
            )

            disk_archive = staging / "guest/android-17-uml-arm64.img.zst"
            run([
                str(zstd),
                "-T0",
                "-19",
                "--check",
                "--no-progress",
                str(disk),
                "-o",
                str(disk_archive),
            ])

            resolved_manifest = run(
                ["repo", "manifest", "-r"], cwd=aosp_source, capture=True
            )
            provenance = staging / "provenance/aosp-repo-manifest.xml"
            provenance.parent.mkdir(parents=True, exist_ok=True)
            provenance.write_text(resolved_manifest + "\n", encoding="utf-8")
            orangefox_manifest = run(
                ["repo", "manifest", "-r"], cwd=orangefox_source, capture=True
            )
            orangefox_provenance = (
                staging / "provenance/orangefox-repo-manifest.xml"
            )
            orangefox_provenance.write_text(
                orangefox_manifest + "\n", encoding="utf-8"
            )

            kernel_release = kernel_release_file.read_text(
                encoding="utf-8", errors="strict"
            ).strip()
            projects_manifest_sha = sha256_path(android_root / "patches/projects.tsv")
            jelly_patch_count, jelly_manifest_sha = series_provenance(
                outer_root, android_root / "third_party/jelly/series"
            )
            jelly_final_commit = run(
                ["git", "rev-parse", "HEAD"],
                cwd=aosp_source / jelly_metadata["JELLY_DESTINATION"],
                capture=True,
            ).strip()
            aosp_routes = read_routes(android_root / "patches/projects.tsv", android_root)
            orangefox_routes = read_routes(
                android_root / "third_party/orangefox/patches/projects.tsv",
                android_root,
            )
            orangefox_projects_sha = sha256_path(
                android_root / "third_party/orangefox/patches/projects.tsv"
            )
            receipt_rows = [
                ("schema", 2),
                ("release_id", args.release_id),
                ("product_family", PRODUCT_FAMILY),
                ("producer", "android/aosp-17/build/publish-release.sh"),
                ("outer_revision", outer_revision),
                ("outer_dirty", "false"),
                ("aosp_manifest_commit", aosp_metadata["ANDROID17_MANIFEST_COMMIT"]),
                ("aosp_projects_manifest_sha256", projects_manifest_sha),
                ("jelly_upstream_commit", jelly_metadata["JELLY_UPSTREAM_COMMIT"]),
                ("jelly_final_tree", jelly_metadata["JELLY_FINAL_TREE"]),
                ("jelly_patch_manifest_sha256", jelly_manifest_sha),
                ("orangefox_manifest_commit", orangefox_metadata["ORANGEFOX_MANIFEST_COMMIT"]),
                ("orangefox_base_manifest_sha256", orangefox_metadata["ORANGEFOX_BASE_MANIFEST_SHA256"]),
                ("orangefox_projects_manifest_sha256", orangefox_projects_sha),
                ("orangefox_bootable_recovery_commit", orangefox_metadata["ORANGEFOX_BOOTABLE_RECOVERY_COMMIT"]),
                ("orangefox_vendor_recovery_commit", orangefox_metadata["ORANGEFOX_VENDOR_RECOVERY_COMMIT"]),
                ("orangefox_se_omapi_commit", orangefox_metadata["ORANGEFOX_SE_OMAPI_COMMIT"]),
                ("ack_final_tree", kernel_metadata["UML_KERNEL_FINAL_TREE"]),
                ("ack_patch_manifest_sha256", kernel_manifest_sha),
                ("kernel_release", kernel_release),
                ("page_size", 16384),
                ("abi_version", abi_metadata["DARWIN_UML_ABI_VERSION"]),
                ("abi_fingerprint", abi_metadata["DARWIN_UML_ABI_FINGERPRINT"]),
                ("android_product", aosp_metadata["ANDROID17_PRODUCT"]),
                ("android_build_id", build_id),
                ("source_role_aosp", "pinned AOSP 17 Repo workspace"),
                ("source_role_jelly", "pinned external LineageOS Jelly topic"),
                ("source_role_orangefox", "pinned OrangeFox 14.1 recovery workspace"),
                ("source_role_ack", "replayed ACK 6.18 worktree"),
                ("input_role_kernel", "verified standalone ACK UML build"),
                ("input_role_disk", "verified fixed-layout UBD image"),
            ]
            write_receipt(staging / "provenance/build-receipt.tsv", receipt_rows)

            manifest = {
                "schema_version": 3,
                "release_id": args.release_id,
                "directory_name": directory_name,
                "product_family": PRODUCT_FAMILY,
                "acceptance": "accepted",
                "published_at_utc": datetime.datetime.now(
                    datetime.timezone.utc
                ).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
                "android": {
                    "major": int(aosp_metadata["ANDROID17_MAJOR"]),
                    "sdk": sdk,
                    "build_id": build_id,
                    "build_variant": aosp_metadata["ANDROID17_BUILD_VARIANT"],
                    "fingerprint": fingerprint,
                    "vendor_api_level": vendor_api_level,
                    "product": aosp_metadata["ANDROID17_PRODUCT"],
                    "release_config": aosp_metadata["ANDROID17_RELEASE_CONFIG"],
                },
                "aosp": {
                    "manifest_repository": aosp_metadata["ANDROID17_MANIFEST_REPOSITORY"],
                    "manifest_tag": aosp_metadata["ANDROID17_MANIFEST_TAG"],
                    "manifest_commit": aosp_metadata["ANDROID17_MANIFEST_COMMIT"],
                    "manifest_tree": aosp_metadata["ANDROID17_MANIFEST_TREE"],
                    "base_manifest_sha256": aosp_metadata["ANDROID17_BASE_MANIFEST_SHA256"],
                    "projects_manifest_sha256": projects_manifest_sha,
                    "routes": aosp_routes,
                },
                "jelly": {
                    "repository": jelly_metadata["JELLY_UPSTREAM_REPOSITORY"],
                    "branch": jelly_metadata["JELLY_UPSTREAM_BRANCH"],
                    "base_commit": jelly_metadata["JELLY_UPSTREAM_COMMIT"],
                    "base_tree": jelly_metadata["JELLY_UPSTREAM_TREE"],
                    "final_commit": jelly_final_commit,
                    "final_tree": jelly_metadata["JELLY_FINAL_TREE"],
                    "patch_count": jelly_patch_count,
                    "patch_manifest_sha256": jelly_manifest_sha,
                    "destination": jelly_metadata["JELLY_DESTINATION"],
                    "license": jelly_metadata["JELLY_LICENSE"],
                },
                "orangefox": {
                    "sync_repository": orangefox_metadata["ORANGEFOX_SYNC_REPOSITORY"],
                    "sync_branch": orangefox_metadata["ORANGEFOX_SYNC_BRANCH"],
                    "sync_commit": orangefox_metadata["ORANGEFOX_SYNC_COMMIT"],
                    "sync_tree": orangefox_metadata["ORANGEFOX_SYNC_TREE"],
                    "sync_script_sha256": orangefox_metadata["ORANGEFOX_SYNC_SCRIPT_SHA256"],
                    "manifest_repository": orangefox_metadata["ORANGEFOX_MANIFEST_REPOSITORY"],
                    "manifest_branch": orangefox_metadata["ORANGEFOX_MANIFEST_BRANCH"],
                    "manifest_commit": orangefox_metadata["ORANGEFOX_MANIFEST_COMMIT"],
                    "manifest_tree": orangefox_metadata["ORANGEFOX_MANIFEST_TREE"],
                    "base_manifest_sha256": orangefox_metadata["ORANGEFOX_BASE_MANIFEST_SHA256"],
                    "branch": orangefox_metadata["ORANGEFOX_BRANCH"],
                    "bootable_recovery_repository": orangefox_metadata["ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY"],
                    "bootable_recovery_commit": orangefox_metadata["ORANGEFOX_BOOTABLE_RECOVERY_COMMIT"],
                    "bootable_recovery_tree": orangefox_metadata["ORANGEFOX_BOOTABLE_RECOVERY_TREE"],
                    "vendor_recovery_repository": orangefox_metadata["ORANGEFOX_VENDOR_RECOVERY_REPOSITORY"],
                    "vendor_recovery_commit": orangefox_metadata["ORANGEFOX_VENDOR_RECOVERY_COMMIT"],
                    "vendor_recovery_tree": orangefox_metadata["ORANGEFOX_VENDOR_RECOVERY_TREE"],
                    "se_omapi_repository": orangefox_metadata["ORANGEFOX_SE_OMAPI_REPOSITORY"],
                    "se_omapi_commit": orangefox_metadata["ORANGEFOX_SE_OMAPI_COMMIT"],
                    "se_omapi_tree": orangefox_metadata["ORANGEFOX_SE_OMAPI_TREE"],
                    "device_destination": orangefox_metadata["ORANGEFOX_DEVICE_OVERLAY_DESTINATION"],
                    "product": orangefox_metadata["ORANGEFOX_PRODUCT"],
                    "release_config": orangefox_metadata["ORANGEFOX_RELEASE_CONFIG"],
                    "patch_count": sum(route["patch_count"] for route in orangefox_routes),
                    "projects_manifest_sha256": orangefox_projects_sha,
                    "routes": orangefox_routes,
                    "license": orangefox_metadata["ORANGEFOX_LICENSE"],
                },
                "kernel": {
                    "family": kernel_metadata["UML_KERNEL_FAMILY"],
                    "repository": kernel_metadata["UML_KERNEL_UPSTREAM_REPOSITORY"],
                    "branch": kernel_metadata["UML_KERNEL_UPSTREAM_BRANCH"],
                    "tag": kernel_metadata["UML_KERNEL_UPSTREAM_TAG"],
                    "base_commit": kernel_metadata["UML_KERNEL_UPSTREAM_COMMIT"],
                    "base_tree": kernel_metadata["UML_KERNEL_UPSTREAM_TREE"],
                    "final_tree": kernel_metadata["UML_KERNEL_FINAL_TREE"],
                    "patch_count": kernel_patch_count,
                    "patch_manifest_sha256": kernel_manifest_sha,
                    "release": kernel_release,
                    "page_size": 16384,
                    "abi_version": int(abi_metadata["DARWIN_UML_ABI_VERSION"]),
                    "abi_fingerprint": abi_metadata["DARWIN_UML_ABI_FINGERPRINT"],
                    "elf_role": "darwin-uml",
                },
                "outer": {"revision": outer_revision, "dirty": False},
                "producer": {
                    "entry_point": "android/aosp-17/build/publish-release.sh",
                    "functional_options": [
                        "pinned AOSP source",
                        "pinned external Jelly source",
                        "pinned OrangeFox 14.1 source and local topics",
                        "clean replayed ACK source",
                        "verified standalone ACK build",
                        "verified AOSP target files",
                        "verified partition-backed UML initramfs",
                        "verified kernel-less OrangeFox recovery partition",
                        "verified fixed-layout UBD disk",
                        "explicit Darwin SDK",
                        "explicit pinned Clang",
                        "explicit pinned linker",
                        "explicit AOSP VINTF checker",
                        "explicit AOSP zstd",
                    ],
                    "builder": {
                        "os": platform.platform(),
                        "architecture": platform.machine(),
                        "jdk": first_version([str(java), "-version"]),
                        "clang": first_version([str(clang), "--version"]),
                        "linker": first_version([str(linker), "--version"]),
                        "darwin_sdk": sdk_identity(darwin_sdk),
                        "android_build_tools": (
                            f"AOSP {aosp_metadata['ANDROID17_MANIFEST_TAG']} target-files tools"
                        ),
                    },
                },
                "inventory": [],
            }
            for relative, (role, file_format) in sorted(PAYLOAD_SPECS.items()):
                payload = staging / relative
                regular_file(payload, f"published payload {relative}")
                manifest["inventory"].append({
                    "path": relative,
                    "role": role,
                    "format": file_format,
                    "size": payload.stat().st_size,
                })
            (staging / "release-manifest.json").write_text(
                json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            verify_release(staging, android_root, zstd=zstd)
            os.replace(staging, destination)
            temporary_parent.rmdir()
        except BaseException:
            shutil.rmtree(temporary_parent, ignore_errors=True)
            raise

    print(f"published Android release: {destination}")


if __name__ == "__main__":
    main()
