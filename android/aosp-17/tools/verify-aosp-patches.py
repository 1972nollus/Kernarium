#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
import subprocess
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

import pinned_source
# pinned_source puts scripts/ and the contract checks on sys.path.
from contract_checks import parse_assignments  # noqa: E402
import patch_series  # noqa: E402


PROJECT_HEADER = (
    "order",
    "route_id",
    "manifest_project",
    "checkout_path",
    "manifest_revision",
    "base_commit",
    "base_tree",
    "final_tree",
    "series_dir",
)
HEX40 = pinned_source.HEX40


def error(message: str) -> SystemExit:
    return SystemExit(f"AOSP 17 patch verifier: {message}")


def fail(message: str) -> None:
    raise error(message)


def run(*command: str, cwd: Path | None = None) -> str:
    return pinned_source.run(*command, cwd=cwd, error=error)


def digest(path: Path) -> str:
    return pinned_source.digest(path, error)


def table(path: Path, expected_header: tuple[str, ...]) -> list[tuple[str, ...]]:
    return pinned_source.table(path, expected_header, error)


def safe_relative(path: str, label: str) -> None:
    pinned_source.safe_relative(path, label, error)


def manifest_projects(root: ET.Element) -> dict[str, ET.Element]:
    return pinned_source.manifest_projects(root, error)


@dataclass(frozen=True)
class Route:
    order: int
    route_id: str
    project: str
    path: str
    manifest_revision: str
    base_commit: str
    base_tree: str
    final_tree: str
    series_dir: Path


def inspect_series(android_root: Path, values: tuple[str, ...]) -> Route:
    (
        order_text,
        route_id,
        project,
        checkout_path,
        manifest_revision,
        base_commit,
        base_tree,
        final_tree,
        series_dir_text,
    ) = values
    if not order_text.isdecimal():
        fail(f"route {route_id} has a non-numeric order")
    if not re.fullmatch(r"p[0-9]{4}", route_id):
        fail(f"invalid route ID: {route_id}")
    safe_relative(project, "manifest project name")
    safe_relative(checkout_path, "checkout path")
    safe_relative(series_dir_text, "series directory")
    expected_series_dir = f"patches/projects/{route_id}/series"
    if series_dir_text != expected_series_dir:
        fail(f"route {route_id} series must be isolated at {expected_series_dir}")
    for label, value in (
        ("base commit", base_commit),
        ("base tree", base_tree),
        ("final tree", final_tree),
    ):
        if not HEX40.fullmatch(value):
            fail(f"route {route_id} has invalid {label}")
    series_dir = android_root / series_dir_text
    try:
        series = patch_series.read_series(series_dir)
    except patch_series.SeriesError as error:
        fail(f"route {route_id}: {error}")
    if not series.names:
        fail(f"route {route_id} series is empty")
    return Route(
        int(order_text),
        route_id,
        project,
        checkout_path,
        manifest_revision,
        base_commit,
        base_tree,
        final_tree,
        series_dir,
    )


def verify_source(
    android_root: Path,
    aosp_source: Path,
    base_projects: dict[str, ET.Element],
    routes: list[Route],
    expect: str,
) -> None:
    source = aosp_source.resolve()
    if not (source / ".repo/manifests").is_dir():
        fail(f"AOSP source lacks Repo metadata: {source}")
    current_xml = run("repo", "manifest", "-r", cwd=source)
    current_projects = manifest_projects(ET.fromstring(current_xml))
    if set(current_projects) != set(base_projects):
        fail("current Repo project paths differ from the resolved base manifest")
    route_paths = {route.path for route in routes}
    for path, base in base_projects.items():
        if path in route_paths:
            continue
        if current_projects[path].get("revision") != base.get("revision"):
            fail(f"unrouted AOSP project moved from its pinned revision: {path}")

    dirty = run(
        "repo",
        "forall",
        "-c",
        'if test -n "$(git status --porcelain)"; then printf "%s\\n" "$REPO_PATH"; fi',
        cwd=source,
    )
    if dirty:
        fail(f"AOSP projects have uncommitted changes: {dirty}")

    for route in routes:
        checkout = (source / route.path).resolve()
        try:
            checkout.relative_to(source)
        except ValueError:
            fail(f"route {route.route_id} checkout escapes the AOSP source")
        if not (checkout / ".git").exists():
            fail(f"route {route.route_id} is not a Git checkout")
        head = run("git", "rev-parse", "HEAD", cwd=checkout)
        tree = run("git", "rev-parse", "HEAD^{tree}", cwd=checkout)
        if current_projects[route.path].get("name") != route.project:
            fail(f"route {route.route_id} maps to the wrong manifest project")
        if current_projects[route.path].get("revision") != head:
            fail(f"Repo did not resolve route {route.route_id} to its current HEAD")
        if expect == "base":
            if head != route.base_commit or tree != route.base_tree:
                fail(f"route {route.route_id} is not at its exact base")
            continue
        if tree != route.final_tree:
            fail(f"route {route.route_id} tree is {tree}, expected {route.final_tree}")
        ancestor = subprocess.run(
            ["git", "merge-base", "--is-ancestor", route.base_commit, "HEAD"],
            cwd=checkout,
            check=False,
        )
        if ancestor.returncode != 0:
            fail(f"route {route.route_id} is not based on its pinned base")

    if expect == "final":
        sync_tool = android_root / "tools/sync-device-tree.py"
        if not sync_tool.is_file():
            fail("device-tree sync tool is missing")
        run(
            str(sync_tool),
            "--android-root",
            str(android_root),
            "--tree",
            "overlay",
            "--workspace",
            str(source),
            "--verify",
        )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--android-root", type=Path, required=True)
    parser.add_argument("--aosp-source", type=Path)
    parser.add_argument("--expect", choices=("base", "final"), default="final")
    args = parser.parse_args()
    android_root = args.android_root.resolve()

    metadata = parse_assignments(android_root / "metadata/aosp.env", fail)
    base_manifest = android_root / "metadata/base-manifest.xml"
    if digest(base_manifest) != metadata.get("ANDROID17_BASE_MANIFEST_SHA256"):
        fail("resolved base-manifest digest differs from AOSP metadata")
    base_projects = manifest_projects(ET.parse(base_manifest).getroot())

    project_rows = table(android_root / "patches/projects.tsv", PROJECT_HEADER)
    routes = [inspect_series(android_root, row) for row in project_rows]
    if [route.order for route in routes] != list(range(1, len(routes) + 1)):
        fail("project route order is not contiguous")
    if len({route.route_id for route in routes}) != len(routes):
        fail("project route IDs are not unique")
    if len({route.project for route in routes}) != len(routes):
        fail("manifest projects are routed more than once")
    if len({route.path for route in routes}) != len(routes):
        fail("checkout paths are routed more than once")
    for route in routes:
        project = base_projects.get(route.path)
        if project is None or project.get("name") != route.project:
            fail(f"route {route.route_id} is absent from the base manifest")
        if project.get("revision") != route.base_commit:
            fail(f"route {route.route_id} base commit differs from the manifest lock")
        if project.get("upstream") != route.manifest_revision:
            fail(f"route {route.route_id} manifest revision differs from the lock")

    if args.aosp_source is not None:
        verify_source(android_root, args.aosp_source, base_projects, routes, args.expect)
    print(f"verified {len(routes)} routed AOSP 17 project series")


if __name__ == "__main__":
    main()
