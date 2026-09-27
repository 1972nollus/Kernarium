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

from source_fixture import run


def write_fixture(
    root: Path, android_root: Path, topic: Path, verifier: Path, scripts: Path
) -> None:
    run("git", "init", "-q", str(topic))
    (topic / "base.txt").write_text("base\n")
    run("git", "add", "base.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "fixture base", cwd=topic)
    base_commit = run("git", "rev-parse", "HEAD", cwd=topic)
    base_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
    (topic / "first.txt").write_text("first\n")
    run("git", "add", "first.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "fixture: first", cwd=topic)
    (topic / "second.txt").write_text("second\n")
    run("git", "add", "second.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "fixture: second", cwd=topic)

    (android_root / "metadata").mkdir(parents=True)
    route_root = android_root / "patches/projects/p0001"
    (route_root / "series").mkdir(parents=True)
    (android_root / "tools").mkdir()
    shutil.copy2(verifier, android_root / "tools/verify-aosp-patches.py")
    os.chmod(android_root / "tools/verify-aosp-patches.py", 0o755)
    artifacts = root / "kernel/common/artifacts"
    artifacts.mkdir(parents=True)
    shutil.copy2(
        verifier.parents[3] / "kernel/common/artifacts/contract_checks.py",
        artifacts / "contract_checks.py",
    )
    (root / "scripts").mkdir()
    shutil.copy2(scripts / "patch_series.py", root / "scripts/patch_series.py")
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
    empty_tree = "0" * 40
    (android_root / "patches/projects.tsv").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "order\troute_id\tmanifest_project\tcheckout_path\tmanifest_revision\t"
        "base_commit\tbase_tree\tfinal_tree\tseries_dir\n"
        f"1\tp0001\tfixture/project\tfixture/project\trefs/tags/fixture\t"
        f"{base_commit}\t{base_tree}\t{empty_tree}\t"
        "patches/projects/p0001/series\n"
    )


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
        write_fixture(root, android_root, topic, verifier, source_root / "scripts")
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
        listed = (android_root / "patches/projects/p0001/series/series.tsv").read_text()
        for name in ("fixture-first", "fixture-second"):
            if name not in listed:
                raise SystemExit(f"AOSP patch-regenerator fixture lost patch: {name}")
        final_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
        if f"\t{final_tree}\t" not in (android_root / "patches/projects.tsv").read_text():
            raise SystemExit("AOSP patch regenerator did not record the topic tree")

        projects_before = (android_root / "patches/projects.tsv").read_bytes()
        series_before = (
            android_root / "patches/projects/p0001/series/series.tsv"
        ).read_bytes()
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
        if (
            android_root / "patches/projects/p0001/series/series.tsv"
        ).read_bytes() != series_before:
            raise SystemExit("dirty-topic rejection changed the route series")

    print("AOSP 17 patch-regenerator fixture passed")


if __name__ == "__main__":
    main()
