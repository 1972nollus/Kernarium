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


def load_runner_module(runner: str):
    source_path = ROOT / runner
    module_name = f"performance_runner_contract_{source_path.stem}"
    spec = importlib.util.spec_from_file_location(module_name, source_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not load {runner}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def parse_runner_arguments(runner: str, extra: list[str]) -> object:
    module = load_runner_module(runner)
    arguments = [
        str(ROOT / runner),
        "--emulator",
        "emulator",
        "--rootfs",
        "rootfs.erofs",
        *extra,
    ]
    with mock.patch.object(sys, "argv", arguments):
        return module.parse_arguments()


def verify_offline_geekbench_contract() -> None:
    source_path = ROOT / "run_guest_geekbench.py"
    source = source_path.read_text(encoding="utf-8")
    options = parse_runner_arguments(
        "run_guest_geekbench.py",
        ["--console-log", "console.log"],
    )
    expected = (
        "/opt/geekbench/release/geekbench6 "
        "--no-upload"
    )
    if options.command != expected:
        raise RuntimeError(
            f"Geekbench must use the image-local payload {expected}: "
            f"{options.command}"
        )
    if hasattr(options, "allow_network"):
        raise RuntimeError("Geekbench retained the removed --allow-network gate")
    if "--no-upload" not in options.command:
        raise RuntimeError("Geekbench must disable the upload prompt")
    forbidden = (
        "browser.geekbench.com",
        "verify_upload_network",
        "getent ahostsv4",
        "curl -sSI",
    )
    for value in forbidden:
        if value in source:
            raise RuntimeError(
                f"Geekbench retained a network preflight token: {value}"
            )


def verify_geekbench_quiescence_contract() -> None:
    module = load_runner_module("run_guest_geekbench.py")
    expected_units = (
        "docker.service",
        "docker.socket",
        "containerd.service",
        "apt-daily.timer",
        "apt-daily-upgrade.timer",
        "apt-daily.service",
        "apt-daily-upgrade.service",
    )
    if module.QUIESCENCE_UNITS != expected_units:
        raise RuntimeError(
            "Geekbench quiescence must cover Docker, containerd, and both "
            f"apt maintenance pairs: {module.QUIESCENCE_UNITS}"
        )

    successful_guest = mock.Mock()
    successful_guest.run_command.return_value = mock.Mock(
        return_code=0,
        output="",
    )
    module.quiesce_guest(successful_guest, 37)
    successful_guest.run_command.assert_called_once()
    command, timeout = successful_guest.run_command.call_args.args
    if timeout != 37:
        raise RuntimeError(
            f"Geekbench quiescence ignored its timeout: {timeout}"
        )
    if "systemctl stop" not in command:
        raise RuntimeError("Geekbench quiescence does not stop systemd units")
    if "systemctl is-active --quiet" not in command:
        raise RuntimeError("Geekbench quiescence does not verify inactive units")
    if not command.endswith("done && sync"):
        raise RuntimeError("Geekbench quiescence must sync after verification")
    for unit in expected_units:
        if command.count(unit) != 2:
            raise RuntimeError(
                f"Geekbench must stop and verify {unit}: {command}"
            )

    failed_guest = mock.Mock()
    failed_guest.run_command.return_value = mock.Mock(
        return_code=1,
        output="docker.service is still active",
    )
    try:
        module.quiesce_guest(failed_guest, 37)
    except RuntimeError as error:
        if "docker.service is still active" not in str(error):
            raise
    else:
        raise RuntimeError("Geekbench accepted failed guest quiescence")

    timed_out_guest = mock.Mock()
    timed_out_guest.run_command.side_effect = TimeoutError("guest timeout")
    try:
        module.quiesce_guest(timed_out_guest, 37)
    except RuntimeError as error:
        if "timed out after 37 seconds" not in str(error):
            raise
    else:
        raise RuntimeError("Geekbench accepted timed-out guest quiescence")

    options = parse_runner_arguments(
        "run_guest_geekbench.py",
        ["--console-log", "console.log"],
    )
    if options.quiesce_timeout != 120:
        raise RuntimeError(
            "Geekbench quiescence timeout must default to 120 seconds"
        )

    source = (ROOT / "run_guest_geekbench.py").read_text(encoding="utf-8")
    main_source = source[source.index("def main()") :]
    ordered_tokens = (
        "guest.boot_and_login(180)",
        "quiesce_guest(guest, arguments.quiesce_timeout)",
        "profiler = subprocess.Popen",
        "result = guest.run_command(command, arguments.timeout)",
    )
    positions = [main_source.index(token) for token in ordered_tokens]
    if positions != sorted(positions):
        raise RuntimeError(
            "Geekbench must quiesce after login and before profiler/benchmark"
        )


def verify_erofs_root_device_defaults() -> None:
    cases = (
        ("run_guest_geekbench.py", ["--console-log", "console.log"]),
        ("run_guest_command_bench.py", ["--command", "true"]),
        ("run_guest_exec_bench.py", []),
        ("run_guest_link_bench.py", ["--binary", "probe"]),
    )
    for runner, extra in cases:
        options = parse_runner_arguments(runner, extra)
        if options.root_device != "/dev/ubda":
            raise RuntimeError(
                f"{runner}: raw EROFS root must default to /dev/ubda"
            )


def verify_documented_rootfs_wrappers() -> None:
    readme = (ROOT / "README.md").read_text(encoding="utf-8").splitlines()
    entrypoints = [
        index
        for index, line in enumerate(readme)
        if line.lstrip().startswith("python3 tests/perf/run_guest_")
    ]
    if not entrypoints:
        raise RuntimeError("performance README has no guest-runner example")
    expected = (
        "tools/rootfs/with-verified-rootfs.sh --rootfs rootfs.erofs -- \\"
    )
    for index in entrypoints:
        if index == 0 or readme[index - 1].strip() != expected:
            raise RuntimeError(
                "performance guest-runner examples must use the verified-rootfs "
                f"wrapper: {readme[index].strip()}"
            )


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
            pathlib.Path("/rootfs.erofs"),
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
    verify_offline_geekbench_contract()
    verify_geekbench_quiescence_contract()
    verify_erofs_root_device_defaults()
    verify_documented_rootfs_wrappers()
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
