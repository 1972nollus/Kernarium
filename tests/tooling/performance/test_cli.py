#!/usr/bin/env python3
"""Exercise every benchmark entry point without booting a guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import contextlib
import importlib.util
import io
import pathlib
import subprocess
import sys
import tempfile
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
MOBILE_RUNNERS = (
    "run_ios_device_discard_bench.py",
    "run_ios_device_workload.py",
)


class GuestConstructed(Exception):
    """Stops a runner at its Guest construction."""


# The arguments each runner needs, beyond the common ones, to reach its Guest.
RUNNER_ARGUMENTS = {
    "run_guest_geekbench.py": ("--console-log", "{directory}/console.log"),
    "run_guest_command_bench.py": ("--command", "true"),
    "run_guest_exec_bench.py": (
        "--cpus", "1", "--workers", "1", "--executions", "1",
    ),
    "run_guest_link_bench.py": (
        "--cpus", "1", "--workers", "1", "--binary", "{directory}/link-loop",
    ),
}


def verify_engine_forwarding(runner: str) -> None:
    """Run a runner up to its Guest and check the engine and core class."""
    module = load_runner_module(runner)
    for engine in ("jit", "gadgets"):
        with tempfile.TemporaryDirectory(prefix="uml-perf-cli-") as directory:
            pathlib.Path(directory, "link-loop").write_bytes(b"")
            arguments = [
                str(ROOT / runner),
                "--emulator",
                "emulator",
                "--rootfs",
                "rootfs.erofs",
                "--engine",
                engine,
                "--core-class",
                "efficiency",
                *(
                    argument.format(directory=directory)
                    for argument in RUNNER_ARGUMENTS[runner]
                ),
            ]
            with (
                mock.patch.object(sys, "argv", arguments),
                mock.patch.object(
                    module, "Guest", side_effect=GuestConstructed
                ) as constructor,
            ):
                try:
                    module.main()
                except GuestConstructed:
                    pass
                else:
                    raise RuntimeError(f"{runner}: main() built no Guest")
        call = constructor.call_args
        if call.kwargs.get("engine") != engine:
            raise RuntimeError(f"{runner}: Guest must receive engine={engine}")
        if call.kwargs.get("core_class") != "efficiency":
            raise RuntimeError(
                f"{runner}: Guest must receive the selected core class so an "
                "efficiency-core measurement is selectable"
            )
        if any("--execution-engine" in argument for argument in call.args[4]):
            raise RuntimeError(
                f"{runner}: engine selection must not be passed as a kernel "
                "argument"
            )
        if (
            runner == "run_guest_geekbench.py"
            and call.kwargs.get("overlay_shell") is not True
        ):
            raise RuntimeError(
                f"{runner}: Geekbench must enable overlay_shell=True"
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
    module = load_runner_module("run_guest_geekbench.py")
    script = module.WORKLOAD.read_text(encoding="utf-8")
    runs = [
        line
        for line in script.splitlines()
        if line.strip().startswith('"$benchmark"')
    ]
    if not runs or any("--no-upload" not in line for line in runs):
        raise RuntimeError("Geekbench must disable the upload prompt")
    options = parse_runner_arguments(
        "run_guest_geekbench.py",
        ["--console-log", "console.log"],
    )
    if options.workloads is not None:
        raise RuntimeError("Geekbench must run the full benchmark by default")


def session_guest() -> mock.MagicMock:
    """A Guest stand-in that, like Guest, closes when its session ends."""
    guest = mock.MagicMock()
    guest.__enter__.return_value = guest

    def exit_session(*_exception: object) -> bool:
        guest.close()
        return False

    guest.__exit__.side_effect = exit_session
    return guest


def run_geekbench_shutdown_case(
    command_status: int,
    shutdown_ok: bool,
    emulator_status: int | None,
    *,
    workloads: str | None = None,
) -> tuple[int | None, RuntimeError | None, mock.Mock, mock.Mock]:
    module = load_runner_module("run_guest_geekbench.py")
    directory = tempfile.TemporaryDirectory(prefix="uml-geekbench-cli-")
    console_log = pathlib.Path(directory.name, "console.log")
    arguments = types.SimpleNamespace(
        emulator=pathlib.Path("emulator"),
        rootfs=pathlib.Path("rootfs.erofs"),
        profile_dir=None,
        console_log=console_log,
        workloads=workloads,
        kernel_argument=[],
        memory="8G",
        cpus=10,
        engine="jit",
        core_class="performance",
        timeout=30,
        sample_seconds=1,
        sample_command="/usr/bin/sample",
    )
    guest = session_guest()
    guest.run_command.return_value = types.SimpleNamespace(
        return_code=command_status, output="benchmark"
    )
    guest.shutdown.return_value = shutdown_ok
    guest.return_code = emulator_status

    status: int | None = None
    error: RuntimeError | None = None
    with (
        directory,
        mock.patch.object(module, "parse_arguments", return_value=arguments),
        mock.patch.object(module, "Guest", return_value=guest) as constructor,
        contextlib.redirect_stdout(io.StringIO()),
    ):
        try:
            status = module.main()
        except RuntimeError as caught:
            error = caught
    guest.close.assert_called_once_with()
    # The console streams into the log the profiler follows.
    sink = constructor.call_args.kwargs["console_sink"]
    if sink.name != str(console_log) or not sink.closed:
        raise RuntimeError(
            f"Geekbench runner did not stream the console into {console_log}: "
            f"{sink!r}"
        )
    return status, error, guest, constructor


def verify_geekbench_shutdown_contract() -> None:
    status, error, guest, _ = run_geekbench_shutdown_case(0, True, 0)
    if status != 0 or error is not None:
        raise RuntimeError(
            "Geekbench runner rejected a clean emulator exit: "
            f"status={status}, error={error}"
        )
    guest.shutdown.assert_called_once_with(60)

    status, error, _, _ = run_geekbench_shutdown_case(0, True, 23)
    if (
        status is not None
        or error is None
        or "emulator exited with status 23" not in str(error)
    ):
        raise RuntimeError(
            "Geekbench runner allowed a nonzero emulator exit to pass: "
            f"status={status}, error={error}"
        )

    status, error, _, _ = run_geekbench_shutdown_case(7, True, 23)
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
    *,
    audio: bool = False,
) -> tuple[int, str, mock.Mock, mock.Mock]:
    module = load_guest_shell_module()
    arguments = types.SimpleNamespace(
        emulator=pathlib.Path("emulator"),
        rootfs=pathlib.Path("rootfs.erofs"),
        memory="4G",
        cpus=4,
        audio=audio,
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
        mock.patch.object(module, "Guest", return_value=guest) as constructor,
        contextlib.redirect_stdout(stdout),
        contextlib.redirect_stderr(stderr),
    ):
        status = module.main()
    guest.close.assert_called_once_with()
    return status, stderr.getvalue(), guest, constructor


def verify_guest_shell_shutdown_contract() -> None:
    status, stderr, guest, _ = run_guest_shell_case(0, True, 0)
    if status != 0 or stderr:
        raise RuntimeError(
            "guest-shell rejected a successful command and clean emulator exit: "
            f"status={status}, stderr={stderr!r}"
        )
    guest.shutdown.assert_called_once_with(60)

    status, stderr, _, _ = run_guest_shell_case(0, False, None)
    if status == 0 or "did not halt cleanly within 60 seconds" not in stderr:
        raise RuntimeError(
            "guest-shell allowed a successful command to mask shutdown timeout: "
            f"status={status}, stderr={stderr!r}"
        )

    status, stderr, _, _ = run_guest_shell_case(0, True, 23)
    if status == 0 or "emulator exited with status 23" not in stderr:
        raise RuntimeError(
            "guest-shell allowed a successful command to mask emulator failure: "
            f"status={status}, stderr={stderr!r}"
        )

    status, stderr, _, _ = run_guest_shell_case(7, False, None)
    if status != 7 or "did not halt cleanly within 60 seconds" not in stderr:
        raise RuntimeError(
            "guest-shell did not preserve command failure precedence across "
            f"shutdown timeout: status={status}, stderr={stderr!r}"
        )

    status, stderr, _, constructor = run_guest_shell_case(
        0, True, 0, audio=True)
    kernel_arguments = constructor.call_args.args[4]
    if status != 0 or stderr or kernel_arguments.count("--audio") != 1:
        raise RuntimeError(
            "guest-shell did not forward --audio exactly once as a host "
            f"option: status={status}, stderr={stderr!r}, "
            f"arguments={kernel_arguments!r}"
        )


def verify_geekbench_payloads() -> None:
    """The workload script, and its parameters when targeted, are payloads."""
    module = load_runner_module("run_guest_geekbench.py")
    script = module.WORKLOAD.read_bytes()
    for workloads, expected in (
        (None, {module.GUEST_WORKLOAD: script}),
        (
            "4,17",
            {
                module.GUEST_WORKLOAD: script,
                module.GUEST_PARAMETERS: b"UML_GEEKBENCH_WORKLOADS=4,17\n",
            },
        ),
    ):
        status, error, _, constructor = run_geekbench_shutdown_case(
            0, True, 0, workloads=workloads
        )
        payloads = constructor.call_args.kwargs["payloads"]
        contents = {path: payload.contents for path, payload in payloads.items()}
        if status != 0 or error is not None or contents != expected:
            raise RuntimeError(
                f"Geekbench payloads for workloads={workloads!r} were "
                f"{sorted(contents)}: status={status}, error={error}"
            )
        if payloads[module.GUEST_WORKLOAD].mode != 0o755:
            raise RuntimeError("the Geekbench script must be executable")
        command = constructor.return_value.run_command.call_args.args[0]
        if command != module.GUEST_WORKLOAD:
            raise RuntimeError(f"Geekbench ran {command!r}, not its script")
    for workloads in ("", "4,", "4;reboot", ",4"):
        try:
            run_geekbench_shutdown_case(0, True, 0, workloads=workloads)
        except ValueError:
            continue
        raise RuntimeError(f"Geekbench accepted workloads={workloads!r}")


def verify_no_root_device_option() -> None:
    """The launcher chooses the root device; no runner restates it."""
    cases = (
        ("run_guest_geekbench.py", ["--console-log", "console.log"]),
        ("run_guest_command_bench.py", ["--command", "true"]),
        ("run_guest_exec_bench.py", []),
        ("run_guest_link_bench.py", ["--binary", "probe"]),
    )
    for runner, extra in cases:
        options = parse_runner_arguments(runner, extra)
        if hasattr(options, "root_device"):
            raise RuntimeError(f"{runner}: --root-device must not return")


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
    verify_geekbench_shutdown_contract()
    verify_guest_shell_shutdown_contract()
    verify_geekbench_payloads()
    verify_no_root_device_option()
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
    for runner in MOBILE_RUNNERS:
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
    count = len(RUNNERS) + len(MOBILE_RUNNERS)
    print(f"benchmark CLI/import smoke passed ({count} runners)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
