#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Verify pinned OrangeFox metadata, generated topics, and an optional tree."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))
import patch_series  # noqa: E402


HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")
ASSIGNMENT = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")
PROJECT_HEADER = (
    "order", "route_id", "manifest_project", "checkout_path",
    "manifest_upstream", "base_commit", "base_tree", "final_tree", "series_dir",
)
EXTERNALS = (
    (
        "bootable/recovery",
        "ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY",
        "ORANGEFOX_BOOTABLE_RECOVERY_COMMIT",
        "ORANGEFOX_BOOTABLE_RECOVERY_TREE",
    ),
    (
        "vendor/recovery",
        "ORANGEFOX_VENDOR_RECOVERY_REPOSITORY",
        "ORANGEFOX_VENDOR_RECOVERY_COMMIT",
        "ORANGEFOX_VENDOR_RECOVERY_TREE",
    ),
    (
        "external/se_omapi",
        "ORANGEFOX_SE_OMAPI_REPOSITORY",
        "ORANGEFOX_SE_OMAPI_COMMIT",
        "ORANGEFOX_SE_OMAPI_TREE",
    ),
)


class VerificationError(ValueError):
    pass


@dataclass(frozen=True)
class Route:
    order: int
    route_id: str
    project: str
    path: str
    upstream: str
    base_commit: str
    base_tree: str
    final_tree: str


def run(*command: str, cwd: Path | None = None, payload: bytes | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip()
        raise VerificationError(f"command failed ({' '.join(command)}): {detail}")
    return result.stdout.decode(errors="strict").strip()


def digest(path: Path) -> str:
    if path.is_symlink() or not path.is_file():
        raise VerificationError(f"required regular file is missing: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def assignments(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = ASSIGNMENT.fullmatch(line)
        if match is None:
            raise VerificationError(f"{path}:{number}: malformed assignment")
        key, quoted, numeric = match.groups()
        if key in values:
            raise VerificationError(f"{path}:{number}: duplicate assignment {key}")
        values[key] = quoted if quoted is not None else numeric
    return values


def table(path: Path, expected: tuple[str, ...]) -> list[tuple[str, ...]]:
    lines = [
        line for line in path.read_text(encoding="utf-8").splitlines()
        if line and not line.startswith("#")
    ]
    rows = [tuple(row) for row in csv.reader(lines, delimiter="\t")]
    if not rows or rows[0] != expected:
        raise VerificationError(f"table header is invalid: {path}")
    if any(len(row) != len(expected) or any(not value for value in row) for row in rows[1:]):
        raise VerificationError(f"table contains a malformed row: {path}")
    return rows[1:]


def safe_relative(value: str, label: str) -> None:
    path = PurePosixPath(value)
    if (
        not value or value.startswith("/") or "\\" in value or str(path) != value
        or any(part in ("", ".", "..", ".git", ".repo") for part in path.parts)
    ):
        raise VerificationError(f"unsafe {label}: {value}")


def manifest_projects(root: ET.Element) -> dict[str, ET.Element]:
    projects: dict[str, ET.Element] = {}
    for project in root.findall("project"):
        name = project.get("name")
        path = project.get("path", name or "")
        if not name:
            raise VerificationError("resolved manifest project has no name")
        safe_relative(path, "manifest path")
        if path in projects:
            raise VerificationError(f"resolved manifest repeats checkout path: {path}")
        projects[path] = project
    if not projects:
        raise VerificationError("resolved OrangeFox manifest has no projects")
    return projects


def verify_metadata(root: Path) -> tuple[dict[str, str], dict[str, ET.Element]]:
    values = assignments(root / "third_party/orangefox/metadata.env")
    required = {
        "ORANGEFOX_SOURCE_SCHEMA", "ORANGEFOX_SYNC_REPOSITORY",
        "ORANGEFOX_SYNC_BRANCH", "ORANGEFOX_SYNC_COMMIT", "ORANGEFOX_SYNC_TREE",
        "ORANGEFOX_SYNC_SCRIPT_SHA256", "ORANGEFOX_MANIFEST_REPOSITORY",
        "ORANGEFOX_MANIFEST_BRANCH", "ORANGEFOX_MANIFEST_COMMIT",
        "ORANGEFOX_MANIFEST_TREE", "ORANGEFOX_BASE_MANIFEST_SHA256",
        "ORANGEFOX_BRANCH", "ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY",
        "ORANGEFOX_BOOTABLE_RECOVERY_COMMIT", "ORANGEFOX_BOOTABLE_RECOVERY_TREE",
        "ORANGEFOX_VENDOR_RECOVERY_REPOSITORY", "ORANGEFOX_VENDOR_RECOVERY_COMMIT",
        "ORANGEFOX_VENDOR_RECOVERY_TREE", "ORANGEFOX_SE_OMAPI_REPOSITORY",
        "ORANGEFOX_SE_OMAPI_COMMIT", "ORANGEFOX_SE_OMAPI_TREE",
        "ORANGEFOX_DEVICE_OVERLAY_DESTINATION", "ORANGEFOX_PRODUCT",
        "ORANGEFOX_RELEASE_CONFIG", "ORANGEFOX_LICENSE",
    }
    if set(values) != required:
        raise VerificationError(
            f"metadata keys differ (missing={sorted(required - set(values))}, "
            f"extra={sorted(set(values) - required)})"
        )
    expected = {
        "ORANGEFOX_SOURCE_SCHEMA": "1",
        "ORANGEFOX_SYNC_REPOSITORY": "https://gitlab.com/OrangeFox/sync.git",
        "ORANGEFOX_MANIFEST_REPOSITORY": (
            "https://github.com/nebrassy/platform_manifest_twrp_aosp.git"
        ),
        "ORANGEFOX_BRANCH": "fox_14.1",
        "ORANGEFOX_DEVICE_OVERLAY_DESTINATION": "device/kernarium/darwin",
        "ORANGEFOX_PRODUCT": "twrp_darwin",
        "ORANGEFOX_RELEASE_CONFIG": "ap2a",
        "ORANGEFOX_LICENSE": "GPL-3.0-or-later",
    }
    for key, wanted in expected.items():
        if values[key] != wanted:
            raise VerificationError(f"{key} is not the reviewed value")
    for key, value in values.items():
        if key.endswith(("_COMMIT", "_TREE")) and HEX40.fullmatch(value) is None:
            raise VerificationError(f"{key} is not a full Git object ID")
        if key.endswith("_SHA256") and HEX64.fullmatch(value) is None:
            raise VerificationError(f"{key} is not a SHA-256 digest")

    manifest_path = root / "third_party/orangefox/base-manifest.xml"
    if digest(manifest_path) != values["ORANGEFOX_BASE_MANIFEST_SHA256"]:
        raise VerificationError("resolved base manifest digest differs from metadata")
    return values, manifest_projects(ET.parse(manifest_path).getroot())


def inspect_routes(root: Path, projects: dict[str, ET.Element]) -> list[Route]:
    routes: list[Route] = []
    project_rows = table(
        root / "third_party/orangefox/patches/projects.tsv", PROJECT_HEADER
    )
    for expected_order, row in enumerate(project_rows, 1):
        (
            order_text, route_id, project_name, checkout_path, upstream,
            base_commit, base_tree, final_tree, series_dir_text,
        ) = row
        if order_text != str(expected_order) or route_id != f"p{expected_order:04d}":
            raise VerificationError("OrangeFox route order or ID is invalid")
        safe_relative(checkout_path, "route checkout")
        safe_relative(series_dir_text, "route series")
        if any(HEX40.fullmatch(value) is None for value in (base_commit, base_tree, final_tree)):
            raise VerificationError(f"route {route_id} has malformed Git identity")
        project = projects.get(checkout_path)
        if (
            project is None or project.get("name") != project_name
            or project.get("revision") != base_commit or project.get("upstream") != upstream
        ):
            raise VerificationError(f"route {route_id} differs from the manifest lock")
        try:
            series = patch_series.read_series(root / series_dir_text)
        except patch_series.SeriesError as error:
            raise VerificationError(f"route {route_id}: {error}") from error
        if not series.names:
            raise VerificationError(f"route {route_id} series is empty")
        routes.append(
            Route(
                expected_order, route_id, project_name, checkout_path, upstream,
                base_commit, base_tree, final_tree,
            )
        )
    return routes


def remote_urls(checkout: Path) -> set[str]:
    remotes = run("git", "remote", cwd=checkout).splitlines()
    return {run("git", "remote", "get-url", remote, cwd=checkout) for remote in remotes}


def verify_checkout(
    root: Path,
    source: Path,
    values: dict[str, str],
    base_projects: dict[str, ET.Element],
    routes: list[Route],
    expect: str,
    allow_kernel: bool,
) -> None:
    if not source.is_dir() or not (source / ".repo/manifests").is_dir():
        raise VerificationError("OrangeFox source is not a Repo workspace")
    manifest_git = source / ".repo/manifests"
    if run("git", "rev-parse", "HEAD", cwd=manifest_git) != values["ORANGEFOX_MANIFEST_COMMIT"]:
        raise VerificationError("OrangeFox manifest checkout moved from its pin")
    if run("git", "rev-parse", "HEAD^{tree}", cwd=manifest_git) != values["ORANGEFOX_MANIFEST_TREE"]:
        raise VerificationError("OrangeFox manifest tree moved from its pin")

    current_projects = manifest_projects(ET.fromstring(run("repo", "manifest", "-r", cwd=source)))
    if set(current_projects) != set(base_projects):
        raise VerificationError("OrangeFox Repo project paths differ from the lock")
    route_paths = {route.path for route in routes}
    for path, base in base_projects.items():
        current_revision = current_projects[path].get("revision")
        if path not in route_paths and current_revision != base.get("revision"):
            raise VerificationError(f"unrouted OrangeFox project moved: {path}")
    dirty = run(
        "repo", "forall", "-c",
        'if test -n "$(git status --porcelain)"; then printf "%s\\n" "$REPO_PATH"; fi',
        cwd=source,
    )
    if dirty:
        raise VerificationError(f"OrangeFox Repo projects are dirty: {dirty}")

    for path, repository_key, commit_key, tree_key in EXTERNALS:
        checkout = source / path
        if not checkout.is_dir() or checkout.is_symlink():
            raise VerificationError(f"OrangeFox external checkout is missing: {path}")
        if run("git", "status", "--porcelain", "--untracked-files=all", cwd=checkout):
            raise VerificationError(f"OrangeFox external checkout is dirty: {path}")
        if values[repository_key] not in remote_urls(checkout):
            raise VerificationError(f"OrangeFox external origin differs: {path}")
        if path in route_paths:
            # The routed history below owns its base/final identity. The
            # external pin still owns this checkout's transport and base.
            continue
        if run("git", "rev-parse", "HEAD", cwd=checkout) != values[commit_key]:
            raise VerificationError(f"OrangeFox external commit differs: {path}")
        if run("git", "rev-parse", "HEAD^{tree}", cwd=checkout) != values[tree_key]:
            raise VerificationError(f"OrangeFox external tree differs: {path}")

    for route in routes:
        checkout = source / route.path
        head = run("git", "rev-parse", "HEAD", cwd=checkout)
        tree = run("git", "rev-parse", "HEAD^{tree}", cwd=checkout)
        if current_projects[route.path].get("revision") != head:
            raise VerificationError(f"Repo did not resolve route HEAD: {route.route_id}")
        if expect == "base":
            if head != route.base_commit or tree != route.base_tree:
                raise VerificationError(f"route {route.route_id} is not at its exact base")
            continue
        if tree != route.final_tree:
            raise VerificationError(f"route {route.route_id} final tree differs")
        ancestor = subprocess.run(
            ("git", "merge-base", "--is-ancestor", route.base_commit, "HEAD"),
            cwd=checkout, check=False,
        )
        if ancestor.returncode != 0:
            raise VerificationError(f"route {route.route_id} is not based on its pinned base")
    if expect == "final":
        command = [
            str(root / "tools/sync-orangefox-device.py"),
            "--android-root", str(root), "--orangefox-source", str(source), "--verify",
        ]
        if allow_kernel:
            command.append("--allow-prebuilt-kernel")
        run(*command)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--orangefox-source", type=Path)
    parser.add_argument("--expect", choices=("base", "final"), default="final")
    parser.add_argument("--allow-prebuilt-kernel", action="store_true")
    args = parser.parse_args()
    try:
        root = args.android_root.resolve()
        values, projects = verify_metadata(root)
        routes = inspect_routes(root, projects)
        if args.allow_prebuilt_kernel and args.orangefox_source is None:
            raise VerificationError("--allow-prebuilt-kernel requires --orangefox-source")
        if args.orangefox_source is not None:
            verify_checkout(
                root, args.orangefox_source.resolve(), values, projects, routes,
                args.expect, args.allow_prebuilt_kernel,
            )
    except (OSError, UnicodeError, ET.ParseError, VerificationError) as error:
        raise SystemExit(f"verify-orangefox-source.py: {error}") from error
    suffix = " and checkout" if args.orangefox_source is not None else ""
    print(f"verified {len(routes)} OrangeFox project routes{suffix}")


if __name__ == "__main__":
    main()
