#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Qualify Steam rootfs boot, Proton CPU bridges, and the gaming graphics boundary."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
from pathlib import Path
import re
import shlex
import sys
import time


REPOSITORY = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPOSITORY / "tests/integration/support"))
from guest import Guest  # noqa: E402

PROTON = "/opt/proton-uml-arm64ec"
STEAM = "/home/uml/.local/share/Steam"
WINE_DEBUG = "-all,err+seh,err+virtual"


def success(guest: Guest, command: str, timeout: int = 60) -> str:
    result = guest.run_command(command, timeout)
    if result.return_code != 0:
        raise RuntimeError(f"guest command failed ({result.return_code}): {result.output}")
    return result.output


def probe(guest: Guest, name: str, command: str, expected: int, timeout: int) -> str:
    log = f"/tmp/steam-{name}.log"
    status = f"/tmp/steam-{name}.status"
    script = f"{command} >{log} 2>&1; printf '%s\\n' \"$?\" >{status}"
    success(guest, f"runuser -u uml -- sh -c {shlex.quote(script)} & :")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = success(guest, f"if test -f {status}; then printf 'done:'; cat {status}; else echo pending; fi", 15)
        found = re.search(r"done:(\d+)", result)
        if found:
            output = success(guest, f"cat {log}")
            if re.search(r"SIGILL|illegal instruction|undefined instruction|c000001d|nested exception", output, re.I):
                raise RuntimeError(f"{name} encountered a Wine execution fault:\n{output}")
            if int(found[1]) != expected:
                raise RuntimeError(f"{name} returned {found[1]}, expected {expected}:\n{output}")
            print(f"PASS {name} exit={expected}", flush=True)
            return output
        time.sleep(0.5)
    output = success(guest, f"tail -n 100 {log}; ps -u uml -o pid,ppid,stat,wchan:32,comm")
    raise TimeoutError(f"{name} exceeded {timeout}s:\n{output}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emulator", required=True, type=Path)
    parser.add_argument("--rootfs", required=True, type=Path)
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--boot-timeout", type=int, default=120)
    parser.add_argument("--timeout", type=int, default=420)
    parser.add_argument("--log", type=Path)
    parser.add_argument("--experimental-venus", action="store_true")
    args = parser.parse_args()
    guest = Guest(args.emulator.resolve(), args.rootfs.resolve(), args.memory, "tmpfs",
                  kernel_arguments=("--video", "panic=1", "--gpu-display=vnc", "--gpu-vnc-port=0"),
                  systemd_overlay=True, systemd_overlay_size="16G", engine="jit")
    try:
        guest.boot_and_login(args.boot_timeout)
        success(guest, "test -c /dev/video-dec0 && runuser -u uml -- test -w /dev/video-dec0")
        output = success(guest,
            "test \"$(uname -m)\" = aarch64 && test \"$(getconf PAGESIZE)\" = 4096 && "
            "test -z \"$(dpkg --print-foreign-architectures)\" && "
            "test -x /usr/games/gamescope && test -x /usr/local/bin/steam && "
            "test ! -x /usr/bin/startxfce4 && test ! -x /usr/bin/startplasma-wayland && "
            "systemctl is-enabled --quiet steam-gaming.service && "
            f"test -x {STEAM}/steamrtarm64/steam && "
            "test -c /dev/dri/renderD128 && uname -a")
        print("PASS systemd boot, ARM64 userspace, 4 KiB pages, virtio GPU\n" + output, flush=True)
        success(guest, "Xvfb :99 -screen 0 1280x800x24 >/tmp/steam-xvfb.log 2>&1 & :")
        success(guest, "timeout 30 sh -c 'until DISPLAY=:99 xdpyinfo >/dev/null 2>&1; do sleep 1; done'", 40)
        env = f"env DISPLAY=:99 WINEDEBUG={WINE_DEBUG} WINEDLLOVERRIDES=winedbg.exe=d WINEPREFIX=/home/uml/.wine-smoke"
        for name, status in (("x86_64", 42), ("i386", 43)):
            probe(guest, name, f"{env} /usr/local/bin/wine /opt/uml-wine-smoke/{name}-exit-{status}.exe", status, args.timeout)
        success(guest, f"runuser -u uml -- {env} {PROTON}/files/bin-arm64/wineserver -k")
        compat = "/home/uml/proton-smoke"
        success(guest, f"install -d -o uml -g uml {compat}")
        proton_env = (
            f"env DISPLAY=:99 WINEDEBUG={WINE_DEBUG} WINEDLLOVERRIDES=winedbg.exe=d PROTON_USE_ARM64=1 "
            f"STEAM_COMPAT_CLIENT_INSTALL_PATH={STEAM} STEAM_COMPAT_DATA_PATH={compat} "
            "SteamGameId=0 SteamAppId=0"
        )
        probe(guest, "proton", f"{proton_env} {PROTON}/proton waitforexitandrun /opt/uml-wine-smoke/x86_64-exit-42.exe", 42, args.timeout)
        graphics = guest.run_command(
            "runuser -u uml -- env DISPLAY=:99 XDG_RUNTIME_DIR=/run/user/1000 "
            "vulkaninfo --summary >/tmp/steam-vulkan.log 2>&1; "
            "printf 'vulkaninfo status=%s\\n' \"$?\"; "
            "grep -Ei 'ERROR|WARNING|deviceName|driverName|Vulkan Instance|llvmpipe|SIGILL|illegal instruction' /tmp/steam-vulkan.log", 90)
        print(graphics.output, flush=True)
        if args.experimental_venus and not re.search(r"driverName\s*=\s*venus", graphics.output):
            raise RuntimeError("the experimental guest did not expose the Venus Vulkan driver")
        guest.run_command("systemctl start steam-gaming.service", 60)
        deadline = time.monotonic() + 90
        state = ""
        steam_seen_at = None
        steam_bootstrap_running = False
        while time.monotonic() < deadline:
            state = success(guest, "systemctl show steam-gaming.service -p ActiveState -p ExecMainCode -p ExecMainStatus")
            if "ActiveState=failed" in state or "ActiveState=inactive" in state:
                break
            if args.experimental_venus:
                client = guest.run_command(
                    f"pgrep -u uml -f '^{STEAM}/steamrtarm64/steam -gamepadui$' >/dev/null", 15)
                if client.return_code == 0:
                    if steam_seen_at is None:
                        steam_seen_at = time.monotonic()
                    elif time.monotonic() - steam_seen_at >= 5:
                        steam_bootstrap_running = True
                        break
                else:
                    steam_seen_at = None
            time.sleep(0.5)
        # PAM moves the application into a session scope; its journal records
        # need not retain the system service's unit identity.
        journal = success(guest, "journalctl -b -u steam-gaming.service --no-pager -o cat")
        journal += success(guest, "journalctl -b _COMM=gamescope --no-pager -o cat --grep='selecting physical device|Error|Failed|SIGILL|illegal instruction'")
        faults = guest.run_command("journalctl -b --no-pager -o cat --grep='(?i)SIGILL|illegal instruction|undefined instruction|status=4/ILL'")
        # journalctl returns 1 with empty output when the filter has no hits.
        if faults.return_code != 0 and not (faults.return_code == 1 and not faults.output):
            raise RuntimeError(f"cannot inspect the guest fault journal: {faults.output}")
        print(state + journal, flush=True)
        # Inspect command results, not echoed shell commands containing the
        # diagnostic search patterns themselves.
        if re.search(r"SIGILL|illegal instruction|undefined instruction|status=4/ILL|code=c000001d",
                     graphics.output + journal + faults.output, re.I):
            raise RuntimeError("guest encountered an illegal instruction")
        if re.search(r"ExecMainCode=[23]", state) and "ExecMainStatus=4" in state:
            raise RuntimeError("gaming session terminated with an illegal instruction")
        if args.experimental_venus:
            if not steam_bootstrap_running or "ActiveState=active" not in state:
                raise RuntimeError("Gamescope did not launch the native Steam bootstrap")
            if re.search(r"drmModeAddFB2.*failed|bad pitch", journal, re.I):
                raise RuntimeError("Gamescope framebuffer registration failed")
            # KMS may reject direct AR24 client scanout and use composition;
            # require a registered Gamescope framebuffer with its real pitch.
            success(guest,
                "awk '/^framebuffer\\[/ {owned=0; width=0} "
                "/allocated by = gamescope/ {owned=1} "
                "/size\\[0\\]=/ {split($0,a,\"=\"); split(a[2],b,\"x\"); width=b[1]+0} "
                "/pitch\\[0\\]=/ {split($0,a,\"=\"); if (owned && width>0 && a[2]+0==width*4) ok=1} "
                "END {exit !ok}' /sys/kernel/debug/dri/0/framebuffer")
            print("PASS Gamescope framebuffer registration and native Steam bootstrap; login UI not qualified", flush=True)
        else:
            expected = re.search(
                r"Failed to initialize Vulkan|No Vulkan devices|vulkan:.*(?:vkCreateDevice failed|failed to find physical device|failed to initialize|does not support)",
                journal, re.I)
            if not expected or "ActiveState=failed" not in state:
                raise RuntimeError("gaming session did not reach the expected Vulkan graphics limitation")
            print("PASS gaming session reached the expected Mesa/Vulkan graphics limitation", flush=True)
        if not guest.shutdown(30) or guest.return_code != 0:
            raise RuntimeError("Steam guest did not shut down")
        if re.search(r"Kernel panic|BUG:|Oops:|detected stalls on CPUs", guest.output()):
            raise RuntimeError("Steam guest reported a kernel failure")
        return 0
    except BaseException:
        print(guest.output(), file=sys.stderr)
        raise
    finally:
        if args.log:
            args.log.parent.mkdir(parents=True, exist_ok=True)
            args.log.write_text(guest.output())
        guest.close()


if __name__ == "__main__":
    raise SystemExit(main())
