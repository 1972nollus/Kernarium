#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Canonical plain/compressed A64 raw-evidence file handling."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import gzip
import hashlib
import csv
import os
import stat
import tempfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterator, Sequence, TextIO


PLAIN_SUFFIX = ".tsv.raw"
COMPRESSED_SUFFIX = ".tsv.raw.gz"
FULL_PRODUCER_COUNT = 449
FULL_PRODUCER_SET_SHA256 = (
    "568fd8781de3ae3c865d14d75b6db6aef2656970e202ba4e0aae3bc622330ed1"
)
RAW_COLUMNS = [
    "ctest", "case", "source", "line", "encoding", "invalid",
    "guest_page_size", "expected_event", "expected_retirement",
    "expected_state_oracle", "initial_state_digest", "initial_memory_digest",
    "expected_state_digest", "expected_memory_digest", "expected_fault",
    "expected_syndrome", "native_required_features",
    "native_requirement_scope", "native_observed_features",
    "native_missing_features", "interpreter_result", "gadgets_result",
    "jit_result", "occurrences",
]


def lexical_absolute_path(path: Path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def require_no_symlink_components(path: Path, label: str) -> Path:
    """Traverse without following links and canonicalize the existing prefix."""
    absolute = lexical_absolute_path(path)
    flags = os.O_RDONLY
    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    descriptor = os.open(absolute.anchor, flags)
    current = Path(absolute.anchor)
    try:
        for index, part in enumerate(absolute.parts[1:]):
            try:
                status = os.stat(part, dir_fd=descriptor, follow_symlinks=False)
            except FileNotFoundError:
                canonical = current.resolve(strict=True)
                return canonical.joinpath(*absolute.parts[index + 1 :])
            current /= part
            if stat.S_ISLNK(status.st_mode):
                raise ValueError(f"{label} has a symlinked path component: {current}")
            if index + 2 == len(absolute.parts):
                return current.resolve(strict=True)
            if not stat.S_ISDIR(status.st_mode):
                raise ValueError(
                    f"{label} has a nondirectory path component: {current}"
                )
            next_descriptor = os.open(part, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = next_descriptor
        return current.resolve(strict=True)
    finally:
        os.close(descriptor)


def canonical_existing_directory(path: Path, label: str) -> Path:
    absolute = require_no_symlink_components(path, label)
    if not absolute.is_dir():
        raise ValueError(f"{label} is not a regular directory: {absolute}")
    return absolute.resolve(strict=True)


def _nearest_existing_directory(path: Path) -> Path:
    candidate = path if path.is_dir() else path.parent
    while not candidate.exists():
        if candidate == candidate.parent:
            raise ValueError(f"path has no existing directory ancestor: {path}")
        candidate = candidate.parent
    if not candidate.is_dir():
        candidate = candidate.parent
    return candidate


def filesystem_is_case_sensitive(path: Path) -> bool:
    """Read-only case-sensitivity probe for the target's current volume."""
    directory = _nearest_existing_directory(path)
    for candidate in (directory, *directory.parents):
        name = candidate.name
        for index, character in enumerate(name):
            if not character.isascii() or not character.isalpha():
                continue
            replacement = character.swapcase()
            variant = candidate.with_name(
                name[:index] + replacement + name[index + 1 :]
            )
            try:
                return not os.path.samefile(candidate, variant)
            except FileNotFoundError:
                return True
            except OSError:
                break
    # A volume whose entire path contains no alphabetic component is unusual;
    # preserving case distinctions is the conservative read-only fallback.
    return True


def _comparison_parts(left: Path, right: Path) -> tuple[tuple[str, ...], tuple[str, ...]]:
    left = lexical_absolute_path(left)
    right = lexical_absolute_path(right)
    left_directory = _nearest_existing_directory(left)
    right_directory = _nearest_existing_directory(right)
    same_volume = left_directory.stat().st_dev == right_directory.stat().st_dev
    insensitive = same_volume and (
        not filesystem_is_case_sensitive(left_directory)
        or not filesystem_is_case_sensitive(right_directory)
    )
    left_parts = left.parts
    right_parts = right.parts
    if insensitive:
        left_parts = tuple(part.casefold() for part in left_parts)
        right_parts = tuple(part.casefold() for part in right_parts)
    return tuple(left_parts), tuple(right_parts)


def paths_alias(left: Path, right: Path) -> bool:
    if left.exists() and right.exists():
        try:
            if os.path.samefile(left, right):
                return True
        except OSError:
            pass
    left_parts, right_parts = _comparison_parts(left, right)
    return left_parts == right_parts


def path_is_ancestor(ancestor: Path, descendant: Path) -> bool:
    ancestor_parts, descendant_parts = _comparison_parts(ancestor, descendant)
    return (
        len(ancestor_parts) < len(descendant_parts)
        and descendant_parts[: len(ancestor_parts)] == ancestor_parts
    )


def validate_publication_topology(
    file_targets: Sequence[Path],
    *,
    directory_targets: Sequence[Path] = (),
    read_inputs: Sequence[Path] = (),
) -> tuple[tuple[Path, ...], tuple[Path, ...], tuple[Path, ...]]:
    """Fail before staging when read/write path shapes cannot coexist."""
    files = tuple(
        require_no_symlink_components(path, "publication target")
        for path in file_targets
    )
    directories = tuple(
        require_no_symlink_components(path, "publication directory target")
        for path in directory_targets
    )
    reads = tuple(
        require_no_symlink_components(path, "publication read input")
        for path in read_inputs
    )
    writes = (*files, *directories)
    for index, left in enumerate(writes):
        for right in writes[index + 1 :]:
            if paths_alias(left, right):
                raise ValueError("publication write targets alias each other")
            if path_is_ancestor(left, right) or path_is_ancestor(right, left):
                raise ValueError("publication write targets have impossible ancestry")
    for path in files:
        if path.exists() and not path.is_file():
            raise ValueError(f"publication target is not a regular file: {path}")
    for path in directories:
        if path.exists() and not path.is_dir():
            raise ValueError(f"publication directory target is not a directory: {path}")
    for write in writes:
        for read in reads:
            if paths_alias(write, read):
                raise ValueError("publication target aliases a read input")
            if path_is_ancestor(write, read) or path_is_ancestor(read, write):
                raise ValueError(
                    "publication target has impossible read-input ancestry"
                )
    for path in writes:
        canonical_existing_directory(path.parent, "publication target parent")
    return files, directories, reads


@dataclass(frozen=True)
class RawEvidenceFile:
    producer: str
    path: Path
    compressed: bool


def atomic_publish_bytes(
    outputs: Sequence[tuple[Path, bytes]],
    *,
    validate_staged: Callable[[Sequence[tuple[Path, Path]]], None] | None = None,
    before_replace: Callable[[], None] | None = None,
    validate_published: Callable[[], None] | None = None,
) -> None:
    """Write validated bytes through temporary files and atomic replacement."""
    staged: list[tuple[Path, Path]] = []
    try:
        for original, payload in outputs:
            path = require_no_symlink_components(original, "publication target")
            canonical_existing_directory(path.parent, "publication target parent")
            descriptor, temporary_name = tempfile.mkstemp(
                prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
            )
            temporary = Path(temporary_name)
            staged.append((path, temporary))
            with os.fdopen(descriptor, "wb") as destination:
                destination.write(payload)
                destination.flush()
        if validate_staged is not None:
            validate_staged(staged)
        if before_replace is not None:
            before_replace()
        for path, temporary in staged:
            os.replace(temporary, path)
        if validate_published is not None:
            validate_published()
    finally:
        for _path, temporary in staged:
            if temporary.exists():
                temporary.unlink()


def producer_set_sha256(producers: set[str]) -> str:
    payload = "".join(f"{producer}\n" for producer in sorted(producers))
    return hashlib.sha256(payload.encode("utf-8")).hexdigest()


def validate_producer_authority(producers: set[str], page: str) -> None:
    digest = producer_set_sha256(producers)
    if (
        len(producers) != FULL_PRODUCER_COUNT
        or digest != FULL_PRODUCER_SET_SHA256
    ):
        raise ValueError(
            f"full {page} producer authority differs: "
            f"count={len(producers)}, sha256={digest}"
        )


def require_regular_raw_file(path: Path) -> None:
    if path.is_symlink() or not path.is_file():
        raise ValueError(
            f"raw evidence entry is not a nonsymlink regular file: {path}"
        )


def raw_evidence_files(directory: Path) -> list[RawEvidenceFile]:
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError(f"raw evidence path is not a regular directory: {directory}")
    by_producer: dict[str, RawEvidenceFile] = {}
    for suffix, compressed in (
        (PLAIN_SUFFIX, False),
        (COMPRESSED_SUFFIX, True),
    ):
        for path in sorted(directory.glob(f"*{suffix}")):
            require_regular_raw_file(path)
            producer = path.name[: -len(suffix)]
            if not producer:
                raise ValueError(f"raw evidence has an empty producer: {path}")
            if producer in by_producer:
                raise ValueError(
                    f"raw evidence has duplicate plain/compressed producer: "
                    f"{producer}"
                )
            by_producer[producer] = RawEvidenceFile(
                producer=producer, path=path, compressed=compressed
            )
    return [by_producer[name] for name in sorted(by_producer)]


def raw_evidence_file(directory: Path, producer: str) -> RawEvidenceFile:
    """Resolve one producer through the same exact plain/gzip authority."""
    matches = [
        entry
        for entry in raw_evidence_files(directory)
        if entry.producer == producer
    ]
    if len(matches) != 1:
        raise ValueError(
            f"raw evidence producer {producer!r} has {len(matches)} files"
        )
    return matches[0]


@contextmanager
def open_raw_text(entry: RawEvidenceFile) -> Iterator[TextIO]:
    if entry.compressed:
        with gzip.open(entry.path, "rt", encoding="utf-8", newline="") as source:
            yield source
    else:
        with entry.path.open(encoding="utf-8", newline="") as source:
            yield source


def uncompressed_identity(entry: RawEvidenceFile) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    opener = gzip.open if entry.compressed else open
    try:
        with opener(entry.path, "rb") as source:
            while chunk := source.read(1024 * 1024):
                size += len(chunk)
                digest.update(chunk)
    except (EOFError, gzip.BadGzipFile, OSError) as error:
        raise ValueError(f"cannot verify raw evidence stream {entry.path}") from error
    return size, digest.hexdigest()


def validate_raw_profile(entry: RawEvidenceFile, page: str) -> int:
    rows = 0
    try:
        with open_raw_text(entry) as source:
            reader = csv.DictReader(source, delimiter="\t")
            if reader.fieldnames != RAW_COLUMNS:
                raise ValueError(
                    f"{entry.path}: unexpected raw evidence columns"
                )
            for row in reader:
                rows += 1
                if (
                    set(row) != set(RAW_COLUMNS)
                    or None in row
                    or any(row[column] is None for column in RAW_COLUMNS)
                ):
                    raise ValueError(
                        f"{entry.path}: raw evidence row shape differs"
                    )
                if row["ctest"] != entry.producer:
                    raise ValueError(
                        f"{entry.path}: raw producer field differs from filename"
                    )
                if row["guest_page_size"] != page:
                    raise ValueError(
                        f"{entry.path}: raw row differs from page profile {page}"
                    )
    except (EOFError, gzip.BadGzipFile, OSError) as error:
        raise ValueError(f"cannot validate raw evidence stream {entry.path}") from error
    if rows == 0:
        raise ValueError(f"{entry.path}: raw evidence stream is empty")
    return rows
