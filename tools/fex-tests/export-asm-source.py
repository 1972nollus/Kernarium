#!/usr/bin/env python3
"""Export only the pinned FEX files needed to assemble its ASM corpus."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import tempfile


REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = REPOSITORY / "dependencies" / "manifest.tsv"
FIXED_FILES = (
    pathlib.Path("LICENSE"),
    pathlib.Path("unittests/Readme.md"),
    pathlib.Path("Scripts/json_asm_config_parse.py"),
    pathlib.Path("Scripts/json_config_parse.py"),
)


def fail(message: str) -> "NoReturn":
    raise SystemExit(f"export-asm-source.py: {message}")


def fex_identity() -> tuple[str, str]:
    for row in MANIFEST.read_text().splitlines():
        if not row or row.startswith("#") or row.startswith("name\t"):
            continue
        fields = row.split("\t")
        if len(fields) == 7 and fields[0] == "fex":
            return fields[1], fields[4]
    fail("the dependency manifest has no FEX input")


def git_output(source: pathlib.Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(source), *arguments],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return result.stdout.strip()


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def export(source: pathlib.Path, output: pathlib.Path) -> None:
    version, expected_commit = fex_identity()
    if not (source / ".git").is_dir():
        fail(f"FEX source is not a prepared Git checkout: {source}")
    if git_output(source, "rev-parse", "HEAD") != expected_commit:
        fail("FEX source is not at the dependency-manifest commit")
    if git_output(source, "status", "--porcelain", "--untracked-files=all"):
        fail("FEX source checkout is dirty")
    if output.exists() or output.is_symlink():
        fail(f"output already exists: {output}")

    asm_files = sorted((source / "unittests" / "ASM").rglob("*"))
    relative_files = list(FIXED_FILES)
    relative_files.extend(
        path.relative_to(source) for path in asm_files if path.is_file()
    )
    if len(relative_files) < 2000:
        fail("the pinned FEX ASM source inventory is unexpectedly small")

    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = pathlib.Path(
        tempfile.mkdtemp(prefix=f".{output.name}.", dir=output.parent)
    )
    try:
        records: list[dict[str, str]] = []
        for relative in relative_files:
            source_file = source / relative
            if not source_file.is_file():
                fail(f"missing pinned FEX input: {relative}")
            destination = temporary / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source_file, destination)
            records.append(
                {"path": relative.as_posix(), "sha256": sha256(destination)}
            )
        identity = {
            "schema": 1,
            "source": "https://github.com/FEX-Emu/FEX.git",
            "version": version,
            "commit": expected_commit,
            "files": records,
        }
        (temporary / "fex-source.json").write_text(
            json.dumps(identity, indent=2, sort_keys=True) + "\n"
        )
        temporary.rename(output)
    except BaseException:
        shutil.rmtree(temporary, ignore_errors=True)
        raise

    print(f"exported {len(relative_files)} pinned FEX files to {output}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fex_source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    arguments = parser.parse_args()
    export(arguments.fex_source.resolve(), arguments.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
