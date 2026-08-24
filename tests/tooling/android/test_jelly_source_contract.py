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


REPOSITORY = "https://github.com/LineageOS/android_packages_apps_Jelly"
MAINTAINER_NAME = "linux-um-darwin Android patch maintainer"
MAINTAINER_EMAIL = "noreply@linux-um-darwin.invalid"


def run(
    *command: str,
    cwd: Path | None = None,
    payload: bytes | None = None,
    env: dict[str, str] | None = None,
) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        input=payload,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise SystemExit(
            f"Jelly source fixture command failed: {' '.join(command)}\n"
            f"{result.stderr.decode(errors='replace')}"
        )
    return result.stdout.decode(errors="strict").strip()


def commit(topic: Path, path: str, payload: str, subject: str, timestamp: int) -> None:
    destination = topic / path
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(payload)
    run("git", "add", path, cwd=topic)
    environment = os.environ.copy()
    date = f"{timestamp} +0000"
    environment["GIT_AUTHOR_DATE"] = date
    environment["GIT_COMMITTER_DATE"] = date
    run("git", "commit", "-q", "-m", subject, cwd=topic, env=environment)


def parse_env(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        result[key] = value.strip("'")
    return result


def write_fixture(android_root: Path, topic: Path, verifier: Path) -> str:
    run("git", "init", "-q", str(topic))
    run("git", "config", "user.name", MAINTAINER_NAME, cwd=topic)
    run("git", "config", "user.email", MAINTAINER_EMAIL, cwd=topic)
    run("git", "switch", "-q", "-c", "kernarium-android17", cwd=topic)
    commit(topic, "app/base.txt", "base\n", "Jelly fixture base", 1700000000)
    base_commit = run("git", "rev-parse", "HEAD", cwd=topic)
    base_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
    commit(
        topic,
        "app/Android.bp",
        'android_app {\n    name: "Jelly",\n    sdk_version: "37",\n}\n',
        "UML-ANDROID-LOCAL: Jelly fixture SDK",
        1700000010,
    )
    commit(
        topic,
        "app/location.txt",
        "location=false\n",
        "UML-ANDROID-LOCAL: Jelly fixture defaults",
        1700000020,
    )

    jelly = android_root / "third_party/jelly"
    (jelly / "series").mkdir(parents=True)
    tools = android_root / "tools"
    tools.mkdir(parents=True)
    shutil.copy2(verifier, tools / "verify-jelly-source.py")
    os.chmod(tools / "verify-jelly-source.py", 0o755)
    (jelly / "metadata.env").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "JELLY_SOURCE_SCHEMA=1\n"
        f"JELLY_UPSTREAM_REPOSITORY='{REPOSITORY}'\n"
        "JELLY_UPSTREAM_BRANCH='lineage-23.2'\n"
        f"JELLY_UPSTREAM_COMMIT='{base_commit}'\n"
        f"JELLY_UPSTREAM_TREE='{base_tree}'\n"
        "JELLY_PATCH_BRANCH='kernarium-android17'\n"
        f"JELLY_FINAL_COMMIT='{base_commit}'\n"
        f"JELLY_FINAL_TREE='{base_tree}'\n"
        "JELLY_PATCH_COUNT=1\n"
        "JELLY_DESTINATION='packages/apps/Jelly'\n"
        "JELLY_LICENSE='Apache-2.0'\n"
    )
    return base_commit


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    generator = source_root / "android/aosp-17/tools/regenerate-jelly-patches.py"
    verifier = source_root / "android/aosp-17/tools/verify-jelly-source.py"

    with tempfile.TemporaryDirectory(prefix="jelly-source-contract-") as temporary:
        root = Path(temporary)
        android_root = root / "android/aosp-17"
        topic = root / "topic"
        base_commit = write_fixture(android_root, topic, verifier)
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
        manifest = (series / "manifest.tsv").read_text()
        for subject in (
            "UML-ANDROID-LOCAL: Jelly fixture SDK",
            "UML-ANDROID-LOCAL: Jelly fixture defaults",
        ):
            if subject not in manifest:
                raise SystemExit(f"Jelly series manifest lost subject: {subject}")

        aosp = root / "aosp"
        (aosp / ".repo").mkdir(parents=True)
        destination = aosp / "packages/apps/Jelly"
        destination.parent.mkdir(parents=True)
        run("git", "clone", "-q", str(topic), str(destination))
        run("git", "remote", "set-url", "origin", REPOSITORY, cwd=destination)
        run("git", "switch", "-q", "--detach", base_commit, cwd=destination)
        for patch in patches:
            run(
                "git",
                "-c",
                f"user.name={MAINTAINER_NAME}",
                "-c",
                f"user.email={MAINTAINER_EMAIL}",
                "am",
                "--committer-date-is-author-date",
                str(patch),
                cwd=destination,
            )
        run(
            sys.executable,
            "-B",
            str(fixture_verifier),
            "--android-root",
            str(android_root),
            "--aosp-source",
            str(aosp),
        )
        metadata = parse_env(android_root / "third_party/jelly/metadata.env")
        if run("git", "rev-parse", "HEAD", cwd=destination) != metadata[
            "JELLY_FINAL_COMMIT"
        ]:
            raise SystemExit("deterministic Jelly replay changed final commit identity")

        original = patches[0].read_bytes()
        patches[0].write_bytes(original.replace(b"fixture SDK", b"fixture sdk", 1))
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
            raise SystemExit("Jelly verifier accepted altered generated patch bytes")
        patches[0].write_bytes(original)

        metadata_before = (android_root / "third_party/jelly/metadata.env").read_bytes()
        manifest_before = (series / "manifest.tsv").read_bytes()
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
        if (series / "manifest.tsv").read_bytes() != manifest_before:
            raise SystemExit("dirty-topic rejection changed the Jelly series")

    print("Android 17 Jelly source contract passed")


if __name__ == "__main__":
    main()
