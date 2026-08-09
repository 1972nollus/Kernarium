#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify Apple static-archive object metadata, including ThinLTO members."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import collections
import dataclasses
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile


ARCHIVE_MAGIC = b"!<arch>\n"
MACHO_64_MAGIC = b"\xcf\xfa\xed\xfe"
LLVM_BITCODE_MAGICS = (b"\xde\xc0\x17\x0b", b"BC\xc0\xde")
CPU_TYPE_ARM64 = 0x0100000C
MH_OBJECT = 1
LC_BUILD_VERSION = 0x32


@dataclasses.dataclass(frozen=True)
class PlatformPolicy:
    build_platform: int
    minimum_version: tuple[int, int, int]
    sdk: str
    target: str


PLATFORMS = {
    "macos": PlatformPolicy(1, (14, 0, 0), "macosx", "arm64-apple-macos14.0"),
    "ios": PlatformPolicy(2, (16, 0, 0), "iphoneos", "arm64-apple-ios16.0"),
    "ios-simulator": PlatformPolicy(
        7, (16, 0, 0), "iphonesimulator", "arm64-apple-ios16.0-simulator"
    ),
}


class VerificationError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class ArchiveMember:
    name: str
    data: bytes


def _parse_decimal(field: bytes, description: str) -> int:
    try:
        return int(field.decode("ascii").strip() or "0", 10)
    except (UnicodeDecodeError, ValueError) as error:
        raise VerificationError(f"invalid archive {description}") from error


def _gnu_long_name(table: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(table):
        raise VerificationError("archive long-name offset is out of range")
    end = table.find(b"/\n", offset)
    if end < 0:
        end = table.find(b"\x00", offset)
    if end < 0:
        end = len(table)
    try:
        return table[offset:end].decode("utf-8")
    except UnicodeDecodeError as error:
        raise VerificationError("archive member name is not UTF-8") from error


def read_archive(path: Path) -> list[ArchiveMember]:
    data = path.read_bytes()
    if not data.startswith(ARCHIVE_MAGIC):
        raise VerificationError("file is not an ar archive")

    members: list[ArchiveMember] = []
    string_table = b""
    offset = len(ARCHIVE_MAGIC)
    while offset < len(data):
        if len(data) - offset < 60:
            raise VerificationError("truncated archive member header")
        header = data[offset : offset + 60]
        if header[58:60] != b"`\n":
            raise VerificationError("invalid archive member header trailer")
        stored_size = _parse_decimal(header[48:58], "member size")
        payload_start = offset + 60
        payload_end = payload_start + stored_size
        if stored_size < 0 or payload_end > len(data):
            raise VerificationError("archive member extends beyond end of file")
        payload = data[payload_start:payload_end]

        try:
            raw_name = header[:16].decode("ascii", errors="strict").rstrip()
        except UnicodeDecodeError as error:
            raise VerificationError("archive member name is not ASCII") from error
        if raw_name.startswith("#1/"):
            name_length = _parse_decimal(raw_name[3:].encode("ascii"), "name length")
            if name_length > len(payload):
                raise VerificationError("archive extended member name is truncated")
            try:
                name = payload[:name_length].rstrip(b"\x00").decode("utf-8")
            except UnicodeDecodeError as error:
                raise VerificationError("archive member name is not UTF-8") from error
            member_data = payload[name_length:]
        elif raw_name == "//":
            string_table = payload
            name = raw_name
            member_data = payload
        elif raw_name.startswith("/") and raw_name[1:].isdigit():
            name = _gnu_long_name(string_table, int(raw_name[1:], 10))
            member_data = payload
        else:
            name = raw_name[:-1] if raw_name.endswith("/") else raw_name
            member_data = payload

        if name not in {"", "/", "//", "/SYM64", "/SYM64/"} and not name.startswith(
            "__.SYMDEF"
        ):
            if "\x00" in name or "/" in name or name in {".", ".."}:
                raise VerificationError(
                    f"archive member name is not a plain filename: {name!r}"
                )
            members.append(ArchiveMember(name=name, data=member_data))
        offset = payload_end + (stored_size & 1)

    if offset != len(data):
        raise VerificationError("archive has an invalid final padding byte")
    if not members:
        raise VerificationError("archive has no object members")
    return members


def is_link_time_object(data: bytes) -> bool:
    return data.startswith(LLVM_BITCODE_MAGICS)


def _decode_version(value: int) -> tuple[int, int, int]:
    return ((value >> 16) & 0xFFFF, (value >> 8) & 0xFF, value & 0xFF)


def _format_version(version: tuple[int, int, int]) -> str:
    if version[2]:
        return ".".join(str(part) for part in version)
    return f"{version[0]}.{version[1]}"


def verify_macho(member: ArchiveMember, policy: PlatformPolicy) -> None:
    if not member.data.startswith(MACHO_64_MAGIC):
        raise VerificationError(
            f"member {member.name!r} is neither a 64-bit Mach-O object nor LLVM bitcode"
        )
    if len(member.data) < 32:
        raise VerificationError(f"Mach-O member {member.name!r} has a truncated header")
    (
        _magic,
        cpu_type,
        _cpu_subtype,
        file_type,
        command_count,
        command_bytes,
        _flags,
        _reserved,
    ) = struct.unpack_from("<IiiIIIII", member.data)
    if cpu_type != CPU_TYPE_ARM64:
        raise VerificationError(
            f"Mach-O member {member.name!r} has CPU type 0x{cpu_type & 0xFFFFFFFF:08x}, "
            f"expected arm64"
        )
    if file_type != MH_OBJECT:
        raise VerificationError(
            f"Mach-O member {member.name!r} has file type {file_type}, expected MH_OBJECT"
        )
    commands_end = 32 + command_bytes
    if commands_end > len(member.data):
        raise VerificationError(
            f"Mach-O member {member.name!r} has truncated load commands"
        )

    build_versions: list[tuple[int, tuple[int, int, int]]] = []
    command_offset = 32
    for _ in range(command_count):
        if command_offset + 8 > commands_end:
            raise VerificationError(
                f"Mach-O member {member.name!r} has a truncated load command"
            )
        command, command_size = struct.unpack_from("<II", member.data, command_offset)
        if command_size < 8 or command_offset + command_size > commands_end:
            raise VerificationError(
                f"Mach-O member {member.name!r} has an invalid load-command size"
            )
        if command == LC_BUILD_VERSION:
            if command_size < 24:
                raise VerificationError(
                    f"Mach-O member {member.name!r} has a truncated LC_BUILD_VERSION"
                )
            platform, minimum, _sdk, tool_count = struct.unpack_from(
                "<IIII", member.data, command_offset + 8
            )
            if 24 + tool_count * 8 > command_size:
                raise VerificationError(
                    f"Mach-O member {member.name!r} has truncated build tools"
                )
            build_versions.append((platform, _decode_version(minimum)))
        command_offset += command_size
    if command_offset != commands_end:
        raise VerificationError(
            f"Mach-O member {member.name!r} load-command size does not match its header"
        )
    if len(build_versions) != 1:
        raise VerificationError(
            f"Mach-O member {member.name!r} has {len(build_versions)} LC_BUILD_VERSION commands"
        )
    platform, minimum = build_versions[0]
    if platform != policy.build_platform or minimum != policy.minimum_version:
        raise VerificationError(
            f"Mach-O member {member.name!r} has platform {platform} minimum "
            f"{_format_version(minimum)}, expected platform {policy.build_platform} "
            f"minimum {_format_version(policy.minimum_version)}"
        )


def expected_link_time_objects(roots: list[Path]) -> dict[str, bytes]:
    expected: dict[str, bytes] = {}
    for root in roots:
        if not root.is_dir():
            raise VerificationError(f"expected object root is not a directory: {root}")
        for path in sorted(root.rglob("*.o")):
            if path.is_symlink():
                raise VerificationError(f"expected object is a symlink: {path}")
            data = path.read_bytes()
            if not is_link_time_object(data):
                continue
            if path.name in expected:
                raise VerificationError(
                    f"duplicate expected link-time object basename: {path.name}"
                )
            expected[path.name] = data
    if not expected:
        raise VerificationError("expected object roots contain no LLVM bitcode objects")
    return expected


def verify_link_time_target(
    member: ArchiveMember, policy: PlatformPolicy, xcrun: str
) -> None:
    with tempfile.TemporaryDirectory(prefix="uml-archive-bitcode-") as temporary:
        # Never use archive-controlled text as a filesystem path. Member names
        # are retained only for diagnostics and composition matching.
        object_path = Path(temporary) / "link-time-object.bc"
        object_path.write_bytes(member.data)
        command = [
            xcrun,
            "--sdk",
            policy.sdk,
            "clang",
            "-target",
            policy.target,
            "-Werror=override-module",
            "-x",
            "ir",
            "-S",
            "-emit-llvm",
            str(object_path),
            "-o",
            os.devnull,
        ]
        try:
            result = subprocess.run(command, capture_output=True, text=True, check=False)
        except OSError as error:
            raise VerificationError(f"cannot execute {xcrun}: {error}") from error
        if result.returncode != 0:
            diagnostic = " ".join((result.stdout + result.stderr).split())
            raise VerificationError(
                f"link-time member {member.name!r} is malformed or does not target "
                f"{policy.target}: {diagnostic}"
            )


def verify_archive(
    archive: Path,
    platform: str,
    expected_roots: list[Path],
    forbidden_paths: list[str],
    xcrun: str,
) -> tuple[int, int]:
    policy = PLATFORMS[platform]
    members = read_archive(archive)
    expected = expected_link_time_objects(expected_roots)
    actual_link_time = [member for member in members if is_link_time_object(member.data)]
    actual_names = collections.Counter(member.name for member in actual_link_time)
    expected_names = collections.Counter(expected.keys())
    if actual_names != expected_names:
        missing = sorted((expected_names - actual_names).elements())
        unexpected = sorted((actual_names - expected_names).elements())
        raise VerificationError(
            "link-time member composition does not match the expected gadget target "
            f"(missing={missing}, unexpected={unexpected})"
        )

    macho_count = 0
    for member in members:
        if is_link_time_object(member.data):
            # Apple Clang records the absolute input spelling in ThinLTO's
            # source_filename and does not apply its prefix-map options there.
            # Permit that path only after the member byte-matches the configured
            # gadget target and its exact architecture/platform/minimum target
            # triple has been checked independently by the compiler.
            if member.data != expected[member.name]:
                raise VerificationError(
                    f"link-time member {member.name!r} does not match its expected "
                    "gadget-target object"
                )
            verify_link_time_target(member, policy, xcrun)
        else:
            # Ordinary objects have no unavoidable source_filename exception.
            # Unknown member forms fail in verify_macho before they can bypass
            # either the metadata checks or this path-privacy check.
            verify_macho(member, policy)
            for forbidden_path in forbidden_paths:
                if forbidden_path and forbidden_path.encode("utf-8") in member.data:
                    raise VerificationError(
                        f"Mach-O member {member.name!r} contains forbidden path: "
                        f"{forbidden_path}"
                    )
            macho_count += 1
    return macho_count, len(actual_link_time)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=sorted(PLATFORMS))
    parser.add_argument(
        "--expected-link-time-object-root",
        required=True,
        type=Path,
        action="append",
    )
    parser.add_argument("--forbidden-path", action="append", default=[])
    parser.add_argument("--xcrun", default="xcrun")
    args = parser.parse_args()
    try:
        macho_count, link_time_count = verify_archive(
            args.archive,
            args.platform,
            args.expected_link_time_object_root,
            args.forbidden_path,
            args.xcrun,
        )
    except (OSError, VerificationError) as error:
        print(f"verify-apple-static-archive.py: {args.archive}: {error}", file=sys.stderr)
        return 1
    print(
        f"verified {args.platform} archive: {macho_count} arm64 Mach-O members, "
        f"{link_time_count} target-checked gadget link-time members"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
