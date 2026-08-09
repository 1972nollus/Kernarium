#!/usr/bin/env python3
"""Boot one Darwin UML engine and validate its lightweight guest contract."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import ctypes
import pathlib
import re
import shlex
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


HWCAP_FP = 1 << 0
HWCAP_ASIMD = 1 << 1
HWCAP_AES = 1 << 3
HWCAP_PMULL = 1 << 4
HWCAP_SHA1 = 1 << 5
HWCAP_SHA2 = 1 << 6
HWCAP_CRC32 = 1 << 7
HWCAP_ATOMICS = 1 << 8
HWCAP_SHA3 = 1 << 17
HWCAP_SHA512 = 1 << 21
OPTIONAL_HWCAPS = (
    ("hw.optional.arm.FEAT_AES", HWCAP_AES),
    ("hw.optional.arm.FEAT_PMULL", HWCAP_PMULL),
    ("hw.optional.arm.FEAT_SHA1", HWCAP_SHA1),
    ("hw.optional.arm.FEAT_SHA256", HWCAP_SHA2),
    ("hw.optional.arm.FEAT_CRC32", HWCAP_CRC32),
    ("hw.optional.arm.FEAT_LSE", HWCAP_ATOMICS),
    ("hw.optional.arm.FEAT_SHA3", HWCAP_SHA3),
    ("hw.optional.arm.FEAT_SHA512", HWCAP_SHA512),
)


def expected_hwcap() -> int:
    result = HWCAP_FP | HWCAP_ASIMD
    sysctlbyname = ctypes.CDLL(None, use_errno=True).sysctlbyname
    sysctlbyname.argtypes = (
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_void_p,
        ctypes.c_size_t,
    )
    sysctlbyname.restype = ctypes.c_int
    for name, bit in OPTIONAL_HWCAPS:
        enabled = ctypes.c_int()
        size = ctypes.c_size_t(ctypes.sizeof(enabled))
        status = sysctlbyname(
            name.encode(),
            ctypes.byref(enabled),
            ctypes.byref(size),
            None,
            0,
        )
        if status == 0 and enabled.value != 0:
            result |= bit
    return result


def command(guest: Guest, value: str, timeout: int = 60) -> str:
    result = guest.run_command(value, timeout)
    if result.return_code != 0:
        raise RuntimeError(
            f"guest command exited {result.return_code}: {value}\n{result.output}"
        )
    return result.output


def auxiliary_vector(guest: Guest) -> dict[int, int]:
    output = command(guest, "od -An -v -t u8 /proc/self/auxv")
    words = [int(value) for value in re.findall(r"\b[0-9]+\b", output)]
    if len(words) % 2:
        raise RuntimeError(f"malformed auxiliary vector output:\n{output}")
    return dict(zip(words[0::2], words[1::2]))


def cpuinfo_features(guest: Guest) -> set[str]:
    output = command(guest, "grep '^flags' /proc/cpuinfo | head -n 1")
    match = re.search(r"flags\s*:\s*(.*)", output)
    if match is None:
        raise RuntimeError(f"/proc/cpuinfo flags are missing:\n{output}")
    return set(match.group(1).split())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--emulator",
        type=pathlib.Path,
        default=REPOSITORY / ".build" / "cmake-macos" / "bin" / "uml-darwin",
    )
    parser.add_argument(
        "--rootfs", type=pathlib.Path, default=REPOSITORY / "rootfs.erofs"
    )
    parser.add_argument("--memory", default="8G")
    parser.add_argument(
        "--engine", choices=("jit", "gadgets", "interpreter"), default="jit"
    )
    arguments = parser.parse_args()

    guest = Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        (f"--execution-engine={arguments.engine}",),
        init_shell=True,
    )
    try:
        guest.boot_to_shell(180)
        command(
            guest,
            "mount -t proc proc /proc && "
            "mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp",
        )

        auxv = auxiliary_vector(guest)
        actual_hwcap = auxv.get(16)
        actual_hwcap2 = auxv.get(26, 0)
        expected = expected_hwcap()
        if actual_hwcap != expected:
            raise RuntimeError(
                "AT_HWCAP mismatch: "
                f"expected 0x{expected:x}, got {actual_hwcap!r}"
            )
        if actual_hwcap2 != 0:
            raise RuntimeError(
                f"AT_HWCAP2 exposes unsupported features: 0x{actual_hwcap2:x}"
            )
        expected_cpuinfo = {
            (
                "sha2"
                if name == "hw.optional.arm.FEAT_SHA256"
                else "atomics"
                if name == "hw.optional.arm.FEAT_LSE"
                else name.rsplit("FEAT_", 1)[-1].lower()
            )
            for name, bit in OPTIONAL_HWCAPS
            if expected & bit
        }
        expected_cpuinfo.update(("fp", "asimd"))
        actual_cpuinfo = cpuinfo_features(guest)
        if actual_cpuinfo != expected_cpuinfo:
            raise RuntimeError(
                "/proc/cpuinfo feature mismatch: "
                f"expected {sorted(expected_cpuinfo)}, "
                f"got {sorted(actual_cpuinfo)}"
            )

        # The interpreter is a correctness fallback, not a practical carrier
        # for heavyweight optimizing runtimes. Keep its ordinary smoke path to
        # kernel, shell, and architectural-state coverage. Node/V8 and Wasm
        # remain useful translated-engine checks below.
        if arguments.engine == "interpreter":
            command(guest, "test \"$((6 * 7))\" -eq 42")
            print(
                f"PASS engine={arguments.engine} workload=shell "
                f"AT_HWCAP=0x{actual_hwcap:x} AT_HWCAP2=0x{actual_hwcap2:x}"
            )
            return 0

        node = "/opt/node/bin/node"
        node_path = command(guest, f"test -x {shlex.quote(node)} && echo present")
        if "present" not in node_path:
            raise RuntimeError(f"Node executable is missing: {node}")

        dynamic_output = command(
            guest,
            f"{shlex.quote(node)} -e "
            + shlex.quote(
                'const f=new Function("x","return (x+1)|0");'
                "let x=0;for(let i=0;i<1000000;i++)x=f(x);"
                'if(x!==1000000)throw Error("bad JIT result");'
                'console.log("v8-jit-ok")'
            ),
            180,
        )
        if "v8-jit-ok" not in dynamic_output:
            raise RuntimeError(f"Node JIT smoke marker is missing:\n{dynamic_output}")

        wasm_output = command(
            guest,
            f"{shlex.quote(node)} -e "
            + shlex.quote(
                "const b=Buffer.from("
                '"0061736d010000000105016000017f030201000707010372756e0000'
                '0a06010400412a0b","hex");'
                "const m=new WebAssembly.Module(b);"
                "const v=new WebAssembly.Instance(m).exports.run();"
                'if(v!==42)throw Error("bad wasm result");'
                'console.log("wasm-jit-ok")'
            ),
            180,
        )
        if "wasm-jit-ok" not in wasm_output:
            raise RuntimeError(f"WebAssembly smoke marker is missing:\n{wasm_output}")

        for repetition in range(2):
            teardown_output = command(
                guest, f"{shlex.quote(node)} -p " + shlex.quote("6*7"), 60
            )
            if not re.search(r"(?:^|\n)42(?:\n|$)", teardown_output):
                raise RuntimeError(
                    "repeated Node teardown smoke failed "
                    f"on run {repetition + 1}:\n{teardown_output}"
                )

        print(
            f"PASS engine={arguments.engine} "
            f"AT_HWCAP=0x{actual_hwcap:x} AT_HWCAP2=0x{actual_hwcap2:x}"
        )
        return 0
    except BaseException:
        print(guest.output(), file=sys.stderr)
        raise
    finally:
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
