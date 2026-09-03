#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run_git(source: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(source), *arguments],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        fail(f"Git failed in the kernel source: {result.stderr.strip()}")
    return result.stdout.strip()


def fail(message: str) -> None:
    raise SystemExit(f"mainline kernel artifact publisher: {message}")


def absolute_directory(path: Path, label: str) -> Path:
    if not path.is_absolute():
        fail(f"{label} must be an absolute local path")
    resolved = path.resolve()
    if not resolved.is_dir():
        fail(f"{label} is not a directory: {resolved}")
    return resolved


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--page-size", choices=("4k", "16k"), required=True)
    parser.add_argument("--slice", choices=("arm64", "x86_64"), default="arm64")
    parser.add_argument(
        "--profile", choices=("standard", "wine-arm64ec"), default="standard"
    )
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--release-id", required=True)
    args = parser.parse_args()

    line_root = Path(__file__).resolve().parents[1]
    outer_root = line_root.parents[1]
    common_root = outer_root / "kernel/common"
    sys.path.insert(0, str(common_root / "artifacts"))
    from kernel_artifact import (  # noqa: PLC0415
        PAYLOAD_SPECS,
        RELEASE_ID_RE,
        SCHEMA_VERSION,
        parse_env,
        patch_provenance_count,
        sha256_path,
        verify_artifact,
        verify_config,
        verify_config_fragment,
        verify_elf,
    )

    source = absolute_directory(args.source, "kernel source")
    build = absolute_directory(args.build, "kernel build")
    if not args.output_root.is_absolute():
        fail("output root must be an absolute local path")
    output_root = args.output_root.resolve()
    if RELEASE_ID_RE.fullmatch(args.release_id) is None:
        fail("release ID has an unsafe format")
    if args.slice == "x86_64" and args.page_size != "4k":
        fail("the x86_64 userspace slice supports only 4 KiB pages")
    if args.profile == "wine-arm64ec" and (
        args.slice != "arm64" or args.page_size != "4k"
    ):
        fail("the wine-arm64ec profile requires the arm64 4 KiB slice")

    if args.slice == "x86_64":
        slice_root = line_root / "slices/x86_64"
        metadata_path = slice_root / "patches/metadata.env"
        patch_manifest_path = slice_root / "patches/composition.tsv"
    else:
        metadata_path = line_root / "patches/metadata.env"
        patch_manifest_path = line_root / "patches/series/manifest.tsv"
    abi_metadata_path = common_root / "abi/identity.env"
    metadata = parse_env(metadata_path)
    if args.slice == "x86_64":
        common_metadata = parse_env(line_root / "patches/metadata.env")
        if metadata.get("UML_KERNEL_SLICE") != "x86_64":
            fail("x86_64 patch metadata has the wrong slice identity")
        if (
            metadata.get("UML_KERNEL_PARENT_TREE")
            != common_metadata["UML_KERNEL_FINAL_TREE"]
        ):
            fail("x86_64 patch parent tree differs from the common final tree")
        inherited_keys = (
            "UML_KERNEL_RELEASE_VERSION",
            "UML_KERNEL_UPSTREAM_REPOSITORY",
            "UML_KERNEL_UPSTREAM_BRANCH",
            "UML_KERNEL_UPSTREAM_COMMIT",
            "UML_KERNEL_UPSTREAM_TREE",
            "UML_KERNEL_BASE_REF",
            "UML_KERNEL_ABI_VERSION",
            "UML_KERNEL_ABI_FINGERPRINT",
        )
        mismatched = [
            key
            for key in inherited_keys
            if metadata.get(key) != common_metadata.get(key)
        ]
        if mismatched:
            fail(
                "x86_64 patch metadata differs from common identity: "
                + ", ".join(mismatched)
            )
    abi = parse_env(abi_metadata_path)
    if run_git(source, "status", "--porcelain=v1", "--untracked-files=all"):
        fail("kernel source worktree is dirty")
    if (
        run_git(source, "rev-parse", "HEAD^{tree}")
        != metadata["UML_KERNEL_FINAL_TREE"]
    ):
        fail("kernel source tree differs from the tracked final tree")

    page_size = 16384 if args.page_size == "16k" else 4096
    source_paths = {
        "kernel/System.map": build / "System.map",
        "kernel/config": build / ".config",
        "kernel/kernel.release": build / "include/config/kernel.release",
        "kernel/modules.builtin": build / "modules.builtin",
        "kernel/modules.builtin.modinfo": build / "modules.builtin.modinfo",
        "kernel/vmlinux": build / "vmlinux",
    }
    for relative, path in source_paths.items():
        if not path.is_file() or path.is_symlink():
            fail(f"build payload is missing or unsafe for {relative}: {path}")
    for root, directories, files in os.walk(build, followlinks=False):
        directories[:] = [
            name for name in directories if not (Path(root) / name).is_symlink()
        ]
        for name in files:
            if name.endswith(".ko") or ".ko." in name:
                fail(f"loadable module output is forbidden: {Path(root) / name}")

    verify_config(source_paths["kernel/config"], page_size, args.slice)
    if args.profile == "wine-arm64ec":
        verify_config_fragment(
            source_paths["kernel/config"],
            line_root / "profiles/wine-arm64ec/config.fragment",
        )
    verify_elf(
        source_paths["kernel/vmlinux"], page_size, common_root, args.slice
    )
    release = source_paths["kernel/kernel.release"].read_text(
        encoding="utf-8"
    ).strip()
    if not release.startswith(metadata["UML_KERNEL_RELEASE_VERSION"]):
        fail("built kernel release does not match tracked release version")

    family = "mainline-6.18"
    directory_name = f"{family}-{args.slice}-{args.page_size}-{args.release_id}"
    patch_count = int(
        metadata.get(
            "UML_KERNEL_ARTIFACT_PATCH_COUNT",
            metadata["UML_KERNEL_PATCH_COUNT"],
        )
    )
    expected_composition = None
    if args.slice == "x86_64":
        expected_composition = (
            (
                "common",
                "kernel/mainline-6.18/patches/series/manifest.tsv",
            ),
            (
                "x86_64",
                "kernel/mainline-6.18/slices/x86_64/patches/series/manifest.tsv",
            ),
        )
    if (
        patch_provenance_count(
            patch_manifest_path,
            outer_root,
            expected_composition,
        )
        != patch_count
    ):
        fail("tracked patch provenance count differs from kernel metadata")
    output_root.mkdir(parents=True, exist_ok=True)
    if output_root.is_symlink():
        fail("output root must not be a symlink")
    destination = output_root / directory_name
    if destination.exists() or destination.is_symlink():
        fail("kernel artifact destination already exists")
    staging_parent = Path(
        tempfile.mkdtemp(prefix=f".{directory_name}.", dir=output_root)
    )
    staging = staging_parent / directory_name
    staging.mkdir()
    try:
        inventory = []
        for relative, (role, file_format) in sorted(PAYLOAD_SPECS.items()):
            destination_path = staging / relative
            destination_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_paths[relative], destination_path)
            shutil.copymode(source_paths[relative], destination_path)
            inventory.append(
                {
                    "path": relative,
                    "role": role,
                    "format": file_format,
                    "size": destination_path.stat().st_size,
                }
            )
        manifest = {
            "schema_version": SCHEMA_VERSION,
            "release_id": args.release_id,
            "directory_name": directory_name,
            "family": family,
            "slice": args.slice,
            "published_at_utc": datetime.datetime.now(
                datetime.timezone.utc
            ).isoformat(timespec="seconds").replace("+00:00", "Z"),
            "release": release,
            "page_size": page_size,
            "elf_role": "darwin-uml",
            "source": {
                "repository": metadata["UML_KERNEL_UPSTREAM_REPOSITORY"],
                "branch": metadata["UML_KERNEL_UPSTREAM_BRANCH"],
                "base_commit": metadata["UML_KERNEL_UPSTREAM_COMMIT"],
                "base_tree": metadata["UML_KERNEL_UPSTREAM_TREE"],
                "final_tree": metadata["UML_KERNEL_FINAL_TREE"],
                "patch_count": patch_count,
                "patch_manifest_sha256": sha256_path(patch_manifest_path),
            },
            "abi": {
                "version": int(abi["DARWIN_UML_ABI_VERSION"]),
                "fingerprint": abi["DARWIN_UML_ABI_FINGERPRINT"],
            },
            "inventory": inventory,
        }
        (staging / "kernel-artifact.json").write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        verify_artifact(
            staging,
            common_root,
            metadata_path,
            patch_manifest_path,
            abi_metadata_path,
        )
        os.replace(staging, destination)
        staging_parent.rmdir()
    except BaseException:
        shutil.rmtree(staging_parent, ignore_errors=True)
        raise
    print(f"published {args.slice} mainline kernel artifact: {destination}")


if __name__ == "__main__":
    main()
