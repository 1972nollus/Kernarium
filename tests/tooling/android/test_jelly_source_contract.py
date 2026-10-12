#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from source_fixture import commit, parse_env, run


REPOSITORY = "https://github.com/LineageOS/android_packages_apps_Jelly"


def write_fixture(
    root: Path, android_root: Path, topic: Path, verifier: Path, scripts: Path
) -> str:
    run("git", "init", "-q", str(topic))
    run("git", "switch", "-q", "-c", "kernarium-android17", cwd=topic)
    commit(topic, "app/base.txt", "base\n", "Jelly fixture base", 1700000000)
    base_commit = run("git", "rev-parse", "HEAD", cwd=topic)
    base_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
    commit(
        topic,
        "app/Android.bp",
        'android_app {\n    name: "Jelly",\n    sdk_version: "37",\n}\n',
        "Jelly fixture SDK",
        1700000010,
    )
    commit(
        topic,
        "app/location.txt",
        "location=false\n",
        "Jelly fixture defaults",
        1700000020,
    )

    jelly = android_root / "third_party/jelly"
    (jelly / "series").mkdir(parents=True)
    tools = android_root / "tools"
    tools.mkdir(parents=True)
    shutil.copy2(verifier, tools / "verify-jelly-source.py")
    os.chmod(tools / "verify-jelly-source.py", 0o755)
    shutil.copy2(verifier.with_name("pinned_source.py"), tools / "pinned_source.py")
    artifacts = root / "kernel/common/artifacts"
    artifacts.mkdir(parents=True)
    shutil.copy2(
        verifier.parents[3] / "kernel/common/artifacts/contract_checks.py",
        artifacts / "contract_checks.py",
    )
    (root / "scripts").mkdir()
    shutil.copy2(scripts / "patch_series.py", root / "scripts/patch_series.py")
    (jelly / "metadata.env").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "JELLY_SOURCE_SCHEMA=1\n"
        f"JELLY_UPSTREAM_REPOSITORY='{REPOSITORY}'\n"
        "JELLY_UPSTREAM_BRANCH='lineage-23.2'\n"
        f"JELLY_UPSTREAM_COMMIT='{base_commit}'\n"
        f"JELLY_UPSTREAM_TREE='{base_tree}'\n"
        "JELLY_PATCH_BRANCH='kernarium-android17'\n"
        f"JELLY_FINAL_TREE='{base_tree}'\n"
        "JELLY_DESTINATION='packages/apps/Jelly'\n"
        "JELLY_LICENSE='Apache-2.0'\n"
    )
    return base_commit


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    generator = source_root / "android/lineage-24/tools/regenerate-jelly-patches.py"
    verifier = source_root / "android/lineage-24/tools/verify-jelly-source.py"

    with tempfile.TemporaryDirectory(prefix="jelly-source-contract-") as temporary:
        root = Path(temporary)
        android_root = root / "android/lineage-24"
        topic = root / "topic"
        base_commit = write_fixture(
            root, android_root, topic, verifier, source_root / "scripts"
        )
        command = (
            sys.executable,
            "-B",
            str(generator),
            "--android-root",
            str(android_root),
            "--topic-checkout",
            str(topic),
        )
        run(*command)
        fixture_verifier = android_root / "tools/verify-jelly-source.py"
        run(
            sys.executable,
            "-B",
            str(fixture_verifier),
            "--android-root",
            str(android_root),
        )

        series = android_root / "third_party/jelly/series"
        patches = sorted(series.glob("*.patch"))
        if len(patches) != 2:
            raise SystemExit("Jelly regenerator did not emit the topic's two patches")
        listed = (series / "series.tsv").read_text()
        for name in ("Jelly-fixture-SDK", "Jelly-fixture-defaults"):
            if name not in listed:
                raise SystemExit(f"Jelly series lost patch: {name}")
        metadata = parse_env(android_root / "third_party/jelly/metadata.env")
        if metadata["JELLY_FINAL_TREE"] != run(
            "git", "rev-parse", "HEAD^{tree}", cwd=topic
        ):
            raise SystemExit("Jelly regenerator did not record the topic tree")

        def materialize(aosp: Path) -> Path:
            (aosp / ".repo").mkdir(parents=True)
            destination = aosp / "packages/apps/Jelly"
            destination.parent.mkdir(parents=True)
            run("git", "clone", "-q", str(topic), str(destination))
            run("git", "remote", "set-url", "origin", REPOSITORY, cwd=destination)
            run("git", "switch", "-q", "--detach", base_commit, cwd=destination)
            return destination

        def apply(destination: Path) -> subprocess.CompletedProcess:
            return subprocess.run(
                (
                    sys.executable, "-B", str(root / "scripts/patch_series.py"),
                    "apply", "--repo", str(destination),
                    "--series-dir", str(series),
                    "--expect-tree", metadata["JELLY_FINAL_TREE"],
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )

        aosp = root / "aosp"
        if apply(materialize(aosp)).returncode != 0:
            raise SystemExit("Jelly series did not replay to its recorded tree")
        run(
            sys.executable,
            "-B",
            str(fixture_verifier),
            "--android-root",
            str(android_root),
            "--aosp-source",
            str(aosp),
        )

        stray = series / "stray.patch"
        stray.write_bytes(patches[0].read_bytes())
        rejected = subprocess.run(
            (
                sys.executable,
                "-B",
                str(fixture_verifier),
                "--android-root",
                str(android_root),
            ),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if rejected.returncode == 0:
            raise SystemExit("Jelly verifier accepted a patch its series does not list")
        stray.unlink()

        altered = next(patch for patch in patches if "defaults" in patch.name)
        original = altered.read_bytes()
        altered.write_bytes(original.replace(b"+location=false", b"+location=true", 1))
        if apply(materialize(root / "altered")).returncode == 0:
            raise SystemExit("Jelly replay accepted a patch that changes the recorded tree")
        altered.write_bytes(original)

        metadata_before = (android_root / "third_party/jelly/metadata.env").read_bytes()
        series_before = (series / "series.tsv").read_bytes()
        (topic / "dirty.txt").write_text("dirty\n")
        rejected = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
        if rejected.returncode == 0:
            raise SystemExit("Jelly regenerator accepted a dirty topic checkout")
        if (android_root / "third_party/jelly/metadata.env").read_bytes() != metadata_before:
            raise SystemExit("dirty-topic rejection changed Jelly metadata")
        if (series / "series.tsv").read_bytes() != series_before:
            raise SystemExit("dirty-topic rejection changed the Jelly series")

    print("Android 17 Jelly source contract passed")


if __name__ == "__main__":
    main()
