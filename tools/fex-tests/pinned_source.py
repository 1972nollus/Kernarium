# SPDX-License-Identifier: GPL-2.0-or-later
"""Pinned x86-conformance inputs: manifest identities, file digests and checkouts.

Every check raises ValueError; each producer reports it as its own failure.
"""

from __future__ import annotations

import hashlib
import pathlib
import subprocess

MANIFEST = pathlib.Path(__file__).resolve().parents[2] / "dependencies" / "manifest.tsv"


def manifest_identity(name: str) -> tuple[str, str]:
    """Return the version and commit of the x86-conformance input NAME."""
    for line in MANIFEST.read_text().splitlines():
        if not line or line.startswith("#") or line.startswith("name\t"):
            continue
        fields = line.split("\t")
        if len(fields) == 7 and fields[0] == name:
            if "x86-conformance" not in fields[2].split(","):
                raise ValueError(f"{name} is not scoped to x86-conformance")
            return fields[1], fields[4]
    raise ValueError(f"dependency manifest has no {name} input")


def git_output(source: pathlib.Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(source), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout.strip()


def sha256_file(path: pathlib.Path) -> str:
    # hashlib.file_digest needs Python 3.11; these tools run on the host's.
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def verify_checkout(source: pathlib.Path, commit: str, name: str) -> None:
    """Require SOURCE to be a clean Git checkout of COMMIT."""
    if not (source / ".git").exists():
        raise ValueError(f"{name} source is not a Git checkout: {source}")
    if git_output(source, "rev-parse", "HEAD") != commit:
        raise ValueError(f"{name} source is not at the dependency-manifest commit")
    if git_output(source, "status", "--porcelain", "--untracked-files=all"):
        raise ValueError(f"{name} source checkout is dirty")
