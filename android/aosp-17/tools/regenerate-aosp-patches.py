#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

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


PROJECT_HEADER = (
    "order",
    "route_id",
    "manifest_project",
    "checkout_path",
    "manifest_revision",
    "base_commit",
    "base_tree",
    "final_tree",
    "patch_count",
    "series_manifest_sha256",
    "series_dir",
)
HEX40 = re.compile(r"[0-9a-f]{40}")
SAFE_ROUTE = re.compile(r"p[0-9]{4}")


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


def read_project_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    lines = path.read_text().splitlines()
    header_index = next(
        (index for index, line in enumerate(lines) if line and not line.startswith("#")),
        None,
    )
    if header_index is None or tuple(lines[header_index].split("\t")) != PROJECT_HEADER:
        raise RegenerationError("projects.tsv has the wrong header")
    rows: list[list[str]] = []
    for line in lines[header_index + 1 :]:
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != len(PROJECT_HEADER):
            raise RegenerationError("projects.tsv contains a malformed route")
        rows.append(fields)
    return lines[: header_index + 1], rows


def replace_assignment(text: str, key: str, value: str) -> str:
    pattern = re.compile(rf"^{re.escape(key)}=(?:'[^']*'|[0-9]+)$", re.MULTILINE)
    replacement = f"{key}={value}"
    updated, count = pattern.subn(replacement, text)
    if count != 1:
        raise RegenerationError(f"route metadata does not define {key} exactly once")
    return updated


def safe_series_path(android_root: Path, value: str, route_id: str) -> Path:
    relative = Path(value)
    if relative.is_absolute() or ".." in relative.parts:
        raise RegenerationError("route series path is unsafe")
    expected = Path("patches/projects") / route_id / "series"
    if relative != expected:
        raise RegenerationError("route series path does not match its route ID")
    result = (android_root / relative).resolve()
    try:
        result.relative_to(android_root)
    except ValueError as error:
        raise RegenerationError("route series path escapes the Android root") from error
    return result


def verify_topic(topic: Path, base_commit: str, base_tree: str) -> list[str]:
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
        raise RegenerationError("topic is not based on the routed base commit")
    history = run(
        "git", "rev-list", "--reverse", "--parents", f"{base_commit}..HEAD", cwd=topic
    ).splitlines()
    if not history:
        raise RegenerationError("topic has no commits after the routed base")
    commits: list[str] = []
    expected_parent = base_commit
    for line in history:
        fields = line.split()
        if len(fields) != 2 or fields[1] != expected_parent:
            raise RegenerationError("topic history is not clean and linear")
        commit = fields[0]
        subject = run("git", "show", "-s", "--format=%s", commit, cwd=topic).strip()
        if not subject.startswith("UML-ANDROID-LOCAL:"):
            raise RegenerationError(f"topic subject lacks local prefix: {subject}")
        commits.append(commit)
        expected_parent = commit
    return commits


def generate_series(topic: Path, base_commit: str, output: Path) -> list[tuple[str, str, str, str]]:
    result = run(
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
    patches = [Path(line) for line in result.splitlines() if line.strip()]
    entries: list[tuple[str, str, str, str]] = []
    for patch in patches:
        if patch.parent.resolve() != output.resolve() or not patch.is_file():
            raise RegenerationError("git format-patch emitted an unexpected path")
        payload = patch.read_bytes()
        ids = [
            line.split()[0]
            for line in run("git", "patch-id", "--stable", payload=payload).splitlines()
            if line.strip()
        ]
        if len(ids) != 1 or HEX40.fullmatch(ids[0]) is None:
            raise RegenerationError(f"cannot derive stable patch ID: {patch.name}")
        mail = run("git", "mailinfo", "/dev/null", "/dev/null", payload=payload)
        subjects = [
            line.removeprefix("Subject: ")
            for line in mail.splitlines()
            if line.startswith("Subject: ")
        ]
        if len(subjects) != 1 or not subjects[0].startswith("UML-ANDROID-LOCAL:"):
            raise RegenerationError(f"generated patch has an invalid subject: {patch.name}")
        entries.append(
            (
                patch.name,
                ids[0],
                hashlib.sha256(payload).hexdigest(),
                subjects[0],
            )
        )
    return entries


def render_manifest(entries: list[tuple[str, str, str, str]]) -> bytes:
    lines = [
        "# SPDX-License-Identifier: Apache-2.0",
        "# Ordered generated AOSP project topic identity.",
        "order\tfilename\tstable_patch_id\tsha256\tsubject",
    ]
    lines.extend(
        f"{order}\t{filename}\t{patch_id}\t{digest}\t{subject}"
        for order, (filename, patch_id, digest, subject) in enumerate(entries, 1)
    )
    return ("\n".join(lines) + "\n").encode()


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Regenerate one routed AOSP project's generated mail patch series"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--route-id", required=True)
    parser.add_argument("--topic-checkout", type=Path, required=True)
    args = parser.parse_args()

    try:
        android_root = args.android_root.resolve()
        topic = args.topic_checkout.resolve()
        if SAFE_ROUTE.fullmatch(args.route_id) is None:
            raise RegenerationError("route ID must use the pNNNN form")
        projects_path = android_root / "patches/projects.tsv"
        prefix, rows = read_project_rows(projects_path)
        matching = [row for row in rows if row[1] == args.route_id]
        if len(matching) != 1:
            raise RegenerationError("route ID is not present exactly once")
        row = matching[0]
        base_commit, base_tree = row[5], row[6]
        if HEX40.fullmatch(base_commit) is None or HEX40.fullmatch(base_tree) is None:
            raise RegenerationError("route base identity is malformed")
        commits = verify_topic(topic, base_commit, base_tree)
        series_dir = safe_series_path(android_root, row[10], args.route_id)
        route_root = series_dir.parent
        metadata_path = route_root / "metadata.env"
        metadata_before = metadata_path.read_bytes()
        projects_before = projects_path.read_bytes()
        series_backup = route_root / ".series.regenerate-backup"
        if series_backup.exists():
            raise RegenerationError(f"stale regeneration backup exists: {series_backup}")

        with tempfile.TemporaryDirectory(
            prefix=".series.regenerate-", dir=route_root
        ) as temporary_name:
            generated = Path(temporary_name) / "series"
            generated.mkdir()
            entries = generate_series(topic, base_commit, generated)
            if len(entries) != len(commits):
                raise RegenerationError("generated patch count differs from topic history")
            manifest = render_manifest(entries)
            (generated / "manifest.tsv").write_bytes(manifest)
            manifest_digest = hashlib.sha256(manifest).hexdigest()
            final_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic).strip()
            if HEX40.fullmatch(final_tree) is None:
                raise RegenerationError("topic final tree is malformed")

            metadata = metadata_before.decode(errors="strict")
            metadata = replace_assignment(
                metadata, "AOSP_FINAL_TREE", f"'{final_tree}'"
            )
            metadata = replace_assignment(
                metadata, "AOSP_PATCH_COUNT", str(len(entries))
            )
            metadata = replace_assignment(
                metadata, "AOSP_SERIES_MANIFEST_SHA256", f"'{manifest_digest}'"
            )
            row[7] = final_tree
            row[8] = str(len(entries))
            row[9] = manifest_digest
            projects = "\n".join(prefix + ["\t".join(item) for item in rows]) + "\n"

            metadata_temp = route_root / ".metadata.env.regenerate"
            projects_temp = projects_path.with_name(".projects.tsv.regenerate")
            metadata_temp.write_text(metadata)
            projects_temp.write_text(projects)
            try:
                series_dir.rename(series_backup)
                generated.rename(series_dir)
                os.replace(metadata_temp, metadata_path)
                os.replace(projects_temp, projects_path)
                verifier = android_root / "tools/verify-aosp-patches.py"
                run(str(verifier), "--android-root", str(android_root))
            except BaseException:
                metadata_path.write_bytes(metadata_before)
                projects_path.write_bytes(projects_before)
                if series_dir.exists():
                    shutil.rmtree(series_dir)
                if series_backup.exists():
                    series_backup.rename(series_dir)
                metadata_temp.unlink(missing_ok=True)
                projects_temp.unlink(missing_ok=True)
                raise
            shutil.rmtree(series_backup)

        print(
            f"regenerated {args.route_id}: {len(commits)} patches, final tree {final_tree}"
        )
    except (OSError, UnicodeError, RegenerationError) as error:
        raise SystemExit(f"AOSP patch regenerator: {error}") from error


if __name__ == "__main__":
    main()
