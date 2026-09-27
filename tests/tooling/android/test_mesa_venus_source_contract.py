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


REPOSITORY = "https://gitlab.freedesktop.org/mesa/mesa.git"
GUEST_SERIES = "tools/rootfs/vulkan/patches/mesa"


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
            f"Mesa Venus source fixture command failed: {' '.join(command)}\n"
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
    run(
        "git",
        "commit",
        "-q",
        "-m",
        subject,
        cwd=topic,
        env=environment,
    )


def parse_env(path: Path) -> dict[str, str]:
    result: dict[str, str] = {}
    for raw in path.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        key, value = line.split("=", 1)
        result[key] = value.strip("'")
    return result


def write_guest_series(
    source_root: Path, scripts: Path, topic: Path, base: str, head: str
) -> None:
    """Export the fixture's guest commits as the guest series this port pins."""
    fixture_scripts = source_root / "scripts"
    fixture_scripts.mkdir()
    (source_root / GUEST_SERIES).mkdir(parents=True)
    shutil.copy2(scripts / "patch_series.py", fixture_scripts / "patch_series.py")
    run(
        sys.executable, "-B", str(fixture_scripts / "patch_series.py"), "export",
        "--source", str(topic), "--base", base, "--head", head,
        "--series-dir", str(source_root / GUEST_SERIES), "--tree",
    )


def write_fixture(
    source_root: Path, android_root: Path, topic: Path, verifier: Path
) -> tuple[str, str]:
    run("git", "init", "-q", str(topic))
    run("git", "switch", "-q", "-c", "kernarium-android17", cwd=topic)
    commit(topic, "VERSION", "26.2.1\n", "VERSION: fixture upstream", 1700000000)
    upstream_commit = run("git", "rev-parse", "HEAD", cwd=topic)
    upstream_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
    commit(topic, "src/virtio/vulkan/vn_a.c", "a\n", "venus: fixture", 1700000010)
    commit(topic, "src/virtio/vulkan/vn_b.c", "b\n", "venus: fixture two", 1700000020)
    guest_tree = run("git", "rev-parse", "HEAD^{tree}", cwd=topic)
    guest_commit = run("git", "rev-parse", "HEAD", cwd=topic)
    write_guest_series(
        source_root, verifier.parents[3] / "scripts", topic, upstream_commit, guest_commit
    )
    commit(
        topic,
        "src/virtio/venus-protocol/vn_protocol_driver.h",
        "/* paired */\n",
        "Mesa Venus pair the generated protocol headers",
        1700000030,
    )
    commit(
        topic,
        "src/virtio/vulkan/Android.bp",
        'cc_library_shared {\n    name: "vulkan.virtio",\n}\n',
        "Mesa Venus generated Soong port",
        1700000040,
    )

    venus = android_root / "third_party/mesa-venus"
    (venus / "series").mkdir(parents=True)
    tools = android_root / "tools"
    tools.mkdir(parents=True)
    shutil.copy2(verifier, tools / "verify-mesa-venus-source.py")
    os.chmod(tools / "verify-mesa-venus-source.py", 0o755)
    artifacts = source_root / "kernel/common/artifacts"
    artifacts.mkdir(parents=True)
    shutil.copy2(
        verifier.parents[3] / "kernel/common/artifacts/contract_checks.py",
        artifacts / "contract_checks.py",
    )
    (venus / "metadata.env").write_text(
        "# SPDX-License-Identifier: Apache-2.0\n"
        "MESA_VENUS_SOURCE_SCHEMA=1\n"
        f"MESA_VENUS_UPSTREAM_REPOSITORY='{REPOSITORY}'\n"
        "MESA_VENUS_UPSTREAM_RELEASE='mesa-26.2.1'\n"
        f"MESA_VENUS_UPSTREAM_COMMIT='{upstream_commit}'\n"
        f"MESA_VENUS_UPSTREAM_TREE='{upstream_tree}'\n"
        "MESA_VENUS_GUEST_EPOCH='source-22'\n"
        f"MESA_VENUS_GUEST_SERIES='{GUEST_SERIES}'\n"
        f"MESA_VENUS_GUEST_TREE='{guest_tree}'\n"
        "MESA_VENUS_PATCH_BRANCH='kernarium-android17'\n"
        f"MESA_VENUS_FINAL_TREE='{guest_tree}'\n"
        "MESA_VENUS_DESTINATION='external/mesa3d-venus'\n"
        "MESA_VENUS_HAL_MODULE='vulkan.virtio'\n"
        "MESA_VENUS_LICENSE='MIT'\n"
    )
    return upstream_commit, guest_commit


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    args = parser.parse_args()
    source_root = args.source_root.resolve()
    generator = source_root / "android/aosp-17/tools/regenerate-mesa-venus-patches.py"
    verifier = source_root / "android/aosp-17/tools/verify-mesa-venus-source.py"

    with tempfile.TemporaryDirectory(prefix="mesa-venus-contract-") as temporary:
        root = Path(temporary)
        android_root = root / "android/aosp-17"
        topic = root / "topic"
        upstream_commit, guest_commit = write_fixture(
            root, android_root, topic, verifier
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
        fixture_verifier = android_root / "tools/verify-mesa-venus-source.py"
        run(
            sys.executable,
            "-B",
            str(fixture_verifier),
            "--android-root",
            str(android_root),
        )

        series = android_root / "third_party/mesa-venus/series"
        patches = sorted(series.glob("*.patch"))
        if len(patches) != 2:
            raise SystemExit(
                "the regenerator did not emit exactly the Android port commits"
            )
        listed = (series / "series.tsv").read_text()
        for name in (
            "Mesa-Venus-pair-the-generated-protocol-headers",
            "Mesa-Venus-generated-Soong-port",
        ):
            if name not in listed:
                raise SystemExit(f"Mesa Venus series lost patch: {name}")
        if "venus-fixture" in listed:
            raise SystemExit("the Android series absorbed the pinned guest series")
        final_tree = parse_env(android_root / "third_party/mesa-venus/metadata.env")[
            "MESA_VENUS_FINAL_TREE"
        ]
        if final_tree != run("git", "rev-parse", "HEAD^{tree}", cwd=topic):
            raise SystemExit("the regenerator did not record the topic tree")

        def materialize(aosp: Path) -> Path:
            (aosp / ".repo").mkdir(parents=True)
            destination = aosp / "external/mesa3d-venus"
            destination.parent.mkdir(parents=True)
            run(
                "git", "clone", "-q", "-b", "kernarium-android17",
                str(topic), str(destination),
            )
            run("git", "remote", "set-url", "origin", REPOSITORY, cwd=destination)
            run("git", "reset", "-q", "--hard", guest_commit, cwd=destination)
            return destination

        def apply(destination: Path) -> subprocess.CompletedProcess:
            return subprocess.run(
                (
                    sys.executable, "-B", str(root / "scripts/patch_series.py"),
                    "apply", "--repo", str(destination), "--series-dir", str(series),
                    "--expect-tree", final_tree,
                ),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )

        aosp = root / "aosp"
        if apply(materialize(aosp)).returncode != 0:
            raise SystemExit("the Mesa Venus series did not replay to its recorded tree")
        run(
            sys.executable,
            "-B",
            str(fixture_verifier),
            "--android-root",
            str(android_root),
            "--aosp-source",
            str(aosp),
        )
        metadata = parse_env(android_root / "third_party/mesa-venus/metadata.env")
        if metadata["MESA_VENUS_UPSTREAM_COMMIT"] != upstream_commit:
            raise SystemExit("the regenerator moved the pinned upstream commit")

        # A guest series that moves under the recorded pin must be refused.
        guest = root / GUEST_SERIES / "series.tsv"
        original_guest = guest.read_bytes()
        guest.write_bytes(original_guest.replace(
            f"tree\t{metadata['MESA_VENUS_GUEST_TREE']}".encode(),
            f"tree\t{metadata['MESA_VENUS_UPSTREAM_TREE']}".encode(),
        ))
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
            raise SystemExit("the verifier accepted a moved guest Mesa series")
        guest.write_bytes(original_guest)

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
            raise SystemExit("the verifier accepted a patch its series does not list")
        stray.unlink()

        altered = next(patch for patch in patches if "protocol-headers" in patch.name)
        original = altered.read_bytes()
        altered.write_bytes(original.replace(b"+/* paired */", b"+/* PAIRED */", 1))
        if apply(materialize(root / "altered")).returncode == 0:
            raise SystemExit("the replay accepted a patch that changes the recorded tree")
        altered.write_bytes(original)

        metadata_before = (
            android_root / "third_party/mesa-venus/metadata.env"
        ).read_bytes()
        series_before = (series / "series.tsv").read_bytes()
        (topic / "dirty.txt").write_text("dirty\n")
        rejected = subprocess.run(
            command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
        )
        if rejected.returncode == 0:
            raise SystemExit("the regenerator accepted a dirty topic checkout")
        if (
            android_root / "third_party/mesa-venus/metadata.env"
        ).read_bytes() != metadata_before:
            raise SystemExit("dirty-topic rejection changed Mesa Venus metadata")
        if (series / "series.tsv").read_bytes() != series_before:
            raise SystemExit("dirty-topic rejection changed the Mesa Venus series")

    print("Android 17 Mesa Venus source contract passed")


if __name__ == "__main__":
    main()
