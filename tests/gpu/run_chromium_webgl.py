#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot KDE, launch Chromium's WebGL Aquarium, and capture it over VNC."""

from __future__ import annotations

import argparse
import base64
import os
from pathlib import Path
import re
import shlex
import signal
import socket
import subprocess
import sys
import time


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import boot_kde_vnc  # noqa: E402


EXTENSION_MANIFEST = """{
  "manifest_version": 3,
  "name": "UML Aquarium verifier",
  "version": "1.0",
  "content_scripts": [{
    "matches": ["https://webglsamples.org/aquarium/*"],
    "js": ["verify.js"],
    "run_at": "document_idle"
  }]
}
"""
EXTENSION_SCRIPT = """(() => {
  const rasterMarker = document.createElement("div");
  rasterMarker.textContent = "RASTER OK";
  rasterMarker.style.cssText = [
    "position:fixed",
    "right:8px",
    "top:8px",
    "width:220px",
    "height:64px",
    "z-index:2147483647",
    "display:flex",
    "align-items:center",
    "justify-content:center",
    "background:#ff00ff",
    "color:#00ff00",
    "font:32px monospace"
  ].join(";");
  document.documentElement.appendChild(rasterMarker);

  let attempts = 0;
  const timer = setInterval(() => {
    attempts += 1;
    const message = document.getElementById("msgContainer")?.textContent || "";
    if (message.includes("does not appear your computer supports WebGL")) {
      console.error("UML_AQUARIUM_ERROR|" + message.replace(/\\s+/g, " ").trim());
      clearInterval(timer);
      return;
    }
    const fps = document.getElementById("fps")?.textContent.trim();
    const fpsValue = Number.parseFloat(fps);
    if (!Number.isFinite(fpsValue) || fpsValue <= 0) {
      if (attempts >= 120) {
        console.error("UML_AQUARIUM_ERROR|rendering did not start");
        clearInterval(timer);
      }
      return;
    }
    const canvas = document.getElementById("canvas");
    const gl = canvas?.getContext("webgl2") || canvas?.getContext("webgl");
    if (!gl) {
      console.error("UML_AQUARIUM_ERROR|WebGL context unavailable");
      clearInterval(timer);
      return;
    }
    const extension = gl.getExtension("WEBGL_debug_renderer_info");
    const renderer = extension
      ? gl.getParameter(extension.UNMASKED_RENDERER_WEBGL)
      : gl.getParameter(gl.RENDERER);
    console.log("UML_AQUARIUM_OK|FPS=" + fps
      + "|VERSION=" + gl.getParameter(gl.VERSION)
      + "|RENDERER=" + renderer);
    clearInterval(timer);
  }, 1000);
})();
"""
LAUNCHER = """#!/bin/sh
set -eu
runtime=/run/user/1000
remaining=120
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
    https://webglsamples.org/aquarium/aquarium.html
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
def unused_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


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


def raster_marker_visible(capture_path: Path) -> bool:
    data = capture_path.read_bytes()
    header = re.match(br"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise RuntimeError("VNC capture is not an 8-bit binary PPM image")
    width, height = (int(value) for value in header.groups())
    pixels = memoryview(data)[header.end() :]
    if len(pixels) != width * height * 3:
        raise RuntimeError("VNC capture has an invalid PPM payload size")

    green_pixels = 0
    for y in range(80, min(height, 180)):
        row = y * width * 3
        for x in range(max(0, width - 280), width):
            offset = row + x * 3
            red, green, blue = pixels[offset : offset + 3]
            if red <= 80 and green >= 180 and blue <= 80:
                green_pixels += 1
    return green_pixels >= 50


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--client",
        type=Path,
        default=REPO_ROOT / ".build/cmake-macos/bin/uml-vnc-smoke-client",
    )
    parser.add_argument(
        "--artifact-root",
        type=Path,
        default=REPO_ROOT / ".build/chromium-webgl",
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
    parser.add_argument("--timeout", type=float, default=240)
    options = parser.parse_args()

    port = unused_port()
    password = "webgl"
    launcher_options = boot_kde_vnc.parse_arguments(
        ["--port", str(port), "--password", password]
    )
    command = boot_kde_vnc.build_harness_command(launcher_options)
    separator = command.index("--")
    overlay_commands = (
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
            EXTENSION_SCRIPT,
        ),
        install_text(
            "/usr/local/libexec/uml-chromium-aquarium",
            LAUNCHER.replace(
                "@CHROMIUM_EXTRA_FLAGS@",
                " ".join(shlex.quote(flag) for flag in options.chromium_flag),
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
            renderer_line = next(
                line for line in text.splitlines() if "UML_AQUARIUM_OK|" in line
            )
            if "virgl" not in renderer_line.lower():
                raise RuntimeError(
                    "Chromium WebGL did not report the Virgl renderer: "
                    + renderer_line
                )
            time.sleep(15)
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
            print(renderer_line)
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
