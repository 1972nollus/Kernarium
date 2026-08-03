#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot KDE, launch Chromium's WebGL Aquarium, and capture it over VNC."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import json
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import sys
import time


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import boot_kde_vnc  # noqa: E402


AQUARIUM_URL = (
    "https://webglsamples.org/aquarium/aquarium.html?numFish=500"
)
EXTENSION_MANIFEST = """{
  "manifest_version": 3,
  "name": "UML Aquarium verifier",
  "version": "1.0",
  "content_scripts": [{
    "matches": ["https://webglsamples.org/aquarium/*"],
    "js": ["verify.js"],
    "run_at": "document_start"
  }]
}
"""
EXTENSION_SCRIPT = """(() => {
  const warmupSeconds = @WARMUP_SECONDS@;
  const sampleSeconds = @SAMPLE_SECONDS@;
  const resourceErrors = [];
  let lastResourceError = "";
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
    if (document.readyState !== "complete") {
      await new Promise((resolve) =>
        addEventListener("load", resolve, {once: true}));
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
    const readinessDeadline = Date.now() + 120000;
    while (Date.now() < readinessDeadline) {
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
      throw new Error("page resources or rendering did not stabilize");
    }

    const canvas = document.getElementById("canvas");
    const gl = canvas?.getContext("webgl2") || canvas?.getContext("webgl");
    if (!gl || gl.isContextLost()) {
      throw new Error("WebGL context unavailable or lost");
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
      + "|FISH=500"
      + "|CANVAS=" + canvas.width + "x" + canvas.height
      + "|VERSION=" + gl.getParameter(gl.VERSION)
      + "|RENDERER=" + renderer);
  };

  run().catch((error) => fail(error?.stack || error));
})();
"""
LAUNCHER = """#!/bin/sh
set -eu
runtime=/run/user/1000
remaining=600
while [ ! -S /tmp/.X11-unix/X0 ] || [ ! -S "$runtime/bus" ]; do
    if [ "$remaining" -le 0 ]; then
        echo "UML_CHROMIUM_ERROR: KDE X11 bridge did not become ready"
        exit 1
    fi
    remaining=$((remaining - 1))
    sleep 1
done
xauthority="$(find "$runtime" -maxdepth 1 -type f -name 'xauth_*' -print |
    head -n 1)"
if [ -z "$xauthority" ]; then
    echo "UML_CHROMIUM_ERROR: Xauthority cookie was not found"
    exit 1
fi
runuser -u plasma -- env \
    DISPLAY=:0 \
    XAUTHORITY="$xauthority" \
    XDG_RUNTIME_DIR="$runtime" \
    glxinfo -B || true
exec runuser -u plasma -- env \
    HOME=/home/plasma \
    DISPLAY=:0 \
    WAYLAND_DISPLAY=wayland-0 \
    XAUTHORITY="$xauthority" \
    XDG_RUNTIME_DIR="$runtime" \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
    chromium \
    @CHROMIUM_EXTRA_FLAGS@ \
    --no-first-run \
    --disable-crash-reporter \
    --enable-logging=stderr \
    --start-maximized \
    --user-data-dir=/tmp/chromium-aquarium \
    --disable-extensions-except=/var/tmp/uml-aquarium-extension \
    --load-extension=/var/tmp/uml-aquarium-extension \
    @AQUARIUM_URL@
"""
SERVICE = """[Unit]
Description=Chromium WebGL Aquarium verification
After=display-manager.service
Wants=display-manager.service

[Service]
Type=simple
ExecStart=/usr/local/libexec/uml-chromium-aquarium
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=graphical.target
"""
VNC_PORT_LINE = re.compile(r"VNC display: [^:\r\n]+:(\d+) ")


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
    return (
        f"install -d -m 0755 {parent}; "
        f"printf '%s' '{encoded}' | base64 -d > {path}; "
        f"chmod {mode} {path}"
    )


def wait_for_log(
    process: subprocess.Popen[bytes],
    log_path: Path,
    markers: tuple[str, ...],
    timeout: float,
) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = log_path.read_text(encoding="utf-8", errors="replace")
        if any(marker in text for marker in markers):
            return text
        if process.poll() is not None:
            raise RuntimeError(
                f"guest exited before Chromium verification (status "
                f"{process.returncode})"
            )
        time.sleep(0.5)
    raise TimeoutError("Chromium WebGL verification did not finish in time")


def stop_guest(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    os.killpg(process.pid, signal.SIGINT)
    try:
        process.wait(timeout=45)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=15)


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
    for key in ("AVG_FPS", "MIN_FPS", "MAX_FPS"):
        try:
            fields[key] = float(str(fields[key]))
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"Aquarium metric {key} is invalid") from error
    for key in ("SAMPLES", "WARMUP_SECONDS", "RESOURCES", "FISH"):
        try:
            fields[key] = int(str(fields[key]))
        except (KeyError, ValueError) as error:
            raise RuntimeError(f"Aquarium metric {key} is invalid") from error
    return fields


def main() -> int:
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
    parser.add_argument(
        "--rootfs",
        type=Path,
        default=REPO_ROOT / "rootfs.img",
    )
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets", "interpreter"),
        default="jit",
    )
    parser.add_argument("--warmup-seconds", type=int, default=15)
    parser.add_argument("--sample-seconds", type=int, default=30)
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
    parser.add_argument("--timeout", type=float, default=360)
    options = parser.parse_args()
    if options.cpus < 1:
        parser.error("--cpus must be positive")
    if options.warmup_seconds < 1:
        parser.error("--warmup-seconds must be positive")
    if options.sample_seconds < 1:
        parser.error("--sample-seconds must be positive")

    # The display binds port zero itself and writes the chosen port to the
    # guest console.  This avoids the bind/release race of a host probe socket.
    port = 0
    password = "webgl"
    launcher_options = boot_kde_vnc.parse_arguments(
        [
            "--port",
            str(port),
            "--password",
            password,
            "--emulator",
            str(options.emulator),
            "--rootfs",
            str(options.rootfs),
            "--memory",
            options.memory,
            "--cpus",
            str(options.cpus),
            "--engine",
            options.engine,
        ]
    )
    command = boot_kde_vnc.build_harness_command(launcher_options)
    separator = command.index("--")
    overlay_commands = (
        "for unit in docker.service docker.socket containerd.service "
        "packagekit.service apt-daily.service apt-daily.timer "
        "apt-daily-upgrade.service apt-daily-upgrade.timer; do "
        "ln -sf /dev/null /etc/systemd/system/$unit; done",
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
            EXTENSION_SCRIPT.replace(
                "@WARMUP_SECONDS@", str(options.warmup_seconds)
            ).replace("@SAMPLE_SECONDS@", str(options.sample_seconds)),
        ),
        install_text(
            "/usr/local/libexec/uml-chromium-aquarium",
            LAUNCHER.replace(
                "@CHROMIUM_EXTRA_FLAGS@",
                " ".join(shlex.quote(flag) for flag in options.chromium_flag),
            ).replace("@AQUARIUM_URL@", AQUARIUM_URL),
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

    artifact = options.artifact_root / time.strftime("%Y%m%d-%H%M%S")
    artifact.mkdir(parents=True)
    log_path = artifact / "console.log"
    capture_path = artifact / "aquarium.ppm"
    with log_path.open("wb") as log:
        process = subprocess.Popen(
            command,
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            text = wait_for_log(
                process,
                log_path,
                (
                    "UML_AQUARIUM_OK|",
                    "UML_AQUARIUM_ERROR|",
                    "UML_CHROMIUM_ERROR:",
                ),
                options.timeout,
            )
            if "UML_AQUARIUM_OK|" not in text:
                raise RuntimeError("Aquarium did not render with WebGL")
            port = announced_vnc_port(text)
            renderer_line = next(
                line for line in text.splitlines() if "UML_AQUARIUM_OK|" in line
            )
            if "virgl" not in renderer_line.lower():
                raise RuntimeError(
                    "Chromium WebGL did not report the Virgl renderer: "
                    + renderer_line
                )
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
            metrics = parse_metrics(renderer_line)
            if metrics["SAMPLES"] != options.sample_seconds:
                raise RuntimeError("Aquarium returned an incomplete FPS window")
            if metrics["WARMUP_SECONDS"] != options.warmup_seconds:
                raise RuntimeError("Aquarium used the wrong warm-up window")
            if metrics["FISH"] != 500:
                raise RuntimeError("Aquarium used the wrong fish count")
            metrics.update(
                {
                    "engine": options.engine,
                    "cpus": options.cpus,
                    "memory": options.memory,
                    "url": AQUARIUM_URL,
                    "capture": str(capture_path.resolve()),
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
            print(f"Aquarium capture: {capture_path}")
        finally:
            stop_guest(process)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        print(f"run_chromium_webgl.py: {error}", file=sys.stderr)
        raise SystemExit(1)
