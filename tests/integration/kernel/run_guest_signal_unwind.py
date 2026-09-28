#!/usr/bin/env python3
"""Check that userspace can unwind through an arm64 signal frame.

A handler installed without SA_RESTORER returns to the kernel's sigreturn
trampoline, and libgcc recognises the signal frame by reading the
instructions there, as on native arm64. The guest builds a small C++ probe
and checks backtrace() in a handler, the frame record above the signal
frame, and cancellation of threads that poll for it and that are blocked in
read(), whose forced unwind crosses the cancellation signal's frame.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import pathlib
import sys


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest, GuestFile  # noqa: E402


PROBE = pathlib.Path(__file__).with_name("signal_unwind_probe.cpp")
GUEST_SOURCE = "/usr/local/libexec/uml-test/signal_unwind_probe.cpp"
GUEST_BINARY = "/tmp/signal-unwind-probe"
BUILD_TIMEOUT_SECONDS = 180
PROBE_TIMEOUT_SECONDS = 150


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
        payloads={GUEST_SOURCE: GuestFile(PROBE.read_bytes())},
    ) as guest:
        guest.boot_to_shell(180)
        guest.check_command("mount -t tmpfs -o mode=1777 tmpfs /tmp", 20)
        guest.check_command(
            "c++ -std=c++20 -O2 -g -fno-omit-frame-pointer -rdynamic -pthread "
            f"-o {GUEST_BINARY} {GUEST_SOURCE}",
            BUILD_TIMEOUT_SECONDS,
        )
        result = guest.run_command(GUEST_BINARY, PROBE_TIMEOUT_SECONDS)
        print(result.output.strip())
        if result.return_code != 0:
            raise RuntimeError(
                f"the signal-unwind probe reported {result.return_code} "
                "failure(s)"
            )
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
