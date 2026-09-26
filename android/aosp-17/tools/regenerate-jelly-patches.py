#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate Jelly's generated patch series from its maintained Git topic."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
import patch_series  # noqa: E402


ASSIGNMENT = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")


class RegenerationError(ValueError):
    pass


def run(*command: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
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


def verify_topic(topic: Path, metadata: dict[str, str]) -> None:
    if topic.is_symlink() or not topic.is_dir():
        raise RegenerationError("topic checkout must be a non-symlink directory")
    if Path(run("git", "rev-parse", "--show-toplevel", cwd=topic)).resolve() != topic:
        raise RegenerationError("topic path is not the Git checkout root")
    if run("git", "branch", "--show-current", cwd=topic) != metadata[
        "JELLY_PATCH_BRANCH"
    ]:
        raise RegenerationError("topic checkout is not on JELLY_PATCH_BRANCH")
    base = metadata["JELLY_UPSTREAM_COMMIT"]
    if run("git", "rev-parse", f"{base}^{{tree}}", cwd=topic) != metadata[
        "JELLY_UPSTREAM_TREE"
    ]:
        raise RegenerationError("topic has the wrong pinned upstream tree")


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Regenerate Jelly's generated patch series"
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
        metadata_text = metadata_path.read_text()
        metadata = parse_metadata(metadata_path)
        verify_topic(topic, metadata)
        final_tree = patch_series.export_series(
            topic, metadata["JELLY_UPSTREAM_COMMIT"], "HEAD", series,
            license_id="Apache-2.0",
        )
        metadata_path.write_text(
            replace_assignment(metadata_text, "JELLY_FINAL_TREE", f"'{final_tree}'")
        )
        run(str(verifier), "--android-root", str(android_root))
    except (OSError, UnicodeError, RegenerationError, patch_series.SeriesError) as error:
        raise SystemExit(f"regenerate-jelly-patches.py: {error}") from error

    print(f"regenerated Jelly from {metadata['JELLY_PATCH_BRANCH']}: final tree {final_tree}")


if __name__ == "__main__":
    main()
