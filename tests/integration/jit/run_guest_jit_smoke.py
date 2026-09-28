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

from guest import Guest, GuestFile  # noqa: E402


GUEST_HWCAP_SANITY = "/usr/local/libexec/uml-test/hwcap-sanity"
HWCAP_FP = 1 << 0
HWCAP_ASIMD = 1 << 1
HWCAP_AES = 1 << 3
HWCAP_PMULL = 1 << 4
HWCAP_SHA1 = 1 << 5
HWCAP_SHA2 = 1 << 6
HWCAP_CRC32 = 1 << 7
HWCAP_ATOMICS = 1 << 8
HWCAP_FPHP = 1 << 9
HWCAP_ASIMDHP = 1 << 10
HWCAP_CPUID = 1 << 11
HWCAP_ASIMDRDM = 1 << 12
HWCAP_JSCVT = 1 << 13
HWCAP_FCMA = 1 << 14
HWCAP_LRCPC = 1 << 15
HWCAP_SHA3 = 1 << 17
HWCAP_ASIMDDP = 1 << 20
HWCAP_SHA512 = 1 << 21
HWCAP_ASIMDFHM = 1 << 23
HWCAP_DIT = 1 << 24
HWCAP_USCAT = 1 << 25
HWCAP_ILRCPC = 1 << 26
HWCAP_FLAGM = 1 << 27
HWCAP_SB = 1 << 29
HWCAP2_FLAGM2 = 1 << 7
HWCAP2_FRINT = 1 << 8
HWCAP2_I8MM = 1 << 13
HWCAP2_BF16 = 1 << 14
OPTIONAL_HWCAPS = (
    ("hw.optional.arm.FEAT_AES", HWCAP_AES, "aes"),
    ("hw.optional.arm.FEAT_PMULL", HWCAP_PMULL, "pmull"),
    ("hw.optional.arm.FEAT_SHA1", HWCAP_SHA1, "sha1"),
    ("hw.optional.arm.FEAT_SHA256", HWCAP_SHA2, "sha2"),
    ("hw.optional.arm.FEAT_CRC32", HWCAP_CRC32, "crc32"),
    ("hw.optional.arm.FEAT_LSE", HWCAP_ATOMICS, "atomics"),
    ("hw.optional.arm.FEAT_FP16", HWCAP_FPHP, "fphp"),
    ("hw.optional.arm.FEAT_FP16", HWCAP_ASIMDHP, "asimdhp"),
    ("hw.optional.arm.FEAT_RDM", HWCAP_ASIMDRDM, "asimdrdm"),
    ("hw.optional.arm.FEAT_FCMA", HWCAP_FCMA, "fcma"),
    ("hw.optional.arm.FEAT_LRCPC", HWCAP_LRCPC, "lrcpc"),
    ("hw.optional.arm.FEAT_SHA3", HWCAP_SHA3, "sha3"),
    ("hw.optional.arm.FEAT_DotProd", HWCAP_ASIMDDP, "asimddp"),
    ("hw.optional.arm.FEAT_SHA512", HWCAP_SHA512, "sha512"),
    ("hw.optional.arm.FEAT_FHM", HWCAP_ASIMDFHM, "asimdfhm"),
    ("hw.optional.arm.FEAT_DIT", HWCAP_DIT, "dit"),
    ("hw.optional.arm.FEAT_LSE2", HWCAP_USCAT, "uscat"),
    ("hw.optional.arm.FEAT_LRCPC2", HWCAP_ILRCPC, "ilrcpc"),
    ("hw.optional.arm.FEAT_FlagM", HWCAP_FLAGM, "flagm"),
    ("hw.optional.arm.FEAT_SB", HWCAP_SB, "sb"),
)
PROBE_DEPENDENT_HWCAPS = (
    ("hw.optional.arm.FEAT_JSCVT", HWCAP_JSCVT, "jscvt"),
)
OPTIONAL_HWCAPS2 = (
    ("hw.optional.arm.FEAT_FlagM2", HWCAP2_FLAGM2, "flagm2"),
    ("hw.optional.arm.FEAT_FRINTTS", HWCAP2_FRINT, "frint"),
    ("hw.optional.arm.FEAT_I8MM", HWCAP2_I8MM, "i8mm"),
    ("hw.optional.arm.FEAT_BF16", HWCAP2_BF16, "bf16"),
)
# The Darwin kernel's /proc/cpuinfo table is a separate diagnostic contract
# from auxv. Pin its currently published subset without hiding any extra or
# missing AT_HWCAP bit from the exact checks above.
CPUINFO_HWCAPS = (
    (HWCAP_AES, "aes"),
    (HWCAP_PMULL, "pmull"),
    (HWCAP_SHA1, "sha1"),
    (HWCAP_SHA2, "sha2"),
    (HWCAP_CRC32, "crc32"),
    (HWCAP_ATOMICS, "atomics"),
    (HWCAP_SHA3, "sha3"),
    (HWCAP_SHA512, "sha512"),
)


def host_feature(name: str) -> bool:
    sysctlbyname = ctypes.CDLL(None, use_errno=True).sysctlbyname
    sysctlbyname.argtypes = (
        ctypes.c_char_p,
        ctypes.c_void_p,
        ctypes.POINTER(ctypes.c_size_t),
        ctypes.c_void_p,
        ctypes.c_size_t,
    )
    sysctlbyname.restype = ctypes.c_int
    enabled = ctypes.c_int()
    size = ctypes.c_size_t(ctypes.sizeof(enabled))
    status = sysctlbyname(
        name.encode(), ctypes.byref(enabled), ctypes.byref(size), None, 0
    )
    return (
        status == 0
        and size.value == ctypes.sizeof(enabled)
        and enabled.value != 0
    )


def expected_hwcap(
    features: tuple[tuple[str, int, str], ...], initial: int = 0
) -> int:
    result = initial
    for name, bit, _ in features:
        if host_feature(name):
            result |= bit
    return result


def auxiliary_vector(guest: Guest) -> dict[int, int]:
    output = guest.check_command("od -An -v -t u8 /proc/self/auxv", 60)
    words = [int(value) for value in re.findall(r"\b[0-9]+\b", output)]
    if len(words) % 2:
        raise RuntimeError(f"malformed auxiliary vector output:\n{output}")
    return dict(zip(words[0::2], words[1::2]))


def cpuinfo_features(guest: Guest) -> set[str]:
    output = guest.check_command("grep '^flags' /proc/cpuinfo | head -n 1", 60)
    match = re.search(r"flags\s*:\s*(.*)", output)
    if match is None:
        raise RuntimeError(f"/proc/cpuinfo flags are missing:\n{output}")
    return set(match.group(1).split())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--emulator", type=pathlib.Path, required=True)
    parser.add_argument("--rootfs", type=pathlib.Path, required=True)
    parser.add_argument("--memory", default="8G")
    parser.add_argument(
        "--engine", choices=("jit", "gadgets"), default="jit"
    )
    parser.add_argument(
        "--hwcap-sanity-binary", type=pathlib.Path, required=True
    )
    arguments = parser.parse_args()
    hwcap_sanity = arguments.hwcap_sanity_binary.read_bytes()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        arguments.memory,
        "tmpfs",
        init_shell=True,
        engine=arguments.engine,
        payloads={GUEST_HWCAP_SANITY: GuestFile(hwcap_sanity, 0o755)},
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command(
            "mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp", 60
        )

        auxv = auxiliary_vector(guest)
        actual_hwcap = auxv.get(16)
        actual_hwcap2 = auxv.get(26, 0)
        # HWCAP_CPUID is unconditional, exactly as arm64 Linux advertises it:
        # every engine emulates the EL0 identification-register reads.
        expected = expected_hwcap(
            OPTIONAL_HWCAPS, HWCAP_FP | HWCAP_ASIMD | HWCAP_CPUID
        )
        probe_permitted = expected_hwcap(PROBE_DEPENDENT_HWCAPS)
        if (
            actual_hwcap is None
            or (actual_hwcap & ~HWCAP_JSCVT) != expected
            or (actual_hwcap & HWCAP_JSCVT & ~probe_permitted) != 0
        ):
            raise RuntimeError(
                "AT_HWCAP mismatch: "
                f"expected 0x{expected:x} with probe-dependent mask "
                f"0x{probe_permitted:x}, got {actual_hwcap!r}"
            )
        expected2 = expected_hwcap(OPTIONAL_HWCAPS2)
        if actual_hwcap2 != expected2:
            raise RuntimeError(
                "AT_HWCAP2 mismatch: "
                f"expected 0x{expected2:x}, got 0x{actual_hwcap2:x}"
            )
        expected_cpuinfo = {
            flag
            for bit, flag in CPUINFO_HWCAPS
            if actual_hwcap & bit
        }
        expected_cpuinfo.update(("fp", "asimd"))
        actual_cpuinfo = cpuinfo_features(guest)
        if actual_cpuinfo != expected_cpuinfo:
            raise RuntimeError(
                "/proc/cpuinfo feature mismatch: "
                f"expected {sorted(expected_cpuinfo)}, "
                f"got {sorted(actual_cpuinfo)}"
            )

        # Every advertised family must also execute correctly. The payload
        # reads the auxiliary vector itself and skips what is not advertised.
        guest.check_command(GUEST_HWCAP_SANITY, 60, expected="OK")

        node = "/opt/node/bin/node"
        node_path = guest.check_command(f"test -x {shlex.quote(node)} && echo present", 60)
        if "present" not in node_path:
            raise RuntimeError(f"Node executable is missing: {node}")

        dynamic_output = guest.check_command(
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

        wasm_output = guest.check_command(
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
            teardown_output = guest.check_command(
                f"{shlex.quote(node)} -p " + shlex.quote("6*7"), 60
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


if __name__ == "__main__":
    raise SystemExit(main())
