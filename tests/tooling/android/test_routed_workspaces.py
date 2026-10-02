#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the AOSP and OrangeFox source verifiers against routed Repo workspaces.

A fake `repo` on PATH answers the two commands the verifiers use: `manifest
-r`, which reports every listed project at its checked-out HEAD, and `forall
-c`, which runs a shell command in each project with REPO_PATH set. Each
workspace has one routed project with a generated series, unrouted projects
at their pinned revisions, and the synchronized device tree.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from source_fixture import commit, parse_env, run


FAKE_REPO = '''\
import os
import subprocess
import sys
from pathlib import Path

here = Path.cwd().resolve()
workspace = next((d for d in (here, *here.parents) if (d / ".repo").is_dir()), None)
if workspace is None:
    sys.exit("fake repo: not inside a Repo workspace")
projects = [
    line.split("\\t")
    for line in (workspace / ".repo/fixture-projects.tsv").read_text().splitlines()
    if line
]
if sys.argv[1:] == ["manifest", "-r"]:
    print("<manifest>")
    for name, path, upstream in projects:
        revision = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=workspace / path,
            check=True, stdout=subprocess.PIPE, text=True,
        ).stdout.strip()
        print(f'  <project name="{name}" path="{path}" revision="{revision}" '
              f'upstream="{upstream}"/>')
    print("</manifest>")
elif len(sys.argv) == 4 and sys.argv[1:3] == ["forall", "-c"]:
    for name, path, _ in projects:
        environment = dict(os.environ, REPO_PATH=path, REPO_PROJECT=name)
        status = subprocess.run(
            ["sh", "-c", sys.argv[3]], cwd=workspace / path, env=environment
        ).returncode
        if status:
            sys.exit(status)
else:
    sys.exit(f"fake repo: unsupported arguments {sys.argv[1:]}")
'''


def fail(message: str) -> None:
    raise SystemExit(f"routed workspace fixture: {message}")


def project(workspace: Path, path: str, timestamp: int) -> tuple[str, str]:
    """Create a checkout with one base commit; return its commit and tree."""
    checkout = workspace / path
    checkout.mkdir(parents=True)
    run("git", "init", "-q", str(checkout))
    commit(checkout, "base.txt", f"{path} base\n", f"{path}: base", timestamp)
    return (
        run("git", "rev-parse", "HEAD", cwd=checkout),
        run("git", "rev-parse", "HEAD^{tree}", cwd=checkout),
    )


def topic(checkout: Path, timestamp: int) -> tuple[str, str]:
    """Add one topic commit; return the topic commit and its tree."""
    commit(checkout, "topic.txt", "topic\n", "fixture: topic", timestamp)
    return (
        run("git", "rev-parse", "HEAD", cwd=checkout),
        run("git", "rev-parse", "HEAD^{tree}", cwd=checkout),
    )


def export(checkout: Path, base: str, series: Path, license_id: str) -> None:
    import patch_series  # noqa: PLC0415

    series.mkdir(parents=True)
    patch_series.export_series(checkout, base, "HEAD", series, license_id=license_id)


def write_projects(workspace: Path, rows: list[tuple[str, str, str]]) -> None:
    (workspace / ".repo/fixture-projects.tsv").write_text(
        "".join(f"{name}\t{path}\t{upstream}\n" for name, path, upstream in rows)
    )


def write_manifest(path: Path, rows: list[tuple[str, str, str, str]]) -> str:
    text = "<manifest>\n" + "".join(
        f'  <project name="{name}" path="{checkout}" revision="{revision}" '
        f'upstream="{upstream}"/>\n'
        for name, checkout, revision, upstream in rows
    ) + "</manifest>\n"
    path.write_text(text)
    return hashlib.sha256(text.encode()).hexdigest()


def copy_device_tree(source: Path, workspace: Path) -> None:
    destination = workspace / "device/kernarium/darwin"
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(source, destination, copy_function=shutil.copy2)


class Verifier:
    def __init__(self, tool: Path, android_root: Path, source_option: str,
                 workspace: Path, environment: dict[str, str]) -> None:
        self.tool = tool
        self.android_root = android_root
        self.source_option = source_option
        self.workspace = workspace
        self.environment = environment

    def run(self, expect: str, *extra: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            (
                sys.executable, "-B", str(self.tool),
                "--android-root", str(self.android_root),
                self.source_option, str(self.workspace),
                "--expect", expect, *extra,
            ),
            env=self.environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )

    def accepts(self, case: str, expect: str, *extra: str) -> None:
        result = self.run(expect, *extra)
        if result.returncode != 0:
            fail(f"{self.tool.name} refused {case}: {result.stdout.strip()}")

    def refuses(self, case: str, expected: str, expect: str, *extra: str) -> None:
        result = self.run(expect, *extra)
        if result.returncode == 0 or expected not in result.stdout:
            fail(
                f"{self.tool.name} did not refuse {case} with '{expected}': "
                f"{result.stdout.strip()}"
            )


def detach(checkout: Path, revision: str) -> None:
    run("git", "checkout", "-q", "--detach", revision, cwd=checkout)


def check_route_states(verifier: Verifier, routes: list[tuple[Path, str, str]],
                       label: str, *final_extra: str) -> None:
    """The base/final rules, the dirty check and the ancestry check.

    Each route is (checkout, base commit, final commit); the first one is
    the route the negative cases change.
    """
    route, base, final = routes[0]
    for checkout, route_base, _ in routes:
        detach(checkout, route_base)
    verifier.accepts(f"the {label} routes at their bases", "base")
    verifier.refuses(
        f"the {label} routes at their bases for --expect final",
        "p0001", "final", *final_extra,
    )
    for checkout, _, route_final in routes:
        detach(checkout, route_final)
    verifier.accepts(f"the {label} routes at their final trees", "final", *final_extra)
    verifier.refuses(
        f"the {label} routes past their bases for --expect base",
        "route p0001 is not at its exact base", "base",
    )

    (route / "untracked.txt").write_text("dirty\n")
    verifier.refuses(f"a dirty {label} project", "build/make", "final", *final_extra)
    (route / "untracked.txt").unlink()

    # The same final tree on a history that does not contain the base.
    orphan = run(
        "git", "commit-tree", f"{final}^{{tree}}", "-m", "fixture: orphan", cwd=route
    )
    detach(route, orphan)
    verifier.refuses(
        f"a {label} route that is not based on its pin",
        "route p0001 is not based on its pinned base", "final", *final_extra,
    )
    detach(route, final)
    verifier.accepts(f"the restored {label} routes", "final", *final_extra)


def check_route_checkout(verifier: Verifier, root: Path, workspace: Path,
                         projects: list[tuple[str, str, str]], label: str) -> None:
    """The first route must keep its manifest name and stay inside the workspace."""
    name, path, upstream = projects[0]
    write_projects(workspace, [(f"{name}-renamed", path, upstream), *projects[1:]])
    verifier.refuses(
        f"a {label} route whose current manifest names another project",
        "route p0001 maps to the wrong manifest project", "final",
    )
    write_projects(workspace, projects)

    route = workspace / path
    outside = root / f"outside-{label}"
    route.rename(outside)
    route.symlink_to(outside)
    verifier.refuses(
        f"a {label} route checkout outside the workspace",
        f"route p0001 checkout escapes the {label} source", "final",
    )
    route.unlink()
    outside.rename(route)


def check_aosp(source_root: Path, root: Path, environment: dict[str, str]) -> None:
    android_root = root / "aosp-policy"
    workspace = root / "aosp"
    (workspace / ".repo/manifests").mkdir(parents=True)
    route_base, route_base_tree = project(workspace, "build/make", 1700000000)
    pinned, _ = project(workspace, "external/zstd", 1700000010)
    route = workspace / "build/make"
    route_final, route_final_tree = topic(route, 1700000020)
    projects = [
        ("platform/build", "build/make", "refs/tags/fixture"),
        ("platform/external/zstd", "external/zstd", "refs/tags/fixture"),
    ]
    write_projects(workspace, projects)

    (android_root / "metadata").mkdir(parents=True)
    (android_root / "tools").mkdir()
    shutil.copy2(
        source_root / "android/aosp-17/tools/sync-device-tree.py",
        android_root / "tools/sync-device-tree.py",
    )
    overlay = android_root / "overlay"
    overlay.mkdir()
    (overlay / "device.mk").write_text("# fixture overlay\n")
    copy_device_tree(overlay, workspace)
    digest = write_manifest(android_root / "metadata/base-manifest.xml", [
        ("platform/build", "build/make", route_base, "refs/tags/fixture"),
        ("platform/external/zstd", "external/zstd", pinned, "refs/tags/fixture"),
    ])
    (android_root / "metadata/aosp.env").write_text(
        "AOSP_SOURCE_SCHEMA=1\n"
        f"ANDROID17_BASE_MANIFEST_SHA256='{digest}'\n"
    )
    export(route, route_base, android_root / "patches/projects/p0001/series", "Apache-2.0")
    (android_root / "patches/projects.tsv").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "order\troute_id\tmanifest_project\tcheckout_path\tmanifest_revision\t"
        "base_commit\tbase_tree\tfinal_tree\tseries_dir\n"
        f"1\tp0001\tplatform/build\tbuild/make\trefs/tags/fixture\t"
        f"{route_base}\t{route_base_tree}\t{route_final_tree}\t"
        "patches/projects/p0001/series\n"
    )

    verifier = Verifier(
        source_root / "android/aosp-17/tools/verify-aosp-patches.py",
        android_root, "--aosp-source", workspace, environment,
    )
    check_route_states(verifier, [(route, route_base, route_final)], "AOSP")

    commit(workspace / "external/zstd", "moved.txt", "moved\n", "moved", 1700000030)
    verifier.refuses(
        "a moved unrouted AOSP project",
        "unrouted AOSP project moved from its pinned revision: external/zstd",
        "final",
    )
    detach(workspace / "external/zstd", pinned)

    project(workspace, "external/extra", 1700000040)
    write_projects(workspace, [*projects, ("platform/external/extra", "external/extra", "x")])
    verifier.refuses(
        "an AOSP project the base manifest lacks",
        "Repo project paths differ", "final",
    )
    write_projects(workspace, projects)

    (workspace / "device/kernarium/darwin/device.mk").unlink()
    verifier.refuses(
        "an unsynchronized AOSP device overlay", "destination inventory differs", "final"
    )
    copy_device_tree(overlay, workspace)

    check_route_checkout(verifier, root, workspace, projects, "AOSP")
    verifier.accepts("the restored AOSP workspace", "final")


def check_orangefox(source_root: Path, root: Path, environment: dict[str, str]) -> None:
    real = parse_env(source_root / "android/aosp-17/third_party/orangefox/metadata.env")
    android_root = root / "orangefox-policy"
    workspace = root / "orangefox"
    manifests = workspace / ".repo/manifests"
    manifests.mkdir(parents=True)
    run("git", "init", "-q", str(manifests))
    commit(manifests, "default.xml", "<manifest/>\n", "manifest", 1700000000)
    route_base, route_base_tree = project(workspace, "build/make", 1700000010)
    recovery_base, recovery_base_tree = project(workspace, "bootable/recovery", 1700000020)
    vendor, vendor_tree = project(workspace, "vendor/recovery", 1700000030)
    omapi, omapi_tree = project(workspace, "external/se_omapi", 1700000040)
    route = workspace / "build/make"
    recovery = workspace / "bootable/recovery"
    route_final, route_final_tree = topic(route, 1700000050)
    recovery_final, recovery_final_tree = topic(recovery, 1700000060)
    for path, key in (
        ("bootable/recovery", "ORANGEFOX_BOOTABLE_RECOVERY_REPOSITORY"),
        ("vendor/recovery", "ORANGEFOX_VENDOR_RECOVERY_REPOSITORY"),
        ("external/se_omapi", "ORANGEFOX_SE_OMAPI_REPOSITORY"),
    ):
        run("git", "remote", "add", "origin", real[key], cwd=workspace / path)
    projects = [
        ("android_build", "build/make", "android-14"),
        ("android_bootable_recovery", "bootable/recovery", "android-14"),
        ("android_vendor_recovery", "vendor/recovery", "android-14"),
        ("android_external_se_omapi", "external/se_omapi", "android-14"),
    ]
    write_projects(workspace, projects)

    orangefox = android_root / "third_party/orangefox"
    orangefox.mkdir(parents=True)
    (android_root / "tools").mkdir()
    shutil.copy2(
        source_root / "android/aosp-17/tools/sync-device-tree.py",
        android_root / "tools/sync-device-tree.py",
    )
    device = orangefox / "device"
    device.mkdir()
    (device / "device.mk").write_text("# fixture device\n")
    copy_device_tree(device, workspace)
    digest = write_manifest(orangefox / "base-manifest.xml", [
        ("android_build", "build/make", route_base, "android-14"),
        ("android_bootable_recovery", "bootable/recovery", recovery_base, "android-14"),
        ("android_vendor_recovery", "vendor/recovery", vendor, "android-14"),
        ("android_external_se_omapi", "external/se_omapi", omapi, "android-14"),
    ])
    values = dict(real)
    values.update({
        "ORANGEFOX_MANIFEST_COMMIT": run("git", "rev-parse", "HEAD", cwd=manifests),
        "ORANGEFOX_MANIFEST_TREE": run("git", "rev-parse", "HEAD^{tree}", cwd=manifests),
        "ORANGEFOX_BASE_MANIFEST_SHA256": digest,
        "ORANGEFOX_BOOTABLE_RECOVERY_COMMIT": recovery_base,
        "ORANGEFOX_BOOTABLE_RECOVERY_TREE": recovery_base_tree,
        "ORANGEFOX_VENDOR_RECOVERY_COMMIT": vendor,
        "ORANGEFOX_VENDOR_RECOVERY_TREE": vendor_tree,
        "ORANGEFOX_SE_OMAPI_COMMIT": omapi,
        "ORANGEFOX_SE_OMAPI_TREE": omapi_tree,
    })
    (orangefox / "metadata.env").write_text(
        "".join(f"{key}='{value}'\n" for key, value in values.items())
    )
    rows = []
    for order, (name, path, base, base_tree, final_tree) in enumerate((
        ("android_build", "build/make", route_base, route_base_tree, route_final_tree),
        ("android_bootable_recovery", "bootable/recovery", recovery_base,
         recovery_base_tree, recovery_final_tree),
    ), 1):
        series = f"third_party/orangefox/patches/projects/p{order:04d}/series"
        export(workspace / path, base, android_root / series, "GPL-3.0-or-later")
        rows.append(
            f"{order}\tp{order:04d}\t{name}\t{path}\tandroid-14\t"
            f"{base}\t{base_tree}\t{final_tree}\t{series}\n"
        )
    (orangefox / "patches/projects.tsv").write_text(
        "# SPDX-License-Identifier: GPL-3.0-or-later\n"
        "order\troute_id\tmanifest_project\tcheckout_path\tmanifest_upstream\t"
        "base_commit\tbase_tree\tfinal_tree\tseries_dir\n" + "".join(rows)
    )

    verifier = Verifier(
        source_root / "android/aosp-17/tools/verify-orangefox-source.py",
        android_root, "--orangefox-source", workspace, environment,
    )
    check_route_states(verifier, [
        (route, route_base, route_final),
        (recovery, recovery_base, recovery_final),
    ], "OrangeFox")
    detach(route, route_base)
    verifier.refuses(
        "a second OrangeFox route past its base for --expect base",
        "route p0002 is not at its exact base", "base",
    )
    detach(route, route_final)

    kernel = workspace / "device/kernarium/darwin/prebuilt/kernel"
    kernel.parent.mkdir()
    kernel.write_bytes(b"kernel")
    kernel.chmod(0o755)
    verifier.refuses(
        "a prebuilt OrangeFox kernel without --allow-prebuilt-kernel",
        "destination inventory differs", "final",
    )
    verifier.accepts(
        "a prebuilt OrangeFox kernel with --allow-prebuilt-kernel",
        "final", "--allow-prebuilt-kernel",
    )
    shutil.rmtree(kernel.parent)

    commit(workspace / "external/se_omapi", "moved.txt", "moved\n", "moved", 1700000070)
    verifier.refuses(
        "a moved unrouted OrangeFox project",
        "external/se_omapi", "final",
    )
    detach(workspace / "external/se_omapi", omapi)

    run("git", "remote", "set-url", "origin", "https://example.invalid/recovery.git",
        cwd=workspace / "vendor/recovery")
    verifier.refuses(
        "an OrangeFox external with another origin",
        "OrangeFox external origin differs: vendor/recovery", "final",
    )
    run("git", "remote", "set-url", "origin", real["ORANGEFOX_VENDOR_RECOVERY_REPOSITORY"],
        cwd=workspace / "vendor/recovery")

    commit(manifests, "default.xml", "<manifest></manifest>\n", "moved", 1700000080)
    verifier.refuses(
        "a moved OrangeFox manifest checkout",
        "OrangeFox manifest checkout moved from its pin", "final",
    )
    run("git", "reset", "-q", "--hard", "HEAD~1", cwd=manifests)

    project(workspace, "external/extra", 1700000090)
    write_projects(workspace, [*projects, ("android_external_extra", "external/extra", "x")])
    verifier.refuses(
        "an OrangeFox project the base manifest lacks",
        "Repo project paths differ", "final",
    )
    write_projects(workspace, projects)
    check_route_checkout(verifier, root, workspace, projects, "OrangeFox")
    verifier.accepts("the restored OrangeFox workspace", "final")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    sys.path.insert(0, str(source_root / "scripts"))

    with tempfile.TemporaryDirectory(prefix="routed-workspaces-") as temporary:
        root = Path(temporary).resolve()
        tools = root / "bin"
        tools.mkdir()
        (tools / "repo").write_text(f"#!{sys.executable}\n{FAKE_REPO}")
        (tools / "repo").chmod(0o755)
        environment = dict(os.environ, PATH=f"{tools}{os.pathsep}{os.environ['PATH']}")
        check_aosp(source_root, root, environment)
        check_orangefox(source_root, root, environment)

    print("routed AOSP and OrangeFox workspace verifiers passed")


if __name__ == "__main__":
    main()
