#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the frozen Chromium WebGL Aquarium probe on a Darwin UML guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import json
import math
import os
from pathlib import Path
import re
import shlex
import signal
import statistics
import subprocess
import sys
import time
from typing import Any, BinaryIO, NamedTuple, Sequence


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import boot_kde_vnc  # noqa: E402
import kernarium_vk_gl_cts_common as kernarium_common  # noqa: E402
import run_vk_gl_cts as cts  # noqa: E402
import vulkan_cts_receipt as vk_receipt  # noqa: E402


AQUARIUM_URL = (
    "https://webglsamples.org/aquarium/aquarium.html?numFish=500"
)
FISH_COUNT = 500
DEFAULT_WARMUP_SECONDS = 15
DEFAULT_SAMPLE_SECONDS = 30
# The runner's overall deadline, in seconds from emulator start. Every guest
# wait is derived from it, so a guest-side stall ends in a named console error
# before the runner gives up.
DEFAULT_TIMEOUT_SECONDS = 360
# The launcher's own deadline precedes the runner's by this much, leaving room
# for its diagnostics and error line to reach the console log.
GUEST_REPORT_MARGIN_SECONDS = 45
# Chromium start-up and the probe's readiness checks need at least this long
# beyond the warm-up and sample windows.
CHROMIUM_MINIMUM_SECONDS = 60
# The KWin renderer query retries this long once the Wayland socket exists;
# each D-Bus call is bounded separately, because a compositor whose main loop
# is stuck never answers.
COMPOSITOR_PROBE_SECONDS = 60
DBUS_CALL_SECONDS = 10
# The probe reports this long before the launcher stops Chromium, and keeps as
# much slack beyond its measurement windows.
PAGE_MARGIN_SECONDS = 10
# The boot and the KDE session must keep at least this much guest uptime.
MINIMUM_SESSION_SECONDS = 60
# Kernarium reads the probe's console without a host deadline. This bounds
# the guest waits of that unattended run instead.
KERNARIUM_TIMEOUT_SECONDS = 1800
CONSOLE_BASE64_CHUNK_CHARACTERS = 1024
EXTENSION_MANIFEST = """{
  "manifest_version": 3,
  "name": "UML Aquarium verifier",
  "version": "1.0",
  "content_scripts": [{
    "matches": ["https://webglsamples.org/aquarium/*"],
    "js": ["verify.js"],
    "run_at": "document_start",
    "world": "MAIN"
  }]
}
"""
EXTENSION_SCRIPT = """(() => {
  const warmupSeconds = @WARMUP_SECONDS@;
  const sampleSeconds = @SAMPLE_SECONDS@;
  const fishCount = @FISH_COUNT@;
  // Wall-clock milliseconds by which this probe must report. The guest
  // launcher arms it from the run's deadline just before starting Chromium,
  // and the page must be ready early enough to finish both windows by then.
  const pageDeadline = @PAGE_DEADLINE_MS@;
  const readyDeadline = pageDeadline
    - (warmupSeconds + sampleSeconds + @PAGE_MARGIN_SECONDS@) * 1000;
  const resourceErrors = [];
  let lastResourceError = "";
  const shader = {
    compileCalls: 0,
    compileCpuMs: 0,
    linkCalls: 0,
    linkCpuMs: 0,
    instrumentedMethods: 0,
    instrumentationErrors: 0
  };
  const wrapTimedCall = (prototype, name, callsKey, timeKey) => {
    if (!prototype || typeof prototype[name] !== "function") {
      return;
    }
    const original = prototype[name];
    try {
      Object.defineProperty(prototype, name, {
        configurable: true,
        writable: true,
        value: function(...arguments_) {
          const started = performance.now();
          try {
            return Reflect.apply(original, this, arguments_);
          } finally {
            shader[callsKey] += 1;
            shader[timeKey] += performance.now() - started;
          }
        }
      });
      shader.instrumentedMethods += 1;
    } catch {
      shader.instrumentationErrors += 1;
    }
  };
  for (const contextName of [
    "WebGLRenderingContext", "WebGL2RenderingContext"
  ]) {
    const prototype = globalThis[contextName]?.prototype;
    wrapTimedCall(
      prototype, "compileShader", "compileCalls", "compileCpuMs");
    wrapTimedCall(prototype, "linkProgram", "linkCalls", "linkCpuMs");
  }
  const isIgnoredSiteDecoration = (source) => {
    try {
      const url = new URL(String(source), location.href);
      return url.origin === location.origin && url.pathname === "/favicon.ico";
    } catch {
      return false;
    }
  };

  addEventListener("error", (event) => {
    const target = event.target;
    if (target && target !== globalThis) {
      const source = target.src || target.href || target.localName || "resource";
      if (isIgnoredSiteDecoration(source)) {
        return;
      }
      lastResourceError = String(source);
      resourceErrors.push(lastResourceError);
    } else if (event.message) {
      lastResourceError = String(event.message);
      resourceErrors.push(lastResourceError);
    }
  }, true);
  addEventListener("unhandledrejection", (event) => {
    lastResourceError = String(event.reason || "unhandled rejection");
    resourceErrors.push(lastResourceError);
  });

  const delay = (milliseconds) =>
    new Promise((resolve) => setTimeout(resolve, milliseconds));
  const fail = (message) => {
    console.error("UML_AQUARIUM_ERROR|" + String(message)
      .replace(/\\s+/g, " ").trim());
  };

  const run = async () => {
    while (document.readyState !== "complete" && Date.now() < readyDeadline) {
      await delay(1000);
    }
    if (document.readyState !== "complete") {
      throw new Error("document load did not finish before the page deadline"
        + " (readyState " + document.readyState + ")");
    }

    const rasterMarker = document.createElement("div");
    rasterMarker.textContent = "WARMING UP";
    rasterMarker.style.cssText = [
      "position:fixed",
      "right:8px",
      "top:8px",
      "width:280px",
      "height:64px",
      "z-index:2147483647",
      "display:flex",
      "align-items:center",
      "justify-content:center",
      "background:#ff00ff",
      "color:#00ff00",
      "font:28px monospace"
    ].join(";");
    document.documentElement.appendChild(rasterMarker);

    let positiveReadings = 0;
    let stableResourceSeconds = 0;
    let previousResourceCount = -1;
    while (Date.now() < readyDeadline) {
      if (resourceErrors.length !== 0) {
        throw new Error("resource load failed: " + lastResourceError);
      }
      const resources = globalThis.performance.getEntriesByType("resource");
      const failed = resources.find((entry) =>
        Number.isFinite(entry.responseStatus) &&
        entry.responseStatus >= 400 &&
        !isIgnoredSiteDecoration(entry.name));
      if (failed) {
        throw new Error("resource returned " + failed.responseStatus
          + ": " + failed.name);
      }
      if (resources.length === previousResourceCount) {
        stableResourceSeconds += 1;
      } else {
        previousResourceCount = resources.length;
        stableResourceSeconds = 0;
      }
      const fpsValue = Number.parseFloat(
        document.getElementById("fps")?.textContent || "");
      positiveReadings = Number.isFinite(fpsValue) && fpsValue > 0
        ? positiveReadings + 1
        : 0;
      if (positiveReadings >= 5 && stableResourceSeconds >= 5) {
        break;
      }
      const message = document.getElementById("msgContainer")?.textContent || "";
      if (message.includes("does not appear your computer supports WebGL")) {
        throw new Error(message);
      }
      await delay(1000);
    }
    if (positiveReadings < 5 || stableResourceSeconds < 5) {
      throw new Error("page resources or rendering did not stabilize before"
        + " the page deadline (positive FPS readings " + positiveReadings
        + ", stable resource seconds " + stableResourceSeconds + ")");
    }

    const canvas = document.getElementById("canvas");
    const gl = canvas?.getContext("webgl2");
    if (!gl || gl.isContextLost()) {
      throw new Error("WebGL 2 context unavailable or lost");
    }
    const incompleteImages = Array.from(document.images).filter(
      (image) => !image.complete || image.naturalWidth === 0);
    if (incompleteImages.length !== 0) {
      throw new Error("document images are incomplete");
    }

    for (let second = warmupSeconds; second > 0; --second) {
      rasterMarker.textContent = "WARMUP " + second + "s";
      await delay(1000);
    }

    const samples = [];
    rasterMarker.textContent = "MEASURING";
    console.log("UML_AQUARIUM_MEASURE_BEGIN"
      + "|WARMUP_SECONDS=" + warmupSeconds
      + "|SAMPLE_SECONDS=" + sampleSeconds
      + "|FISH=" + fishCount
      + "|SHADER_COMPILES=" + shader.compileCalls
      + "|SHADER_COMPILE_CPU_MS=" + shader.compileCpuMs.toFixed(3)
      + "|PROGRAM_LINKS=" + shader.linkCalls
      + "|PROGRAM_LINK_CPU_MS=" + shader.linkCpuMs.toFixed(3)
      + "|INSTRUMENTED_METHODS=" + shader.instrumentedMethods
      + "|INSTRUMENTATION_ERRORS=" + shader.instrumentationErrors);
    for (let second = 0; second < sampleSeconds; ++second) {
      await delay(1000);
      if (resourceErrors.length !== 0) {
        throw new Error("resource load failed: " + lastResourceError);
      }
      if (gl.isContextLost()) {
        throw new Error("WebGL context lost during measurement");
      }
      const fpsText = document.getElementById("fps")?.textContent.trim() || "";
      const fpsValue = Number.parseFloat(fpsText);
      if (!Number.isFinite(fpsValue) || fpsValue <= 0) {
        throw new Error("invalid FPS sample: " + fpsText);
      }
      samples.push(fpsValue);
      console.log("UML_AQUARIUM_SAMPLE|INDEX=" + (second + 1)
        + "|FPS=" + fpsValue.toFixed(3));
    }

    const average = samples.reduce((sum, value) => sum + value, 0)
      / samples.length;
    const minimum = Math.min(...samples);
    const maximum = Math.max(...samples);
    const extension = gl.getExtension("WEBGL_debug_renderer_info");
    const renderer = extension
      ? gl.getParameter(extension.UNMASKED_RENDERER_WEBGL)
      : gl.getParameter(gl.RENDERER);
    const resources =
      globalThis.performance.getEntriesByType("resource").length;
    rasterMarker.textContent = average.toFixed(2) + " AVG FPS";
    console.log("UML_AQUARIUM_OK|AVG_FPS=" + average.toFixed(3)
      + "|MIN_FPS=" + minimum.toFixed(3)
      + "|MAX_FPS=" + maximum.toFixed(3)
      + "|SAMPLES=" + samples.length
      + "|WARMUP_SECONDS=" + warmupSeconds
      + "|RESOURCES=" + resources
      + "|FISH=" + fishCount
      + "|CANVAS=" + canvas.width + "x" + canvas.height
      + "|SHADER_COMPILES=" + shader.compileCalls
      + "|SHADER_COMPILE_CPU_MS=" + shader.compileCpuMs.toFixed(3)
      + "|PROGRAM_LINKS=" + shader.linkCalls
      + "|PROGRAM_LINK_CPU_MS=" + shader.linkCpuMs.toFixed(3)
      + "|INSTRUMENTED_METHODS=" + shader.instrumentedMethods
      + "|INSTRUMENTATION_ERRORS=" + shader.instrumentationErrors
      + "|RAW_FPS=" + samples.map((value) => value.toFixed(3)).join(",")
      + "|VERSION=" + gl.getParameter(gl.VERSION)
      + "|RENDERER=" + renderer);
  };

  run().catch((error) => fail(error?.stack || error));
})();
"""
LAUNCHER = """#!/bin/sh
set -eu
renderer_backend="@RENDERER_BACKEND@"
# Guest-uptime deadlines derived from the runner's overall deadline. Every wait
# below ends in a named UML_CHROMIUM_ERROR before the runner gives up.
deadline=@GUEST_DEADLINE@
session_deadline=@SESSION_DEADLINE@
compositor_deadline=@COMPOSITOR_DEADLINE@
dbus_call_seconds=@DBUS_CALL_SECONDS@
page_margin_seconds=@PAGE_MARGIN_SECONDS@
extension=/var/tmp/uml-aquarium-extension
if [ "$renderer_backend" = zink ]; then
    export MESA_LOADER_DRIVER_OVERRIDE=zink
fi
uml_uid="$(id -u uml)"
runtime="/run/user/$uml_uid"
printf 'UML_AQUARIUM_LAUNCHER|UPTIME=%s|DEADLINE=%s\\n' \\
    "$(cut -d' ' -f1 /proc/uptime)" "$deadline"
uptime_seconds()
{
    cut -d. -f1 /proc/uptime
}
emit_session_diagnostics()
{
    echo 'UML_SESSION_DIAGNOSTICS_BEGIN'
    ls -l "$runtime" /tmp/.X11-unix 2>&1 || true
    timeout 10 loginctl list-sessions --no-pager 2>&1 || true
    ps -eo pid,ppid,stat,etimes,wchan:24,args 2>&1 |
        grep -E 'sddm|kwin|plasma|Xwayland|qdbus|chromium|agetty|login' |
        grep -v grep || true
    timeout 15 journalctl --no-pager -b -o short-monotonic \\
        -u sddm.service -u 'getty@*' -n 60 2>&1 || true
    timeout 15 journalctl --no-pager -b -o short-monotonic \\
        _UID="$uml_uid" -n 120 2>&1 || true
    tail -n 60 /home/uml/.local/share/sddm/wayland-session.log 2>/dev/null ||
        true
    echo 'UML_SESSION_DIAGNOSTICS_END'
}
fail()
{
    emit_session_diagnostics
    echo "UML_CHROMIUM_ERROR: $*"
    exit 1
}
if [ "$renderer_backend" = zink ]; then
    display_socket="$runtime/wayland-0"
    display_description="KDE Wayland session"
else
    display_socket=/tmp/.X11-unix/X0
    display_description="KDE X11 bridge"
fi
while [ ! -S "$display_socket" ] || [ ! -S "$runtime/bus" ]; do
    if [ "$(uptime_seconds)" -ge "$session_deadline" ]; then
        fail "$display_description did not become ready by guest uptime" \\
            "${session_deadline}s"
    fi
    sleep 1
done
xauthority=""
chromium_platform_flags=""
if [ "$renderer_backend" = zink ]; then
    chromium_platform_flags="--ozone-platform=wayland"
    kwin_pid="$(pgrep -xo kwin_wayland || true)"
    if [ -z "$kwin_pid" ]; then
        fail "KDE compositor is not running"
    fi
    if ! tr '\\0' '\\n' < "/proc/$kwin_pid/environ" |
        grep -qx 'MESA_LOADER_DRIVER_OVERRIDE=zink'; then
        fail "KDE compositor did not inherit Zink"
    fi
    kwin_renderer=""
    kwin_answered=0
    while :; do
        now="$(uptime_seconds)"
        if [ "$now" -ge "$compositor_deadline" ]; then
            if [ "$kwin_answered" = 0 ]; then
                fail "KDE compositor did not answer on D-Bus by guest" \\
                    "uptime ${compositor_deadline}s"
            fi
            fail "KDE compositor reported no OpenGL renderer by guest" \\
                "uptime ${compositor_deadline}s"
        fi
        call_seconds=$((compositor_deadline - now))
        if [ "$call_seconds" -gt "$dbus_call_seconds" ]; then
            call_seconds=$dbus_call_seconds
        fi
        if kwin_support="$(runuser -u uml -- env \\
            XDG_RUNTIME_DIR="$runtime" \\
            DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \\
            timeout -k 2 "$call_seconds" \\
            qdbus6 org.kde.KWin /KWin supportInformation 2>/dev/null)"; then
            kwin_answered=1
            kwin_renderer="$(printf '%s\\n' "$kwin_support" |
                sed -n 's/^[[:space:]]*OpenGL renderer string:[[:space:]]*//p' |
                head -n 1)"
            if [ -n "$kwin_renderer" ]; then
                break
            fi
        fi
        sleep 1
    done
    if ! printf '%s\\n' "$kwin_renderer" | grep -qi zink ||
        ! printf '%s\\n' "$kwin_renderer" | grep -qi 'virtio-gpu venus'; then
        echo 'UML_ZINK_DIAGNOSTICS_BEGIN'
        runuser -u uml -- env \\
            HOME=/home/uml \\
            XDG_RUNTIME_DIR="$runtime" \\
            MESA_LOADER_DRIVER_OVERRIDE=zink \\
            LIBGL_DEBUG=verbose \\
            MESA_DEBUG=1 \\
            timeout 30 eglinfo -v -a gles -p surfaceless || true
        timeout 15 journalctl --no-pager -b _COMM=kwin_wayland -n 160 || true
        echo 'UML_ZINK_DIAGNOSTICS_END'
        printf 'UML_CHROMIUM_ERROR: KDE compositor renderer is not Zink/Venus: %s\\n' \\
            "$kwin_renderer"
        exit 1
    fi
    printf 'UML_KDE_ZINK_READY|KWIN_PID=%s|RENDERER=%s\\n' \\
        "$kwin_pid" "$kwin_renderer"
else
    xauthority="$(find "$runtime" -maxdepth 1 -type f -name 'xauth_*' -print |
        head -n 1)"
    if [ -z "$xauthority" ]; then
        fail "Xauthority cookie was not found"
    fi
    runuser -u uml -- env \\
        DISPLAY=:0 \\
        XAUTHORITY="$xauthority" \\
        XDG_RUNTIME_DIR="$runtime" \\
        timeout 30 glxinfo -B || true
fi
if [ "@EGL_CONFIG_DIAGNOSTICS@" = 1 ]; then
    echo 'UML_EGL_CONFIG_DIAGNOSTICS_BEGIN'
    timeout 30 chromium --version || true
    if [ "$renderer_backend" = zink ]; then
        runuser -u uml -- env \\
            WAYLAND_DISPLAY=wayland-0 \\
            XDG_RUNTIME_DIR="$runtime" \\
            timeout 30 eglinfo -v -a gles -p wayland || true
    else
        runuser -u uml -- env \\
            DISPLAY=:0 \\
            XAUTHORITY="$xauthority" \\
            XDG_RUNTIME_DIR="$runtime" \\
            timeout 30 eglinfo -v -a gles -p x11 || true
        runuser -u uml -- env \\
            DISPLAY=:0 \\
            XAUTHORITY="$xauthority" \\
            XDG_RUNTIME_DIR="$runtime" \\
            timeout 30 xdpyinfo || true
        runuser -u uml -- env \\
            DISPLAY=:0 \\
            XAUTHORITY="$xauthority" \\
            XDG_RUNTIME_DIR="$runtime" \\
            timeout 30 xwininfo -root -stats || true
    fi
    echo 'UML_EGL_CONFIG_DIAGNOSTICS_END'
fi
printf 'UML_AQUARIUM_GUEST_IDENTITY|PAGE_SIZE=%s|CPUS=%s|MEM_TOTAL_KIB=%s\\n' \\
    "$(getconf PAGESIZE)" "$(getconf _NPROCESSORS_ONLN)" \\
    "$(awk '/^MemTotal:/ { print $2; exit }' /proc/meminfo)"
emit_guest_snapshot()
{
    phase=$1
    index=$2
    cpu_jiffies=$(sed -n 's/^cpu  *//p' /proc/stat | tr -s ' ' ',')
    mem_available=$(awk '/^MemAvailable:/ { print $2; exit }' /proc/meminfo)
    mem_free=$(awk '/^MemFree:/ { print $2; exit }' /proc/meminfo)
    committed=$(awk '/^Committed_AS:/ { print $2; exit }' /proc/meminfo)
    printf 'UML_AQUARIUM_GUEST_%s|INDEX=%s|UPTIME=%s|CLK_TCK=%s|CPU_JIFFIES=%s|MEM_AVAILABLE_KIB=%s|MEM_FREE_KIB=%s|COMMITTED_AS_KIB=%s\\n' \\
        "$phase" "$index" "$(cut -d' ' -f1 /proc/uptime)" \\
        "$(getconf CLK_TCK)" "$cpu_jiffies" "$mem_available" \\
        "$mem_free" "$committed"
}
# Chromium gets the rest of the budget. The probe must report
# page_margin_seconds before Chromium is stopped, so a page that never becomes
# ready still names its own failure.
chromium_seconds=$((deadline - $(uptime_seconds)))
if [ "$chromium_seconds" -le "$page_margin_seconds" ]; then
    fail "no budget remains for Chromium at guest uptime $(uptime_seconds)s"
fi
now_epoch="$(date +%s)"
page_deadline_ms=$(((now_epoch + chromium_seconds - page_margin_seconds) * 1000))
# The split keeps this launcher free of unreplaced template placeholders.
sed -i "s/@PAGE_DEADLINE_MS""@/$page_deadline_ms/" "$extension/verify.js"
if ! grep -q "const pageDeadline = $page_deadline_ms;" "$extension/verify.js"; then
    fail "Aquarium probe page deadline was not armed"
fi
{
    chromium_status=0
    timeout -k 10 "$chromium_seconds" runuser -u uml -- env \\
        HOME=/home/uml \\
        DISPLAY=:0 \\
        WAYLAND_DISPLAY=wayland-0 \\
        XAUTHORITY="$xauthority" \\
        XDG_RUNTIME_DIR="$runtime" \\
        DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \\
        chromium \\
        --use-gl=angle \\
        --use-angle=gles-egl \\
        --ignore-gpu-blocklist \\
        $chromium_platform_flags \\
        @CHROMIUM_EXTRA_FLAGS@ \\
        --no-first-run \\
        --disable-crash-reporter \\
        --enable-logging=stderr \\
        --start-maximized \\
        --user-data-dir=/tmp/chromium-aquarium \\
        --disable-extensions-except="$extension" \\
        --load-extension="$extension" \\
        @AQUARIUM_URL@ 2>&1 || chromium_status=$?
    printf 'UML_CHROMIUM_EXIT|STATUS=%s\\n' "$chromium_status"
} | {
    reported=0
    while IFS= read -r line; do
        printf '%s\\n' "$line"
        case "$line" in
        UML_CHROMIUM_EXIT\\|STATUS=*)
            status="${line#*STATUS=}"
            if [ "$reported" = 1 ]; then
                :
            elif [ "$status" = 124 ] || [ "$status" = 137 ]; then
                echo "UML_CHROMIUM_ERROR: Aquarium did not report by guest" \\
                    "uptime ${deadline}s"
            else
                echo "UML_CHROMIUM_ERROR: Chromium exited with status" \\
                    "$status before Aquarium reported"
            fi
            ;;
        *UML_AQUARIUM_MEASURE_BEGIN*)
            emit_guest_snapshot BEGIN 0
            ;;
        *UML_AQUARIUM_SAMPLE*)
            sample_index=$(printf '%s\\n' "$line" |
                sed -n 's/.*|INDEX=\\([0-9][0-9]*\\)|.*/\\1/p')
            emit_guest_snapshot SAMPLE "${sample_index:-0}"
            ;;
        *UML_AQUARIUM_OK*)
            reported=1
            emit_guest_snapshot END 0
            if [ "@AUTO_POWEROFF@" = 1 ]; then
                echo 'UML_AQUARIUM_GUEST_POWEROFF|RESULT=pass'
                sync
                poweroff -f
            fi
            ;;
        *UML_AQUARIUM_ERROR*)
            reported=1
            emit_guest_snapshot ERROR 0
            if [ "@AUTO_POWEROFF@" = 1 ]; then
                echo 'UML_AQUARIUM_GUEST_POWEROFF|RESULT=error'
                sync
                poweroff -f
            fi
            ;;
        esac
    done
}
"""
SERVICE = """[Unit]
Description=Chromium WebGL Aquarium verification
After=display-manager.service network-online.target
Wants=display-manager.service network-online.target

[Service]
Type=simple
ExecStart=/usr/local/libexec/uml-chromium-aquarium
TasksMax=infinity
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=graphical.target
"""
VNC_PORT_LINE = re.compile(r"VNC display: [^:\r\n]+:(\d+) ")
SAMPLE_LINE = re.compile(
    r"UML_AQUARIUM_SAMPLE\|INDEX=([0-9]+)\|FPS=([0-9]+(?:\.[0-9]+)?)"
)
GUEST_SNAPSHOT_LINE = re.compile(
    r"UML_AQUARIUM_GUEST_(BEGIN|SAMPLE|END|ERROR)\|([^\r\n\"]+)"
)
NATIVE_DISPLAY_FINAL = re.compile(
    r"Native display final: guest ([0-9]+) presented ([0-9]+) "
    r"dropped ([0-9]+) cpu-fallback ([0-9]+)"
)


def announced_vnc_port(console: str) -> int:
    """Return the port atomically bound by the VNC server itself."""
    match = VNC_PORT_LINE.search(console)
    if match is None:
        raise RuntimeError("guest did not announce its dynamically bound VNC port")
    port = int(match.group(1))
    if port == 0:
        raise RuntimeError("guest VNC server reported an invalid dynamic port")
    return port


def install_text(path: str, content: str, mode: str = "0644") -> str:
    encoded = base64.b64encode(content.encode("utf-8")).decode("ascii")
    parent = str(Path(path).parent)
    commands = [f"install -d -m 0755 {parent}", f": > {path}"]
    for offset in range(0, len(encoded), CONSOLE_BASE64_CHUNK_CHARACTERS):
        chunk = encoded[offset : offset + CONSOLE_BASE64_CHUNK_CHARACTERS]
        commands.append(f"printf '%s' '{chunk}' | base64 -d >> {path}")
    commands.append(f"chmod {mode} {path}")
    return "\n".join(commands)


def build_probe_script(
    warmup_seconds: int = DEFAULT_WARMUP_SECONDS,
    sample_seconds: int = DEFAULT_SAMPLE_SECONDS,
) -> str:
    if warmup_seconds < 1 or sample_seconds < 1:
        raise ValueError("Aquarium windows must be positive")
    return (
        EXTENSION_SCRIPT.replace("@WARMUP_SECONDS@", str(warmup_seconds))
        .replace("@SAMPLE_SECONDS@", str(sample_seconds))
        .replace("@FISH_COUNT@", str(FISH_COUNT))
        .replace("@PAGE_MARGIN_SECONDS@", str(PAGE_MARGIN_SECONDS))
    )


class GuestBudget(NamedTuple):
    """Guest-uptime deadlines, in seconds, for each launcher phase."""

    deadline: int
    session_deadline: int
    compositor_deadline: int


def guest_budget(
    timeout: float,
    renderer_backend: str,
    warmup_seconds: int = DEFAULT_WARMUP_SECONDS,
    sample_seconds: int = DEFAULT_SAMPLE_SECONDS,
) -> GuestBudget:
    """Split the runner's deadline into the launcher's phase deadlines.

    Guest uptime starts with the emulator, so a deadline expressed in uptime
    also absorbs however long the boot took. Chromium keeps a reserve for its
    measurement, the Zink compositor probe comes before that, and the session
    wait gets what remains.
    """
    if renderer_backend not in ("virgl", "zink"):
        raise ValueError("unsupported Chromium renderer backend")
    deadline = math.floor(timeout) - GUEST_REPORT_MARGIN_SECONDS
    compositor_deadline = deadline - (
        warmup_seconds + sample_seconds + CHROMIUM_MINIMUM_SECONDS
    )
    session_deadline = compositor_deadline - (
        COMPOSITOR_PROBE_SECONDS if renderer_backend == "zink" else 0
    )
    if session_deadline < MINIMUM_SESSION_SECONDS:
        raise ValueError(
            f"a {timeout:g} s deadline leaves the boot and KDE session less "
            f"than {MINIMUM_SESSION_SECONDS} s"
        )
    return GuestBudget(deadline, session_deadline, compositor_deadline)


def build_guest_launcher(
    chromium_flags: list[str] | tuple[str, ...] = (),
    *,
    auto_poweroff: bool,
    budget: GuestBudget | None = None,
    egl_config_diagnostics: bool = False,
    renderer_backend: str = "virgl",
) -> str:
    if renderer_backend not in ("virgl", "zink"):
        raise ValueError("unsupported Chromium renderer backend")
    if budget is None:
        budget = guest_budget(DEFAULT_TIMEOUT_SECONDS, renderer_backend)
    return (
        LAUNCHER.replace(
            "@CHROMIUM_EXTRA_FLAGS@",
            " ".join(shlex.quote(flag) for flag in chromium_flags),
        )
        .replace("@AQUARIUM_URL@", shlex.quote(AQUARIUM_URL))
        .replace("@AUTO_POWEROFF@", "1" if auto_poweroff else "0")
        .replace(
            "@EGL_CONFIG_DIAGNOSTICS@",
            "1" if egl_config_diagnostics else "0",
        )
        .replace("@RENDERER_BACKEND@", renderer_backend)
        .replace("@GUEST_DEADLINE@", str(budget.deadline))
        .replace("@SESSION_DEADLINE@", str(budget.session_deadline))
        .replace("@COMPOSITOR_DEADLINE@", str(budget.compositor_deadline))
        .replace("@DBUS_CALL_SECONDS@", str(DBUS_CALL_SECONDS))
        .replace("@PAGE_MARGIN_SECONDS@", str(PAGE_MARGIN_SECONDS))
    )


def validate_renderer(renderer_line: str, backend: str) -> None:
    lowered = renderer_line.lower()
    if "webgl 2.0" not in lowered or any(
        name in lowered for name in ("llvmpipe", "softpipe", "swrast")
    ):
        raise RuntimeError(
            f"Chromium WebGL did not report hardware {backend}: "
            + renderer_line
        )
    # The guest Virgl renderer string embeds the host renderer, which is
    # itself Zink, so "zink" alone cannot identify the guest driver. Only the
    # guest Zink driver runs on the Virtio-GPU Venus device.
    on_venus = "virtio-gpu venus" in lowered
    if backend == "virgl":
        matched = "virgl" in lowered and not on_venus
    elif backend == "zink":
        matched = "zink" in lowered and on_venus
    else:
        raise ValueError("unsupported Chromium renderer backend")
    if not matched:
        raise RuntimeError(
            f"Chromium WebGL did not report {backend}: " + renderer_line
        )


def load_vulkan_rootfs_contract(
    receipt_path: Path,
) -> tuple[dict[str, object], cts.RootfsContract]:
    receipt = vk_receipt.validate_receipt(receipt_path)
    derivative = receipt["derivative"]
    guest_receipt = receipt["guest_receipt"]
    executable = receipt["cts"]["executable"]
    return receipt, cts.RootfsContract(
        filesystem_type=derivative["filesystem"],
        size=derivative["size"],
        sha256=derivative["sha256"],
        glcts_sha256=executable["sha256"],
        filesystem_uuid=derivative["filesystem_uuid"],
        guest_receipt_path=guest_receipt["path"],
        guest_receipt_sha256=guest_receipt["sha256"],
    )


def build_kernarium_setup() -> str:
    commands = (
        "for unit in docker.service docker.socket containerd.service "
        "packagekit.service apt-daily.service apt-daily.timer "
        "apt-daily-upgrade.service apt-daily-upgrade.timer; do "
        "ln -sf /dev/null \"/etc/systemd/system/$unit\"; done",
        install_text(
            "/var/tmp/uml-aquarium-extension/manifest.json",
            EXTENSION_MANIFEST,
        ),
        install_text(
            "/var/tmp/uml-aquarium-extension/verify.js",
            build_probe_script(),
        ),
        install_text(
            "/usr/local/libexec/uml-chromium-aquarium",
            build_guest_launcher(
                auto_poweroff=True,
                budget=guest_budget(KERNARIUM_TIMEOUT_SECONDS, "virgl"),
            ),
            "0755",
        ),
        install_text(
            "/etc/systemd/system/uml-chromium-aquarium.service",
            SERVICE,
        ),
        "install -d -m 0755 /etc/systemd/system/graphical.target.wants; "
        "ln -sf ../uml-chromium-aquarium.service "
        "/etc/systemd/system/graphical.target.wants/"
        "uml-chromium-aquarium.service; "
        "echo 'Kernarium Aquarium performance probe enabled'",
    )
    return "\n".join(commands) + "\n"


def _parse_process_cpu_time(value: str) -> float:
    days = 0
    clock = value
    if "-" in value:
        day_text, clock = value.split("-", 1)
        days = int(day_text)
    components = clock.split(":")
    if len(components) == 2:
        hours = 0
        minutes, seconds = components
    elif len(components) == 3:
        hours, minutes, seconds = components
    else:
        raise ValueError(f"invalid process CPU time: {value}")
    return (
        days * 86400
        + int(hours) * 3600
        + int(minutes) * 60
        + float(seconds)
    )


def process_tree_snapshot(root_pid: int) -> dict[str, object]:
    result = subprocess.run(
        ["ps", "-axo", "pid=,ppid=,rss=,time="],
        check=True,
        capture_output=True,
        text=True,
    )
    table: dict[int, tuple[int, int, float]] = {}
    for line in result.stdout.splitlines():
        columns = line.split()
        if len(columns) != 4:
            continue
        try:
            pid, parent, rss_kib = (int(value) for value in columns[:3])
            cpu_seconds = _parse_process_cpu_time(columns[3])
        except ValueError:
            continue
        table[pid] = (parent, rss_kib, cpu_seconds)
    selected = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, (parent, _, _) in table.items():
            if pid not in selected and parent in selected:
                selected.add(pid)
                changed = True
    live = selected & table.keys()
    if not live:
        raise RuntimeError(f"cannot sample host process tree {root_pid}")
    return {
        "pids": sorted(live),
        "cpu_seconds": sum(table[pid][2] for pid in live),
        "rss_kib": sum(table[pid][1] for pid in live),
        "monotonic_seconds": time.monotonic(),
    }


def summarize_host_measurement(
    begin: dict[str, object],
    end: dict[str, object],
    peak_rss_kib: int,
    snapshots: list[dict[str, object]],
) -> dict[str, object]:
    cpu_seconds = float(end["cpu_seconds"]) - float(begin["cpu_seconds"])
    wall_seconds = (
        float(end["monotonic_seconds"])
        - float(begin["monotonic_seconds"])
    )
    if cpu_seconds < 0 or wall_seconds <= 0:
        raise RuntimeError("host process accounting moved backwards")
    return {
        "active_cpu_seconds": cpu_seconds,
        "wall_seconds": wall_seconds,
        "average_cpu_cores": cpu_seconds / wall_seconds,
        "begin_rss_kib": int(begin["rss_kib"]),
        "end_rss_kib": int(end["rss_kib"]),
        "peak_rss_kib": peak_rss_kib,
        "begin_pids": begin["pids"],
        "end_pids": end["pids"],
        "snapshots": snapshots,
    }


GUEST_RESULT_MARKERS = (
    "UML_AQUARIUM_OK|",
    "UML_AQUARIUM_ERROR|",
    "UML_CHROMIUM_ERROR:",
)


def guest_failure(text: str) -> str:
    """Return the guest's first named failure, without console decoration."""
    for line in text.splitlines():
        for marker in GUEST_RESULT_MARKERS[1:]:
            index = line.find(marker)
            if index >= 0:
                return line[index:].split('", source:', 1)[0].strip()
    return "the guest reported no failure"


def describe_missing_report(
    text: str, timeout: float, budget: GuestBudget
) -> str:
    """Explain a run whose guest named no result by the runner's deadline."""
    if "UML_AQUARIUM_LAUNCHER|" not in text:
        return (
            f"the guest did not start the Aquarium launcher within {timeout:g} s"
        )
    return (
        "the Aquarium launcher started but reported nothing by its guest "
        f"uptime deadline of {budget.deadline} s or the runner's {timeout:g} s "
        "deadline; the guest itself is unresponsive"
    )


def wait_for_log(
    process: subprocess.Popen[bytes],
    log_path: Path,
    markers: tuple[str, ...],
    timeout: float,
    *,
    host_process_pid: int | None = None,
) -> tuple[str, dict[str, object] | None]:
    deadline = time.monotonic() + timeout
    host_begin: dict[str, object] | None = None
    host_end: dict[str, object] | None = None
    host_snapshots: list[dict[str, object]] = []
    peak_rss_kib = 0
    while time.monotonic() < deadline:
        text = log_path.read_text(encoding="utf-8", errors="replace")
        if (
            host_process_pid is not None
            and "UML_AQUARIUM_MEASURE_BEGIN|" in text
            and "UML_AQUARIUM_OK|" not in text
            and "UML_AQUARIUM_ERROR|" not in text
        ):
            snapshot = process_tree_snapshot(host_process_pid)
            if host_begin is None:
                host_begin = snapshot
            host_snapshots.append(snapshot)
            peak_rss_kib = max(peak_rss_kib, int(snapshot["rss_kib"]))
        if any(marker in text for marker in markers):
            if host_process_pid is not None and host_begin is not None:
                host_end = process_tree_snapshot(host_process_pid)
                host_snapshots.append(host_end)
                peak_rss_kib = max(
                    peak_rss_kib, int(host_end["rss_kib"])
                )
            host = (
                summarize_host_measurement(
                    host_begin, host_end, peak_rss_kib, host_snapshots
                )
                if host_begin is not None and host_end is not None
                else None
            )
            return text, host
        if process.poll() is not None:
            raise RuntimeError(
                f"guest exited before Chromium verification (status "
                f"{process.returncode})"
            )
        time.sleep(0.5)
    raise TimeoutError("Chromium WebGL verification did not finish in time")


def start_guest(
    command: Sequence[str], log: BinaryIO, **options: Any
) -> subprocess.Popen[bytes]:
    """Start the guest harness as the leader of its own session.

    stop_guest signals the harness's process group, and a terminal interrupt
    then reaches only the runner, which stops the guest itself.
    """
    return subprocess.Popen(
        command,
        stdout=log,
        stderr=subprocess.STDOUT,
        start_new_session=True,
        **options,
    )


def stop_guest(process: subprocess.Popen[bytes]) -> None:
    """Stop the guest's isolated process group, escalating when necessary.

    The emulator can ignore a graceful signal while a guest is wedged. Keep
    escalation bounded so an interrupted validation cannot leave it consuming
    host resources. Only a reaped harness counts as stopped.
    """
    for guest_signal, timeout in (
        (signal.SIGINT, 45),
        (signal.SIGTERM, 15),
        (signal.SIGKILL, 15),
    ):
        if process.poll() is not None:
            return
        try:
            os.killpg(process.pid, guest_signal)
        except ProcessLookupError:
            # No group has the harness's ID, so the harness leads none. That
            # is no evidence it exited: signal the harness itself.
            process.send_signal(guest_signal)
        try:
            process.wait(timeout=timeout)
            return
        except subprocess.TimeoutExpired:
            continue
    raise RuntimeError(
        f"guest harness {process.pid} did not stop after SIGKILL"
    )


def read_ppm(capture_path: Path) -> tuple[int, int, memoryview]:
    data = capture_path.read_bytes()
    header = re.match(br"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise RuntimeError("VNC capture is not an 8-bit binary PPM image")
    width, height = (int(value) for value in header.groups())
    pixels = memoryview(data)[header.end() :]
    if len(pixels) != width * height * 3:
        raise RuntimeError("VNC capture has an invalid PPM payload size")
    return width, height, pixels


def raster_marker_visible(capture_path: Path) -> bool:
    width, height, pixels = read_ppm(capture_path)
    green_pixels = 0
    for y in range(80, min(height, 180)):
        row = y * width * 3
        for x in range(max(0, width - 280), width):
            offset = row + x * 3
            red, green, blue = pixels[offset : offset + 3]
            if red <= 80 and green >= 180 and blue <= 80:
                green_pixels += 1
    return green_pixels >= 50


def aquarium_scene_visible(capture_path: Path) -> bool:
    width, height, pixels = read_ppm(capture_path)
    x_begin = min(80, width)
    x_end = max(x_begin, min(width - 320, 1000))
    y_begin = min(140, height)
    y_end = max(y_begin, height - 40)
    quantized_colors: set[int] = set()
    colorful = 0
    samples = 0
    minimum_luminance = 3 * 255
    maximum_luminance = 0
    for y in range(y_begin, y_end, 4):
        row = y * width * 3
        for x in range(x_begin, x_end, 4):
            offset = row + x * 3
            red, green, blue = pixels[offset : offset + 3]
            quantized_colors.add(
                (red >> 4) << 8 | (green >> 4) << 4 | (blue >> 4)
            )
            luminance = red + green + blue
            minimum_luminance = min(minimum_luminance, luminance)
            maximum_luminance = max(maximum_luminance, luminance)
            colorful += max(red, green, blue) - min(red, green, blue) > 40
            samples += 1
    return (
        samples >= 1000
        and len(quantized_colors) >= 64
        and maximum_luminance - minimum_luminance >= 250
        and colorful >= samples // 10
    )


def parse_metrics(line: str) -> dict[str, object]:
    marker = line.find("UML_AQUARIUM_OK|")
    if marker < 0:
        raise RuntimeError("Aquarium success marker is missing")
    payload = line[marker:].split('", source:', 1)[0]
    fields: dict[str, object] = {}
    for field in payload.split("|")[1:]:
        key, separator, value = field.partition("=")
        if not separator:
            continue
        fields[key] = value.strip()
    for key in (
        "AVG_FPS",
        "MIN_FPS",
        "MAX_FPS",
        "SHADER_COMPILE_CPU_MS",
        "PROGRAM_LINK_CPU_MS",
    ):
        try:
            fields[key] = float(str(fields[key]))
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"Aquarium metric {key} is invalid") from error
        if not math.isfinite(float(fields[key])):
            raise RuntimeError(f"Aquarium metric {key} is not finite")
    for key in (
        "SAMPLES",
        "WARMUP_SECONDS",
        "RESOURCES",
        "FISH",
        "SHADER_COMPILES",
        "PROGRAM_LINKS",
        "INSTRUMENTED_METHODS",
        "INSTRUMENTATION_ERRORS",
    ):
        try:
            fields[key] = int(str(fields[key]))
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"Aquarium metric {key} is invalid") from error
    try:
        raw_fps = [float(value) for value in str(fields["RAW_FPS"]).split(",")]
    except (KeyError, ValueError) as error:
        raise RuntimeError("Aquarium raw FPS samples are invalid") from error
    if (
        len(raw_fps) != fields["SAMPLES"]
        or any(not math.isfinite(value) or value <= 0 for value in raw_fps)
    ):
        raise RuntimeError("Aquarium raw FPS sample window is invalid")
    average = sum(raw_fps) / len(raw_fps)
    if not math.isclose(average, float(fields["AVG_FPS"]), abs_tol=0.001):
        raise RuntimeError("Aquarium aggregate does not match its raw samples")
    if not math.isclose(
        min(raw_fps), float(fields["MIN_FPS"]), abs_tol=0.001
    ) or not math.isclose(
        max(raw_fps), float(fields["MAX_FPS"]), abs_tol=0.001
    ):
        raise RuntimeError("Aquarium extrema do not match its raw samples")
    if (
        float(fields["SHADER_COMPILE_CPU_MS"]) < 0
        or float(fields["PROGRAM_LINK_CPU_MS"]) < 0
        or int(fields["SHADER_COMPILES"]) < 0
        or int(fields["PROGRAM_LINKS"]) < 0
        or int(fields["INSTRUMENTED_METHODS"]) < 1
        or int(fields["INSTRUMENTATION_ERRORS"]) != 0
    ):
        raise RuntimeError("Aquarium shader instrumentation is invalid")
    frame_times = sorted(1000.0 / value for value in raw_fps)
    percentile_index = max(0, math.ceil(0.95 * len(frame_times)) - 1)
    fields["RAW_FPS"] = raw_fps
    fields["MEDIAN_FPS"] = statistics.median(raw_fps)
    fields["P95_FRAME_TIME_MS"] = frame_times[percentile_index]
    return fields


def parse_sample_lines(transcript: str) -> list[float]:
    matches = SAMPLE_LINE.findall(transcript)
    indexes = [int(index) for index, _ in matches]
    if indexes != list(range(1, len(matches) + 1)):
        raise RuntimeError("Aquarium sample markers are missing or out of order")
    samples = [float(value) for _, value in matches]
    if any(not math.isfinite(value) or value <= 0 for value in samples):
        raise RuntimeError("Aquarium sample marker is invalid")
    return samples


def _marker_fields(payload: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for field in payload.split("|"):
        key, separator, value = field.partition("=")
        if separator and key:
            fields[key] = value.strip()
    return fields


def parse_guest_snapshots(
    transcript: str,
    expected_samples: int,
) -> dict[str, object]:
    rows: list[dict[str, object]] = []
    for phase, payload in GUEST_SNAPSHOT_LINE.findall(transcript):
        values = _marker_fields(payload)
        try:
            cpu_jiffies = [
                int(value) for value in values["CPU_JIFFIES"].split(",")
            ]
            row: dict[str, object] = {
                "phase": phase.lower(),
                "index": int(values["INDEX"]),
                "uptime_seconds": float(values["UPTIME"]),
                "clock_ticks_per_second": int(values["CLK_TCK"]),
                "cpu_jiffies": cpu_jiffies,
                "memory_available_kib": int(values["MEM_AVAILABLE_KIB"]),
                "memory_free_kib": int(values["MEM_FREE_KIB"]),
                "committed_as_kib": int(values["COMMITTED_AS_KIB"]),
            }
        except (KeyError, ValueError) as error:
            raise RuntimeError("Aquarium guest snapshot is malformed") from error
        if len(cpu_jiffies) < 8 or any(value < 0 for value in cpu_jiffies):
            raise RuntimeError("Aquarium guest CPU snapshot is malformed")
        rows.append(row)
    phases = [row["phase"] for row in rows]
    if phases != ["begin"] + ["sample"] * expected_samples + ["end"]:
        raise RuntimeError("Aquarium guest snapshot window is incomplete")
    sample_indexes = [
        row["index"] for row in rows if row["phase"] == "sample"
    ]
    if sample_indexes != list(range(1, expected_samples + 1)):
        raise RuntimeError("Aquarium guest snapshots are out of order")
    begin = rows[0]
    end = rows[-1]
    begin_cpu = begin["cpu_jiffies"]
    end_cpu = end["cpu_jiffies"]
    assert isinstance(begin_cpu, list) and isinstance(end_cpu, list)
    if len(begin_cpu) != len(end_cpu):
        raise RuntimeError("Aquarium guest CPU shape changed during measurement")
    deltas = [after - before for before, after in zip(begin_cpu, end_cpu)]
    if any(value < 0 for value in deltas):
        raise RuntimeError("Aquarium guest CPU counters moved backwards")
    clock_ticks = int(begin["clock_ticks_per_second"])
    if clock_ticks <= 0 or end["clock_ticks_per_second"] != clock_ticks:
        raise RuntimeError("Aquarium guest clock-tick identity changed")
    # Linux's guest and guest_nice columns are already included in user and
    # nice, so only the first eight fields contribute to aggregate CPU time.
    total_jiffies = sum(deltas[:8])
    idle_jiffies = deltas[3] + deltas[4]
    active_jiffies = total_jiffies - idle_jiffies
    wall_seconds = float(end["uptime_seconds"]) - float(
        begin["uptime_seconds"]
    )
    if wall_seconds <= 0:
        raise RuntimeError("Aquarium guest uptime did not advance")
    return {
        "snapshots": rows,
        "active_cpu_seconds": active_jiffies / clock_ticks,
        "total_cpu_seconds": total_jiffies / clock_ticks,
        "wall_seconds": wall_seconds,
        "average_cpu_cores": active_jiffies / clock_ticks / wall_seconds,
        "minimum_available_memory_kib": min(
            int(row["memory_available_kib"]) for row in rows
        ),
        "maximum_committed_as_kib": max(
            int(row["committed_as_kib"]) for row in rows
        ),
    }


def parse_guest_identity(transcript: str) -> dict[str, int]:
    marker = re.search(
        r"UML_AQUARIUM_GUEST_IDENTITY\|PAGE_SIZE=([0-9]+)"
        r"\|CPUS=([0-9]+)\|MEM_TOTAL_KIB=([0-9]+)",
        transcript,
    )
    if marker is None:
        raise RuntimeError("Aquarium guest identity is missing")
    page_size, cpus, memory_kib = (int(value) for value in marker.groups())
    if page_size not in (4096, 16384) or cpus < 1 or memory_kib < 1:
        raise RuntimeError("Aquarium guest identity is invalid")
    return {
        "page_size": page_size,
        "cpus": cpus,
        "memory_total_kib": memory_kib,
    }


def parse_native_display_final(transcript: str) -> dict[str, int]:
    matches = NATIVE_DISPLAY_FINAL.findall(transcript)
    if len(matches) != 1:
        raise RuntimeError("Aquarium native-display final report is missing")
    guest, presented, dropped, cpu_fallback = (
        int(value) for value in matches[0]
    )
    if guest <= 0 or presented <= 0 or cpu_fallback != 0:
        raise RuntimeError("Aquarium did not retain direct native presentation")
    return {
        "guest_frames": guest,
        "presented_frames": presented,
        "dropped_frames": dropped,
        "cpu_fallback_frames": cpu_fallback,
    }


def validate_native_shutdown(transcript: str) -> None:
    if "Memory manager not clean during takedown." in transcript:
        raise RuntimeError(
            "Virtio-GPU memory manager was not clean during guest shutdown"
        )
    missing = [
        marker
        for marker in (
            "UML_AQUARIUM_GUEST_POWEROFF|RESULT=pass",
            "Darwin UML stopped: result=0 exitcode=0",
        )
        if marker not in transcript
    ]
    if missing:
        raise RuntimeError(
            f"Aquarium guest omitted clean shutdown markers: {missing!r}"
        )


def git_state() -> dict[str, object]:
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    dirty_paths = subprocess.run(
        ["git", "status", "--short"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    return {"revision": revision, "dirty_paths": dirty_paths}


def rootfs_contract_metadata(
    contract: cts.RootfsContract,
) -> dict[str, object]:
    return {
        "filesystem_type": contract.filesystem_type,
        "size": contract.size,
        "sha256": contract.sha256,
        "filesystem_uuid": contract.filesystem_uuid,
        "guest_receipt_path": contract.guest_receipt_path,
        "guest_receipt_sha256": contract.guest_receipt_sha256,
    }


def build_host_environment(display: str) -> dict[str, str]:
    environment = os.environ.copy()
    if display == "native":
        environment["UML_GPU_FRAME_STATS"] = "1"
    return environment


def main() -> int:
    identity = cts.load_identity()
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--client",
        type=Path,
        default=REPO_ROOT / ".build/presets/macos-integration/bin/uml-vnc-smoke-client",
    )
    parser.add_argument(
        "--artifact-root",
        type=Path,
        default=REPO_ROOT / ".build/chromium-webgl",
    )
    parser.add_argument(
        "--emulator",
        type=Path,
        default=REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin",
    )
    kernarium_common.add_rootfs_arguments(parser)
    parser.add_argument("--display", choices=("native", "vnc"), default="vnc")
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--dpi", type=int, default=160)
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets"),
        default="jit",
    )
    parser.add_argument(
        "--renderer",
        choices=("virgl", "zink"),
        default="virgl",
        help="guest Gallium renderer to require (default: virgl)",
    )
    parser.add_argument(
        "--serial-socket",
        type=Path,
        help="expose the guest serial console for live diagnostics",
    )
    parser.add_argument(
        "--warmup-seconds", type=int, default=DEFAULT_WARMUP_SECONDS
    )
    parser.add_argument(
        "--sample-seconds", type=int, default=DEFAULT_SAMPLE_SECONDS
    )
    parser.add_argument(
        "--chromium-flag",
        action="append",
        default=[],
        help="append a Chromium flag for diagnostic comparison",
    )
    parser.add_argument(
        "--ignore-image-chromium-config",
        action="store_true",
        help="remove the image's Chromium compatibility config in the overlay",
    )
    parser.add_argument(
        "--egl-config-diagnostics",
        action="store_true",
        help=(
            "capture the guest EGL X11 configs and X visual inventory before "
            "launching Chromium"
        ),
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=DEFAULT_TIMEOUT_SECONDS,
        help=(
            "overall deadline in seconds from emulator start; every guest "
            f"wait is derived from it (default: {DEFAULT_TIMEOUT_SECONDS})"
        ),
    )
    options = parser.parse_args()
    receipt_kind = None
    if options.rootfs_receipt is not None:
        try:
            receipt_kind = json.loads(
                options.rootfs_receipt.read_text(encoding="utf-8")
            ).get("artifact")
        except (OSError, json.JSONDecodeError, AttributeError):
            pass
    if receipt_kind == "uml-vulkan-rootfs":
        try:
            options.derivative_receipt, options.rootfs_contract = (
                load_vulkan_rootfs_contract(options.rootfs_receipt.resolve())
            )
        except vk_receipt.ReceiptError as error:
            parser.error(str(error))
        if options.rootfs_type != "ext4":
            parser.error("a Vulkan derivative receipt requires an ext4 rootfs")
    else:
        kernarium_common.finish_rootfs_options(parser, options, identity)
    if (
        options.cpus < 1
        or options.width < 1
        or options.height < 1
        or options.dpi < 1
    ):
        parser.error("--cpus, --width, --height, and --dpi must be positive")
    if options.warmup_seconds < 1:
        parser.error("--warmup-seconds must be positive")
    if options.sample_seconds < 1:
        parser.error("--sample-seconds must be positive")
    try:
        budget = guest_budget(
            options.timeout,
            options.renderer,
            options.warmup_seconds,
            options.sample_seconds,
        )
    except ValueError as error:
        parser.error(f"--timeout: {error}")

    rootfs = options.rootfs.resolve()
    kernarium_common.validate_rootfs(
        rootfs, identity, options.rootfs_contract
    )

    port = 0
    password = "webgl"
    launcher_arguments = [
        "--display",
        options.display,
        "--emulator",
        str(options.emulator),
        "--rootfs",
        str(rootfs),
        "--rootfs-type",
        options.rootfs_contract.filesystem_type,
        # Every image this runner accepts is the unified KDE image or an ext4
        # derivative sealed from it, never a starter disk.
        "--image-profile",
        "legacy",
        "--memory",
        options.memory,
        "--cpus",
        str(options.cpus),
        "--engine",
        options.engine,
        "--width",
        str(options.width),
        "--height",
        str(options.height),
        "--dpi",
        str(options.dpi),
    ]
    if options.display == "vnc":
        # The display binds port zero itself and writes the chosen port to the
        # guest console. This avoids the bind/release race of a host probe.
        launcher_arguments.extend(
            ("--port", str(port), "--password", password)
        )
    if options.serial_socket is not None:
        launcher_arguments.extend(
            ("--serial-socket", str(options.serial_socket.resolve()))
        )
    launcher_options = boot_kde_vnc.parse_arguments(launcher_arguments)
    command = boot_kde_vnc.build_harness_command(launcher_options)
    separator = boot_kde_vnc.guest_argument_separator(command)
    overlay_commands = (
        "for unit in docker.service docker.socket containerd.service "
        "packagekit.service apt-daily.service apt-daily.timer "
        "apt-daily-upgrade.service apt-daily-upgrade.timer; do "
        "ln -sf /dev/null /etc/systemd/system/$unit; done",
        *(
            (
                "systemctl set-environment "
                "MESA_LOADER_DRIVER_OVERRIDE=zink",
            )
            if options.renderer == "zink"
            else ()
        ),
        *(
            ("rm -f /etc/chromium.d/uml-virgl",)
            if options.ignore_image_chromium_config
            else ()
        ),
        install_text(
            "/var/tmp/uml-aquarium-extension/manifest.json",
            EXTENSION_MANIFEST,
        ),
        install_text(
            "/var/tmp/uml-aquarium-extension/verify.js",
            build_probe_script(
                options.warmup_seconds, options.sample_seconds
            ),
        ),
        install_text(
            "/usr/local/libexec/uml-chromium-aquarium",
            build_guest_launcher(
                options.chromium_flag,
                auto_poweroff=options.display == "native",
                budget=budget,
                egl_config_diagnostics=options.egl_config_diagnostics,
                renderer_backend=options.renderer,
            ),
            "0755",
        ),
        install_text(
            "/etc/systemd/system/uml-chromium-aquarium.service",
            SERVICE,
        ),
        "install -d -m 0755 /etc/systemd/system/graphical.target.wants; "
        "ln -sf ../uml-chromium-aquarium.service "
        "/etc/systemd/system/graphical.target.wants/"
        "uml-chromium-aquarium.service; "
        "systemctl start --no-block uml-chromium-aquarium.service",
    )
    additions: list[str] = []
    for overlay_command in overlay_commands:
        additions.extend(("--command", overlay_command))
    command[separator:separator] = additions

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-{options.page_profile}-{options.display}"
    )
    artifact.mkdir(parents=True, exist_ok=False)
    log_path = artifact / "console.log"
    capture_path = artifact / "aquarium.ppm"
    run_metadata: dict[str, object] = {
        "schema_version": 2,
        "adapter": "macos",
        "workload": "webgl-aquarium-500-v1",
        "git": git_state(),
        "rootfs": str(rootfs),
        "rootfs_receipt": (
            str(options.rootfs_receipt.resolve())
            if options.rootfs_receipt is not None
            else None
        ),
        "rootfs_contract": rootfs_contract_metadata(
            options.rootfs_contract
        ),
        "display": options.display,
        "page_profile": options.page_profile,
        "engine": options.engine,
        "renderer_backend": options.renderer,
        "serial_socket": (
            str(options.serial_socket.resolve())
            if options.serial_socket is not None
            else None
        ),
        "cpus": options.cpus,
        "memory": options.memory,
        "width": options.width,
        "height": options.height,
        "dpi": options.dpi,
        "fish": FISH_COUNT,
        "warmup_seconds": options.warmup_seconds,
        "sample_seconds": options.sample_seconds,
        "egl_config_diagnostics": options.egl_config_diagnostics,
        "timeout_seconds": options.timeout,
        "guest_budget": budget._asdict(),
        "url": AQUARIUM_URL,
        "command": command,
    }
    (artifact / "run.json").write_text(
        json.dumps(run_metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    environment = build_host_environment(options.display)
    with log_path.open("wb") as log:
        process = start_guest(command, log, env=environment)
        try:
            try:
                text, host_measurement = wait_for_log(
                    process,
                    log_path,
                    GUEST_RESULT_MARKERS,
                    options.timeout,
                    host_process_pid=process.pid,
                )
            except TimeoutError as error:
                raise TimeoutError(
                    describe_missing_report(
                        log_path.read_text(encoding="utf-8", errors="replace"),
                        options.timeout,
                        budget,
                    )
                ) from error
            if "UML_AQUARIUM_OK|" not in text:
                raise RuntimeError(
                    "Aquarium did not render with WebGL: "
                    + guest_failure(text)
                )
            renderer_line = next(
                line for line in text.splitlines() if "UML_AQUARIUM_OK|" in line
            )
            validate_renderer(renderer_line, options.renderer)
            if options.renderer == "zink" and "UML_KDE_ZINK_READY|" not in text:
                raise RuntimeError("KDE did not report a Zink compositor")
            metrics = parse_metrics(renderer_line)
            if metrics["SAMPLES"] != options.sample_seconds:
                raise RuntimeError("Aquarium returned an incomplete FPS window")
            if metrics["WARMUP_SECONDS"] != options.warmup_seconds:
                raise RuntimeError("Aquarium used the wrong warm-up window")
            if metrics["FISH"] != FISH_COUNT:
                raise RuntimeError("Aquarium used the wrong fish count")
            guest_identity = parse_guest_identity(text)
            expected_page_size = (
                16384 if options.page_profile == "16k" else 4096
            )
            if (
                guest_identity["page_size"] != expected_page_size
                or guest_identity["cpus"] != options.cpus
            ):
                raise RuntimeError("Aquarium guest used a different configuration")
            native_display: dict[str, int] | None = None
            if options.display == "native":
                process.wait(timeout=120)
                text = log_path.read_text(encoding="utf-8", errors="replace")
                if process.returncode != 0:
                    raise RuntimeError(
                        f"Aquarium guest exited with status {process.returncode}"
                    )
                validate_native_shutdown(text)
                native_display = parse_native_display_final(text)
            else:
                port = announced_vnc_port(text)
                time.sleep(2)
                subprocess.run(
                    [
                        str(options.client.resolve()),
                        "--password",
                        password,
                        "--capture",
                        str(capture_path),
                        str(port),
                    ],
                    check=True,
                    timeout=120,
                )
                if not raster_marker_visible(capture_path):
                    raise RuntimeError(
                        "Chromium rendered WebGL but lost the 2D raster text marker"
                    )
                if not aquarium_scene_visible(capture_path):
                    raise RuntimeError(
                        "Aquarium VNC capture lacks a complete, varied 3D scene"
                    )
                text = log_path.read_text(encoding="utf-8", errors="replace")
            samples = parse_sample_lines(text)
            if samples != metrics["RAW_FPS"]:
                raise RuntimeError("Aquarium sample stream differs from summary")
            guest_measurement = parse_guest_snapshots(
                text, options.sample_seconds
            )
            if host_measurement is None:
                raise RuntimeError("Aquarium host measurement is missing")
            metrics.update(
                {
                    "engine": options.engine,
                    "cpus": options.cpus,
                    "memory": options.memory,
                    "url": AQUARIUM_URL,
                    "capture": (
                        str(capture_path.resolve())
                        if options.display == "vnc"
                        else None
                    ),
                    "host_measurement": host_measurement,
                    "guest_identity": guest_identity,
                    "guest_measurement": guest_measurement,
                    "native_display_final": native_display,
                }
            )
            (artifact / "metrics.json").write_text(
                json.dumps(metrics, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(renderer_line)
            print(
                "Aquarium steady-state average: "
                f"{metrics['AVG_FPS']:.3f} FPS "
                f"({metrics['SAMPLES']} samples)"
            )
            if options.display == "vnc":
                print(f"Aquarium capture: {capture_path}")
            run_metadata.update(
                {
                    "emulator_return_code": process.returncode,
                    "metrics": metrics,
                    "clean_shutdown": options.display == "native",
                }
            )
            (artifact / "run.json").write_text(
                json.dumps(run_metadata, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(f"Artifacts: {artifact}")
        finally:
            stop_guest(process)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        print(f"run_chromium_webgl.py: {error}", file=sys.stderr)
        raise SystemExit(1)
