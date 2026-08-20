#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def run(*command: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"AOSP patch-regenerator fixture command failed: {' '.join(command)}\n"
            f"{result.stderr}"
        )
    return result.stdout.strip()


def write_fixture(android_root: Path, topic: Path, verifier: Path) -> None:
    run("git", "init", "-q", str(topic))
    run("git", "config", "user.name", "Android fixture", cwd=topic)
    run("git", "config", "user.email", "android-fixture@example.invalid", cwd=topic)
    (topic / "base.txt").write_text("base\n")
    run("git", "add", "base.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "fixture base", cwd=topic)
    base_commit = run("git", "rev-parse", "HEAD", cwd=topic)
    base_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
    (topic / "first.txt").write_text("first\n")
    run("git", "add", "first.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "UML-ANDROID-LOCAL: fixture: first", cwd=topic)
    (topic / "second.txt").write_text("second\n")
    run("git", "add", "second.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "UML-ANDROID-LOCAL: fixture: second", cwd=topic)

    (android_root / "metadata").mkdir(parents=True)
    route_root = android_root / "patches/projects/p0001"
    (route_root / "series").mkdir(parents=True)
    (android_root / "tools").mkdir()
    shutil.copy2(verifier, android_root / "tools/verify-aosp-patches.py")
    os.chmod(android_root / "tools/verify-aosp-patches.py", 0o755)
    manifest = (
        '<manifest><project name="fixture/project" path="fixture/project" '
        f'revision="{base_commit}" upstream="refs/tags/fixture"/></manifest>\n'
    )
    (android_root / "metadata/base-manifest.xml").write_text(manifest)
    manifest_digest = hashlib.sha256(manifest.encode()).hexdigest()
    (android_root / "metadata/aosp.env").write_text(
        "AOSP_SOURCE_SCHEMA=1\n"
        f"ANDROID17_BASE_MANIFEST_SHA256='{manifest_digest}'\n"
    )
    empty_digest = "0" * 64
    empty_tree = "0" * 40
    (android_root / "patches/projects.tsv").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "order\troute_id\tmanifest_project\tcheckout_path\tmanifest_revision\t"
        "base_commit\tbase_tree\tfinal_tree\tpatch_count\t"
        "series_manifest_sha256\tseries_dir\n"
        f"1\tp0001\tfixture/project\tfixture/project\trefs/tags/fixture\t"
        f"{base_commit}\t{base_tree}\t{empty_tree}\t1\t{empty_digest}\t"
        "patches/projects/p0001/series\n"
    )
    (route_root / "metadata.env").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "AOSP_PROJECT_PATCH_SCHEMA=1\n"
        "AOSP_ROUTE_ID='p0001'\n"
        "AOSP_MANIFEST_PROJECT='fixture/project'\n"
        "AOSP_CHECKOUT_PATH='fixture/project'\n"
        "AOSP_MANIFEST_REVISION='refs/tags/fixture'\n"
        f"AOSP_BASE_COMMIT='{base_commit}'\n"
        f"AOSP_BASE_TREE='{base_tree}'\n"
        f"AOSP_FINAL_TREE='{empty_tree}'\n"
        "AOSP_PATCH_COUNT=1\n"
        f"AOSP_SERIES_MANIFEST_SHA256='{empty_digest}'\n"
        "AOSP_SERIES_DIR='patches/projects/p0001/series'\n"
    )
    (route_root / "series/manifest.tsv").write_text("placeholder\n")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    generator = source_root / "android/aosp-17/tools/regenerate-aosp-patches.py"
    verifier = source_root / "android/aosp-17/tools/verify-aosp-patches.py"

    with tempfile.TemporaryDirectory(prefix="aosp-patch-regenerator-") as temporary:
        root = Path(temporary)
        android_root = root / "android/aosp-17"
        topic = root / "topic"
        write_fixture(android_root, topic, verifier)
        command = (
            sys.executable,
            "-B",
            str(generator),
            "--android-root",
            str(android_root),
            "--route-id",
            "p0001",
            "--topic-checkout",
            str(topic),
        )
        run(*command)
        run(
            sys.executable,
            "-B",
            str(android_root / "tools/verify-aosp-patches.py"),
            "--android-root",
            str(android_root),
        )
        patches = sorted((android_root / "patches/projects/p0001/series").glob("*.patch"))
        if len(patches) != 2:
            raise SystemExit("AOSP patch-regenerator fixture did not emit two patches")
        manifest = (android_root / "patches/projects/p0001/series/manifest.tsv").read_text()
        if "UML-ANDROID-LOCAL: fixture: first" not in manifest:
            raise SystemExit("AOSP patch-regenerator fixture lost the first subject")
        if "UML-ANDROID-LOCAL: fixture: second" not in manifest:
            raise SystemExit("AOSP patch-regenerator fixture lost the second subject")

        projects_before = (android_root / "patches/projects.tsv").read_bytes()
        metadata_before = (android_root / "patches/projects/p0001/metadata.env").read_bytes()
        (topic / "dirty.txt").write_text("dirty\n")
        rejected = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if rejected.returncode == 0:
            raise SystemExit("AOSP patch regenerator accepted a dirty topic checkout")
        if (android_root / "patches/projects.tsv").read_bytes() != projects_before:
            raise SystemExit("dirty-topic rejection changed projects.tsv")
        if (android_root / "patches/projects/p0001/metadata.env").read_bytes() != metadata_before:
            raise SystemExit("dirty-topic rejection changed route metadata")

    print("AOSP 17 patch-regenerator fixture passed")


if __name__ == "__main__":
    main()
