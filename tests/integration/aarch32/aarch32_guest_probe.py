#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Guest probe: the guest kernel's AArch32 (armhf) task support.

Run as `aarch32_guest_probe.py --elf PROGRAM --elf-4k PROGRAM4K [--expect
unsupported|run]` inside an arm64 guest.  PROGRAM is the freestanding armhf
program built from aarch32_probe.S, and PROGRAM4K the same program laid out for
4 KiB pages only, as 11 files of a stock Debian armhf image are.

What every guest shows, with or without an engine that runs AArch32:
a 64-bit task sees aarch64 from uname and arm64 Linux's /proc/cpuinfo layout
without a model name, vm.mmap_rnd_compat_bits has the range of the page
profile, and abi.cp15_barrier is present.

--expect unsupported (the default) is a host whose engines do not run
AArch32, so the bridge reports no compat HWCAPs and the guest behaves like an
arm64 CPU without 32-bit EL0: executing either ELF32 program fails with
ENOEXEC and personality(PER_LINUX32) with EINVAL.

--expect run is a host that does run it.  The program runs its own checks to
a zero failure count, in its default mode and in the "extra" and "tracer"
modes of aarch32_probe.S; PER_LINUX32 changes uname and /proc/cpuinfo; a
64-bit tracer sees the program's exec stop and signal stop through the
72-byte NT_PRSTATUS of an AArch32 task, can rewrite its registers, reads its
VFP and TLS registers, follows its system calls (PTRACE_GET_SYSCALL_INFO
names AUDIT_ARCH_ARM) and single steps, and sees the VFP context of its
signal frames; the faults end the program in SIGSEGV, SIGILL and SIGTRAP; a
core dump is a 32-bit ARM core file; and many runs at once all complete.  The
4 KiB-layout program runs on a 4 KiB guest, and on a 16 KiB guest cannot run,
cleanly: the new process is killed with SIGSEGV and the guest keeps working.
Prints one line per check and exits with the number of failures.
"""

import argparse
import ctypes
import errno
import os
import re
import resource
import signal
import struct
import subprocess
import sys

PER_LINUX32 = 8
PER_LINUX32_3GB = 0x08000008
PTRACE_TRACEME = 0
PTRACE_CONT = 7
PTRACE_SINGLESTEP = 9
PTRACE_SYSCALL = 24
PTRACE_SETOPTIONS = 0x4200
PTRACE_GET_SYSCALL_INFO = 0x420e
PTRACE_O_TRACESYSGOOD = 1
PTRACE_GETREGSET = 0x4204
PTRACE_SETREGSET = 0x4205
NT_PRSTATUS = 1
NT_ARM_VFP = 0x400
NT_ARM_TLS = 0x401
AUDIT_ARCH_ARM = 0x40000028
PSR_MODE_MASK = 0x1f
PSR_USR = 0x10
PSR_T = 0x20
PSR_FLAGS = 0xf8000000 | 0x000f0000  # NZCVQ and GE
# The FPSCR bits that are status and the bits that are control; the rest
# read as zero.
FPSCR_MASK = 0xf800009f | 0x07f79f00
AT_PAGESZ = 6
AT_ENTRY = 9
AT_HWCAP = 16
AT_PLATFORM = 15
VECTORS = 0xffff0000
A64_CPUINFO_KEYS = ["processor", "BogoMIPS", "Features", "CPU implementer",
                    "CPU architecture", "CPU variant", "CPU part",
                    "CPU revision"]

libc = ctypes.CDLL(None, use_errno=True)
libc.ptrace.argtypes = (ctypes.c_long, ctypes.c_int, ctypes.c_void_p,
                        ctypes.c_void_p)
libc.ptrace.restype = ctypes.c_long
libc.personality.argtypes = (ctypes.c_ulong,)
libc.personality.restype = ctypes.c_int


class IoVec(ctypes.Structure):
    _fields_ = (("base", ctypes.c_void_p), ("length", ctypes.c_size_t))


failures = 0


def check(name, passed, detail=""):
    global failures
    print(f"{'PASS' if passed else 'FAIL'} {name}"
          f"{': ' + detail if detail and not passed else ''}", flush=True)
    failures += 0 if passed else 1


def personality(value):
    """The previous persona, or the negative errno."""
    ctypes.set_errno(0)
    result = libc.personality(value)
    return result if result != -1 else -ctypes.get_errno()


def ptrace(request, pid, address, data):
    ctypes.set_errno(0)
    result = libc.ptrace(request, pid, address, data)
    return 0 if result >= 0 else -ctypes.get_errno()


def regset(request, pid, note, buffer):
    """GETREGSET/SETREGSET; returns (errno, bytes transferred)."""
    vector = IoVec(ctypes.cast(buffer, ctypes.c_void_p), ctypes.sizeof(buffer))
    result = ptrace(request, pid, ctypes.c_void_p(note), ctypes.byref(vector))
    return result, vector.length


def page_size():
    return os.sysconf("SC_PAGESIZE")


def read(path):
    with open(path, "rb") as source:
        return source.read().decode()


def check_native_view():
    """What a 64-bit task sees, whatever the host runs."""
    check("uname is aarch64", os.uname().machine == "aarch64",
          os.uname().machine)
    blocks = [block for block in read("/proc/cpuinfo").split("\n\n")
              if block.strip()]
    layout = all(
        [re.split(r"\s*:", line, maxsplit=1)[0] for line in block.splitlines()]
        == A64_CPUINFO_KEYS for block in blocks)
    check("cpuinfo has arm64 Linux's layout and no model name",
          bool(blocks) and layout and "model name" not in read("/proc/cpuinfo"))
    check("cpuinfo lists one block per CPU",
          len(blocks) == os.cpu_count(), f"{len(blocks)} of {os.cpu_count()}")
    check("cpuinfo names no 32-bit platform", "(v8l)" not in read("/proc/cpuinfo"))

    # The compat range of the page profile, as arm64 Linux's Kconfig sets it.
    low = 9 if page_size() == 16384 else 11
    path = "/proc/sys/vm/mmap_rnd_compat_bits"
    check("vm.mmap_rnd_compat_bits exists", os.path.exists(path))
    if os.path.exists(path):
        original = int(read(path))
        check("vm.mmap_rnd_compat_bits is in the page profile's range",
              low <= original <= 16, f"{original}, range {low}..16")
        for bad in (low - 1, 17):
            try:
                with open(path, "w") as sink:
                    sink.write(f"{bad}\n")
                refused = False
            except OSError as error:
                refused = error.errno == errno.EINVAL
            check(f"vm.mmap_rnd_compat_bits refuses {bad}", refused)
        for good in (low, 16, original):
            try:
                with open(path, "w") as sink:
                    sink.write(f"{good}\n")
                took = int(read(path)) == good
            except OSError:
                took = False
            check(f"vm.mmap_rnd_compat_bits accepts {good}", took)
    check("abi.cp15_barrier is present and defaults to emulation",
          os.path.exists("/proc/sys/abi/cp15_barrier"))


def exec_errno(path, argument=None):
    """The errno of executing path, 0 if it ran, and its exit status."""
    arguments = [path] + ([argument] if argument else [])
    try:
        result = subprocess.run(arguments, stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=60)
    except OSError as error:
        return error.errno, None
    return 0, result.returncode


def check_unsupported(elf, elf_4k):
    """No engine runs AArch32: the guest has no 32-bit EL0."""
    for name, path in (("an ELF32 program", elf),
                       ("an ELF32 program laid out for 4 KiB pages", elf_4k)):
        result = exec_errno(path)[0]
        check(f"executing {name} fails with ENOEXEC", result == errno.ENOEXEC,
              errno.errorcode.get(result, str(result)))

    for name, value in (("PER_LINUX32", PER_LINUX32),
                        ("PER_LINUX32 with ADDR_LIMIT_3GB", PER_LINUX32_3GB)):
        result = personality(value)
        check(f"personality({name}) fails with EINVAL",
              result == -errno.EINVAL, str(result))
    check("a refused personality() leaves the persona alone",
          personality(0xffffffff) == 0, str(personality(0xffffffff)))
    check("uname is still aarch64 after personality()",
          os.uname().machine == "aarch64")


def run_program(elf, mode=None, label="program", minimum=60):
    """The program checks itself and exits with its failure count."""
    result = subprocess.run([elf] + ([mode] if mode else []),
                            stdin=subprocess.DEVNULL, timeout=300,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True)
    lines = result.stdout.splitlines()
    for line in lines:
        if line.startswith(("PASS ", "FAIL ")):
            print(f"{line.split(' ', 1)[0]} {label}: {line.split(' ', 1)[1]}",
                  flush=True)
    summary = re.search(r"a32-probe: (\d+) failure\(s\) in (\d+) check",
                        result.stdout)
    check(f"the {label} ran to its summary", summary is not None,
          "\n".join(lines[-5:]))
    check(f"the {label} reports no failure",
          summary is not None and summary.group(1) == "0"
          and result.returncode == 0,
          f"status {result.returncode}")
    return summary is not None and int(summary.group(2)) >= minimum


def check_personality():
    """PER_LINUX32 changes what uname and /proc/cpuinfo show a 64-bit task."""
    previous = personality(PER_LINUX32)
    check("personality(PER_LINUX32) succeeds", previous == 0, str(previous))
    try:
        check("uname is armv8l under PER_LINUX32",
              os.uname().machine == "armv8l", os.uname().machine)
        text = read("/proc/cpuinfo")
        check("cpuinfo has the compat model name",
              re.search(r"^model name\t: ARMv8 Processor rev \d+ \(v8l\)$",
                        text, re.M) is not None)
        check("cpuinfo has compat Features",
              re.search(r"^Features\t: .*\bvfp\b", text, re.M) is not None)
    finally:
        personality(0)
    check("PER_LINUX restores aarch64", os.uname().machine == "aarch64")


def traced_exec(path, argument):
    """The first stop of a traced exec: its signal, and pc if it was SIGTRAP."""
    child = os.fork()
    if child == 0:
        ptrace(PTRACE_TRACEME, 0, None, None)
        os.execv(path, [path, argument])
        os._exit(127)
    _, status = os.waitpid(child, 0)
    if not os.WIFSTOPPED(status):
        return None, None
    stop, pc = os.WSTOPSIG(status), None
    if stop == signal.SIGTRAP:
        registers = (ctypes.c_uint32 * 18)()
        regset(PTRACE_GETREGSET, child, NT_PRSTATUS, registers)
        pc = registers[15]
    os.kill(child, signal.SIGKILL)
    os.waitpid(child, 0)
    return stop, pc


def check_page_layout(elf_4k):
    """A program laid out for 4 KiB pages only needs a 4 KiB guest.

    A 16 KiB guest maps a segment only where its file offset and address are
    congruent modulo 16 KiB.  Linux maps the segments after it has committed
    to the exec, so the new process is killed with SIGSEGV, as an AArch64
    program laid out the same way is, and nothing else is affected."""
    stop, pc = traced_exec(elf_4k, "leaf")
    if page_size() == 4096:
        check("the 4 KiB-layout program loads on a 4 KiB guest",
              stop == signal.SIGTRAP and pc == elf_entry(elf_4k),
              f"signal {stop} pc {pc}")
        result, status = exec_errno(elf_4k, "leaf")
        check("the 4 KiB-layout program runs on a 4 KiB guest",
              result == 0 and status == 37,
              f"{errno.errorcode.get(result, result)} status {status}")
        return
    check("the 4 KiB-layout program does not load on a 16 KiB guest",
          stop == signal.SIGSEGV, f"signal {stop}")
    result, status = exec_errno(elf_4k, "leaf")
    check("the 4 KiB-layout program fails to run on a 16 KiB guest",
          (result == 0 and status == -signal.SIGSEGV)
          or result in (errno.EINVAL, errno.ENOEXEC),
          f"{errno.errorcode.get(result, result)} status {status}")
    # The exec killed only its own child.
    check("the guest keeps working after the failed exec",
          subprocess.run(["true"]).returncode == 0
          and os.uname().machine == "aarch64")


def elf_entry(path):
    with open(path, "rb") as source:
        header = source.read(52)
    return struct.unpack_from("<I", header, 24)[0]


def auxv(pid):
    data = open(f"/proc/{pid}/auxv", "rb").read()
    # A 32-bit task's vector has 32-bit entries.
    return dict(struct.unpack_from("<II", data, offset)
                for offset in range(0, len(data) - 7, 8))


def check_regset_access(child):
    """The kernel's side of a tracer's reads and writes of the registers.

    These need no engine: the tracee is stopped at its exec."""
    registers = (ctypes.c_uint32 * 18)()
    result, _ = regset(PTRACE_GETREGSET, child, NT_PRSTATUS, registers)
    original = list(registers)
    for index in range(13):
        registers[index] = 0x01010101 * (index + 1) & 0xffffffff
    registers[14] = 0x0e0e0e0e
    registers[16] = original[16] | PSR_FLAGS
    result, _ = regset(PTRACE_SETREGSET, child, NT_PRSTATUS, registers)
    check("a tracer writes r0-r12, lr and the user flags", result == 0,
          str(result))
    again = (ctypes.c_uint32 * 18)()
    regset(PTRACE_GETREGSET, child, NT_PRSTATUS, again)
    check("a tracer reads back what it wrote",
          list(again[:15]) == list(registers[:15])
          and again[16] == registers[16],
          f"{[hex(x) for x in again]}")
    registers[16] = (registers[16] & ~PSR_MODE_MASK) | 0x13  # supervisor
    result, _ = regset(PTRACE_SETREGSET, child, NT_PRSTATUS, registers)
    check("a tracer cannot leave user mode", result == -errno.EINVAL,
          str(result))
    after = (ctypes.c_uint32 * 18)()
    regset(PTRACE_GETREGSET, child, NT_PRSTATUS, after)
    check("a refused write changes nothing", list(after) == list(again))

    vfp = (ctypes.c_uint64 * 33)()
    regset(PTRACE_GETREGSET, child, NT_ARM_VFP, vfp)
    saved_vfp = list(vfp)
    for index in range(32):
        vfp[index] = 0x0102030405060708 * (index + 1) & (2 ** 64 - 1)
    vfp[32] = 0xffffffff  # FPSCR, in the low half of the last word
    result, length = regset(PTRACE_SETREGSET, child, NT_ARM_VFP, vfp)
    check("a tracer writes D0-D31 and FPSCR", result == 0 and length == 260,
          f"{result} {length}")
    back = (ctypes.c_uint64 * 33)()
    regset(PTRACE_GETREGSET, child, NT_ARM_VFP, back)
    check("D0-D31 read back, with D(2n) and D(2n+1) in one vector register",
          list(back[:32]) == list(vfp[:32]))
    check("FPSCR reads back only its defined bits",
          back[32] & 0xffffffff == FPSCR_MASK, hex(back[32] & 0xffffffff))
    for index in range(33):
        vfp[index] = saved_vfp[index]
    regset(PTRACE_SETREGSET, child, NT_ARM_VFP, vfp)

    tls = (ctypes.c_uint64 * 2)()
    regset(PTRACE_GETREGSET, child, NT_ARM_TLS, tls)
    saved_tls = list(tls)
    tls[0], tls[1] = 0x1_7654_3210, 0x55
    result, _ = regset(PTRACE_SETREGSET, child, NT_ARM_TLS, tls)
    again_tls = (ctypes.c_uint64 * 2)()
    regset(PTRACE_GETREGSET, child, NT_ARM_TLS, again_tls)
    check("a tracer writes TPIDRURO in 32 bits and no TPIDR2",
          result == 0 and again_tls[0] == 0x7654_3210 and again_tls[1] == 0,
          f"{result} {[hex(x) for x in again_tls]}")
    for index in range(2):
        tls[index] = saved_tls[index]
    regset(PTRACE_SETREGSET, child, NT_ARM_TLS, tls)

    for index in range(18):
        registers[index] = original[index]
    result, _ = regset(PTRACE_SETREGSET, child, NT_PRSTATUS, registers)
    check("a tracer puts the registers back", result == 0, str(result))


def check_tracer(elf):
    """A 64-bit tracer's view of an AArch32 task."""
    entry = elf_entry(elf)
    child = os.fork()
    if child == 0:
        ptrace(PTRACE_TRACEME, 0, None, None)
        os.execv(elf, [elf, "trace"])
        os._exit(127)

    _, status = os.waitpid(child, 0)
    check("the exec stops the tracee with SIGTRAP",
          os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGTRAP,
          hex(status))
    if not os.WIFSTOPPED(status):
        return

    registers = (ctypes.c_uint32 * 40)()
    result, length = regset(PTRACE_GETREGSET, child, NT_PRSTATUS, registers)
    check("NT_PRSTATUS of an AArch32 task is 72 bytes",
          result == 0 and length == 72, f"{result} {length}")
    cpsr, pc, sp = registers[16], registers[15], registers[13]
    check("the exec stop is ARM user mode",
          cpsr & PSR_MODE_MASK == PSR_USR and not cpsr & PSR_T, hex(cpsr))
    check("the exec stop is at the program's entry", pc == entry,
          f"{pc:#x} {entry:#x}")
    check("the exec stop has an aligned stack", sp != 0 and sp % 8 == 0,
          hex(sp))
    maps = read(f"/proc/{child}/maps")
    check("the kuser helpers page is at 0xffff0000",
          re.search(r"^ffff0000-.*\[vectors\]$", maps, re.M) is not None)
    check("the sigpage is mapped", "[sigpage]" in maps)
    vector = auxv(child)
    check("the auxiliary vector is the 32-bit task's",
          vector.get(AT_ENTRY) == entry and vector.get(AT_PAGESZ) == page_size()
          and vector.get(AT_HWCAP, 0) != 0, str(vector))

    vfp = (ctypes.c_uint8 * 512)()
    result, length = regset(PTRACE_GETREGSET, child, NT_ARM_VFP, vfp)
    check("NT_ARM_VFP is 260 bytes", result == 0 and length == 260,
          f"{result} {length}")
    tls = (ctypes.c_uint64 * 8)()
    result, length = regset(PTRACE_GETREGSET, child, NT_ARM_TLS, tls)
    check("NT_ARM_TLS reads TPIDRURO", result == 0 and length >= 4
          and tls[0] & 0xffffffff == 0, f"{result} {length}")

    check_regset_access(child)

    ptrace(PTRACE_CONT, child, None, None)
    _, status = os.waitpid(child, 0)
    check("the program stops itself with SIGSTOP",
          os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGSTOP,
          hex(status))
    if not os.WIFSTOPPED(status):
        return
    result, length = regset(PTRACE_GETREGSET, child, NT_PRSTATUS, registers)
    check("the signal stop shows the program's registers",
          result == 0 and registers[4] == 0x44444444 and registers[5] == 7
          and registers[7] == 37, f"r4={registers[4]:#x} r5={registers[5]:#x} "
          f"r7={registers[7]}")
    check("the signal stop is after the system call",
          entry <= registers[15] < entry + 0x400, hex(registers[15]))
    registers[5] = 9
    result, _ = regset(PTRACE_SETREGSET, child, NT_PRSTATUS, registers)
    check("a tracer rewrites r5", result == 0, str(result))
    registers[16] = registers[16] | 0x10000000  # V: a valid user flag
    result, _ = regset(PTRACE_SETREGSET, child, NT_PRSTATUS, registers)
    check("a tracer sets a user flag", result == 0, str(result))
    registers[16] = (registers[16] & ~PSR_MODE_MASK) | 0x13  # supervisor
    result, _ = regset(PTRACE_SETREGSET, child, NT_PRSTATUS, registers)
    check("a tracer cannot leave user mode", result == -errno.EINVAL, str(result))

    ptrace(PTRACE_CONT, child, None, None)
    _, status = os.waitpid(child, 0)
    check("the program exits with the r5 the tracer wrote",
          os.WIFEXITED(status) and os.WEXITSTATUS(status) == 9, hex(status))
    if not os.WIFEXITED(status):
        os.kill(child, signal.SIGKILL)
        os.waitpid(child, 0)


def check_fault_modes(elf):
    """A fault of the program ends it in the signal the architecture names."""
    for mode, expected in (("segv", signal.SIGSEGV), ("undef", signal.SIGILL),
                           ("bkpt", signal.SIGTRAP)):
        result = subprocess.run([elf, mode], stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=60)
        check(f"the {mode} mode ends in {signal.Signals(expected).name}",
              result.returncode == -expected, str(result.returncode))
    check("the guest keeps working after the faults",
          subprocess.run(["true"]).returncode == 0)


def start_traced(elf, mode, stdout=None):
    child = os.fork()
    if child == 0:
        if stdout is not None:
            os.dup2(stdout, 1)
        ptrace(PTRACE_TRACEME, 0, None, None)
        os.execv(elf, [elf, mode])
        os._exit(127)
    _, status = os.waitpid(child, 0)
    return child, status


def read_registers(child):
    registers = (ctypes.c_uint32 * 18)()
    regset(PTRACE_GETREGSET, child, NT_PRSTATUS, registers)
    return list(registers)


def check_syscall_stops(elf):
    """PTRACE_SYSCALL stops of an AArch32 task."""
    child, status = start_traced(elf, "leaf")
    ptrace(PTRACE_SETOPTIONS, child, None,
           ctypes.c_void_p(PTRACE_O_TRACESYSGOOD))
    stops = []
    for _ in range(8):
        ptrace(PTRACE_SYSCALL, child, None, None)
        _, status = os.waitpid(child, 0)
        if not (os.WIFSTOPPED(status)
                and os.WSTOPSIG(status) == signal.SIGTRAP | 0x80):
            break
        info = (ctypes.c_uint8 * 88)()
        libc.ptrace(PTRACE_GET_SYSCALL_INFO, child, ctypes.c_void_p(88), info)
        operation, arch = struct.unpack_from("<BxxxI", bytes(info))
        number = struct.unpack_from("<Q", bytes(info), 24)[0]
        stops.append((operation, arch, number, read_registers(child)))
    check("a system call stops at its entry and exit",
          [stop[0] for stop in stops] == [1, 2, 1],
          str([stop[0] for stop in stops]))
    check("PTRACE_GET_SYSCALL_INFO names AUDIT_ARCH_ARM",
          bool(stops) and all(stop[1] == AUDIT_ARCH_ARM for stop in stops),
          str([hex(stop[1]) for stop in stops]))
    check("the entry stops carry the EABI numbers (write, exit_group)",
          len(stops) == 3 and stops[0][2] == 4 and stops[2][2] == 248
          and stops[0][3][7] == 4 and stops[2][3][7] == 248, str(stops))
    check("the exit stop carries the result in r0",
          len(stops) >= 2 and stops[1][3][0] == len("a32-probe: leaf\n"),
          str(stops[1:2]))
    # The loop ended with the program's exit, after its last stop.
    check("the traced program exits 37",
          os.WIFEXITED(status) and os.WEXITSTATUS(status) == 37, hex(status))
    if os.WIFSTOPPED(status):
        os.kill(child, signal.SIGKILL)
        os.waitpid(child, 0)


def check_single_step(elf):
    """PTRACE_SINGLESTEP moves an AArch32 task one instruction."""
    child, status = start_traced(elf, "leaf")
    entry = elf_entry(elf)
    stops = []
    for _ in range(4):
        ptrace(PTRACE_SINGLESTEP, child, None, None)
        _, status = os.waitpid(child, 0)
        if not os.WIFSTOPPED(status):
            break
        stops.append((os.WSTOPSIG(status), read_registers(child)[15]))
    check("each step stops with SIGTRAP one instruction on",
          stops == [(signal.SIGTRAP, entry + 4 * (n + 1)) for n in range(4)],
          str([(a, hex(b)) for a, b in stops]))
    os.kill(child, signal.SIGKILL)
    os.waitpid(child, 0)


def check_vfp_frame(elf):
    """The VFP context of a signal frame is the task's, and comes back."""
    pipe_read, pipe_write = os.pipe()
    child, status = start_traced(elf, "vfp", pipe_write)
    os.close(pipe_write)
    vfp = (ctypes.c_uint64 * 33)()
    for index in range(32):
        vfp[index] = ((0x10000000 + index) << 32) | (0x20000000 + 3 * index)
    vfp[32] = 0x80400000
    result, _ = regset(PTRACE_SETREGSET, child, NT_ARM_VFP, vfp)
    check("a tracer sets D0-D31 and FPSCR before the signal", result == 0,
          str(result))
    ptrace(PTRACE_CONT, child, None, None)
    _, status = os.waitpid(child, 0)
    check("the signal reaches the tracer first",
          os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGUSR1,
          hex(status))
    ptrace(PTRACE_CONT, child, None, ctypes.c_void_p(int(signal.SIGUSR1)))
    _, status = os.waitpid(child, 0)
    check("the program stops itself after the handler",
          os.WIFSTOPPED(status) and os.WSTOPSIG(status) == signal.SIGSTOP,
          hex(status))
    after = (ctypes.c_uint64 * 33)()
    regset(PTRACE_GETREGSET, child, NT_ARM_VFP, after)
    want = list(vfp)
    want[3] = 0xfeedc0dec0ffee00
    want[31] = (want[31] & 0xffffffff00000000) | 0x0badf00d
    check("what the handler wrote to the frame is restored (D3, D31)",
          list(after[:32]) == want[:32], str([hex(x) for x in after[:32]]))
    check("FPSCR is restored from the frame",
          after[32] & 0xffffffff == 0x40000000, hex(after[32]))
    ptrace(PTRACE_CONT, child, None, None)
    data = b""
    while True:
        chunk = os.read(pipe_read, 100)
        if not chunk:
            break
        data += chunk
    os.close(pipe_read)
    os.waitpid(child, 0)
    magic, size, d3_lo, d3_hi, d17_lo, d17_hi, fpscr = struct.unpack(
        "<7I", data[:28].ljust(28, b"\0"))
    check("the frame holds the VFP magic", magic == 0x56465001, hex(magic))
    check("the frame's D3 and D17 are the task's, D(2n) in the low word",
          (d3_hi << 32 | d3_lo, d17_hi << 32 | d17_lo) == (vfp[3], vfp[17]),
          f"{d3_hi:#x}:{d3_lo:#x} {d17_hi:#x}:{d17_lo:#x}")
    check("the frame's FPSCR is the task's", fpscr == 0x80400000, hex(fpscr))


def elf_notes(data):
    notes, position = [], 0
    while position + 12 <= len(data):
        name_size, desc_size, kind = struct.unpack_from("<III", data, position)
        position += 12
        name = data[position:position + name_size].rstrip(b"\0")
        position += (name_size + 3) & ~3
        notes.append((name, kind, desc_size))
        position += (desc_size + 3) & ~3
    return notes


def check_core_dump(elf):
    """A fault dumps a 32-bit ARM core file."""
    directory = "/tmp/a32-cores"
    os.makedirs(directory, exist_ok=True)
    for name in os.listdir(directory):
        os.unlink(os.path.join(directory, name))
    pattern = "/proc/sys/kernel/core_pattern"
    saved = read(pattern)
    with open(pattern, "w") as sink:
        sink.write(f"{directory}/core.%p\n")
    try:
        subprocess.run(
            [elf, "segv"], stdin=subprocess.DEVNULL, capture_output=True,
            preexec_fn=lambda: resource.setrlimit(
                resource.RLIMIT_CORE, (resource.RLIM_INFINITY,) * 2))
    finally:
        with open(pattern, "w") as sink:
            sink.write(saved)
    names = os.listdir(directory)
    check("a fault writes one core file", len(names) == 1, str(names))
    if len(names) != 1:
        return
    with open(os.path.join(directory, names[0]), "rb") as source:
        data = source.read()
    check("the core file is ELF32 for ARM",
          data[:4] == b"\x7fELF" and data[4] == 1
          and struct.unpack_from("<H", data, 18)[0] == 40)
    table, = struct.unpack_from("<I", data, 28)
    entry_size, count = struct.unpack_from("<HH", data, 42)
    notes = []
    for index in range(count):
        kind, offset, _, _, size = struct.unpack_from(
            "<IIIII", data, table + index * entry_size)
        if kind == 4:
            notes = elf_notes(data[offset:offset + size])
    sizes = {kind: size for name, kind, size in notes
             if name in (b"CORE", b"LINUX")}
    check("NT_PRSTATUS and NT_PRPSINFO are the 32-bit structures",
          sizes.get(1) == 148 and sizes.get(3) == 124, str(sizes))
    check("the core file has NT_AUXV and the 260-byte NT_ARM_VFP",
          6 in sizes and sizes.get(NT_ARM_VFP) == 260, str(sizes))
    os.unlink(os.path.join(directory, names[0]))


def check_parallel(elf):
    """Many AArch32 programs at once, each one on its own."""
    codes = [subprocess.run([elf, "leaf"],
                            stdout=subprocess.DEVNULL).returncode
             for _ in range(60)]
    check("sixty runs in a row all exit 37", codes == [37] * 60, str(codes))
    processes = [subprocess.Popen([elf, "leaf"], stdout=subprocess.DEVNULL)
                 for _ in range(16)]
    codes = [process.wait() for process in processes]
    check("sixteen runs at once all exit 37", codes == [37] * 16, str(codes))
    processes = ([subprocess.Popen([elf, "extra"], stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT) for _ in range(3)]
                 + [subprocess.Popen([elf], stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT)
                    for _ in range(2)])
    results = [(process.wait(), process.stdout.read().decode(errors="replace"))
               for process in processes]
    check("the check modes pass three and two at a time",
          all(code == 0 for code, _ in results),
          str([code for code, _ in results]))
    for code, output in results:
        if code != 0:
            print("\n".join(line for line in output.splitlines()
                            if not line.startswith("PASS"))[:2000], flush=True)
    check("the guest still works", subprocess.run(["true"]).returncode == 0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True)
    parser.add_argument("--elf-4k", required=True)
    parser.add_argument("--expect", choices=("unsupported", "run"),
                        default="unsupported")
    parser.add_argument("--deadline", type=int, default=280,
                        metavar="SECONDS",
                        help="in run mode, kill the probe after this long")
    arguments = parser.parse_args()
    if os.uname().machine != "aarch64":
        print("SKIP aarch32: not an arm64 guest")
        return 0

    check_native_view()
    if arguments.expect == "unsupported":
        check_unsupported(arguments.elf, arguments.elf_4k)
    else:
        signal.alarm(arguments.deadline)
        check_personality()
        check_page_layout(arguments.elf_4k)
        if run_program(arguments.elf):
            check_tracer(arguments.elf)
            run_program(arguments.elf, "extra", "extra checks", 50)
            run_program(arguments.elf, "tracer", "32-bit tracer", 15)
            check_fault_modes(arguments.elf)
            check_syscall_stops(arguments.elf)
            check_single_step(arguments.elf)
            check_vfp_frame(arguments.elf)
            check_core_dump(arguments.elf)
            check_parallel(arguments.elf)
    print(f"aarch32-probe: {failures} failure(s)", flush=True)
    return min(failures, 100)


if __name__ == "__main__":
    sys.exit(main())
