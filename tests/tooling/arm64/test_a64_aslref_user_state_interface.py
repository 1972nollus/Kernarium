#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check the bounded A64 user-register adapter against pinned Arm semantics."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


POSITIVE = r"""
func main() => integer
begin
    A64Oracle_ResetUserRegisterState();

    // All four widths are reads of the same architectural X register.  The
    // narrower forms truncate the high bits rather than changing storage.
    X{64}(0) = '1101111010101101101111101110111111001010111111101011101010111110';
    let first8 : bits(8) = X{8}(0);
    let first16 : bits(16) = X{16}(0);
    let first32 : bits(32) = X{32}(0);
    let first64 : bits(64) = X{64}(0);
    assert first8 == '10111110';
    assert first16 == '1011101010111110';
    assert first32 == '11001010111111101011101010111110';
    assert first64 ==
        '1101111010101101101111101110111111001010111111101011101010111110';

    // W writes are the only non-64-bit writes in the pinned accessor and
    // zero-extend into the underlying 64-bit register.
    X{32}(1) = Ones{32};
    let second8 : bits(8) = X{8}(1);
    let second16 : bits(16) = X{16}(1);
    let second32 : bits(32) = X{32}(1);
    let second64 : bits(64) = X{64}(1);
    assert second8 == Ones{8};
    assert second16 == Ones{16};
    assert second32 == Ones{32};
    assert second64 == Zeros{32} :: Ones{32};
    X{64}(1) = Ones{64};
    let rewritten64 : bits(64) = X{64}(1);
    assert rewritten64 == Ones{64};

    // X31 discards valid W/X writes and reads as zero at every read width.
    X{32}(31) = Ones{32};
    X{64}(31) = Ones{64};
    let zero8 : bits(8) = X{8}(31);
    let zero16 : bits(16) = X{16}(31);
    let zero32 : bits(32) = X{32}(31);
    let zero64 : bits(64) = X{64}(31);
    assert zero8 == Zeros{8};
    assert zero16 == Zeros{16};
    assert zero32 == Zeros{32};
    assert zero64 == Zeros{64};

    print "A64_USER_STATE_INTERFACE_PASS";
    return 0;
end;
"""


NEGATIVES = (
    # The pinned getter rejects widths outside 8/16/32/64.
    r"""
func main() => integer
begin
    let invalid : bits(24) = X{24}(0);
    return UInt(invalid);
end;
""",
    # The pinned setter accepts architectural W/X writes only.
    r"""
func main() => integer
begin
    X{8}(0) = Ones{8};
    return 0;
end;
""",
    r"""
func main() => integer
begin
    X{16}(0) = Ones{16};
    return 0;
end;
""",
)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--aslref",
        type=Path,
        default=(
            Path(os.environ["UML_ASLREF_TEST_EXECUTABLE"])
            if "UML_ASLREF_TEST_EXECUTABLE" in os.environ
            else None
        ),
    )
    parser.add_argument(
        "--architecture-root",
        type=Path,
        default=(
            Path(os.environ["UML_A64_ARCHITECTURE_DIRECTORY"])
            if "UML_A64_ARCHITECTURE_DIRECTORY" in os.environ
            else None
        ),
    )
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def source_contract(interface: Path, architecture_root: Path | None) -> None:
    adapter = interface.read_text(encoding="utf-8")
    require(
        "let rw : integer{} = width as integer{8, 16, 32, 64};" in adapter,
        "adapter X getter no longer exposes the pinned four read widths",
    )
    require(
        "assert width IN {32, 64};" in adapter,
        "adapter X setter no longer restricts writes to W/X widths",
    )
    if architecture_root is None:
        return
    official_path = architecture_root / "shared_pseudocode.xml"
    require(official_path.is_file(), "pinned shared_pseudocode.xml is absent")
    official = official_path.read_text(encoding="utf-8")
    match = re.search(
        r"accessor\s+<anchor[^>]*>X</anchor>\{width\}\(n : integer\).*?"
        r"(?=accessor\s+<anchor[^>]*>X</anchor>\{width\}\(lr : integer)",
        official,
        flags=re.DOTALL,
    )
    require(match is not None, "pinned official scalar X accessor is absent")
    scalar = match.group(0)
    require(
        "assert width IN {32,64};" in scalar
        and "width as integer{8, 16, 32, 64}" in scalar,
        "pinned official scalar X read/write width contract drifted",
    )


def execute_contract(executable: Path, interface: Path) -> None:
    require(executable.is_file(), "configured ASLRef executable is absent")
    with tempfile.TemporaryDirectory(prefix="a64-user-state-interface-") as name:
        directory = Path(name)

        def execute(source: str, suffix: str) -> subprocess.CompletedProcess[str]:
            harness = directory / f"harness-{suffix}.asl"
            harness.write_text(source, encoding="utf-8")
            return subprocess.run(
                [
                    str(executable),
                    "--type-check-strict",
                    "--exec",
                    "-1",
                    str(interface),
                    str(harness),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

        positive = execute(POSITIVE, "positive")
        require(
            positive.returncode == 0
            and positive.stdout.strip() == "A64_USER_STATE_INTERFACE_PASS",
            "ASLRef rejected the positive X accessor fixture:\n"
            + positive.stderr
            + positive.stdout,
        )
        for index, source in enumerate(NEGATIVES):
            negative = execute(source, f"negative-{index}")
            require(
                negative.returncode != 0,
                f"ASLRef accepted X accessor negative fixture {index}",
            )


def main() -> int:
    options = arguments()
    interface = Path(__file__).with_name("a64_aslref_user_state_interface.asl")
    source_contract(interface, options.architecture_root)
    if options.aslref is not None:
        execute_contract(options.aslref, interface)
    print("A64 ASLRef user-state interface fixtures passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
