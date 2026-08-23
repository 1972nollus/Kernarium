#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin Darwin's fixed-vCPU carrier scheduler and native-window policy."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import re
from pathlib import Path


class PolicyError(AssertionError):
    """The prepared Darwin UML source no longer satisfies the policy."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PolicyError(message)


def mask_comments_and_literals(source: str) -> str:
    result = list(source)
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            end = len(source) if end < 0 else end
            for cursor in range(index, end):
                result[cursor] = " "
            index = end
            continue
        if source.startswith("/*", index):
            end = source.find("*/", index + 2)
            require(end >= 0, "unterminated C block comment")
            end += 2
            for cursor in range(index, end):
                if result[cursor] != "\n":
                    result[cursor] = " "
            index = end
            continue
        if source[index] in ('"', "'"):
            quote = source[index]
            result[index] = " "
            index += 1
            while index < len(source):
                character = source[index]
                if character != "\n":
                    result[index] = " "
                index += 1
                if character == "\\" and index < len(source):
                    if source[index] != "\n":
                        result[index] = " "
                    index += 1
                elif character == quote:
                    break
            continue
        index += 1
    return "".join(result)


def function(source: str, name: str) -> str:
    masked = mask_comments_and_literals(source)
    matches = list(
        re.finditer(rf"(?m)^[^\n;{{}}]*\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{", masked)
    )
    require(len(matches) == 1, f"expected one definition of {name}, found {len(matches)}")
    opening = masked.find("{", matches[0].start(), matches[0].end())
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return source[matches[0].start() : index + 1]
    raise PolicyError(f"unterminated function definition: {name}")


def require_pattern(source: str, pattern: str, message: str) -> None:
    require(
        re.search(pattern, mask_comments_and_literals(source), re.DOTALL) is not None,
        message,
    )


def validate(
    linux_source: Path,
    kernel_config: Path,
    page_16k_config: Path,
    page_4k_config: Path,
    kernel_build_script: Path,
    project_cmake: Path,
    kernarium_runtime: Path,
    kernarium_runtime_loader: Path,
) -> None:
    process = (linux_source / "arch/um/os-Darwin/skas/process.c").read_text()
    smp = (linux_source / "arch/um/os-Darwin/smp.c").read_text()
    processor = (linux_source / "arch/arm64/um/asm/processor.h").read_text()
    signal_source = (linux_source / "arch/um/os-Darwin/signal.c").read_text()
    time = (linux_source / "arch/um/os-Darwin/time.c").read_text()
    main_source = (linux_source / "arch/um/os-Darwin/main.c").read_text()
    host_abi = (linux_source / "arch/um/os-Darwin/uml_darwin_host.h").read_text()
    kernel_start = (linux_source / "arch/um/kernel/skas/process.c").read_text()
    kernel_time = (linux_source / "arch/um/kernel/time.c").read_text()
    config = kernel_config.read_text()
    config_16k = page_16k_config.read_text()
    config_4k = page_4k_config.read_text()
    build_script = kernel_build_script.read_text()
    project = project_cmake.read_text()
    runtime = kernarium_runtime.read_text()
    runtime_loader = kernarium_runtime_loader.read_text()
    makefile = (linux_source / "arch/arm64/Makefile.um").read_text()
    um_arch = (linux_source / "arch/um/kernel/um_arch.c").read_text()
    address_space = (
        linux_source / "arch/um/include/shared/darwin-address-space.h"
    ).read_text()

    for setting in (
        "CONFIG_PRINTK_TIME=y",
        "CONFIG_HZ=100",
        "CONFIG_NO_HZ_IDLE=y",
        "CONFIG_FONTS=y",
        "CONFIG_FONT_8x16=y",
        "CONFIG_FONT_TER16x32=y",
    ):
        require(
            re.search(rf"(?m)^{re.escape(setting)}$", config) is not None,
            f"fixed-carrier kernel policy lacks {setting}",
        )
    for disabled in (
        "CONFIG_HZ_PERIODIC",
        "CONFIG_NO_HZ_FULL",
    ):
        require(
            re.search(rf"(?m)^# {re.escape(disabled)} is not set$", config) is not None,
            f"fixed-carrier kernel policy does not disable {disabled}",
        )
    # UML declares its own integer CONFIG_HZ in arch/um/Kconfig and never
    # sources kernel/Kconfig.hz, so the frequency-choice symbols other
    # architectures use do not exist for this port. Requesting them looked like
    # the tick was pinned in two places while only CONFIG_HZ ever applied; keep
    # them out so the single real setting stays the only one.
    for absent in (
        "CONFIG_HZ_100",
        "CONFIG_HZ_250",
        "CONFIG_HZ_300",
        "CONFIG_HZ_1000",
    ):
        require(
            re.search(rf"(?m)^(# )?{re.escape(absent)}\b", config) is None,
            f"kernel policy names {absent}, which this architecture lacks",
        )
    require(
        re.search(r"(?m)^CONFIG_KERNEL_STACK_ORDER=", config) is None,
        "common policy hard-codes page-count rather than task-stack bytes",
    )
    require(
        re.search(r"(?m)^CONFIG_KERNEL_STACK_ORDER=3$", config_16k) is not None,
        "16 KiB profile does not provide a 128 KiB task stack",
    )
    require(
        re.search(r"(?m)^CONFIG_KERNEL_STACK_ORDER=5$", config_4k) is not None,
        "4 KiB profile does not provide a 128 KiB task stack",
    )
    require(
        "--set-val" in build_script and
        "unsupported numeric kernel config value" in build_script,
        "kernel build helper cannot safely apply page-derived stack orders",
    )
    sched_clock = function(kernel_time, "sched_clock")
    require_pattern(
        sched_clock,
        r"now\s*=\s*os_nsecs\s*\(\s*\).*"
        r"cmpxchg\s*\(\s*&sched_clock_start\s*,\s*0ULL\s*,\s*now\s*\).*"
        r"return\s+now\s*-\s*start",
        "normal UML scheduler and printk timestamps are not host-clock precise",
    )
    require(
        "fbcon=font:VGA8x16" in runtime and
        "fbcon=font:TER16x32" in runtime and
        "fbconFontArgument(dotsPerInch)" in runtime,
        "Kernarium does not select its fbcon font from the advertised DPI",
    )

    require(
        "UML_DARWIN_ADDRESS_SPACE" not in makefile + build_script,
        "Darwin kernel builds still expose divergent macOS/iOS address profiles",
    )
    for setting in (
        "START := 0xc00000000",
        "UML_DARWIN_TASK_SIZE := 0x800000000000UL",
        "UML_DARWIN_GUEST_ADDRESS_LIMIT := 0x800000000000UL",
        "UML_DARWIN_NATIVE_ADDRESS_START := 0xc00000000UL",
        "UML_DARWIN_NATIVE_ADDRESS_END := 0xf80000000UL",
        "UML_DARWIN_NATIVE_AUXILIARY_DIVISOR := 1",
        "UML_DARWIN_NATIVE_AUXILIARY_MINIMUM := 0x40000000UL",
        "UML_DARWIN_NATIVE_AUXILIARY_MAXIMUM := 0x100000000UL",
        "UML_DARWIN_DYNAMIC_NATIVE_LAYOUT := 1",
    ):
        require(
            len(re.findall(rf"(?m)^{re.escape(setting)}$", makefile)) == 1,
            f"shared Darwin kernel address profile lacks exact setting: {setting}",
        )
    for setting in (
        "set(UML_GUEST_ADDRESS_LIMIT 0x0000800000000000ULL)",
        "set(UML_NATIVE_ADDRESS_START 0x0000000c00000000ULL)",
        "set(UML_NATIVE_ADDRESS_END 0x0000000f80000000ULL)",
        "set(UML_NATIVE_LINK_ADDRESS 0x0000000c00000000ULL)",
        "set(UML_NATIVE_ADDRESS_CEILING 0x0000000f80000000ULL)",
        "set(UML_MACOS_NATIVE_ADDRESS 0x0000600000000000ULL)",
        "set(UML_NATIVE_AUXILIARY_DIVISOR 1)",
        "set(UML_NATIVE_AUXILIARY_MINIMUM 0x0000000040000000ULL)",
        "set(UML_NATIVE_AUXILIARY_MAXIMUM 0x0000000100000000ULL)",
        "set(UML_DYNAMIC_NATIVE_LAYOUT 1)",
    ):
        require(
            len(re.findall(rf"(?m)^\s*{re.escape(setting)}$", project)) == 1,
            f"Apple host products lack exact shared address setting: {setting}",
        )
    require(
        "set(UML_GUEST_ADDRESS_LIMIT 0x0000600000000000ULL)" not in project and
        "set(UML_DYNAMIC_NATIVE_LAYOUT 0)" not in project and
        "UML_DARWIN_HOST_TASK_SIZE" not in makefile,
        "the shared kernel still has a platform-specific static address profile",
    )
    require(
        "DARWIN_UML_NATIVE_HIGH_PLACEMENT=$<BOOL:${UML_TARGET_MACOS}>" in project,
        "macOS does not select its high runtime native placement",
    )
    require(
        "uml_darwin_native_end = UML_DARWIN_NATIVE_ADDRESS_END" in um_arch and
        "return uml_darwin_native_end" in address_space and
        "uml_darwin_native_end = native_start + memory + auxiliary" in um_arch,
        "the shared kernel does not derive its native-hole end from relocation",
    )

    masked_process = mask_comments_and_literals(process)
    masked_smp = mask_comments_and_literals(smp)
    require("darwin_task_thread" not in masked_process, "per-task pthread registry returned")
    require("pthread_create" not in masked_process, "guest-task creation must not create pthreads")
    require("pthread_exit" not in masked_process, "task context must not terminate its carrier")
    require(
        not any(
            selector in process + smp
            for selector in (
                "TARGET_OS_SIMULATOR",
                "TARGET_OS_IPHONE",
                "UML_DARWIN_ADDRESS_SPACE",
            )
        ),
        "an Apple platform or address profile still selects another scheduler",
    )

    create_task = function(process, "new_thread")
    require_pattern(create_task, r"JB_IP\s*=\s*\(unsigned long\)\s*handler", "new task lacks saved PC")
    require_pattern(
        create_task,
        r"JB_SP\s*=\s*\(unsigned long\)\s*stack\s*\+\s*UM_THREAD_SIZE\s*;",
        "new task stack pointer is not the aligned AAPCS64 stack top",
    )
    require("pthread_" not in mask_comments_and_literals(create_task), "new_thread uses host pthread state")

    switch = function(process, "switch_threads_dead")
    require_pattern(
        switch,
        r"if\s*\(\s*from_dead\s*\)\s*UML_LONGJMP\s*\(\s*to_key\s*,\s*1\s*\)\s*;"
        r".*UML_SETJMP\s*\(\s*from_key\s*\).*UML_LONGJMP\s*\(\s*to_key\s*,\s*1\s*\)",
        "task switch must save only live tasks and resume on the current carrier",
    )
    require("pthread_" not in mask_comments_and_literals(switch), "task switch uses host pthread state")

    start_idle = function(process, "start_idle_thread")
    require_pattern(
        start_idle,
        r"darwin_start_primary_cpu_carrier\s*\(\s*stack\s*,\s*switch_buffer\s*\)",
        "vCPU0 is not launched as a fixed carrier",
    )

    userspace = function(process, "userspace")
    require_pattern(
        userspace,
        r"darwin_uml_enter_guest\s*\(.*darwin_ipi_guest_end\s*\(\s*\)\s*;"
        r"\s*if\s*\(\s*event\s*==\s*DARWIN_UML_EVENT_STOP\s*\)\s*"
        r"darwin_host_stop\s*\(\s*\)",
        "an active translated guest cannot turn a public stop into carrier shutdown",
    )
    idle = function(time, "os_idle_sleep")
    require_pattern(
        idle,
        r"stop_fd\s*=\s*darwin_uml_stop_fd\s*\(\s*\).*pselect\s*\(.*"
        r"darwin_host_stop_checkpoint\s*\(\s*\)",
        "a tickless idle carrier cannot be woken by a public stop",
    )
    require("darwin_uml_stop_fd" in host_abi and
            "darwin_uml_stop_requested" in host_abi,
            "host/kernel ABI lacks the stop notification channel")
    fatal_exit = function(main_source, "fatal_exit")
    require("uml_cleanup" not in mask_comments_and_literals(fatal_exit),
            "last-ditch signal cleanup can schedule from a vCPU idle task")
    require_pattern(fatal_exit, r"_exit\s*\(\s*128\s*\+\s*signal\s*\)",
                    "last-ditch signal handling is not immediate")

    require(len(re.findall(r"\bpthread_create\s*\(", masked_smp)) == 1, "Darwin must have one carrier creation site")
    start_carrier = function(smp, "start_cpu_carrier")
    require_pattern(
        start_carrier,
        r"pthread_create\s*\([^;]*\bcpu_carrier\b",
        "carrier creation does not target the common vCPU entry",
    )

    carrier = function(smp, "cpu_carrier")
    require_pattern(
        carrier,
        r"pthread_setspecific\s*\(\s*current_cpu_key\s*,\s*data\s*\).*"
        r"darwin_set_current_cpu\s*\(\s*data->cpu\s*\).*setjmp",
        "carrier identity and name are not established on its native stack",
    )
    require_pattern(carrier, r"setjmp\s*\(\s*data->native_context\s*\)", "carrier lacks native return context")
    require_pattern(carrier, r"UML_LONGJMP\s*\(\s*switch_buffer\s*,\s*1\s*\)", "carrier never enters a Linux task")

    exit_carrier = function(smp, "darwin_exit_cpu_carrier")
    require("pthread_exit" not in mask_comments_and_literals(exit_carrier), "carrier exits from a guest task stack")
    require_pattern(exit_carrier, r"UML_LONGJMP\s*\(\s*&carrier->native_context", "carrier does not return to its native stack")

    secondary = function(smp, "os_start_secondary")
    restart = function(smp, "os_restart_secondary")
    require("pthread_" not in mask_comments_and_literals(secondary), "secondary bootstrap replaces its carrier")
    require_pattern(restart, r"new_thread\s*\(.*new_thread_handler.*UML_LONGJMP", "hotplug does not reuse its carrier")

    name_carrier = function(smp, "name_cpu_carrier")
    require(
        '"uml-vcpu-"' in name_carrier,
        "carrier thread names lack their meaningful prefix",
    )
    require_pattern(
        name_carrier,
        r"char\s+name\s*\[\s*32\s*\].*value\s*%\s*10.*"
        r"pthread_setname_np\s*\(\s*name\s*\)",
        "carrier thread names are not meaningful",
    )
    require(
        not any(
            variadic in mask_comments_and_literals(name_carrier)
            for variadic in ("snprintf", "sprintf", "printf")
        ),
        "carrier naming crosses the incompatible Linux/Apple variadic ABI",
    )
    relax = function(smp, "darwin_cpu_relax")
    require_pattern(
        relax,
        r"pending_ipis\s*\[\s*cpu\s*\].*um_get_signals\s*\(\s*\).*"
        r"darwin_ipi_poll\s*\(\s*\)",
        "an IRQ-enabled cross-CPU spin wait cannot service pending fixed-carrier work",
    )
    require_pattern(
        function(processor, "cpu_relax"),
        r"#ifdef\s+UML_HOST_DARWIN.*darwin_cpu_relax\s*\(\s*\)",
        "arm64 Darwin spin waits bypass the fixed-carrier IPI checkpoint",
    )
    require_pattern(
        kernel_start,
        r"#ifndef\s+UML_HOST_DARWIN\s+set_sigstack\s*\(",
        "the Darwin launcher still owns CPU0's signal stack",
    )
    require_pattern(carrier, r"set_sigstack\s*\(\s*NULL\s*,\s*0\s*\)", "CPU0 carrier lacks a signal stack")
    require_pattern(
        carrier,
        r"sigaddset\s*\(\s*&timer\s*,\s*SIGALRM\s*\).*"
        r"pthread_sigmask\s*\(\s*SIG_UNBLOCK\s*,\s*&timer",
        "vCPU0 does not exclusively accept the early calibration timer",
    )
    set_handler = function(signal_source, "set_handler")
    require_pattern(
        set_handler,
        r"if\s*\(\s*signal\s*==\s*SIGALRM\s*\)\s*return\s*;.*"
        r"pthread_sigmask\s*\(\s*SIG_UNBLOCK",
        "handler installation can reassign the process timer to the launcher",
    )
    runtime_mask = function(
        runtime_loader, "isolateGuestTimerFromApplicationThreads"
    )
    require(
        "__attribute__((constructor))" in runtime_loader
        and "__attribute__((constructor))" not in runtime,
        "Kernarium does not isolate the process timer before application main",
    )
    require_pattern(
        runtime_mask,
        r"blockGuestTimer\s*\(\s*\)",
        "Kernarium application threads do not inherit SIGALRM blocked",
    )
    runtime_run_start = runtime.index("int runtimeRun(")
    runtime_run_end = runtime.index("void runtimeRequestStop", runtime_run_start)
    runtime_run = runtime[runtime_run_start:runtime_run_end]
    require_pattern(
        runtime_run,
        r"ScopedGuestTimerBlock\s+guestTimerBlock\s*;.*"
        r"uml_emulator_run\s*\(",
        "Kernarium launcher can accept ITIMER_REAL during a managed run",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--linux-source", type=Path, required=True)
    parser.add_argument("--kernel-config", type=Path, required=True)
    parser.add_argument("--page-16k-config", type=Path, required=True)
    parser.add_argument("--page-4k-config", type=Path, required=True)
    parser.add_argument("--kernel-build-script", type=Path, required=True)
    parser.add_argument("--project-cmake", type=Path, required=True)
    parser.add_argument("--kernarium-runtime", type=Path, required=True)
    parser.add_argument("--kernarium-runtime-loader", type=Path, required=True)
    arguments = parser.parse_args()
    validate(
        arguments.linux_source.resolve(),
        arguments.kernel_config.resolve(),
        arguments.page_16k_config.resolve(),
        arguments.page_4k_config.resolve(),
        arguments.kernel_build_script.resolve(),
        arguments.project_cmake.resolve(),
        arguments.kernarium_runtime.resolve(),
        arguments.kernarium_runtime_loader.resolve(),
    )
    print("Darwin fixed-vCPU carrier policy verified")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
