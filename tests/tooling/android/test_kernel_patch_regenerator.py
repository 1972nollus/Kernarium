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


def run(*command: str, cwd: Path | None = None, payload: bytes | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"ACK patch-regenerator fixture command failed: {' '.join(command)}\n"
            f"{result.stderr.decode(errors='replace')}"
        )
    return result.stdout.decode(errors="strict").strip()


def patch_identity(path: Path) -> tuple[str, str]:
    payload = path.read_bytes()
    patch_ids = [
        line.split()[0]
        for line in run("git", "patch-id", "--stable", payload=payload).splitlines()
        if line.strip()
    ]
    if len(patch_ids) != 1:
        raise SystemExit("ACK patch-regenerator fixture has an invalid seed patch")
    return patch_ids[0], hashlib.sha256(payload).hexdigest()


def write_fixture(root: Path, topic: Path, verifier: Path) -> tuple[Path, str]:
    run("git", "init", "-q", str(topic))
    run("git", "config", "user.name", "ACK fixture", cwd=topic)
    run("git", "config", "user.email", "ack-fixture@example.invalid", cwd=topic)
    (topic / "base.txt").write_text("base\n")
    run("git", "add", "base.txt", cwd=topic)
    run("git", "commit", "-q", "-m", "fixture ACK base", cwd=topic)
    base_commit = run("git", "rev-parse", "HEAD", cwd=topic)
    base_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)

    first_path = topic / "arch/um/os-Darwin/smp.c"
    first_path.parent.mkdir(parents=True)
    first_path.write_text("void first_checkpoint(void) {}\n")
    run("git", "add", "arch/um/os-Darwin/smp.c", cwd=topic)
    first_subject = "UML-DARWIN-LOCAL: fixture: first"
    run("git", "commit", "-q", "-m", first_subject, cwd=topic)
    first_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)

    android_root = root / "android/aosp-17"
    android_root.mkdir(parents=True)
    line_root = root / "kernel/android-6.18"
    series = line_root / "patches/series"
    series.mkdir(parents=True)
    common = root / "kernel/common/tooling"
    common.mkdir(parents=True)
    shutil.copy2(verifier, common / "verify-series-policy.py")
    os.chmod(common / "verify-series-policy.py", 0o755)

    emitted = run(
        "git",
        "format-patch",
        "--numbered",
        "--full-index",
        "--binary",
        "--no-signature",
        "--output-directory",
        str(series),
        f"{base_commit}..HEAD",
        cwd=topic,
    )
    patch = Path(emitted)
    patch_id, digest = patch_identity(patch)
    (series / "manifest.tsv").write_text(
        "# SPDX-License-Identifier: GPL-2.0-or-later\n"
        "# Ordered final-state topic identity manifest. Fields are order, file, stable\n"
        "# patch-id, SHA-256 of the git-generated patch, and commit subject.\n"
        "order\tfilename\tstable_patch_id\tsha256\tsubject\n"
        f"1\t{patch.name}\t{patch_id}\t{digest}\t{first_subject}\n"
    )
    (line_root / "patches/topic-path-overlaps.tsv").write_text(
        "# SPDX-License-Identifier: GPL-2.0-or-later\n"
        "path\ttopic_orders\trationale\n"
    )
    (line_root / "patches/metadata.env").write_text(
        "# SPDX-License-Identifier: GPL-2.0-or-later\n"
        "UML_KERNEL_SERIES_SCHEMA=3\n"
        "UML_KERNEL_FAMILY='android-6.18'\n"
        f"UML_KERNEL_UPSTREAM_COMMIT='{base_commit}'\n"
        f"UML_KERNEL_UPSTREAM_TREE='{base_tree}'\n"
        f"UML_KERNEL_FINAL_TREE='{first_tree}'\n"
        "UML_KERNEL_PATCH_COUNT=1\n"
    )

    second_path = topic / "arch/um/kernel/time.c"
    second_path.parent.mkdir(parents=True)
    second_path.write_text("void second_checkpoint(void) {}\n")
    run("git", "add", "arch/um/kernel/time.c", cwd=topic)
    run(
        "git",
        "commit",
        "-q",
        "-m",
        "UML-DARWIN-LOCAL: fixture: second",
        cwd=topic,
    )
    return line_root, base_commit


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    generator = source_root / "android/aosp-17/tools/regenerate-kernel-patches.py"
    verifier = source_root / "kernel/common/tooling/verify-series-policy.py"

    with tempfile.TemporaryDirectory(prefix="ack-patch-regenerator-") as temporary:
        root = Path(temporary)
        topic = root / "topic"
        line_root, base_commit = write_fixture(root, topic, verifier)
        android_root = root / "android/aosp-17"
        command = (
            sys.executable,
            "-B",
            str(generator),
            "--android-root",
            str(android_root),
            "--line-root",
            str(line_root),
            "--topic-checkout",
            str(topic),
        )
        run(*command)
        run(
            sys.executable,
            "-B",
            str(line_root.parent / "common/tooling/verify-series-policy.py"),
            "--line-root",
            str(line_root),
            "manifest",
        )
        run(
            sys.executable,
            "-B",
            str(line_root.parent / "common/tooling/verify-series-policy.py"),
            "--line-root",
            str(line_root),
            "--source",
            str(topic),
            "--base",
            base_commit,
            "topology",
        )
        patches = sorted((line_root / "patches/series").glob("*.patch"))
        if len(patches) != 2:
            raise SystemExit("ACK patch-regenerator fixture did not emit two patches")
        manifest = (line_root / "patches/series/manifest.tsv").read_text()
        for subject in (
            "UML-DARWIN-LOCAL: fixture: first",
            "UML-DARWIN-LOCAL: fixture: second",
        ):
            if subject not in manifest:
                raise SystemExit(f"ACK patch-regenerator fixture lost subject: {subject}")
        metadata = (line_root / "patches/metadata.env").read_text()
        final_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
        if f"UML_KERNEL_FINAL_TREE='{final_tree}'" not in metadata:
            raise SystemExit("ACK patch regenerator did not update the final tree")
        if "UML_KERNEL_PATCH_COUNT=2" not in metadata:
            raise SystemExit("ACK patch regenerator did not update the patch count")

        metadata_before = (line_root / "patches/metadata.env").read_bytes()
        rejected = subprocess.run(
            (*command[:6], str(root / "kernel/mainline-6.18"), *command[7:]),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if rejected.returncode == 0:
            raise SystemExit("ACK patch regenerator accepted the mainline line root")
        if (line_root / "patches/metadata.env").read_bytes() != metadata_before:
            raise SystemExit("wrong-line rejection changed ACK metadata")

        (topic / "dirty.txt").write_text("dirty\n")
        rejected = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if rejected.returncode == 0:
            raise SystemExit("ACK patch regenerator accepted a dirty topic checkout")
        if (line_root / "patches/metadata.env").read_bytes() != metadata_before:
            raise SystemExit("dirty-topic rejection changed ACK metadata")

    print("Android 17 ACK patch-regenerator fixture passed")


if __name__ == "__main__":
    main()
