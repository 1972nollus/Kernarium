#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate Jelly's checked-in mail patches from its maintained Git topic."""

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
ASSIGNMENT = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")


class RegenerationError(ValueError):
    pass


def run(
    *command: str, cwd: Path | None = None, payload: bytes | None = None
) -> str:
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
    return result.stdout.decode(errors="strict").strip()


def parse_metadata(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for line_number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = ASSIGNMENT.fullmatch(line)
        if match is None:
            raise RegenerationError(f"metadata line {line_number} is malformed")
        key, quoted, numeric = match.groups()
        values[key] = quoted if quoted is not None else numeric
    return values


def replace_assignment(text: str, key: str, value: str) -> str:
    pattern = re.compile(rf"^{re.escape(key)}=(?:'[^']*'|[0-9]+)$", re.MULTILINE)
    updated, count = pattern.subn(f"{key}={value}", text)
    if count != 1:
        raise RegenerationError(f"metadata does not define {key} exactly once")
    return updated


def verify_topic(topic: Path, metadata: dict[str, str]) -> list[str]:
    if topic.is_symlink() or not topic.is_dir():
        raise RegenerationError("topic checkout must be a non-symlink directory")
    if Path(run("git", "rev-parse", "--show-toplevel", cwd=topic)).resolve() != topic:
        raise RegenerationError("topic path is not the Git checkout root")
    if run("git", "status", "--porcelain", "--untracked-files=all", cwd=topic):
        raise RegenerationError("topic checkout has uncommitted changes")
    if run("git", "branch", "--show-current", cwd=topic) != metadata[
        "JELLY_PATCH_BRANCH"
    ]:
        raise RegenerationError("topic checkout is not on JELLY_PATCH_BRANCH")
    base = metadata["JELLY_UPSTREAM_COMMIT"]
    if run("git", "rev-parse", f"{base}^{{tree}}", cwd=topic) != metadata[
        "JELLY_UPSTREAM_TREE"
    ]:
        raise RegenerationError("topic has the wrong pinned upstream tree")
    history = run(
        "git", "rev-list", "--reverse", "--parents", f"{base}..HEAD", cwd=topic
    ).splitlines()
    if not history:
        raise RegenerationError("Jelly topic has no local commits")
    commits: list[str] = []
    parent = base
    for row in history:
        fields = row.split()
        if len(fields) != 2 or fields[1] != parent:
            raise RegenerationError("Jelly topic history is not clean and linear")
        subject = run("git", "show", "-s", "--format=%s", fields[0], cwd=topic)
        if not subject.startswith("UML-ANDROID-LOCAL: Jelly "):
            raise RegenerationError(f"Jelly topic subject lacks local prefix: {subject}")
        commits.append(fields[0])
        parent = fields[0]
    return commits


def generate_series(
    topic: Path, base: str, output: Path
) -> list[tuple[str, str, str, str]]:
    emitted = run(
        "git",
        "format-patch",
        "--numbered",
        "--full-index",
        "--binary",
        "--no-signature",
        "--output-directory",
        str(output),
        f"{base}..HEAD",
        cwd=topic,
    )
    patches = [Path(line) for line in emitted.splitlines() if line]
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
            raise RegenerationError(f"cannot identify generated patch: {patch.name}")
        mail = run("git", "mailinfo", "/dev/null", "/dev/null", payload=payload)
        subjects = [
            line.removeprefix("Subject: ")
            for line in mail.splitlines()
            if line.startswith("Subject: ")
        ]
        if len(subjects) != 1 or not subjects[0].startswith(
            "UML-ANDROID-LOCAL: Jelly "
        ):
            raise RegenerationError(f"generated patch subject is invalid: {patch.name}")
        entries.append(
            (
                patch.name,
                patch_ids[0],
                hashlib.sha256(payload).hexdigest(),
                subjects[0],
            )
        )
    return entries


def render_manifest(entries: list[tuple[str, str, str, str]]) -> str:
    lines = [
        "# SPDX-License-Identifier: Apache-2.0",
        "# Ordered generated Jelly topic identity. Patch bytes come only from git format-patch.",
        "order\tfilename\tstable_patch_id\tsha256\tsubject",
    ]
    lines.extend(
        f"{order}\t{filename}\t{patch_id}\t{digest}\t{subject}"
        for order, (filename, patch_id, digest, subject) in enumerate(entries, 1)
    )
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Regenerate Jelly's generated mail patch series"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--topic-checkout", type=Path, required=True)
    args = parser.parse_args()

    try:
        android_root = args.android_root.resolve()
        topic = args.topic_checkout.resolve()
        jelly_root = android_root / "third_party/jelly"
        metadata_path = jelly_root / "metadata.env"
        series = jelly_root / "series"
        verifier = android_root / "tools/verify-jelly-source.py"
        if not metadata_path.is_file() or not series.is_dir() or not verifier.is_file():
            raise RegenerationError("Android root lacks the Jelly source contract")
        metadata_before = metadata_path.read_bytes()
        metadata = parse_metadata(metadata_path)
        commits = verify_topic(topic, metadata)
        final_commit = run("git", "rev-parse", "HEAD", cwd=topic)
        final_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)

        with tempfile.TemporaryDirectory(prefix=".jelly-regenerate-", dir=jelly_root) as temp:
            temporary = Path(temp)
            generated = temporary / "series"
            generated.mkdir()
            entries = generate_series(topic, metadata["JELLY_UPSTREAM_COMMIT"], generated)
            if len(entries) != len(commits):
                raise RegenerationError("generated series differs from topic commit count")
            (generated / "manifest.tsv").write_text(render_manifest(entries))
            metadata_text = metadata_before.decode(errors="strict")
            metadata_text = replace_assignment(
                metadata_text, "JELLY_FINAL_COMMIT", f"'{final_commit}'"
            )
            metadata_text = replace_assignment(
                metadata_text, "JELLY_FINAL_TREE", f"'{final_tree}'"
            )
            metadata_text = replace_assignment(
                metadata_text, "JELLY_PATCH_COUNT", str(len(entries))
            )
            generated_metadata = temporary / "metadata.env"
            generated_metadata.write_text(metadata_text)

            backup = jelly_root / ".series.regenerate-backup"
            if backup.exists():
                raise RegenerationError(f"stale Jelly series backup exists: {backup}")
            series.rename(backup)
            generated.rename(series)
            os.replace(generated_metadata, metadata_path)
            try:
                run(
                    str(verifier),
                    "--android-root",
                    str(android_root),
                )
            except Exception:
                shutil.rmtree(series)
                backup.rename(series)
                metadata_path.write_bytes(metadata_before)
                raise
            shutil.rmtree(backup)
    except (OSError, UnicodeError, RegenerationError) as error:
        raise SystemExit(f"regenerate-jelly-patches.py: {error}") from error

    print(f"regenerated {len(commits)} Jelly patches from {metadata['JELLY_PATCH_BRANCH']}")


if __name__ == "__main__":
    main()
