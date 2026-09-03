#!/usr/bin/env python3
"""Boot the ARM64EC Wine appliance and execute AMD64 and i386 PE probes."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import os
import pathlib
import re
import shlex
import sys
import time


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests" / "integration" / "support"))

from guest import Guest  # noqa: E402


def require_success(guest: Guest, command: str, timeout: int = 60) -> str:
    result = guest.run_command(command, timeout)
    if result.return_code != 0:
        raise RuntimeError(f"guest command failed ({result.return_code}):\n{result.output}")
    return result.output


def wine_diagnostics(guest: Guest, log_path: str) -> str:
    result = guest.run_command(
        "timeout 5 ps -u uml -o pid=,ppid=,stat=,wchan:32=,etime=,comm= "
        "--sort=pid || true; "
        "for comm in wineserver wineboot.exe rundll32.exe; do "
        "for pid in $(timeout 3 pgrep -u uml -x \"$comm\"); do "
        "printf 'pid=%s comm=%s cmdline=' \"$pid\" \"$comm\"; "
        "timeout 2 tr '\\0' ' ' </proc/$pid/cmdline 2>/dev/null || true; "
        "printf '\\n'; "
        "printf 'pid=%s wchan=' \"$pid\"; "
        "timeout 2 cat /proc/$pid/wchan 2>/dev/null || true; "
        "printf '\\n'; "
        "printf 'pid=%s syscall=' \"$pid\"; "
        "timeout 2 cat /proc/$pid/syscall 2>/dev/null || true; "
        "printf '\\n'; "
        "timeout 3 grep -E '/opt/wine-arm64ec|/home/uml/.wine' "
        "/proc/$pid/maps 2>/dev/null | tail -n 40 || true; "
        "done; done; "
        f"tail -n 120 {shlex.quote(log_path)} 2>/dev/null || true",
        60,
    )
    return result.output


def run_wine_probe(
    guest: Guest,
    name: str,
    executable: str,
    expected_status: int,
    wine_debug: str,
    timeout: int,
) -> str:
    status_path = f"/tmp/uml-wine-{name}.status"
    log_path = f"/tmp/uml-wine-{name}.log"
    pid_path = f"/run/uml-wine-{name}.pid"
    command = (
        "env DISPLAY=:99 "
        f"WINEDEBUG={shlex.quote(wine_debug)} "
        "WINEPREFIX=/home/uml/.wine-smoke /usr/local/bin/wine "
        f"{shlex.quote(executable)} >{shlex.quote(log_path)} 2>&1; "
        "status=$?; "
        f"printf '%s\\n' \"$status\" >{shlex.quote(status_path)}"
    )
    require_success(
        guest,
        f"rm -f {shlex.quote(status_path)} {shlex.quote(log_path)}; "
        f"runuser -u uml -- sh -c {shlex.quote(command)} & "
        f"echo $! >{shlex.quote(pid_path)}",
    )

    deadline = time.monotonic() + timeout
    status = None
    while time.monotonic() < deadline:
        result = guest.run_command(
            f"if test -r {shlex.quote(status_path)}; then "
            f"printf 'done:'; cat {shlex.quote(status_path)}; "
            "else printf 'pending\\n'; fi",
            10,
        )
        match = re.search(r"done:(-?\d+)", result.output)
        if match:
            status = int(match.group(1))
            break
        time.sleep(0.25)

    if status is None:
        diagnostics = wine_diagnostics(guest, log_path)
        guest.run_command(
            f"kill $(cat {shlex.quote(pid_path)}) 2>/dev/null || true; "
            "runuser -u uml -- env WINEPREFIX=/home/uml/.wine-smoke "
            "/opt/wine-arm64ec/bin/wineserver -k 2>/dev/null || true",
            30,
        )
        raise TimeoutError(
            f"Wine {name} probe exceeded {timeout}s:\n{diagnostics}"
        )

    output = guest.run_command(
        f"cat {shlex.quote(log_path)} 2>/dev/null || true", 30
    ).output
    if status != expected_status:
        diagnostics = wine_diagnostics(guest, log_path)
        raise RuntimeError(
            f"Wine {name} probe returned {status}, expected {expected_status}:\n"
            f"{diagnostics}"
        )
    return output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=pathlib.Path)
    parser.add_argument("--rootfs", required=True, type=pathlib.Path)
    parser.add_argument("--memory", default="4G")
    parser.add_argument("--boot-timeout", type=int, default=90)
    parser.add_argument("--timeout", type=int, default=360)
    parser.add_argument(
        "--wine-debug", default=os.environ.get("UML_WINEDEBUG", "-all")
    )
    arguments = parser.parse_args()

    emulator = arguments.emulator.resolve()
    rootfs = arguments.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        parser.error(f"emulator is not executable: {emulator}")
    if not rootfs.is_file():
        parser.error(f"rootfs is not a regular file: {rootfs}")
    if arguments.boot_timeout <= 0 or arguments.timeout <= 0:
        parser.error("timeouts must be positive")

    guest = Guest(
        emulator,
        rootfs,
        arguments.memory,
        "tmpfs",
        kernel_arguments=("panic=1",),
        systemd_overlay=True,
        engine="jit",
    )
    try:
        guest.boot_and_login(arguments.boot_timeout)
        require_success(
            guest,
            "test \"$(uname -m)\" = aarch64 && "
            "test -z \"$(dpkg --print-foreign-architectures)\" && "
            "test -x /usr/local/bin/wine && "
            "test -x /opt/uml-wine-smoke/x86_64-exit-42.exe && "
            "test -x /opt/uml-wine-smoke/i386-exit-43.exe && "
            "test -f /opt/wine-arm64ec/lib/wine/i386-windows/ntdll.dll && "
            "test \"$(find /opt/wine-arm64ec -name xtajit64.dll -type f | wc -l)\" -eq 1 && "
            "test \"$(find /opt/wine-arm64ec -name xtajit.dll -type f | wc -l)\" -eq 1",
        )
        require_success(
            guest,
            "command -v startxfce4 >/dev/null && command -v xfwm4 >/dev/null && "
            "command -v Xvfb >/dev/null && command -v xdpyinfo >/dev/null && "
            "systemctl is-enabled --quiet lightdm.service",
        )
        require_success(
            guest,
            "Xvfb :99 -screen 0 1024x768x24 >/tmp/Xvfb.log 2>&1 & "
            "echo $! >/run/uml-wine-xvfb.pid",
        )
        require_success(
            guest,
            "timeout 30 sh -c 'until DISPLAY=:99 xdpyinfo >/dev/null 2>&1; "
            "do sleep 1; done'",
            40,
        )
        require_success(
            guest,
            "runuser -u uml -- env DISPLAY=:99 dbus-run-session -- "
            "xfwm4 --replace >/tmp/xfwm4.log 2>&1 & "
            "echo $! >/run/uml-wine-xfwm.pid",
        )
        require_success(
            guest,
            "timeout 30 sh -c 'until pgrep -u uml -x xfwm4 >/dev/null; "
            "do sleep 1; done'",
            40,
        )
        amd64_output = run_wine_probe(
            guest,
            "amd64",
            "/opt/uml-wine-smoke/x86_64-exit-42.exe",
            42,
            arguments.wine_debug,
            arguments.timeout,
        )
        i386_output = run_wine_probe(
            guest,
            "i386",
            "/opt/uml-wine-smoke/i386-exit-43.exe",
            43,
            arguments.wine_debug,
            arguments.timeout,
        )
        require_success(
            guest,
            "runuser -u uml -- env WINEPREFIX=/home/uml/.wine-smoke "
            "/opt/wine-arm64ec/bin/wineserver -k",
        )
        if not guest.shutdown(30):
            raise RuntimeError("ARM64EC Wine guest did not shut down")
    except BaseException:
        print(guest.output(), file=sys.stderr)
        raise
    finally:
        guest.close()

    print(amd64_output)
    print(i386_output)
    print(
        "PASS kernel=arm64 userspace=arm64 wine=arm64ec "
        "windows=amd64,i386 cpu=gadgets"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
