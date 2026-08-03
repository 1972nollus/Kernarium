#!/usr/bin/env python3
"""Safely extract and canonically hash dependency source trees.

The extractor deliberately does not use ``TarFile.extractall``.  It validates
the complete archive before creating the destination and recreates only the
portable metadata needed by a source tree: directories, regular-file contents,
the regular-file executable bit, and safe relative symbolic links.
"""

from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import argparse
import hashlib
import os
from pathlib import Path
import posixpath
import shutil
import stat
import struct
import tarfile
from dataclasses import dataclass
from typing import BinaryIO, Sequence


_TREE_HASH_DOMAIN = b"linux-um-darwin-source-tree-v1\0"


class SourceArchiveError(Exception):
    """An archive or source tree violates the accepted format."""


@dataclass(frozen=True)
class _ArchiveEntry:
    relative_parts: tuple[str, ...]
    kind: str
    member: tarfile.TarInfo

    @property
    def relative_name(self) -> str:
        return "/".join(self.relative_parts)


@dataclass(frozen=True)
class _TreeEntry:
    relative_parts: tuple[str, ...]
    path: Path
    mode: int

    @property
    def relative_name(self) -> str:
        return "/".join(self.relative_parts)


def _validate_expected_root(expected_root: str) -> None:
    if (
        not expected_root
        or expected_root in {".", ".."}
        or "/" in expected_root
        or "\\" in expected_root
        or "\0" in expected_root
    ):
        raise SourceArchiveError(
            "expected archive root must be one safe path component"
        )
    if len(expected_root) >= 2 and expected_root[0].isalpha() and expected_root[1] == ":":
        raise SourceArchiveError("expected archive root must not be a drive path")


def _member_relative_parts(name: str, expected_root: str) -> tuple[str, ...]:
    if not name or "\0" in name or "\\" in name:
        raise SourceArchiveError(f"unsafe archive member name: {name!r}")
    if name.startswith("/"):
        raise SourceArchiveError(f"absolute archive member path: {name!r}")

    trimmed_name = name[:-1] if name.endswith("/") else name
    parts = trimmed_name.split("/")
    if any(part in {"", ".", ".."} for part in parts):
        raise SourceArchiveError(f"non-canonical archive member path: {name!r}")
    if len(parts[0]) >= 2 and parts[0][0].isalpha() and parts[0][1] == ":":
        raise SourceArchiveError(f"drive-qualified archive member path: {name!r}")
    if parts[0] != expected_root:
        raise SourceArchiveError(
            f"archive member {name!r} is outside expected root {expected_root!r}"
        )
    return tuple(parts[1:])


def _validate_symlink_target(relative_parts: tuple[str, ...], target: str) -> None:
    if not target or "\0" in target or "\\" in target:
        raise SourceArchiveError(
            f"unsafe symbolic-link target for {'/'.join(relative_parts)!r}: {target!r}"
        )
    if posixpath.isabs(target):
        raise SourceArchiveError(
            f"absolute symbolic-link target for {'/'.join(relative_parts)!r}: {target!r}"
        )
    first_component = target.split("/", 1)[0]
    if (
        len(first_component) >= 2
        and first_component[0].isalpha()
        and first_component[1] == ":"
    ):
        raise SourceArchiveError(
            f"drive-qualified symbolic-link target for {'/'.join(relative_parts)!r}: "
            f"{target!r}"
        )

    parent = "/".join(relative_parts[:-1])
    resolved = posixpath.normpath(posixpath.join(parent, target))
    if resolved == ".." or resolved.startswith("../") or posixpath.isabs(resolved):
        raise SourceArchiveError(
            f"symbolic link {'/'.join(relative_parts)!r} escapes the source tree: "
            f"{target!r}"
        )


def _validated_archive_entries(
    archive: tarfile.TarFile, expected_root: str
) -> list[_ArchiveEntry]:
    entries_by_path: dict[tuple[str, ...], _ArchiveEntry] = {}

    for member in archive.getmembers():
        relative_parts = _member_relative_parts(member.name, expected_root)
        if not relative_parts:
            if not member.isdir():
                raise SourceArchiveError(
                    f"archive root entry {member.name!r} is not a directory"
                )
            continue

        if member.isdir():
            kind = "directory"
        elif member.isfile():
            kind = "file"
        elif member.issym():
            kind = "symlink"
            _validate_symlink_target(relative_parts, member.linkname)
        elif member.islnk():
            # Recreating a hard link safely requires resolving archive member
            # ordering and identity.  Source releases do not need that metadata,
            # so reject it instead of silently changing its meaning.
            raise SourceArchiveError(
                f"hard-link archive member is not supported: {member.name!r}"
            )
        elif member.ischr() or member.isblk():
            raise SourceArchiveError(f"device archive member is forbidden: {member.name!r}")
        elif member.isfifo():
            raise SourceArchiveError(f"FIFO archive member is forbidden: {member.name!r}")
        else:
            raise SourceArchiveError(
                f"unsupported archive member type for {member.name!r}"
            )

        if relative_parts in entries_by_path:
            raise SourceArchiveError(
                f"duplicate archive member path: {'/'.join(relative_parts)!r}"
            )
        entries_by_path[relative_parts] = _ArchiveEntry(
            relative_parts=relative_parts, kind=kind, member=member
        )

    if not entries_by_path:
        raise SourceArchiveError("archive contains no source-tree entries")

    for relative_parts in entries_by_path:
        for component_count in range(1, len(relative_parts)):
            ancestor = entries_by_path.get(relative_parts[:component_count])
            if ancestor is not None and ancestor.kind != "directory":
                raise SourceArchiveError(
                    f"archive member {entries_by_path[relative_parts].relative_name!r} "
                    f"is nested below non-directory {ancestor.relative_name!r}"
                )

    return list(entries_by_path.values())


def _ensure_parent_directory(destination: Path) -> None:
    parent = destination.parent
    if not parent.exists() or not parent.is_dir():
        raise SourceArchiveError(
            f"destination parent must already be a directory: {parent}"
        )


def _copy_regular_file(source: BinaryIO, destination: Path) -> None:
    with destination.open("xb") as output:
        shutil.copyfileobj(source, output, length=1024 * 1024)


def extract_source_archive(
    archive_path: Path, expected_root: str, destination: Path
) -> None:
    """Validate and extract an archive, stripping its expected top directory."""

    _validate_expected_root(expected_root)
    _ensure_parent_directory(destination)
    if os.path.lexists(destination):
        raise SourceArchiveError(f"destination already exists: {destination}")

    destination_created = False
    try:
        with tarfile.open(archive_path, mode="r:*") as archive:
            entries = _validated_archive_entries(archive, expected_root)
            destination.mkdir(mode=0o755)
            destination_created = True

            files = sorted(
                (entry for entry in entries if entry.kind == "file"),
                key=lambda entry: entry.relative_parts,
            )
            symlinks = sorted(
                (entry for entry in entries if entry.kind == "symlink"),
                key=lambda entry: entry.relative_parts,
            )

            required_directories: set[tuple[str, ...]] = set()
            for entry in entries:
                last_component = (
                    len(entry.relative_parts)
                    if entry.kind == "directory"
                    else len(entry.relative_parts) - 1
                )
                for component_count in range(1, last_component + 1):
                    required_directories.add(entry.relative_parts[:component_count])

            for relative_parts in sorted(
                required_directories, key=lambda parts: (len(parts), parts)
            ):
                # Parents have already been created.  Do not use exist_ok: on a
                # case-folding or Unicode-normalizing filesystem it could merge
                # two distinct archive paths.
                destination.joinpath(*relative_parts).mkdir(mode=0o755)

            for entry in files:
                output_path = destination.joinpath(*entry.relative_parts)
                source = archive.extractfile(entry.member)
                if source is None:
                    raise SourceArchiveError(
                        f"archive member has no readable contents: {entry.member.name!r}"
                    )
                with source:
                    _copy_regular_file(source, output_path)
                output_path.chmod(0o755 if entry.member.mode & 0o111 else 0o644)

            # mkdir observes the process umask.  Normalize all directories
            # before symbolic links exist so chmod can never follow one.
            for directory, _, _ in os.walk(destination):
                Path(directory).chmod(0o755)

            for entry in symlinks:
                output_path = destination.joinpath(*entry.relative_parts)
                if os.path.lexists(output_path):
                    raise SourceArchiveError(
                        f"archive path collides on this filesystem: {entry.relative_name!r}"
                    )
                output_path.symlink_to(entry.member.linkname)
    except Exception:
        if destination_created:
            shutil.rmtree(destination)
        raise


def _collect_tree_entries(root: Path) -> list[_TreeEntry]:
    entries: list[_TreeEntry] = []

    def visit(directory: Path, parent_parts: tuple[str, ...]) -> None:
        try:
            children = list(os.scandir(directory))
        except OSError as error:
            raise SourceArchiveError(f"cannot read source directory {directory}: {error}") from error

        for child in children:
            relative_parts = parent_parts + (child.name,)
            try:
                child_stat = child.stat(follow_symlinks=False)
            except OSError as error:
                raise SourceArchiveError(f"cannot inspect source entry {child.path}: {error}") from error
            path = Path(child.path)
            entries.append(
                _TreeEntry(relative_parts=relative_parts, path=path, mode=child_stat.st_mode)
            )
            if stat.S_ISDIR(child_stat.st_mode):
                visit(path, relative_parts)

    visit(root, ())
    return entries


def _encoded_path(relative_name: str) -> bytes:
    try:
        return os.fsencode(relative_name)
    except UnicodeEncodeError as error:
        raise SourceArchiveError(
            f"source-tree path cannot be encoded reproducibly: {relative_name!r}"
        ) from error


def _update_sized(hash_object: "hashlib._Hash", value: bytes) -> None:
    hash_object.update(struct.pack(">Q", len(value)))
    hash_object.update(value)


def _validated_excluded_path(relative_name: str) -> tuple[str, ...]:
    if (
        not relative_name
        or "\0" in relative_name
        or "\\" in relative_name
        or relative_name.startswith("/")
    ):
        raise SourceArchiveError(f"unsafe excluded tree path: {relative_name!r}")
    parts = tuple(relative_name.split("/"))
    if any(part in {"", ".", ".."} for part in parts):
        raise SourceArchiveError(f"non-canonical excluded tree path: {relative_name!r}")
    if len(parts[0]) >= 2 and parts[0][0].isalpha() and parts[0][1] == ":":
        raise SourceArchiveError(f"drive-qualified excluded tree path: {relative_name!r}")
    return parts


def canonical_tree_hash(
    root: Path, excluded_paths: Sequence[str] = ()
) -> str:
    """Return a metadata-independent SHA-256 digest for a source tree."""

    try:
        root_stat = root.lstat()
    except OSError as error:
        raise SourceArchiveError(f"cannot inspect source-tree root {root}: {error}") from error
    if not stat.S_ISDIR(root_stat.st_mode):
        raise SourceArchiveError(f"source-tree root is not a directory: {root}")

    excluded = {_validated_excluded_path(path) for path in excluded_paths}
    entries = _collect_tree_entries(root)
    for entry in entries:
        if entry.relative_parts in excluded and stat.S_ISDIR(entry.mode):
            raise SourceArchiveError(
                f"excluded tree path is a directory: {entry.relative_name!r}"
            )
    entries = [entry for entry in entries if entry.relative_parts not in excluded]
    entries.sort(key=lambda entry: _encoded_path(entry.relative_name))

    digest = hashlib.sha256()
    digest.update(_TREE_HASH_DOMAIN)
    for relative_parts in sorted(excluded):
        digest.update(b"X")
        _update_sized(digest, _encoded_path("/".join(relative_parts)))
    for entry in entries:
        relative_name = entry.relative_name
        encoded_name = _encoded_path(relative_name)

        if stat.S_ISDIR(entry.mode):
            digest.update(b"D")
            _update_sized(digest, encoded_name)
        elif stat.S_ISREG(entry.mode):
            digest.update(b"F")
            _update_sized(digest, encoded_name)
            digest.update(b"1" if entry.mode & 0o111 else b"0")
            try:
                file_size = entry.path.stat(follow_symlinks=False).st_size
                digest.update(struct.pack(">Q", file_size))
                bytes_read = 0
                with entry.path.open("rb") as source:
                    while chunk := source.read(1024 * 1024):
                        digest.update(chunk)
                        bytes_read += len(chunk)
                if bytes_read != file_size:
                    raise SourceArchiveError(
                        f"source file changed while hashing: {relative_name!r}"
                    )
            except OSError as error:
                raise SourceArchiveError(
                    f"cannot hash source file {entry.path}: {error}"
                ) from error
        elif stat.S_ISLNK(entry.mode):
            try:
                target = os.readlink(entry.path)
            except OSError as error:
                raise SourceArchiveError(
                    f"cannot read symbolic link {entry.path}: {error}"
                ) from error
            _validate_symlink_target(entry.relative_parts, target)
            digest.update(b"L")
            _update_sized(digest, encoded_name)
            _update_sized(digest, _encoded_path(target))
        elif stat.S_ISCHR(entry.mode) or stat.S_ISBLK(entry.mode):
            raise SourceArchiveError(f"device is forbidden in source tree: {entry.path}")
        elif stat.S_ISFIFO(entry.mode):
            raise SourceArchiveError(f"FIFO is forbidden in source tree: {entry.path}")
        elif stat.S_ISSOCK(entry.mode):
            raise SourceArchiveError(f"socket is forbidden in source tree: {entry.path}")
        else:
            raise SourceArchiveError(f"unsupported source-tree entry: {entry.path}")

    return digest.hexdigest()


def _extract_command(arguments: argparse.Namespace) -> None:
    extract_source_archive(
        archive_path=arguments.archive,
        expected_root=arguments.expected_root,
        destination=arguments.destination,
    )


def _tree_hash_command(arguments: argparse.Namespace) -> None:
    print(canonical_tree_hash(arguments.root, arguments.exclude))


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    extract_parser = subparsers.add_parser(
        "extract", help="safely extract a tar archive and strip its pinned root"
    )
    extract_parser.add_argument("--archive", required=True, type=Path)
    extract_parser.add_argument("--expected-root", required=True)
    extract_parser.add_argument("--destination", required=True, type=Path)
    extract_parser.set_defaults(handler=_extract_command)

    hash_parser = subparsers.add_parser(
        "tree-hash", help="print the canonical SHA-256 digest of a source tree"
    )
    hash_parser.add_argument("--root", required=True, type=Path)
    hash_parser.add_argument(
        "--exclude", action="append", default=[],
        help="exclude one exact canonical relative file path from the digest",
    )
    hash_parser.set_defaults(handler=_tree_hash_command)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = _argument_parser()
    arguments = parser.parse_args(argv)
    try:
        arguments.handler(arguments)
    except (SourceArchiveError, OSError, tarfile.TarError) as error:
        print(f"source_archive.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
