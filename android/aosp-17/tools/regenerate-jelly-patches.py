#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate Jelly's generated patch series from its maintained Git topic."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from pathlib import Path

from pinned_source import RegenerationError, regenerate, run


def upstream_base(topic: Path, metadata: dict[str, str]) -> str:
    """The topic sits directly on the pinned upstream commit."""
    base = metadata["JELLY_UPSTREAM_COMMIT"]
    if run(
        "git", "rev-parse", f"{base}^{{tree}}", cwd=topic, error=RegenerationError
    ) != metadata["JELLY_UPSTREAM_TREE"]:
        raise RegenerationError("topic has the wrong pinned upstream tree")
    return base


if __name__ == "__main__":
    regenerate(
        tool="regenerate-jelly-patches.py",
        description="Regenerate Jelly's generated patch series",
        label="Jelly",
        subject="Jelly",
        directory="jelly",
        prefix="JELLY",
        find_base=upstream_base,
    )
