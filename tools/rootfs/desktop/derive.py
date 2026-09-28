#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive a desktop image that carries its own setup from its source image.

Runs as root on an arm64 Linux host with erofs-utils: it checks the source
against images.json, extracts it with ownership, modes and extended
attributes, runs the image's setup script in a chroot of the extracted tree,
and writes a new EROFS image with the source's compressor beside a receipt
naming both images. README.md beside this script owns the contract.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

DIRECTORY = Path(__file__).resolve().parent
DESCRIPTOR = DIRECTORY / "images.json"
SETUP_ENVIRONMENT = {
    "PATH": "/usr/sbin:/usr/bin:/sbin:/bin",
    "HOME": "/root",
    "LC_ALL": "C",
}


class DerivationError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def run(command: list[str], **keywords: object) -> subprocess.CompletedProcess:
    print("+ " + " ".join(command), flush=True)
    return subprocess.run(command, check=True, **keywords)


def superblock(image: Path) -> dict[str, str]:
    output = subprocess.run(
        ["dump.erofs", "-s", str(image)],
        check=True, capture_output=True, text=True,
    ).stdout
    fields = {}
    for line in output.splitlines():
        key, separator, value = line.partition(":")
        if separator:
            fields[key.strip()] = value.strip()
    return fields


def tool_version(tool: str) -> str:
    output = subprocess.run(
        [tool, "--version"], check=True, capture_output=True, text=True
    ).stdout
    return output.splitlines()[0].strip()


def derive(image: str, source: Path, output: Path, work: Path) -> dict:
    descriptor = json.loads(DESCRIPTOR.read_text(encoding="utf-8"))
    try:
        entry = descriptor["images"][image]
    except KeyError as error:
        raise DerivationError(f"images.json names no image {image}") from error
    expected = entry["source"]
    if source.stat().st_size != expected["size"]:
        raise DerivationError(f"{source} is not {expected['file']}: size")
    source_digest = sha256(source)
    if source_digest != expected["sha256"]:
        raise DerivationError(f"{source} is not {expected['file']}: digest")
    if output.exists():
        raise DerivationError(f"refusing to replace {output}")
    receipt_path = output.with_name(output.name + ".json")
    if receipt_path.exists():
        raise DerivationError(f"refusing to replace {receipt_path}")
    setup = DIRECTORY / entry["setup"]

    stage = Path(tempfile.mkdtemp(prefix=f"desktop-{image}-", dir=work))
    root = stage / "root"
    mounted: list[Path] = []
    try:
        run([
            "fsck.erofs", f"--extract={root}", "--xattrs", "--preserve",
            str(source),
        ])
        for target, arguments in (
            (root / "proc", ["-t", "proc", "proc"]),
            (root / "dev", ["--bind", "/dev"]),
        ):
            if not target.is_dir() or target.is_symlink():
                raise DerivationError(f"the image has no {target.name} directory")
            run(["mount", *arguments, str(target)])
            mounted.append(target)
        with setup.open("rb") as script:
            run(
                ["chroot", str(root), "/bin/sh", "-s"],
                stdin=script, env=SETUP_ENVIRONMENT,
            )
        while mounted:
            run(["umount", str(mounted.pop())])
        temporary = output.with_name(f".{output.name}.{os.getpid()}")
        run([
            "mkfs.erofs", f"-z{entry['compression']}", str(temporary),
            str(root),
        ])
        run(["fsck.erofs", str(temporary)])
        os.link(temporary, output)
        temporary.unlink()
    finally:
        while mounted:
            subprocess.run(["umount", str(mounted.pop())], check=False)
        shutil.rmtree(stage)

    source_fields = superblock(source)
    output_fields = superblock(output)
    receipt = {
        "schema_version": 1,
        "image": image,
        "source": {
            "file": expected["file"],
            "size": expected["size"],
            "sha256": source_digest,
            "inodes": int(source_fields["Filesystem inode count"]),
        },
        "setup": {
            "file": entry["setup"],
            "sha256": sha256(setup),
        },
        "output": {
            "file": output.name,
            "size": output.stat().st_size,
            "sha256": sha256(output),
            "inodes": int(output_fields["Filesystem inode count"]),
            "uuid": output_fields["Filesystem UUID"],
            "compression": entry["compression"],
        },
        "tools": {
            "mkfs.erofs": tool_version("mkfs.erofs"),
            "host": f"{platform.system()} {platform.release()} {platform.machine()}",
        },
    }
    receipt_path.write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return receipt


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True,
                        help="an image named in images.json")
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="the new image; never an existing file")
    parser.add_argument("--work", type=Path,
                        help="where the tree is extracted (default: beside "
                             "the output)")
    options = parser.parse_args()
    if os.geteuid() != 0:
        print("derive.py: extraction and the chroot need root", file=sys.stderr)
        return 1
    if not re.fullmatch(r"[A-Za-z0-9._-]+\.erofs", options.output.name):
        print("derive.py: the output must be a plain *.erofs name that a "
              "kernel command line can carry", file=sys.stderr)
        return 1
    output = options.output.resolve()
    work = (options.work or output.parent).resolve()
    try:
        receipt = derive(options.image, options.source.resolve(), output, work)
    except (DerivationError, subprocess.CalledProcessError, OSError) as error:
        print(f"derive.py: {error}", file=sys.stderr)
        return 1
    print(json.dumps(receipt["output"], indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
