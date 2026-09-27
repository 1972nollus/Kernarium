#!/usr/bin/env python3
"""Check that a faulting nofault kernel read recovers instead of panicking.

copy_from_kernel_nofault() and the other nofault accessors resume a faulting
access at a recovery point that only the fault handler reaches. sysrq t dumps
every task, and its workqueue report for each idle kernel worker reads through
that worker's NULL pool-workqueue pointer with copy_from_kernel_nofault(). The
read must fail quietly: the guest keeps running and the host reports no fault
for it, since a recovered nofault read is expected kernel behaviour. A lost
recovery point panics the guest instead. The kernel has no BPF tracing, so
bpf_probe_read_kernel() cannot reach this path.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import re
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    arguments = parser.parse_args()

    with Guest(
        arguments.emulator.resolve(),
        arguments.rootfs.resolve(),
        "1G",
        "tmpfs",
        ("ncpus=2",),
        init_shell=True,
        engine=arguments.engine,
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("mount -t proc proc /proc", 20)
        # The dump goes to the kernel log only; the console would take minutes.
        dumped = guest.run_command(
            "echo 1 > /proc/sys/kernel/printk && echo t > /proc/sysrq-trigger && "
            "echo dumps=$(dmesg | grep -c 'sysrq: Show State') "
            "workers=$(dmesg | grep -c ' task:kworker/')",
            120,
        )
        found = re.search(r"dumps=(\d+) workers=(\d+)", dumped.output)
        dumps, workers = (int(found[1]), int(found[2])) if found else (0, 0)
        # The emulator's stderr shares the console, so a host fault report
        # made during the dump lands inside the command's output.
        reports = len(re.findall(r"^Darwin POSIX ", dumped.output, re.MULTILINE))
        print(f"sysrq t ran {dumps} time(s), listed {workers} kernel workers "
              f"and printed {reports} host fault report line(s)")
        if dumped.return_code != 0 or dumps != 1 or workers < 1:
            raise RuntimeError("sysrq t did not dump the kernel workers:\n"
                               + dumped.output)
        if reports:
            raise RuntimeError("the host reported recovered nofault reads:\n"
                               + dumped.output)
        alive = guest.run_command("echo alive", 30)
        if alive.return_code != 0 or "alive" not in alive.output:
            raise RuntimeError("the guest stopped answering after sysrq t")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
