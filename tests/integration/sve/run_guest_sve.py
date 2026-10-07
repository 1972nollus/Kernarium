#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the guest SVE tests on one execution engine.

An engine presents SVE to its guest only at SveSupport complete, and the
guest kernel keeps SVE task state only from the bridge ABI that puts the SVE
registers in the arm64 task tail. Unless both hold, the runner names the
test, says why it is skipped and exits with CTest's skip status. Otherwise
it sets DARWIN_UML_GUEST_SVE=1 and boots sve_probe.cpp as the guest's /init
in "kernel" mode: the guest kernel's non-streaming SVE task state through
signal frames, sigreturn and ptrace (sve_probe.cpp). On a host with SME it
boots the probe once more with DARWIN_UML_GUEST_SME=1 too, where the probe
adds its streaming-mode checks.

"--modes off" instead boots the probe with both policies off, on any host
and engine, and checks that the run shows no SVE at all.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import subprocess
import sys

# The script's own directory and the harness's are on sys.path.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "support"))

from guest import ConsoleProcess, emulator_arguments  # noqa: E402


SKIP = 77
SUPPORT_LEVELS = ("none", "control", "complete")
# Each mode: the probe's mode and whether the SME policy is on.
MODES = {"kernel": ("kernel", False), "sme": ("kernel", True), "off": ("off", False)}
SUMMARY = re.compile(
    rb"sve-probe: (?P<mode>[a-z]+): (?P<failures>\d+) failure\(s\) in "
    rb"(?P<checks>\d+) check\(s\)"
)
HOST_CRASH = re.compile(rb"SIGILL|[Ii]llegal instruction|host fault|Kernel panic")


def host_has_sme() -> bool:
    result = subprocess.run(
        ["sysctl", "-n", "hw.optional.arm.FEAT_SME"],
        capture_output=True, text=True, stdin=subprocess.DEVNULL, timeout=30,
    )
    return result.returncode == 0 and result.stdout.strip() == "1"


def build_support(tool: pathlib.Path) -> dict[str, str]:
    """Each engine's SVE support level, and "kernel": "yes" or "no"."""
    output = subprocess.run(
        [str(tool)], capture_output=True, text=True, check=True,
        stdin=subprocess.DEVNULL, timeout=30,
    ).stdout
    return dict(field.split("=", 1) for field in output.split())


def run_probe_boot(arguments: argparse.Namespace, mode: str) -> int:
    """Boot sve_probe as /init in `mode`; returns its failure count."""
    command = emulator_arguments(
        arguments.emulator, arguments.rootfs, "512M",
        (f"ncpus={arguments.ncpus}", f"SVE_PROBE_MODE={mode}",
         "console=tty0", "console=ttyS0", *arguments.kernel_argument),
        engine=arguments.engine,
    )
    # The launcher's per-run initramfs takes the probe as its /init.
    command.insert(command.index(str(arguments.emulator)) + 1,
                   f"--guest-init={arguments.probe}")
    with ConsoleProcess(command) as guest:
        match = guest._read_until(SUMMARY, arguments.timeout)
        guest.wait_for_exit(30)
        output = guest.output()
        for line in output.splitlines():
            if line.startswith(("PASS ", "FAIL ", "sve-probe: ")):
                print(line)
        if HOST_CRASH.search(output.encode()):
            raise RuntimeError(f"{mode}: the guest or the emulator crashed")
        if match.group("mode").decode() != mode or int(match.group("checks")) == 0:
            raise RuntimeError(f"{mode}: the probe ran no checks")
        return int(match.group("failures"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--name", required=True, help="the registered test name")
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--engine", choices=("jit", "gadgets"), required=True)
    parser.add_argument("--probe", required=True, type=pathlib.Path)
    parser.add_argument("--engine-support-tool", type=pathlib.Path)
    parser.add_argument("--modes", default="kernel,sme",
                        help='"kernel" and "sme", comma-separated, or "off" alone')
    parser.add_argument("--ncpus", type=int, default=2)
    parser.add_argument("--timeout", type=int, default=240)
    parser.add_argument("--kernel-argument", action="append", default=[])
    arguments = parser.parse_args()
    arguments.emulator = arguments.emulator.resolve()
    arguments.rootfs = arguments.rootfs.resolve()
    arguments.probe = arguments.probe.resolve()
    modes = arguments.modes.split(",")
    if unknown := set(modes) - set(MODES):
        parser.error(f"unknown modes: {sorted(unknown)}")
    if "off" in modes and modes != ["off"]:
        parser.error('"off" runs alone')

    if modes != ["off"]:
        if arguments.engine_support_tool is None:
            parser.error("--engine-support-tool is required unless --modes off")
        support = build_support(arguments.engine_support_tool)
        if support["kernel"] != "yes":
            print(f"{arguments.name}: SKIP: the kernel's bridge ABI has no SVE "
                  "task state")
            return SKIP
        level = support[arguments.engine]
        if level != "complete":
            print(f"{arguments.name}: SKIP: the {arguments.engine} engine's SVE "
                  f"support is {level}; it presents SVE only at complete")
            return SKIP

    failures = 0
    for mode in modes:
        probe_mode, with_sme = MODES[mode]
        if with_sme and not host_has_sme():
            print(f"== {arguments.name}: {mode}: not run, the host has no SME")
            continue
        os.environ.pop("DARWIN_UML_GUEST_SVE", None)
        os.environ.pop("DARWIN_UML_GUEST_SME", None)
        if probe_mode != "off":
            os.environ["DARWIN_UML_GUEST_SVE"] = "1"
        if with_sme:
            os.environ["DARWIN_UML_GUEST_SME"] = "1"
        print(f"== {arguments.name}: {mode}", flush=True)
        failures += run_probe_boot(arguments, probe_mode)
    if failures:
        raise RuntimeError(f"{arguments.name}: {failures} failure(s)")
    print(f"{arguments.name}: every mode passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
