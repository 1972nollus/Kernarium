#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path


HEX40 = re.compile(r"[0-9a-f]{40}")


class RegenerationError(ValueError):
    pass


def run(*command: str, cwd: Path | None = None, payload: bytes | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip()
        raise RegenerationError(f"command failed: {' '.join(command)}: {detail}")
    return result.stdout.decode(errors="strict")


def metadata_values(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    assignment = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([^#]+))$")
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = assignment.fullmatch(line)
        if match is None:
            raise RegenerationError(f"{path}:{number}: malformed metadata assignment")
        key, quoted, unquoted = match.groups()
        if key in values:
            raise RegenerationError(f"{path}:{number}: duplicate metadata key {key}")
        values[key] = quoted if quoted is not None else unquoted.strip()
    return values


def replace_assignment(text: str, key: str, value: str) -> str:
    pattern = re.compile(rf"^{re.escape(key)}=(?:'[^']*'|[^\n#]+)$", re.MULTILINE)
    updated, count = pattern.subn(f"{key}={value}", text)
    if count != 1:
        raise RegenerationError(f"kernel metadata does not define {key} exactly once")
    return updated


def manifest_subjects(path: Path) -> list[str]:
    subjects: list[str] = []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#") or line.startswith("order\t"):
            continue
        fields = line.split("\t", 4)
        if len(fields) != 5:
            raise RegenerationError("existing kernel manifest is malformed")
        subjects.append(fields[4])
    return subjects


def verify_topic(
    topic: Path, base_commit: str, base_tree: str, existing_subjects: list[str]
) -> list[str]:
    if topic.is_symlink() or not topic.is_dir():
        raise RegenerationError("topic checkout must be a non-symlink directory")
    top = Path(run("git", "rev-parse", "--show-toplevel", cwd=topic).strip()).resolve()
    if top != topic:
        raise RegenerationError("topic path is not the root of its Git checkout")
    if run("git", "status", "--porcelain", cwd=topic).strip():
        raise RegenerationError("topic checkout has uncommitted changes")
    if run("git", "rev-parse", f"{base_commit}^{{tree}}", cwd=topic).strip() != base_tree:
        raise RegenerationError("topic base commit has the wrong tree")
    ancestor = subprocess.run(
        ("git", "merge-base", "--is-ancestor", base_commit, "HEAD"), cwd=topic
    )
    if ancestor.returncode != 0:
        raise RegenerationError("topic is not based on the pinned ACK commit")
    history = run(
        "git", "rev-list", "--reverse", "--parents", f"{base_commit}..HEAD", cwd=topic
    ).splitlines()
    if not history:
        raise RegenerationError("topic has no commits after the pinned ACK base")
    commits: list[str] = []
    expected_parent = base_commit
    for index, line in enumerate(history):
        fields = line.split()
        if len(fields) != 2 or fields[1] != expected_parent:
            raise RegenerationError("topic history is not clean and linear")
        commit = fields[0]
        subject = run("git", "show", "-s", "--format=%s", commit, cwd=topic).strip()
        inherited_subject = (
            index < len(existing_subjects) and subject == existing_subjects[index]
        )
        if not inherited_subject and not subject.startswith("UML-DARWIN-LOCAL:"):
            raise RegenerationError(
                f"ACK topic lacks its inherited subject or local prefix: {subject}"
            )
        commits.append(commit)
        expected_parent = commit
    return commits


def generate_series(topic: Path, base_commit: str, output: Path) -> list[tuple[str, str, str, str]]:
    emitted = run(
        "git",
        "format-patch",
        "--numbered",
        "--full-index",
        "--binary",
        "--no-signature",
        "--output-directory",
        str(output),
        f"{base_commit}..HEAD",
        cwd=topic,
    )
    patches = [Path(line) for line in emitted.splitlines() if line.strip()]
    entries: list[tuple[str, str, str, str]] = []
    for patch in patches:
        if patch.parent.resolve() != output.resolve() or not patch.is_file():
            raise RegenerationError("git format-patch emitted an unexpected path")
        payload = patch.read_bytes()
        patch_ids = [
            line.split()[0]
            for line in run("git", "patch-id", "--stable", payload=payload).splitlines()
            if line.strip()
        ]
        if len(patch_ids) != 1 or HEX40.fullmatch(patch_ids[0]) is None:
            raise RegenerationError(f"cannot derive stable patch ID: {patch.name}")
        mail = run("git", "mailinfo", "/dev/null", "/dev/null", payload=payload)
        subjects = [
            line.removeprefix("Subject: ")
            for line in mail.splitlines()
            if line.startswith("Subject: ")
        ]
        if len(subjects) != 1:
            raise RegenerationError(f"cannot derive patch subject: {patch.name}")
        entries.append(
            (
                patch.name,
                patch_ids[0],
                hashlib.sha256(payload).hexdigest(),
                subjects[0],
            )
        )
    return entries


def render_manifest(entries: list[tuple[str, str, str, str]]) -> bytes:
    lines = [
        "# SPDX-License-Identifier: GPL-2.0-or-later",
        "# Ordered final-state topic identity manifest. Fields are order, file, stable",
        "# patch-id, SHA-256 of the git-generated patch, and commit subject.",
        "order\tfilename\tstable_patch_id\tsha256\tsubject",
    ]
    lines.extend(
        f"{order}\t{filename}\t{patch_id}\t{digest}\t{subject}"
        for order, (filename, patch_id, digest, subject) in enumerate(entries, 1)
    )
    return ("\n".join(lines) + "\n").encode()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Regenerate the Android 17 ACK kernel mail patch series"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--line-root", type=Path, required=True)
    parser.add_argument("--topic-checkout", type=Path, required=True)
    args = parser.parse_args()

    try:
        android_root = args.android_root.resolve()
        outer_root = android_root.parent.parent
        expected_android_root = outer_root / "android/aosp-17"
        if android_root != expected_android_root:
            raise RegenerationError("Android root must be the aosp-17 owner")
        line_root = args.line_root.resolve()
        expected_line_root = outer_root / "kernel/android-6.18"
        if line_root != expected_line_root:
            raise RegenerationError("line root must be kernel/android-6.18 in the same tree")

        metadata_path = line_root / "patches/metadata.env"
        series_dir = line_root / "patches/series"
        metadata_before = metadata_path.read_bytes()
        metadata = metadata_values(metadata_path)
        if metadata.get("UML_KERNEL_FAMILY") != "android-6.18":
            raise RegenerationError("kernel metadata selects the wrong family")
        base_commit = metadata.get("UML_KERNEL_UPSTREAM_COMMIT", "")
        base_tree = metadata.get("UML_KERNEL_UPSTREAM_TREE", "")
        if HEX40.fullmatch(base_commit) is None or HEX40.fullmatch(base_tree) is None:
            raise RegenerationError("pinned ACK base identity is malformed")

        existing_subjects = manifest_subjects(series_dir / "manifest.tsv")
        topic = args.topic_checkout.resolve()
        commits = verify_topic(topic, base_commit, base_tree, existing_subjects)
        series_backup = line_root / "patches/.series.regenerate-backup"
        if series_backup.exists():
            raise RegenerationError(f"stale regeneration backup exists: {series_backup}")

        with tempfile.TemporaryDirectory(
            prefix=".series.regenerate-", dir=line_root / "patches"
        ) as temporary_name:
            generated = Path(temporary_name) / "series"
            generated.mkdir()
            entries = generate_series(topic, base_commit, generated)
            if len(entries) != len(commits):
                raise RegenerationError("generated patch count differs from topic history")
            (generated / "manifest.tsv").write_bytes(render_manifest(entries))
            final_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic).strip()
            if HEX40.fullmatch(final_tree) is None:
                raise RegenerationError("topic final tree is malformed")

            updated_metadata = metadata_before.decode(errors="strict")
            updated_metadata = replace_assignment(
                updated_metadata, "UML_KERNEL_FINAL_TREE", f"'{final_tree}'"
            )
            updated_metadata = replace_assignment(
                updated_metadata, "UML_KERNEL_PATCH_COUNT", str(len(entries))
            )
            metadata_temp = metadata_path.with_name(".metadata.env.regenerate")
            metadata_temp.write_text(updated_metadata)
            try:
                series_dir.rename(series_backup)
                generated.rename(series_dir)
                os.replace(metadata_temp, metadata_path)
                verifier = outer_root / "kernel/common/tooling/verify-series-policy.py"
                run(str(verifier), "--line-root", str(line_root), "manifest")
                run(
                    str(verifier),
                    "--line-root",
                    str(line_root),
                    "--source",
                    str(topic),
                    "--base",
                    base_commit,
                    "--tip",
                    "HEAD",
                    "topology",
                )
            except BaseException:
                metadata_path.write_bytes(metadata_before)
                if series_dir.exists():
                    shutil.rmtree(series_dir)
                if series_backup.exists():
                    series_backup.rename(series_dir)
                metadata_temp.unlink(missing_ok=True)
                raise
            shutil.rmtree(series_backup)

        print(
            f"regenerated android-6.18: {len(commits)} patches, final tree {final_tree}"
        )
    except (OSError, UnicodeError, RegenerationError) as error:
        raise SystemExit(f"ACK patch regenerator: {error}") from error


if __name__ == "__main__":
    main()
