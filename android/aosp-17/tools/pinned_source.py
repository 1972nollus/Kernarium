# SPDX-License-Identifier: Apache-2.0
"""What the Android producer's pinned-source tools share.

Jelly and Mesa Venus each keep a metadata.env with one key prefix, a
generated patch series and a maintained Git topic. Their verifiers and
regenerators differ in their constants, in how a materialized checkout must
sit on its base, and in how the regenerator finds the topic's export base;
everything else is here. The AOSP and OrangeFox route verifiers read the same
kind of tab-separated route table and resolved Repo manifest, so their table,
path, manifest and digest checks are here too, as is the one command runner
every verifier, regenerator and the release publisher uses. Each caller
passes its own error type, so diagnostics keep their per-tool prefixes.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath
from typing import Callable

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "kernel/common/artifacts"))
from contract_checks import (  # noqa: E402
    parse_assignments,
    replace_assignment,
    sha256_path,
)
import patch_series  # noqa: E402


HEX40 = re.compile(r"[0-9a-f]{40}")
UNSAFE_PARTS = frozenset(("", ".", "..", ".git", ".repo"))
Error = Callable[[str], Exception]


def run(
    *command: str,
    cwd: Path | None = None,
    payload: bytes | None = None,
    env: dict[str, str] | None = None,
    capture: bool = True,
    error: Error = ValueError,
) -> str:
    """Run a command and return its standard output without surrounding space.

    A nonzero exit raises `error` naming the command, its status and, for a
    captured command, what it printed. Output that is not UTF-8 is refused.
    An uncaptured command writes to this process's streams and returns "".
    """
    result = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        input=payload,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if result.returncode != 0:
        detail = ""
        if capture:
            detail = (result.stderr or result.stdout).decode(errors="replace").strip()
        raise error(
            f"command failed ({result.returncode}): {' '.join(command)}"
            + (f": {detail}" if detail else "")
        )
    if not capture:
        return ""
    try:
        return result.stdout.decode("utf-8", errors="strict").strip()
    except UnicodeDecodeError as failure:
        raise error(f"command output is not UTF-8: {' '.join(command)}") from failure


def digest(path: Path, error: Error) -> str:
    """Return the SHA-256 of a regular, non-symlink file."""
    if path.is_symlink() or not path.is_file():
        raise error(f"required regular file is missing: {path}")
    return sha256_path(path)


def table(path: Path, header: tuple[str, ...], error: Error) -> list[tuple[str, ...]]:
    """Return the rows of a tab-separated table below its exact header.

    Empty lines and `#` comments are skipped. Every other line, one holding
    only white space included, must be a row with every field filled in.
    """
    numbered = [
        (number, line)
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1)
        if line and not line.startswith("#")
    ]
    if not numbered:
        raise error(f"table is empty: {path}")
    reader = csv.reader((line for _, line in numbered), delimiter="\t")
    rows = [(numbered[reader.line_num - 1][0], tuple(row)) for row in reader]
    (first, found), *data = rows
    if found != header:
        raise error(f"{path}:{first}: table header is invalid")
    for number, row in data:
        if len(row) != len(header) or not all(row):
            raise error(f"{path}:{number}: malformed row")
    return [row for _, row in data]


def safe_relative(value: str, label: str, error: Error) -> None:
    """Refuse anything but a normalized relative path outside Git and Repo state."""
    path = PurePosixPath(value)
    if (
        not value
        or value.startswith("/")
        or "\\" in value
        or str(path) != value
        or any(part in UNSAFE_PARTS for part in path.parts)
    ):
        raise error(f"unsafe {label}: {value}")


def manifest_projects(root: ET.Element, error: Error) -> dict[str, ET.Element]:
    """Map each project of a resolved Repo manifest by its checkout path."""
    projects: dict[str, ET.Element] = {}
    for project in root.findall("project"):
        name = project.get("name")
        if not name:
            raise error("resolved manifest has a project without a name")
        path = project.get("path", name)
        safe_relative(path, "manifest checkout path", error)
        if path in projects:
            raise error(f"resolved manifest repeats checkout path: {path}")
        projects[path] = project
    if not projects:
        raise error("resolved manifest has no projects")
    return projects


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


VENUS_PROTOCOL_BANNER = "/* This file is generated by venus-protocol "


def venus_protocol_banner(values: dict[str, str]) -> str:
    """Return the first line of the Mesa Venus port's generated protocol codec.

    venus-protocol's own build names the replay commit it ran from, and a
    replay gets new commit IDs each time it is recreated. The port's codec
    names the two pinned trees it is generated from instead, so a
    regeneration from the same pins reproduces it byte for byte.
    """
    return (
        f"{VENUS_PROTOCOL_BANNER}tree {values['MESA_VENUS_PROTOCOL_TREE']} "
        f"with Vulkan-Headers tree {values['MESA_VENUS_VULKAN_HEADERS_TREE']}. */"
    )


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
