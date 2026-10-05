#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""glibc's __libc_arm_za_disable() with a pending ZA lazy save.

With HWCAP2_SME, glibc 2.41 calls __libc_arm_za_disable() before fork,
vfork-style process creation and clone: when TPIDR2_EL0 points at a TPIDR2
block with a save buffer, it stores ZA there (the lazy save), clears TPIDR2
and turns ZA off. The probe loads ZA with a pattern through a small code
page, points TPIDR2 at a block, and after os.fork(), os.posix_spawn() and a
thread start checks that the buffer holds the pattern, TPIDR2 reads 0 and
SVCR.ZA is clear, in a forked child too. It needs an engine that executes
the SME control core. Its exit status is its failure count.
"""

import ctypes
import os
import sys
import threading

libc = ctypes.CDLL(None, use_errno=True)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long)
libc.mprotect.argtypes = (ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int)

PROT_READ, PROT_WRITE, PROT_EXEC = 1, 2, 4
MAP_PRIVATE, MAP_ANONYMOUS = 0x02, 0x20
RET = 0xD65F03C0

# Each routine starts at a multiple of eight words in the code page.
ROUTINES = {
    "write_tpidr2": [0xD51BD0A0, RET],          # msr tpidr2_el0, x0
    "read_tpidr2": [0xD53BD0A0, RET],           # mrs x0, tpidr2_el0
    "read_svcr": [0xD53B4240, RET],             # mrs x0, svcr
    "rdsvl": [0x04BF5820, RET],                 # rdsvl x0, #1
    # smstart za, then ldr za[w12, 0], [x0] for each of x1 rows.
    "load_za": [0xD503457F, 0x5280000C, 0xE1000000, 0x8B010000,
                0x1100058C, 0xEB01019F, 0x54FFFF83, RET],
    "smstop_za": [0xD503447F, RET],             # smstop za
}


def build_code() -> dict:
    size = 4096
    page = libc.mmap(None, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)
    if page in (None, ctypes.c_void_p(-1).value):
        raise OSError(ctypes.get_errno(), "mmap")
    words = (ctypes.c_uint32 * (size // 4)).from_address(page)
    addresses = {}
    for index, (name, code) in enumerate(ROUTINES.items()):
        assert len(code) <= 8
        for offset, word in enumerate(code):
            words[index * 8 + offset] = word
        addresses[name] = page + index * 32
    if libc.mprotect(page, size, PROT_READ | PROT_EXEC) != 0:
        raise OSError(ctypes.get_errno(), "mprotect")
    function = ctypes.CFUNCTYPE(ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64)
    return {name: function(address) for name, address in addresses.items()}


code = build_code()
svl = code["rdsvl"](0, 0)
rows = (ctypes.c_uint8 * (svl * svl))()
for index in range(svl * svl):
    rows[index] = (index * 7 + 3) & 0xFF
buffer = (ctypes.c_uint8 * (svl * svl))()
# The TPIDR2 block: save buffer, number of slices, then zero bytes.
block = (ctypes.c_uint64 * 2)()
failures = 0


def check(passed: bool, name: str) -> None:
    global failures
    if not passed:
        failures += 1
    print(f"{'PASS' if passed else 'FAIL'} glibc: {name}", flush=True)


def arm_lazy_save() -> None:
    ctypes.memset(buffer, 0, len(buffer))
    code["load_za"](ctypes.addressof(rows), svl)
    block[0] = ctypes.addressof(buffer)
    block[1] = svl
    code["write_tpidr2"](ctypes.addressof(block), 0)


def committed() -> bool:
    return (code["read_tpidr2"](0, 0) == 0 and
            (code["read_svcr"](0, 0) & 2) == 0 and
            bytes(buffer) == bytes(rows))


def main() -> int:
    print(f"glibc: streaming vector length {svl}", flush=True)

    arm_lazy_save()
    check(code["read_tpidr2"](0, 0) == ctypes.addressof(block)
          and code["read_svcr"](0, 0) & 2 == 2, "the lazy save is armed")
    pid = os.fork()
    if pid == 0:
        os._exit(0 if committed() else 1)
    _, status = os.waitpid(pid, 0)
    check(committed(), "fork commits the lazy save in the parent")
    check(os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
          "and the child starts with it committed")

    arm_lazy_save()
    pid = os.posix_spawn("/bin/true", ["true"], dict(os.environ))
    _, status = os.waitpid(pid, 0)
    check(committed() and os.WIFEXITED(status) and os.WEXITSTATUS(status) == 0,
          "posix_spawn commits the lazy save")

    arm_lazy_save()
    seen = []
    thread = threading.Thread(target=lambda: seen.append(code["read_tpidr2"](0, 0)))
    thread.start()
    thread.join()
    check(committed(), "thread creation commits the lazy save")
    check(seen == [0], "the new thread starts with TPIDR2 0")

    code["smstop_za"](0, 0)
    return failures


if __name__ == "__main__":
    sys.exit(main())
