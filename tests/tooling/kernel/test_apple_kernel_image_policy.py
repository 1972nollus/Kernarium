#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the signed kernel carrier verifier and accepted-artifact caching."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def synthetic_elf(
    *,
    late_nonexecutable_overlap: bool,
    reverse_overlap_order: bool = False,
    overlap_writable: bool = False,
) -> bytes:
    image = bytearray(0x10000)
    programs = [
        (1, 4, 0, 0xC00000000, 0xC00000000, 0x1000, 0x1000, 0x4000),
        (
            1,
            5,
            0x4000,
            0xC00004000,
            0xC00004000,
            0x1800 if late_nonexecutable_overlap else 0x1000,
            0x1800 if late_nonexecutable_overlap else 0x1000,
            0x4000,
        ),
        (
            1,
            6 if overlap_writable else 4,
            0x6000 if late_nonexecutable_overlap else 0x8000,
            0xC00006000 if late_nonexecutable_overlap else 0xC00008000,
            0xC00006000 if late_nonexecutable_overlap else 0xC00008000,
            0x1000,
            0x1000,
            0x4000,
        ),
    ]
    if reverse_overlap_order:
        programs[1:] = reversed(programs[1:])
    identity = b"\x7fELF\x02\x01\x01" + bytes(9)
    ELF_HEADER.pack_into(
        image,
        0,
        identity,
        3,
        183,
        1,
        0xC00004000,
        ELF_HEADER.size,
        0,
        0,
        ELF_HEADER.size,
        PROGRAM_HEADER.size,
        len(programs),
        0,
        0,
        0,
    )
    for index, program in enumerate(programs):
        PROGRAM_HEADER.pack_into(
            image,
            ELF_HEADER.size + index * PROGRAM_HEADER.size,
            *program,
        )
    return bytes(image)


def verify_carrier_tool(verifier: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="kernarium-carrier-policy-") as directory:
        valid = Path(directory) / "valid-vmlinux"
        invalid = Path(directory) / "invalid-vmlinux"
        reverse_invalid = Path(directory) / "reverse-invalid-vmlinux"
        valid.write_bytes(synthetic_elf(late_nonexecutable_overlap=False))
        invalid.write_bytes(synthetic_elf(late_nonexecutable_overlap=True))
        reverse_invalid.write_bytes(
            synthetic_elf(
                late_nonexecutable_overlap=True,
                reverse_overlap_order=True,
                overlap_writable=True,
            )
        )
        accepted = subprocess.run(
            [sys.executable, "-B", str(verifier), "--image", str(valid)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(accepted.returncode == 0, f"safe carrier rejected:\n{accepted.stderr}")
        unsafe_cases = (
            (
                invalid,
                "non-executable PT_LOAD follows signed executable bytes",
            ),
            (
                reverse_invalid,
                "signed executable PT_LOAD shares a host page with writable",
            ),
        )
        for unsafe, diagnostic in unsafe_cases:
            rejected = subprocess.run(
                [sys.executable, "-B", str(verifier), "--image", str(unsafe)],
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            require(
                rejected.returncode != 0,
                f"mixed loader page was accepted: {unsafe.name}",
            )
            require(
                diagnostic in rejected.stderr,
                "carrier verifier rejected the hazard for the wrong reason:\n"
                f"{rejected.stderr}",
            )


def verify_accepted_artifact_cache(artifact_cmake: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="accepted-artifact-cache-") as directory:
        temporary = Path(directory)
        managed = temporary / "managed"
        external = temporary / "external" / "copied-artifact"
        script = temporary / "verify.cmake"
        script.write_text(
            f'''cmake_minimum_required(VERSION 3.27)
include([=[{artifact_cmake.resolve()}]=])

# A first configure whose value already names the accepted default follows it.
set(TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v1"}]=] CACHE PATH "test")
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v1"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v1"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v1"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "accepted artifact did not follow its first default")
endif()

# A value which still equals the prior generated default follows acceptance.
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v2"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v2"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v2"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "accepted artifact did not follow the new default")
endif()

# An explicit whole-bundle override remains stable even when it is another
# publication below the normal managed artifact root.
set(TEST_MANAGED_ARTIFACT [=[{managed / "local-override"}]=]
    CACHE PATH "test" FORCE)
unset(TEST_MANAGED_ARTIFACT)
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v3"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "accepted-v3"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "managed whole-bundle override was replaced")
endif()

# An accepted default may later equal the pinned path without changing its
# override identity. It must remain pinned across the following change too.
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "local-override"}]=]
    "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_ACCEPTED_DEFAULT STREQUAL
       [=[{managed / "local-override"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "matching accepted default reclassified an override")
endif()
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v4"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "local-override"}]=] OR
   TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "managed override followed a later default")
endif()

# Clearing an override opts back into the current accepted default.
set(TEST_MANAGED_ARTIFACT "" CACHE PATH "test" FORCE)
unset(TEST_MANAGED_ARTIFACT)
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v4"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v4"}]=])
    message(FATAL_ERROR "cleared override did not restore acceptance")
endif()
uml_cache_accepted_artifact_path(
    TEST_MANAGED_ARTIFACT [=[{managed / "accepted-v5"}]=] "test")
if(NOT TEST_MANAGED_ARTIFACT STREQUAL [=[{managed / "accepted-v5"}]=] OR
   NOT TEST_MANAGED_ARTIFACT_FOLLOW_ACCEPTED)
    message(FATAL_ERROR "restored acceptance did not follow a later default")
endif()

set(TEST_EXTERNAL_ARTIFACT [=[{external}]=] CACHE PATH "test")
uml_cache_accepted_artifact_path(
    TEST_EXTERNAL_ARTIFACT [=[{managed / "accepted-v3"}]=] "test")
if(NOT TEST_EXTERNAL_ARTIFACT STREQUAL [=[{external}]=])
    message(FATAL_ERROR "external artifact override was replaced")
endif()
''',
            encoding="utf-8",
        )
        checked = subprocess.run(
            ["cmake", "-P", str(script)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        require(
            checked.returncode == 0,
            "accepted artifact cache policy failed:\n"
            f"{checked.stdout}{checked.stderr}",
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifact-cmake", type=Path, required=True)
    parser.add_argument("--carrier-verifier", type=Path, required=True)
    arguments = parser.parse_args()

    verify_accepted_artifact_cache(arguments.artifact_cmake)
    verify_carrier_tool(arguments.carrier_verifier)

    print("Apple kernel carrier and artifact cache policy verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
