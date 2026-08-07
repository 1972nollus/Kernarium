#!/usr/bin/env python3
"""Exercise every benchmark entry point without booting a guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import ast
import importlib.util
import pathlib
import subprocess
import sys
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2] / "perf"
RUNNERS = (
    "run_guest_geekbench.py",
    "run_guest_command_bench.py",
    "run_guest_exec_bench.py",
    "run_guest_link_bench.py",
)


def verify_engine_forwarding(runner: str) -> None:
    source_path = ROOT / runner
    source = source_path.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(source_path))
    guest_calls = [
        node
        for node in ast.walk(tree)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Name)
        and node.func.id == "Guest"
    ]
    if len(guest_calls) != 1:
        raise RuntimeError(
            f"{runner}: expected exactly one Guest construction, "
            f"found {len(guest_calls)}"
        )
    engine_keywords = [
        keyword
        for keyword in guest_calls[0].keywords
        if keyword.arg == "engine"
    ]
    expected = "arguments.engine"
    if len(engine_keywords) != 1 or ast.unparse(engine_keywords[0].value) != expected:
        raise RuntimeError(
            f"{runner}: Guest must receive engine={expected}"
        )
    core_keywords = [
        keyword
        for keyword in guest_calls[0].keywords
        if keyword.arg == "core_class"
    ]
    expected_core = "arguments.core_class"
    if (
        len(core_keywords) != 1
        or ast.unparse(core_keywords[0].value) != expected_core
    ):
        raise RuntimeError(
            f"{runner}: Guest must receive core_class={expected_core} so an "
            "efficiency-core measurement is selectable"
        )
    if "--execution-engine" in source:
        raise RuntimeError(
            f"{runner}: engine selection must not be passed as a kernel argument"
        )
    if runner == "run_guest_geekbench.py":
        shell_keywords = {
            keyword.arg for keyword in guest_calls[0].keywords
        } & {"init_shell", "overlay_shell"}
        if shell_keywords:
            raise RuntimeError(
                f"{runner}: Geekbench must use the normal systemd overlay path"
            )
        systemd_keywords = [
            keyword
            for keyword in guest_calls[0].keywords
            if keyword.arg == "systemd_overlay"
        ]
        if (
            len(systemd_keywords) != 1
            or not isinstance(systemd_keywords[0].value, ast.Constant)
            or systemd_keywords[0].value.value is not True
        ):
            raise RuntimeError(
                f"{runner}: Geekbench must enable systemd_overlay=True"
            )


def verify_geekbench_network_gate() -> None:
    source_path = ROOT / "run_guest_geekbench.py"
    spec = importlib.util.spec_from_file_location(
        "geekbench_runner_contract", source_path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load Geekbench runner")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    base_arguments = [
        str(source_path),
        "--emulator",
        "emulator",
        "--rootfs",
        "rootfs.img",
        "--console-log",
        "console.log",
    ]
    with (
        mock.patch.object(sys, "argv", base_arguments),
        mock.patch.object(module, "Guest") as guest,
    ):
        try:
            module.main()
        except ValueError as error:
            if "--allow-network" not in str(error):
                raise
        else:
            raise RuntimeError("Geekbench runner accepted an implicit upload")
        guest.assert_not_called()
    with (
        mock.patch.object(sys, "argv", [*base_arguments, "--allow-network"]),
        mock.patch.object(
            module,
            "verify_upload_network",
            side_effect=RuntimeError("network denied"),
        ),
        mock.patch.object(module, "Guest") as guest,
    ):
        try:
            module.main()
        except RuntimeError as error:
            if str(error) != "network denied":
                raise
        else:
            raise RuntimeError("Geekbench runner booted after preflight failure")
        guest.assert_not_called()


def verify_core_class_launcher() -> None:
    """The core-class choice must only change the launcher prefix."""
    support = pathlib.Path(__file__).resolve().parents[2] / "integration" / "support"
    spec = importlib.util.spec_from_file_location(
        "guest_core_class_contract", support / "guest.py"
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load the guest support module")
    guest = importlib.util.module_from_spec(spec)
    # dataclasses resolves a class's defining module through sys.modules, so
    # register the module before executing it.
    sys.modules[spec.name] = guest
    try:
        spec.loader.exec_module(guest)
    except BaseException:
        del sys.modules[spec.name]
        raise

    def build(core_class: str) -> list[str]:
        return guest.emulator_arguments(
            pathlib.Path("/emulator"),
            pathlib.Path("/rootfs.img"),
            "8G",
            core_class=core_class,
        )

    performance = build("performance")
    if performance[0] != "/emulator":
        raise RuntimeError(
            "performance-core runs must launch the emulator directly: "
            f"{performance}"
        )

    if sys.platform == "darwin":
        efficiency = build("efficiency")
        prefix = [guest.TASKPOLICY, "-c", "background"]
        if efficiency[: len(prefix)] != prefix:
            raise RuntimeError(
                f"efficiency-core runs must apply {prefix}: {efficiency}"
            )
        if efficiency[len(prefix) :] != performance:
            raise RuntimeError(
                "the core class must change only the launcher prefix: "
                f"{efficiency} versus {performance}"
            )

    try:
        build("little")
    except ValueError:
        pass
    else:
        raise RuntimeError("an unknown core class was accepted")


def main() -> int:
    verify_geekbench_network_gate()
    verify_core_class_launcher()
    for runner in RUNNERS:
        verify_engine_forwarding(runner)
        result = subprocess.run(
            (sys.executable, str(ROOT / runner), "--help"),
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0 or "usage:" not in result.stdout:
            print(
                f"{runner}: CLI/import smoke failed\n"
                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}",
                file=sys.stderr,
            )
            return 1
    print(f"benchmark CLI/import smoke passed ({len(RUNNERS)} runners)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
