#!/usr/bin/env python3
"""Check that a tracer's PSTATE writes are validated like native arm64's.

The engine single-steps a task whose PSTATE.SS is set, so PTRACE_SETREGSET
and PTRACE_POKEUSER must filter a tracer's PSTATE as native valid_user_regs()
does. The guest probe writes SS, RES0, TCO, DIT and SSBS bits, and mode and
DAIF bits, reads the result back, and continues the child to check that it
exits instead of stopping after each instruction.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("ptrace_pstate_probe.py")
PROBE_TIMEOUT_SECONDS = 60


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
        guest.check_command(
            "mount -t proc proc /proc && mount -t tmpfs -o mode=1777 tmpfs /tmp",
            20,
        )
        guest.upload(PROBE.read_bytes(), "/tmp/probe.py")
        result = guest.run_command("python3 -B /tmp/probe.py", PROBE_TIMEOUT_SECONDS)
        print(result.output.strip())
        if result.return_code != 0:
            raise RuntimeError(
                f"the PSTATE probe reported {result.return_code} failure(s)"
            )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
