#!/usr/bin/env python3
"""Boot one Darwin UML engine and validate its lightweight guest contract."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import ctypes
import pathlib
import re
import shlex
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


GUEST_HWCAP_SANITY = "/usr/local/libexec/uml-test/hwcap-sanity"
GUEST_CPU_IDENTITY = "/usr/local/libexec/uml-test/cpu-identity.py"
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
    ("hw.optional.arm.FEAT_JSCVT", HWCAP_JSCVT, "jscvt"),
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
OPTIONAL_HWCAPS2 = (
    ("hw.optional.arm.FEAT_FlagM2", HWCAP2_FLAGM2, "flagm2"),
    ("hw.optional.arm.FEAT_FRINTTS", HWCAP2_FRINT, "frint"),
    ("hw.optional.arm.FEAT_I8MM", HWCAP2_I8MM, "i8mm"),
    ("hw.optional.arm.FEAT_BF16", HWCAP2_BF16, "bf16"),
)
# Every AT_HWCAP and AT_HWCAP2 bit an engine may advertise, by the name arm64
# Linux gives it in /proc/cpuinfo. A set bit missing here fails the smoke,
# so a new advertisement has to name itself.
HWCAP_NAMES = {
    HWCAP_FP: "fp",
    HWCAP_ASIMD: "asimd",
    HWCAP_CPUID: "cpuid",
    **{bit: name for _, bit, name in OPTIONAL_HWCAPS},
}
HWCAP2_NAMES = {bit: name for _, bit, name in OPTIONAL_HWCAPS2}
# The lines of arm64 Linux's /proc/cpuinfo block (arch/arm64/kernel/cpuinfo.c)
# up to each value, plus the model name that it prints in the same place for a
# 32-bit task. A blank line ends every block.
CPUINFO_LINES = (
    "processor\t: ",
    "model name\t: ",
    "BogoMIPS\t: ",
    "Features\t: ",
    "CPU implementer\t: ",
    "CPU architecture: ",
    "CPU variant\t: ",
    "CPU part\t: ",
    "CPU revision\t: ",
)
# Reads MIDR_EL1 with MRS, as any task may, and asks LLVM for the host
# features it derives from /proc/cpuinfo, as llvmpipe and -mcpu=native do.
CPU_IDENTITY_PROBE = """\
import ctypes, glob, mmap, re, tempfile
libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long)
libc.mmap.restype = ctypes.c_void_p
# nop; mrs x0, midr_el1; ret, mapped from a file so the kernel keeps the
# instruction cache coherent.
with tempfile.TemporaryFile(dir="/tmp") as code:
    code.write(bytes.fromhex("1f2003d5000038d5c0035fd6"))
    code.flush()
    address = libc.mmap(None, mmap.PAGESIZE, mmap.PROT_READ | mmap.PROT_EXEC,
                        mmap.MAP_PRIVATE, code.fileno(), 0)
if address in (None, ctypes.c_void_p(-1).value):
    raise OSError(ctypes.get_errno(), "mmap of the MRS code failed")
print(f"midr=0x{ctypes.CFUNCTYPE(ctypes.c_uint64)(address)():x}")
libraries = glob.glob("/usr/lib/*-linux-gnu/libLLVM.so.*")
if not libraries:
    raise SystemExit("the rootfs has no libLLVM")
newest = max(libraries, key=lambda path: [
    int(part) for part in re.findall(r"[0-9]+", path.rsplit("/", 1)[1])])
features = ctypes.CDLL(newest).LLVMGetHostCPUFeatures
features.restype = ctypes.c_char_p
print(f"llvm={features().decode()}")
"""


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


def advertised_names(hwcap: int, hwcap2: int) -> list[str]:
    """The names of the set bits, AT_HWCAP's before AT_HWCAP2's, each word
    from bit 0 up: arm64 Linux's hwcap_str order."""
    names = []
    for word, value, table in (
        ("AT_HWCAP", hwcap, HWCAP_NAMES),
        ("AT_HWCAP2", hwcap2, HWCAP2_NAMES),
    ):
        for bit in range(64):
            if value & (1 << bit):
                if (1 << bit) not in table:
                    raise RuntimeError(f"{word} bit {bit} has no known name")
                names.append(table[1 << bit])
    return names


def cpuinfo_blocks(guest: Guest) -> list[dict[str, str]]:
    """Each processor's block of the file's exact bytes, by line label."""
    encoded = guest.check_command("base64 -w0 /proc/cpuinfo", 60)
    text = base64.b64decode("".join(encoded.split())).decode()
    blocks = text.split("\n\n")
    if blocks[-1] != "":
        raise RuntimeError(
            "/proc/cpuinfo does not end in a blank line: "
            f"...{text[-160:]!r}"
        )
    parsed = []
    for block in blocks[:-1]:
        lines = block.split("\n")
        if len(lines) != len(CPUINFO_LINES) or not all(
            line.startswith(label) for line, label in zip(lines, CPUINFO_LINES)
        ):
            raise RuntimeError(
                f"/proc/cpuinfo block {block!r} does not have the lines "
                f"{CPUINFO_LINES}"
            )
        parsed.append(
            {
                label.partition("\t")[0].partition(":")[0]: line[len(label) :]
                for line, label in zip(lines, CPUINFO_LINES)
            }
        )
    return parsed


def check_cpuinfo(
    guest: Guest, hwcap: int, hwcap2: int, midr: int
) -> None:
    """/proc/cpuinfo describes exactly the CPU a task sees in arm64 Linux's
    format: the names of its AT_HWCAP and AT_HWCAP2 bits in hwcap_str order,
    and the MIDR_EL1 fields MRS returns."""
    online = int(guest.check_command("getconf _NPROCESSORS_ONLN", 60))
    blocks = cpuinfo_blocks(guest)
    if len(blocks) != online:
        raise RuntimeError(
            f"/proc/cpuinfo has {len(blocks)} blocks for {online} online CPUs"
        )
    expected_features = " ".join(advertised_names(hwcap, hwcap2))
    expected_identity = {
        "CPU implementer": f"0x{(midr >> 24) & 0xff:02x}",
        # arm64 Linux prints 8 whatever MIDR_EL1's architecture field holds.
        "CPU architecture": "8",
        "CPU variant": f"0x{(midr >> 20) & 0xf:x}",
        "CPU part": f"0x{(midr >> 4) & 0xfff:03x}",
        "CPU revision": f"{midr & 0xf}",
    }
    for number, values in enumerate(blocks):
        if values["processor"] != str(number):
            raise RuntimeError(
                f"/proc/cpuinfo block {number} names processor "
                f"{values['processor']}"
            )
        if values["Features"] != expected_features:
            raise RuntimeError(
                "/proc/cpuinfo Features mismatch: "
                f"expected {expected_features!r}, got {values['Features']!r}"
            )
        for key, expected in expected_identity.items():
            if values[key] != expected:
                raise RuntimeError(
                    f"/proc/cpuinfo {key} is {values[key]!r}; MIDR_EL1 "
                    f"0x{midr:x} gives {expected!r}"
                )
        if {**values, "processor": "0"} != blocks[0]:
            raise RuntimeError(
                f"/proc/cpuinfo block {number} differs from block 0 "
                "beyond its processor number"
            )


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
        payloads={
            GUEST_HWCAP_SANITY: GuestFile(hwcap_sanity, 0o755),
            GUEST_CPU_IDENTITY: GuestFile(CPU_IDENTITY_PROBE.encode()),
        },
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
        if actual_hwcap != expected:
            raise RuntimeError(
                "AT_HWCAP mismatch: "
                f"expected 0x{expected:x}, got {actual_hwcap!r}"
            )
        expected2 = expected_hwcap(OPTIONAL_HWCAPS2)
        if actual_hwcap2 != expected2:
            raise RuntimeError(
                "AT_HWCAP2 mismatch: "
                f"expected 0x{expected2:x}, got 0x{actual_hwcap2:x}"
            )
        identity = guest.check_command(
            f"python3 -B {GUEST_CPU_IDENTITY}", 120
        )
        midr_match = re.search(r"^midr=0x([0-9a-f]+)$", identity, re.M)
        llvm_match = re.search(r"^llvm=(.*)$", identity, re.M)
        if midr_match is None or llvm_match is None:
            raise RuntimeError(f"CPU identity probe failed:\n{identity}")
        check_cpuinfo(
            guest, actual_hwcap, actual_hwcap2, int(midr_match.group(1), 16)
        )
        # LLVM finds the host's features only in the Features line; with
        # none it reports no feature at all, not even the baseline NEON.
        llvm_features = llvm_match.group(1).strip().split(",")
        if "+neon" not in llvm_features:
            raise RuntimeError(
                f"LLVM sees no host NEON: {llvm_match.group(1)!r}"
            )

        # Every advertised family must also execute correctly. The payload
        # reads the auxiliary vector itself and skips what is not advertised.
        guest.check_command(GUEST_HWCAP_SANITY, 60, expected="OK")

        node = "/opt/node/bin/node"
        guest.check_command(f"test -x {shlex.quote(node)}", 60)

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
