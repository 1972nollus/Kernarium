#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Regenerate the Mesa Venus Android port series from its maintained topic.

The topic checkout is the materialized `external/mesa3d-venus` tree: the pinned
upstream Mesa revision, the replayed guest Vulkan series, then the Android port
commits on `MESA_VENUS_PATCH_BRANCH`. Only the Android commits are generated
here; the guest series stays owned by `tools/rootfs/vulkan/`.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts"))
from contract_checks import parse_assignments, replace_assignment  # noqa: E402
import patch_series  # noqa: E402




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


def guest_base(topic: Path, metadata: dict[str, str]) -> str:
    """Find the replayed guest Mesa commit the Android port sits on."""
    guest_tree = metadata["MESA_VENUS_GUEST_TREE"]
    commit = run("git", "rev-parse", "HEAD", cwd=topic)
    for _ in range(64):
        if run("git", "rev-parse", f"{commit}^{{tree}}", cwd=topic) == guest_tree:
            return commit
        parents = run("git", "rev-list", "--parents", "-n", "1", commit, cwd=topic).split()
        if len(parents) != 2:
            break
        commit = parents[1]
    raise RegenerationError(
        "the topic does not contain the pinned guest Mesa tree; replay the port "
        "from the pinned upstream revision and the guest Vulkan series first"
    )


def verify_topic(topic: Path, metadata: dict[str, str]) -> str:
    if topic.is_symlink() or not topic.is_dir():
        raise RegenerationError("topic checkout must be a non-symlink directory")
    if Path(run("git", "rev-parse", "--show-toplevel", cwd=topic)).resolve() != topic:
        raise RegenerationError("topic path is not the Git checkout root")
    if run("git", "branch", "--show-current", cwd=topic) != metadata[
        "MESA_VENUS_PATCH_BRANCH"
    ]:
        raise RegenerationError("topic checkout is not on MESA_VENUS_PATCH_BRANCH")
    return guest_base(topic, metadata)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Regenerate the Mesa Venus Android port patch series"
    )
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--topic-checkout", type=Path, required=True)
    args = parser.parse_args()

    try:
        android_root = args.android_root.resolve()
        topic = args.topic_checkout.resolve()
        venus_root = android_root / "third_party/mesa-venus"
        metadata_path = venus_root / "metadata.env"
        series = venus_root / "series"
        verifier = android_root / "tools/verify-mesa-venus-source.py"
        if not metadata_path.is_file() or not series.is_dir() or not verifier.is_file():
            raise RegenerationError("Android root lacks the Mesa Venus source contract")
        metadata_text = metadata_path.read_text()
        metadata = parse_assignments(metadata_path, RegenerationError)
        base = verify_topic(topic, metadata)
        final_tree = patch_series.export_series(
            topic, base, "HEAD", series, license_id="Apache-2.0"
        )
        metadata_path.write_text(
            replace_assignment(
                metadata_text, "MESA_VENUS_FINAL_TREE", f"'{final_tree}'", RegenerationError
            )
        )
        run(str(verifier), "--android-root", str(android_root))
    except (OSError, UnicodeError, RegenerationError, patch_series.SeriesError) as error:
        raise SystemExit(f"regenerate-mesa-venus-patches.py: {error}") from error

    print(
        f"regenerated the Mesa Venus Android port from "
        f"{metadata['MESA_VENUS_PATCH_BRANCH']}: final tree {final_tree}"
    )


if __name__ == "__main__":
    main()
