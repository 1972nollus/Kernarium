#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import re
import subprocess
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


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
SERIES_HEADER = ("order", "filename", "stable_patch_id", "sha256", "subject")
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")


def fail(message: str) -> None:
    raise SystemExit(f"AOSP 17 patch verifier: {message}")


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
        fail(f"command failed ({' '.join(command)}): {detail}")
    return result.stdout.decode(errors="strict")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def assignments(path: Path) -> dict[str, str]:
    pattern = re.compile(r"^([A-Z][A-Z0-9_]*)=(?:'([^']*)'|([0-9]+))$")
    values: dict[str, str] = {}
    for number, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        match = pattern.fullmatch(line)
        if match is None:
            fail(f"{path}:{number}: malformed assignment")
        key, quoted, numeric = match.groups()
        if key in values:
            fail(f"{path}:{number}: duplicate assignment {key}")
        values[key] = quoted if quoted is not None else numeric
    return values


def table(path: Path, expected_header: tuple[str, ...]) -> list[tuple[str, ...]]:
    lines = [
        line
        for line in path.read_text().splitlines()
        if line.strip() and not line.startswith("#")
    ]
    if not lines:
        fail(f"table is empty: {path}")
    rows = [tuple(row) for row in csv.reader(lines, delimiter="\t")]
    if rows[0] != expected_header:
        fail(f"table header is invalid: {path}")
    for number, row in enumerate(rows[1:], 2):
        if len(row) != len(expected_header) or any(not field for field in row):
            fail(f"{path}:{number}: malformed row")
    return rows[1:]


def safe_relative(path: str, label: str) -> None:
    if "\\" in path or path.startswith("/"):
        fail(f"unsafe {label}: {path}")
    parts = PurePosixPath(path).parts
    if not parts or any(part in {"", ".", "..", ".git", ".repo"} for part in parts):
        fail(f"unsafe {label}: {path}")


def manifest_projects(root: ET.Element) -> dict[str, ET.Element]:
    projects: dict[str, ET.Element] = {}
    for project in root.findall("project"):
        name = project.get("name")
        if not name:
            fail("resolved manifest contains a project without a name")
        path = project.get("path", name)
        safe_relative(path, "manifest checkout path")
        if path in projects:
            fail(f"resolved manifest repeats checkout path: {path}")
        projects[path] = project
    return projects


@dataclass(frozen=True)
class SeriesEntry:
    order: int
    filename: str
    patch_id: str
    subject: str


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
    patch_count: int
    series_dir: Path
    entries: tuple[SeriesEntry, ...]


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
        patch_count_text,
        manifest_digest,
        series_dir_text,
    ) = values
    if not order_text.isdecimal() or not patch_count_text.isdecimal():
        fail(f"route {route_id} has a non-numeric order or patch count")
    order = int(order_text)
    patch_count = int(patch_count_text)
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
    if patch_count <= 0 or not HEX64.fullmatch(manifest_digest):
        fail(f"route {route_id} has invalid patch identity")

    route_root = android_root / "patches/projects" / route_id
    metadata = assignments(route_root / "metadata.env")
    expected_metadata = {
        "AOSP_PROJECT_PATCH_SCHEMA": "1",
        "AOSP_ROUTE_ID": route_id,
        "AOSP_MANIFEST_PROJECT": project,
        "AOSP_CHECKOUT_PATH": checkout_path,
        "AOSP_MANIFEST_REVISION": manifest_revision,
        "AOSP_BASE_COMMIT": base_commit,
        "AOSP_BASE_TREE": base_tree,
        "AOSP_FINAL_TREE": final_tree,
        "AOSP_PATCH_COUNT": patch_count_text,
        "AOSP_SERIES_MANIFEST_SHA256": manifest_digest,
        "AOSP_SERIES_DIR": series_dir_text,
    }
    if metadata != expected_metadata:
        fail(f"route metadata differs from projects.tsv: {route_id}")

    series_dir = android_root / series_dir_text
    manifest_path = series_dir / "manifest.tsv"
    if digest(manifest_path) != manifest_digest:
        fail(f"route {route_id} series manifest digest differs from projects.tsv")
    rows = table(manifest_path, SERIES_HEADER)
    if len(rows) != patch_count:
        fail(f"route {route_id} has {len(rows)} patches, expected {patch_count}")

    entries: list[SeriesEntry] = []
    seen_files: set[str] = set()
    seen_ids: set[str] = set()
    for expected_order, row in enumerate(rows, 1):
        row_order, filename, patch_id, patch_digest, subject = row
        if row_order != str(expected_order):
            fail(f"route {route_id} series order is not contiguous")
        if not re.fullmatch(r"[A-Za-z0-9._-]+\.patch", filename):
            fail(f"route {route_id} has unsafe patch filename: {filename}")
        if filename in seen_files or patch_id in seen_ids:
            fail(f"route {route_id} repeats a patch filename or ID")
        if not HEX40.fullmatch(patch_id) or not HEX64.fullmatch(patch_digest):
            fail(f"route {route_id} has malformed patch identity: {filename}")
        patch = series_dir / filename
        payload = patch.read_bytes()
        if hashlib.sha256(payload).hexdigest() != patch_digest:
            fail(f"route {route_id} patch digest mismatch: {filename}")
        actual_ids = [
            line.split()[0]
            for line in run("git", "patch-id", "--stable", payload=payload).splitlines()
            if line.strip()
        ]
        if actual_ids != [patch_id]:
            fail(f"route {route_id} patch ID mismatch: {filename}")
        mail = run("git", "mailinfo", "/dev/null", "/dev/null", payload=payload)
        subjects = [line.removeprefix("Subject: ") for line in mail.splitlines() if line.startswith("Subject: ")]
        if subjects != [subject] or not subject.startswith("UML-ANDROID-LOCAL:"):
            fail(f"route {route_id} subject mismatch or missing local prefix: {filename}")

        changed = 0
        for raw in payload.decode(errors="strict").splitlines():
            match = re.fullmatch(r"diff --git a/(.+) b/(.+)", raw)
            if match is None:
                continue
            changed += 1
            safe_relative(match.group(1), "patch source path")
            safe_relative(match.group(2), "patch destination path")
        if changed == 0:
            fail(f"route {route_id} patch changes no project path: {filename}")
        seen_files.add(filename)
        seen_ids.add(patch_id)
        entries.append(SeriesEntry(expected_order, filename, patch_id, subject))

    actual_patches = {path.name for path in series_dir.glob("*.patch")}
    if actual_patches != seen_files:
        fail(f"route {route_id} patch inventory differs from its series manifest")
    return Route(
        order,
        route_id,
        project,
        checkout_path,
        manifest_revision,
        base_commit,
        base_tree,
        final_tree,
        patch_count,
        series_dir,
        tuple(entries),
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
    ).strip()
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
        head = run("git", "rev-parse", "HEAD", cwd=checkout).strip()
        tree = run("git", "rev-parse", "HEAD^{tree}", cwd=checkout).strip()
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
        commits = run(
            "git", "rev-list", "--reverse", f"{route.base_commit}..HEAD", cwd=checkout
        ).splitlines()
        if len(commits) != route.patch_count:
            fail(f"route {route.route_id} has {len(commits)} commits")
        for commit, entry in zip(commits, route.entries, strict=True):
            subject = run("git", "show", "-s", "--format=%s", commit, cwd=checkout).strip()
            if subject != entry.subject:
                fail(f"route {route.route_id} commit subject differs at {entry.order}")
            shown = subprocess.run(
                ["git", "show", "--format=email", "--full-index", "--binary", commit],
                cwd=checkout,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            if shown.returncode != 0:
                fail(f"cannot inspect route {route.route_id} commit {commit}")
            ids = [
                line.split()[0]
                for line in run("git", "patch-id", "--stable", payload=shown.stdout).splitlines()
                if line.strip()
            ]
            if ids != [entry.patch_id]:
                fail(f"route {route.route_id} commit patch ID differs at {entry.order}")

    if expect == "final":
        sync_tool = android_root / "tools/sync-device-overlay.py"
        if not sync_tool.is_file():
            fail("device-overlay sync tool is missing")
        run(
            str(sync_tool),
            "--android-root",
            str(android_root),
            "--aosp-source",
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

    metadata = assignments(android_root / "metadata/aosp.env")
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
