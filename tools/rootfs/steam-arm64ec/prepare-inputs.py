#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Network stage, invoked only through dependencies/prepare.sh."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import csv
import shutil
from pathlib import Path
import platform
import re
import subprocess
import urllib.request


HERE = Path(__file__).resolve().parent


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download(url: str, path: Path, expected: str | None = None, *, fetch: bool = True) -> dict:
    if path.is_symlink():
        raise RuntimeError(f"refusing symlink input: {path}")
    if not path.is_file():
        if not fetch:
            raise RuntimeError(f"prepared payload is missing: {path}")
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".partial")
        print(f"Downloading {url}", flush=True)
        with urllib.request.urlopen(url, timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        if expected and sha256(temporary) != expected:
            raise RuntimeError(f"downloaded checksum differs: {path.name}")
        temporary.rename(path)
    digest = sha256(path)
    if expected and digest != expected:
        raise RuntimeError(f"cached payload checksum differs: {path}")
    return {"url": url, "file": str(path), "sha256": digest}


def payload_specs(root: Path, source: Path, lock: dict) -> list[tuple[str, Path, str]]:
    payloads = []
    for entry in json.loads((HERE / "downloads.json").read_text()):
        parent = source / "contrib" if entry["group"] == "contrib" else root / "piper-inputs"
        payloads.append((entry["url"], parent / entry["file"], entry["sha256"]))
    manifest = (HERE / "steam-client.manifest").read_text()
    for _, body in re.findall(r'"([^"\n]+)"\s*\{([^{}]+)\}', manifest):
        fields = dict(re.findall(r'"([^"\n]+)"\s*"([^"\n]+)"', body))
        if "file" not in fields:
            continue
        name = fields["file"]
        if Path(name).name != name or not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
            raise RuntimeError("unsafe Steam package name")
        payloads.append((f"https://client-update.steamstatic.com/{name}", root / "steam-packages" / name, fields["sha2"]))
    base, version = lock["steam_runtime_repository"], lock["steam_runtime_snapshot"]
    payloads.append((f"{base}/{version}/steam-runtime-steamrt-arm64.tar.xz", root / "steam-runtime.tar.xz", lock["steam_runtime_sha256"]))
    return payloads


def prepare_payloads(root: Path, source: Path, lock: dict) -> list[dict]:
    payloads = [download(url, path, digest) for url, path, digest in payload_specs(root, source, lock)]
    manifest = root / "steam-client.manifest"
    if not manifest.exists():
        shutil.copy2(HERE / "steam-client.manifest", manifest)
    payloads.append(download("upstream:steam-client-manifest", manifest, lock["steam_manifest_sha256"]))
    cargo = root / "cargo"
    cargo.mkdir(exist_ok=True)
    subprocess.run([
        "sudo", "-n", "podman", "run", "--rm", "--pull=never",
        "-u", f"{os.getuid()}:{os.getgid()}",
        "-v", f"{source}:{source}:ro", "-v", f"{cargo}:{cargo}",
        "-e", f"CARGO_HOME={cargo}", "-w", str(source / "gst-plugins-rs"),
        lock["sdk"], "cargo", "fetch", "--locked",
    ], check=True)
    return payloads


def validate_policy(lock: dict) -> str:
    metadata = HERE.parents[2] / "dependencies/steam-arm64ec"
    env = dict(os.environ, UML_DEPENDENCY_MANIFEST=str(metadata / "manifest.tsv"),
               UML_DEPENDENCY_PATCH_ROOT=str(metadata))
    subprocess.run([str(HERE.parents[2] / "dependencies/prepare.sh"), "--validate-manifest-only"], env=env, check=True)
    with (metadata / "manifest.tsv").open() as stream:
        rows = {row["name"]: row for row in csv.DictReader((line for line in stream if not line.startswith("#")), delimiter="\t")}
    if rows["proton"]["commit"] != lock["proton_commit"] or rows["proton"]["repository"] != lock["proton_repository"]:
        raise RuntimeError("Proton source inventories disagree")
    if not re.fullmatch(r"[0-9a-f]{40}", lock["proton_commit"]):
        raise RuntimeError("Proton must be pinned to a full commit")
    if not re.search(r"@sha256:[0-9a-f]{64}$", lock["sdk"]):
        raise RuntimeError("SDK must be pinned to its OCI digest")
    if sha256(HERE / "steam-client.manifest") != lock["steam_manifest_sha256"]:
        raise RuntimeError("upstream Steam manifest checksum differs")
    for entry in json.loads((HERE / "downloads.json").read_text()):
        if entry["group"] not in {"contrib", "piper-inputs"} or Path(entry["file"]).name != entry["file"]:
            raise RuntimeError("invalid build payload path")
        if not re.fullmatch(r"[0-9a-f]{64}", entry["sha256"]) or not entry["url"].startswith("https://"):
            raise RuntimeError("build payload lacks an HTTPS URL or SHA-256")
    policy = hashlib.sha256()
    files = [HERE / name for name in ("sources.json", "downloads.json", "steam-client.manifest", "prepare-base.sh")]
    files += sorted(path for path in metadata.rglob("*") if path.is_file() and path.suffix in {".tsv", ".patch"})
    for path in files:
        policy.update(path.relative_to(HERE.parents[2]).as_posix().encode())
        policy.update(bytes.fromhex(sha256(path)))
    return policy.hexdigest()


def verify_source(source: Path, lock: dict) -> list[dict]:
    if run("git", "rev-parse", "HEAD", cwd=source) != lock["proton_commit"]:
        raise RuntimeError("prepared Proton commit differs")
    if run("git", "status", "--porcelain", "--untracked-files=no", cwd=source):
        raise RuntimeError("prepared Proton source or submodules have tracked changes")
    modules = []
    for line in run("git", "submodule", "status", "--recursive", cwd=source).splitlines():
        commit, path, *_ = line.split()
        if not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise RuntimeError(f"submodule is missing or differs: {path}")
        modules.append({"path": path, "commit": commit})
    metadata = HERE.parents[2] / "dependencies/steam-arm64ec/manifest.tsv"
    with metadata.open() as stream:
        rows = {row["name"]: row for row in csv.DictReader((line for line in stream if not line.startswith("#")), delimiter="\t")}
    if {entry["path"]: entry["commit"] for entry in modules}.get("wine") != rows["proton-wine"]["commit"]:
        raise RuntimeError("Proton Wine submodule and patch inventory disagree")
    return modules


def run(*args: str, cwd: Path | None = None) -> str:
    return subprocess.check_output(args, cwd=cwd, text=True).strip()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--no-fetch", action="store_true")
    parser.add_argument("--validate-manifest-only", action="store_true")
    args = parser.parse_args()
    lock = json.loads((HERE / "sources.json").read_text())
    policy = validate_policy(lock)
    if args.validate_manifest_only:
        print("Steam source manifest is valid")
        return
    root = args.root.resolve()
    source = root / "proton"
    receipt = root / "inputs.json"
    if args.verify or args.no_fetch:
        recorded = json.loads(receipt.read_text())
        if any(recorded.get(key) != value for key, value in lock.items()):
            raise RuntimeError("prepared input identities differ from the current lock")
        if recorded.get("policy_sha256") != policy:
            raise RuntimeError("prepared input policy differs; run preparation with the current lock")
        if verify_source(source, lock) != recorded["submodules"]:
            raise RuntimeError("prepared submodule inventory differs")
        for url, path, digest in payload_specs(root, source, lock):
            download(url, path, digest, fetch=False)
        recorded_paths = {entry["file"] for entry in recorded["payloads"]}
        for name in ("base.tar.zst", "base.packages.tsv", "steam-client.manifest"):
            if str(root / name) not in recorded_paths:
                raise RuntimeError(f"prepared payload receipt is missing: {name}")
        for entry in recorded["payloads"]:
            path = Path(entry["file"])
            if not path.is_relative_to(root):
                raise RuntimeError("prepared payload is outside the input root")
            download(entry["url"], path, entry["sha256"], fetch=False)
        sdk = json.loads(run("sudo", "-n", "podman", "image", "inspect", lock["sdk"]))[0]
        if sdk["Id"] != recorded["sdk_id"]:
            raise RuntimeError("prepared SDK identity differs")
        print(f"Verified Steam build inputs: {root}")
        return
    if platform.system() != "Linux" or platform.machine() != "aarch64":
        raise RuntimeError("Steam preparation requires native AArch64 Linux")
    root.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        subprocess.run(["git", "init", str(source)], check=True)
        run("git", "remote", "add", "origin", lock["proton_repository"], cwd=source)
        subprocess.run(["git", "fetch", "--depth=1", "origin", lock["proton_commit"]], cwd=source, check=True)
        run("git", "checkout", "--detach", "FETCH_HEAD", cwd=source)
    if run("git", "rev-parse", "HEAD", cwd=source) != lock["proton_commit"]:
        raise RuntimeError("refusing to change an existing source with a different commit")
    if run("git", "remote", "get-url", "origin", cwd=source) != lock["proton_repository"]:
        raise RuntimeError("prepared source origin differs")
    if run("git", "status", "--porcelain", "--untracked-files=no", cwd=source):
        raise RuntimeError("refusing to update a source with tracked changes")
    subprocess.run(["git", "submodule", "update", "--init", "--recursive", "--depth=1", "--jobs=8"], cwd=source, check=True)
    modules = verify_source(source, lock)
    subprocess.run(["sudo", "-n", "apt-get", "update"], check=True)
    subprocess.run([
        "sudo", "-n", "apt-get", "install", "-y", "--no-install-recommends",
        "podman", "uidmap", "slirp4netns", "fuse-overlayfs", "debootstrap",
        "erofs-utils", "rsync", "curl", "ca-certificates", "git", "make",
        "python3", "xz-utils", "zstd", "unzip", "xvfb", "xauth",
    ], check=True)
    if subprocess.run(["sudo", "-n", "podman", "image", "exists", lock["sdk"]]).returncode != 0:
        subprocess.run(["sudo", "-n", "podman", "pull", lock["sdk"]], check=True)
    sdk = json.loads(run("sudo", "-n", "podman", "image", "inspect", lock["sdk"]))[0]
    recorded = dict(lock)
    recorded["policy_sha256"] = policy
    recorded["proton_commit"] = run("git", "rev-parse", "HEAD", cwd=source)
    recorded["submodules"] = modules
    recorded["sdk_id"] = sdk["Id"]
    recorded["sdk_digest"] = sdk["Digest"]
    recorded["payloads"] = prepare_payloads(root, source, lock)
    subprocess.run(["bash", str(HERE / "prepare-base.sh"), str(root)], check=True)
    for name in ("base.tar.zst", "base.packages.tsv"):
        recorded["payloads"].append(download("prepared:debian-trixie-arm64", root / name))
    receipt.write_text(json.dumps(recorded, indent=2) + "\n")
    print(f"Prepared Steam build inputs: {root}")


if __name__ == "__main__":
    main()
