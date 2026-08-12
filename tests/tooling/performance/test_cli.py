#!/usr/bin/env python3
"""Exercise every benchmark entry point without booting a guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import ast
import contextlib
import importlib.util
import io
import pathlib
import subprocess
import sys
import types
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2] / "perf"
TOOLS = pathlib.Path(__file__).resolve().parents[3] / "tools"
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


def load_guest_shell_module():
    source_path = TOOLS / "guest-shell.py"
    spec = importlib.util.spec_from_file_location(
        "performance_guest_shell_contract", source_path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load guest-shell.py")
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


def run_geekbench_shutdown_case(
    command_status: int,
    shutdown_ok: bool,
    emulator_status: int | None,
) -> tuple[int | None, RuntimeError | None, mock.Mock]:
    module = load_runner_module("run_guest_geekbench.py")
    arguments = types.SimpleNamespace(
        emulator=pathlib.Path("emulator"),
        rootfs=pathlib.Path("rootfs.erofs"),
        profile_dir=None,
        console_log=pathlib.Path("console.log"),
        command="geekbench6 --no-upload",
        root_device="/dev/ubda",
        kernel_argument=[],
        memory="8G",
        cpus=10,
        engine="jit",
        core_class="performance",
        timeout=30,
        quiesce_timeout=30,
        sample_seconds=1,
        sample_command="/usr/bin/sample",
    )
    guest = mock.Mock()
    guest.run_command.side_effect = (
        types.SimpleNamespace(return_code=0, output=""),
        types.SimpleNamespace(return_code=command_status, output="benchmark"),
    )
    guest.shutdown.return_value = shutdown_ok
    guest.return_code = emulator_status

    class MirrorThread:
        def __init__(self, *, args, **_kwargs):
            self._ready = args[2]

        def start(self) -> None:
            self._ready.set()

        def join(self, timeout=None) -> None:
            del timeout

    status: int | None = None
    error: RuntimeError | None = None
    with (
        mock.patch.object(module, "parse_arguments", return_value=arguments),
        mock.patch.object(module, "Guest", return_value=guest),
        mock.patch.object(module.threading, "Thread", MirrorThread),
        contextlib.redirect_stdout(io.StringIO()),
    ):
        try:
            status = module.main()
        except RuntimeError as caught:
            error = caught
    guest.close.assert_called_once_with()
    return status, error, guest


def verify_geekbench_shutdown_contract() -> None:
    status, error, guest = run_geekbench_shutdown_case(0, True, 0)
    if status != 0 or error is not None:
        raise RuntimeError(
            "Geekbench runner rejected a clean emulator exit: "
            f"status={status}, error={error}"
        )
    guest.shutdown.assert_called_once_with(60)

    status, error, _ = run_geekbench_shutdown_case(0, True, 23)
    if (
        status is not None
        or error is None
        or "emulator exited with status 23" not in str(error)
    ):
        raise RuntimeError(
            "Geekbench runner allowed a nonzero emulator exit to pass: "
            f"status={status}, error={error}"
        )

    status, error, _ = run_geekbench_shutdown_case(7, True, 23)
    if (
        status is not None
        or error is None
        or "Geekbench exited with status 7" not in str(error)
    ):
        raise RuntimeError(
            "Geekbench runner did not preserve benchmark failure precedence: "
            f"status={status}, error={error}"
        )


def run_guest_shell_case(
    command_status: int,
    shutdown_ok: bool,
    emulator_status: int | None,
) -> tuple[int, str, mock.Mock]:
    module = load_guest_shell_module()
    arguments = types.SimpleNamespace(
        emulator=pathlib.Path("emulator"),
        rootfs=pathlib.Path("rootfs.erofs"),
        memory="4G",
        cpus=4,
        root_device="/dev/ubda",
        engine="jit",
        core_class="performance",
        quiet=True,
        boot_timeout=30,
        timeout=30,
        command=["benchmark"],
    )
    guest = mock.Mock()
    guest.run_command.return_value = types.SimpleNamespace(
        return_code=command_status,
        output="benchmark output",
    )
    guest.shutdown.return_value = shutdown_ok
    guest.return_code = emulator_status
    stderr = io.StringIO()
    stdout = io.StringIO()
    with (
        mock.patch.object(module, "parse_arguments", return_value=arguments),
        mock.patch.object(module, "Guest", return_value=guest),
        contextlib.redirect_stdout(stdout),
        contextlib.redirect_stderr(stderr),
    ):
        status = module.main()
    guest.close.assert_called_once_with()
    return status, stderr.getvalue(), guest


def verify_guest_shell_shutdown_contract() -> None:
    status, stderr, guest = run_guest_shell_case(0, True, 0)
    if status != 0 or stderr:
        raise RuntimeError(
            "guest-shell rejected a successful command and clean emulator exit: "
            f"status={status}, stderr={stderr!r}"
        )
    guest.shutdown.assert_called_once_with(60)

    status, stderr, _ = run_guest_shell_case(0, False, None)
    if status == 0 or "did not halt cleanly within 60 seconds" not in stderr:
        raise RuntimeError(
            "guest-shell allowed a successful command to mask shutdown timeout: "
            f"status={status}, stderr={stderr!r}"
        )

    status, stderr, _ = run_guest_shell_case(0, True, 23)
    if status == 0 or "emulator exited with status 23" not in stderr:
        raise RuntimeError(
            "guest-shell allowed a successful command to mask emulator failure: "
            f"status={status}, stderr={stderr!r}"
        )

    status, stderr, _ = run_guest_shell_case(7, False, None)
    if status != 7 or "did not halt cleanly within 60 seconds" not in stderr:
        raise RuntimeError(
            "guest-shell did not preserve command failure precedence across "
            f"shutdown timeout: status={status}, stderr={stderr!r}"
        )


def verify_performance_runner_shutdown_contract() -> None:
    for runner in RUNNERS:
        source = (ROOT / runner).read_text(encoding="utf-8")
        main_source = source[source.index("def main()") :]
        shutdown = main_source.find("guest.shutdown(60)")
        status = main_source.find("guest.return_code")
        close = main_source.find("guest.close()")
        if min(shutdown, status, close) < 0 or not shutdown < status < close:
            raise RuntimeError(
                f"{runner}: successful timing must require orderly shutdown "
                "and status-0 emulator exit before close()"
            )
        if 'if emulator_status != 0:' not in main_source:
            raise RuntimeError(
                f"{runner}: nonzero emulator exit is not rejected"
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
    verify_geekbench_shutdown_contract()
    verify_guest_shell_shutdown_contract()
    verify_performance_runner_shutdown_contract()
    verify_erofs_root_device_defaults()
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
