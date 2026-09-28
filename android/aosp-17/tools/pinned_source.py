# SPDX-License-Identifier: Apache-2.0
"""What the pinned external sources under third_party/ share.

Jelly and Mesa Venus each keep a metadata.env with one key prefix, a
generated patch series and a maintained Git topic. Their verifiers and
regenerators differ in their constants, in how a materialized checkout must
sit on its base, and in how the regenerator finds the topic's export base;
everything else is here. Each caller passes its own error type, so
diagnostics keep their per-tool prefixes.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts"))
from contract_checks import parse_assignments, replace_assignment  # noqa: E402
import patch_series  # noqa: E402


HEX40 = re.compile(r"[0-9a-f]{40}")
Error = Callable[[str], Exception]


def run(
    *command: str,
    cwd: Path | None = None,
    payload: bytes | None = None,
    error: Error = ValueError,
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
        raise error(f"command failed: {' '.join(command)}: {detail}")
    return result.stdout.decode(errors="strict").strip()


def read_metadata(
    path: Path, required: set[str], label: str, error: Error
) -> dict[str, str]:
    values = parse_assignments(path, error)
    if set(values) != required:
        missing = sorted(required - set(values))
        extra = sorted(set(values) - required)
        raise error(
            f"{label} metadata keys differ (missing={missing}, extra={extra})"
        )
    return values


def check_object_ids(values: dict[str, str], keys: tuple[str, ...], error: Error) -> None:
    for key in keys:
        if HEX40.fullmatch(values[key]) is None:
            raise error(f"{key} is not a full lowercase Git object ID")


def check_branch_name(branch: str, label: str, error: Error) -> None:
    ref = subprocess.run(
        ("git", "check-ref-format", f"refs/heads/{branch}"),
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if ref.returncode != 0:
        raise error(f"{label} patch branch name is invalid")


def read_generated_series(series_dir: Path, label: str, error: Error) -> int:
    """Return how many patches the checked-in series holds."""
    try:
        series = patch_series.read_series(series_dir)
    except patch_series.SeriesError as failure:
        raise error(f"{label} series: {failure}") from failure
    if not series.names:
        raise error(f"{label} series is empty")
    return len(series.names)


def open_checkout(
    aosp_source: Path,
    values: dict[str, str],
    prefix: str,
    label: str,
    error: Error,
) -> Path:
    """Return the clean materialized checkout whose origin is the pinned one."""
    if not aosp_source.is_dir() or not (aosp_source / ".repo").is_dir():
        raise error("AOSP source is not a Repo workspace")
    source = aosp_source / values[f"{prefix}_DESTINATION"]
    if source.is_symlink() or not source.is_dir():
        raise error(f"materialized {label} checkout is missing: {source}")
    top = Path(run("git", "rev-parse", "--show-toplevel", cwd=source, error=error))
    if top.resolve() != source.resolve():
        raise error(f"{label} destination is not the Git checkout root")
    if run("git", "status", "--porcelain", "--untracked-files=all", cwd=source, error=error):
        raise error(f"materialized {label} checkout is dirty")
    if run("git", "remote", "get-url", "origin", cwd=source, error=error) != values[
        f"{prefix}_UPSTREAM_REPOSITORY"
    ]:
        raise error(f"materialized {label} origin differs from metadata")
    return source


class RegenerationError(ValueError):
    pass


def regenerate(
    *,
    tool: str,
    description: str,
    label: str,
    subject: str,
    directory: str,
    prefix: str,
    find_base: Callable[[Path, dict[str, str]], str],
) -> None:
    """Export a source's maintained topic as its generated series.

    The topic must be the checkout root on the source's patch branch;
    find_base returns the commit the series starts from. The regenerated
    series and final tree are then checked by the source's own verifier.
    """
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--topic-checkout", type=Path, required=True)
    args = parser.parse_args()
    branch_key = f"{prefix}_PATCH_BRANCH"

    try:
        android_root = args.android_root.resolve()
        topic = args.topic_checkout.resolve()
        source_root = android_root / "third_party" / directory
        metadata_path = source_root / "metadata.env"
        series = source_root / "series"
        verifier = android_root / f"tools/verify-{directory}-source.py"
        if not metadata_path.is_file() or not series.is_dir() or not verifier.is_file():
            raise RegenerationError(f"Android root lacks the {label} source contract")
        metadata_text = metadata_path.read_text()
        metadata = parse_assignments(metadata_path, RegenerationError)
        if topic.is_symlink() or not topic.is_dir():
            raise RegenerationError("topic checkout must be a non-symlink directory")
        toplevel = run(
            "git", "rev-parse", "--show-toplevel", cwd=topic, error=RegenerationError
        )
        if Path(toplevel).resolve() != topic:
            raise RegenerationError("topic path is not the Git checkout root")
        if run(
            "git", "branch", "--show-current", cwd=topic, error=RegenerationError
        ) != metadata[branch_key]:
            raise RegenerationError(f"topic checkout is not on {branch_key}")
        base = find_base(topic, metadata)
        final_tree = patch_series.export_series(
            topic, base, "HEAD", series, license_id="Apache-2.0"
        )
        metadata_path.write_text(
            replace_assignment(
                metadata_text, f"{prefix}_FINAL_TREE", f"'{final_tree}'", RegenerationError
            )
        )
        run(str(verifier), "--android-root", str(android_root), error=RegenerationError)
    except (OSError, UnicodeError, RegenerationError, patch_series.SeriesError) as error:
        raise SystemExit(f"{tool}: {error}") from error

    print(f"regenerated {subject} from {metadata[branch_key]}: final tree {final_tree}")
