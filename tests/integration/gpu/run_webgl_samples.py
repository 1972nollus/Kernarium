#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run every rendering demo linked by webglsamples.org in one GPU guest."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import json
import math
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import boot_kde_vnc  # noqa: E402
import kernarium_vk_gl_cts_common as kernarium_common  # noqa: E402
import run_vk_gl_cts as cts  # noqa: E402
from run_chromium_webgl import (  # noqa: E402
    GUEST_REPORT_MARGIN_SECONDS,
    MINIMUM_SESSION_SECONDS,
    announced_vnc_port,
    git_state,
    install_text,
    rootfs_contract_metadata,
    stop_guest,
)


# The top-level site has 23 tiles. Replace the three collection indexes with
# every unique rendering page they link: three Lots-O-Images implementations,
# one Lots-O-Objects implementation, and nine Google I/O implementations.
SAMPLES = (
    ("Aquarium", "/aquarium/aquarium.html"),
    ("Blob", "/blob/blob.html"),
    ("Caves", "/caves/caves.html"),
    ("Color-Adjust", "/color-adjust/color-adjust.html"),
    ("City", "/city/city.html"),
    ("Collectibles", "/collectibles/index.html"),
    ("Dynamic Cubemap", "/dynamic-cubemap/dynamic-cubemap.html"),
    ("Electricflower", "/electricflower/electricflower.html"),
    ("Field", "/field/field.html"),
    ("Fishtank", "/fishtank/fishtank.html"),
    ("Halo", "/halo/halo.html"),
    ("Imagesphere", "/imagesphere/imagesphere.html"),
    (
        "Lots-O-Images DrawElements",
        "/lots-o-images/lots-o-images-draw-elements.html",
    ),
    (
        "Lots-O-Images BufferData",
        "/lots-o-images/lots-o-images-buffer-data.html",
    ),
    (
        "Lots-O-Images DrawElementsInstanced",
        "/lots-o-images/lots-o-images-draw-elements-instanced.html",
    ),
    (
        "Lots-O-Objects DrawElements",
        "/lots-o-objects/lots-o-objects-draw-elements.html",
    ),
    ("Multiple Views", "/multiple-views/multiple-views.html"),
    ("Persistence", "/persistence/persistence.html"),
    ("Spacerocks", "/spacerocks/spacerocks.html"),
    ("Sprites", "/sprites/index.html"),
    ("Flexible Toon Shading", "/toon-shading/toon-shading.html"),
    ("Book (Cloth Simulation Demo)", "/book/book.html"),
    ("Video Texture", "/video/video.html"),
    ("ROYGBIV Electroshock", "/electroShock/application.html"),
    ("Google I/O 100 Objects", "/google-io/2011/100-objects.html"),
    ("Google I/O 1000 Objects", "/google-io/2011/1000-objects.html"),
    (
        "Google I/O 1000 Objects Optimized",
        "/google-io/2011/1000-objects-optimized.html",
    ),
    ("Google I/O 10000 Objects", "/google-io/2011/10000-objects.html"),
    (
        "Google I/O 10000 Objects Matrix Optimized",
        "/google-io/2011/10000-objects-matrix-optimized.html",
    ),
    (
        "Google I/O 10000 Objects Optimized",
        "/google-io/2011/10000-objects-optimized.html",
    ),
    ("Google I/O 40000 Objects", "/google-io/2011/40000-objects.html"),
    (
        "Google I/O 40000 Objects Optimized",
        "/google-io/2011/40000-objects-optimized.html",
    ),
    (
        "Google I/O Lots Of Objects",
        "/google-io/2011/lots-of-objects-google.html",
    ),
)

MANIFEST = """{
  "manifest_version": 3,
  "name": "UML WebGL Samples verifier",
  "version": "1.0",
  "permissions": ["tabs"],
  "background": {
    "service_worker": "service-worker.js"
  },
  "content_scripts": [{
    "matches": ["https://webglsamples.org/*"],
    "js": ["probe-main.js"],
    "run_at": "document_start",
    "world": "MAIN"
  }, {
    "matches": ["https://webglsamples.org/*"],
    "js": ["verify.js"],
    "run_at": "document_end"
  }]
}
"""

SERVICE_WORKER_SCRIPT = r"""chrome.runtime.onMessage.addListener(
  (message, sender) => {
    if (!sender.tab?.id || typeof message?.next !== "string"
        || !message.next.startsWith("https://webglsamples.org/")) {
      return;
    }
    const reset = chrome.runtime.getURL("reset.html")
      + "?next=" + encodeURIComponent(message.next);
    chrome.tabs.create({url: reset, active: true}, tab => {
      if (chrome.runtime.lastError || !tab)
        return;
      chrome.tabs.remove(sender.tab.id);
    });
  });
"""

RESET_HTML = """<!doctype html>
<meta charset="utf-8">
<title>Resetting WebGL sample renderer</title>
<script src="reset.js"></script>
"""

RESET_SCRIPT = r"""const next = new URL(location.href).searchParams.get("next");
if (next?.startsWith("https://webglsamples.org/")) {
  setTimeout(() => chrome.tabs.update({url: next}), 750);
}
"""

PROBE_SCRIPT = r"""(() => {
  const state = {contexts: 0, frames: 0, lost: 0, type: "", renderer: ""};
  const seenContexts = new WeakSet();
  const publish = () => {
    const root = document.documentElement;
    if (!root) return;
    root.dataset.umlWebglContexts = String(state.contexts);
    root.dataset.umlWebglFrames = String(state.frames);
    root.dataset.umlWebglLost = String(state.lost);
    root.dataset.umlWebglType = state.type;
    root.dataset.umlWebglRenderer = state.renderer;
  };

  const originalGetContext = HTMLCanvasElement.prototype.getContext;
  const recordContext = (canvas, context, type) => {
    if (!context || seenContexts.has(context))
      return;
    seenContexts.add(context);
    state.contexts += 1;
    state.type = String(type);
    try {
      const info = context.getExtension("WEBGL_debug_renderer_info");
      state.renderer = info
        ? String(context.getParameter(info.UNMASKED_RENDERER_WEBGL))
        : String(context.getParameter(context.RENDERER));
    } catch (_) {}
    canvas.addEventListener("webglcontextlost", () => {
      state.lost += 1;
      publish();
    }, {once: true});
    publish();
  };
  HTMLCanvasElement.prototype.getContext = function(type, ...attributes) {
    const context = originalGetContext.call(this, type, ...attributes);
    if (/^(webgl2?|experimental-webgl)$/i.test(String(type)))
      recordContext(this, context, type);
    return context;
  };

  const findExistingAnimatedContext = () => {
    if (state.contexts > 0 || state.frames === 0)
      return;
    for (const canvas of document.querySelectorAll("canvas")) {
      for (const type of ["webgl2", "webgl", "experimental-webgl"]) {
        const context = originalGetContext.call(canvas, type);
        if (context) {
          recordContext(canvas, context, type);
          return;
        }
      }
    }
  };

  const originalTransfer = HTMLCanvasElement.prototype.transferControlToOffscreen;
  if (originalTransfer) {
    HTMLCanvasElement.prototype.transferControlToOffscreen = function(...args) {
      const canvas = originalTransfer.apply(this, args);
      if (canvas) {
        state.contexts += 1;
        state.type = "offscreen";
        state.renderer = "Chromium GPU emulator (OffscreenCanvas)";
        publish();
      }
      return canvas;
    };
  }

  const originalAnimationFrame = window.requestAnimationFrame;
  window.requestAnimationFrame = function(callback) {
    return originalAnimationFrame.call(window, timestamp => {
      state.frames += 1;
      findExistingAnimatedContext();
      publish();
      callback(timestamp);
    });
  };
  publish();
})();
"""

VERIFY_TEMPLATE = r"""(() => {
  const samples = @SAMPLES@;
  const path = location.pathname;
  let index = Number.parseInt(sessionStorage.getItem("umlSampleIndex") || "0", 10);
  if (!samples[index] || samples[index][1] !== path) {
    index = samples.findIndex(sample => sample[1] === path);
  }
  if (index < 0) {
    console.error("UML_WEBGL_SAMPLE_ERROR|unknown sample path|" + path);
    return;
  }
  sessionStorage.setItem("umlSampleIndex", String(index));

  const marker = document.createElement("div");
  marker.textContent = "TEST " + (index + 1) + "/" + samples.length;
  marker.style.cssText = [
    "position:fixed", "right:8px", "top:8px", "z-index:2147483647",
    "padding:8px", "background:#ff00ff", "color:#00ff00",
    "font:24px monospace", "pointer-events:none"
  ].join(";");
  (document.body || document.documentElement).appendChild(marker);

  const started = Date.now();
  let contextSeenAt = 0;
  let finished = false;
  const fail = reason => {
    if (finished) return;
    finished = true;
    console.error(
      "UML_WEBGL_SAMPLE_ERROR|" + (index + 1) + "/" + samples.length
      + "|" + samples[index][0] + "|" + reason);
  };
  const timer = setInterval(() => {
    const root = document.documentElement;
    const contexts = Number(root.dataset.umlWebglContexts || "0");
    const lost = Number(root.dataset.umlWebglLost || "0");
    const frames = Number(root.dataset.umlWebglFrames || "0");
    const pageText = (document.body?.innerText || "").toLowerCase();
    if (lost > 0) {
      clearInterval(timer);
      fail("WebGL context lost");
      return;
    }
    if (/does not (appear your computer )?support webgl|webgl not supported/.test(pageText)) {
      clearInterval(timer);
      fail("page reported that WebGL is unsupported");
      return;
    }
    if (contexts > 0 && contextSeenAt === 0)
      contextSeenAt = Date.now();
    if (contextSeenAt && Date.now() - contextSeenAt >= 7000) {
      clearInterval(timer);
      finished = true;
      console.log(
        "UML_WEBGL_SAMPLE_OK|" + (index + 1) + "/" + samples.length
        + "|" + samples[index][0]
        + "|CONTEXT=" + (root.dataset.umlWebglType || "webgl")
        + "|FRAMES=" + frames
        + "|RENDERER=" + (root.dataset.umlWebglRenderer || "unknown"));
      if (index + 1 < samples.length) {
        sessionStorage.setItem("umlSampleIndex", String(index + 1));
        marker.textContent = "PASS " + (index + 1) + "/" + samples.length;
        setTimeout(() => {
          chrome.runtime.sendMessage({
            next: "https://webglsamples.org" + samples[index + 1][1]
          });
        }, 4000);
      } else {
        marker.textContent = "ALL " + samples.length + " PASS";
        console.log("UML_WEBGL_ALL_OK|COUNT=" + samples.length);
      }
      return;
    }
    if (Date.now() - started >= 120000) {
      clearInterval(timer);
      fail("no live WebGL context after 120 seconds");
    }
  }, 500);
})();
"""

# Guest-uptime deadlines come from the runner's two deadlines: the session
# wait and first page fetch must leave the first sample its Chromium start-up
# and the probe's own context wait before --first-sample-timeout, and Chromium
# must finish before --timeout. Each ends in a named UML_WEBGL_RUNNER_ERROR.
FIRST_CONTEXT_SECONDS = 120
CHROMIUM_START_SECONDS = 30
FETCH_MINIMUM_SECONDS = 30
LAUNCHER_TEMPLATE = """#!/bin/sh
set -eu
session_deadline=@SESSION_DEADLINE@
fetch_deadline=@FETCH_DEADLINE@
deadline=@GUEST_DEADLINE@
runtime="/run/user/$(id -u uml)"
printf 'UML_WEBGL_LAUNCHER|UPTIME=%s|DEADLINE=%s\\n' \\
    "$(cut -d' ' -f1 /proc/uptime)" "$deadline"
uptime_seconds()
{
    cut -d. -f1 /proc/uptime
}
fail()
{
    echo "UML_WEBGL_RUNNER_ERROR: $*"
    exit 1
}
while [ ! -S /tmp/.X11-unix/X0 ] || [ ! -S "$runtime/bus" ]; do
    if [ "$(uptime_seconds)" -ge "$session_deadline" ]; then
        fail "KDE display did not become ready by guest uptime" \\
            "${session_deadline}s"
    fi
    sleep 1
done
xauthority="$(find "$runtime" -maxdepth 1 -type f -name 'xauth_*' -print |
    head -n 1)"
if [ -z "$xauthority" ]; then
    fail "Xauthority cookie was not found"
fi
rm -rf /tmp/chromium-webgl-samples
first_url="https://webglsamples.org@FIRST_PATH@"
fetch_seconds=$((fetch_deadline - $(uptime_seconds)))
if [ "$fetch_seconds" -le 0 ]; then
    fail "no budget remains for the first page fetch at guest uptime" \\
        "$(uptime_seconds)s"
fi
if ! curl --fail --location --silent --show-error \\
    --connect-timeout 30 --max-time "$fetch_seconds" \\
    --output /run/uml-webgl-first.html "$first_url"; then
    fail "first page fetch failed"
fi
first_size="$(wc -c < /run/uml-webgl-first.html)"
if [ "$first_size" -le 0 ]; then
    fail "first page fetch was empty"
fi
echo "UML_WEBGL_FETCH_OK|BYTES=$first_size|URL=$first_url"
if ! runuser -u uml -- env \\
    DISPLAY=:0 \\
    XAUTHORITY="$xauthority" \\
    XDG_RUNTIME_DIR="$runtime" \\
    timeout 30 glxinfo -B; then
    fail "glxinfo could not query the KDE display"
fi
chromium_status=0
timeout -k 10 "$((deadline - $(uptime_seconds)))" runuser -u uml -- env \\
    HOME=/home/uml \\
    DISPLAY=:0 \\
    WAYLAND_DISPLAY=wayland-0 \\
    XAUTHORITY="$xauthority" \\
    XDG_RUNTIME_DIR="$runtime" \\
    DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \\
    chromium \\
    @CHROMIUM_EXTRA_FLAGS@ \\
    --use-gl=angle \\
    --use-angle=gles-egl \\
    --ignore-gpu-blocklist \\
    --disable-quic \\
    --no-first-run \\
    --disable-crash-reporter \\
    --enable-logging=stderr \\
    --start-maximized \\
    --user-data-dir=/tmp/chromium-webgl-samples \\
    --disable-extensions-except=/var/tmp/uml-webgl-samples-extension \\
    --load-extension=/var/tmp/uml-webgl-samples-extension \\
    "$first_url" || chromium_status=$?
if [ "$chromium_status" = 124 ] || [ "$chromium_status" = 137 ]; then
    fail "WebGL samples did not finish by guest uptime ${deadline}s"
fi
fail "Chromium exited with status $chromium_status"
"""


def launcher_deadlines(
    first_sample_timeout: float, timeout: float
) -> dict[str, int]:
    """Derive the launcher's guest-uptime deadlines from the runner's."""
    deadline = math.floor(timeout) - GUEST_REPORT_MARGIN_SECONDS
    first_sample = math.floor(first_sample_timeout) - GUEST_REPORT_MARGIN_SECONDS
    fetch_deadline = (
        min(first_sample, deadline)
        - FIRST_CONTEXT_SECONDS
        - CHROMIUM_START_SECONDS
    )
    session_deadline = fetch_deadline - FETCH_MINIMUM_SECONDS
    if session_deadline < MINIMUM_SESSION_SECONDS:
        raise ValueError(
            f"a {first_sample_timeout:g} s first-sample deadline leaves the "
            f"boot and KDE session less than {MINIMUM_SESSION_SECONDS} s"
        )
    return {
        "session_deadline": session_deadline,
        "fetch_deadline": fetch_deadline,
        "deadline": deadline,
    }


def build_launcher(
    first_path: str,
    chromium_flags: list[str] | tuple[str, ...],
    deadlines: dict[str, int],
) -> str:
    return (
        LAUNCHER_TEMPLATE.replace("@FIRST_PATH@", first_path)
        .replace(
            "@CHROMIUM_EXTRA_FLAGS@",
            " ".join(shlex.quote(flag) for flag in chromium_flags),
        )
        .replace("@SESSION_DEADLINE@", str(deadlines["session_deadline"]))
        .replace("@FETCH_DEADLINE@", str(deadlines["fetch_deadline"]))
        .replace("@GUEST_DEADLINE@", str(deadlines["deadline"]))
    )


SERVICE = """[Unit]
Description=Chromium WebGL Samples verification
After=display-manager.service network-online.target
Wants=display-manager.service network-online.target

[Service]
Type=simple
ExecStart=/usr/local/libexec/uml-webgl-samples
TasksMax=infinity
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=graphical.target
"""

OK_PATTERN = re.compile(
    r"UML_WEBGL_SAMPLE_OK\|(\d+)/(\d+)\|([^|]+)\|"
    r"CONTEXT=([^|]+)\|FRAMES=(\d+)\|RENDERER=([^\r\n\"]+)"
)
GRAPHICS_FAILURES = (
    "context error reported",
    "Illegal resource",
    "Illegal command buffer",
    "response 0x1204 (command 0x207)",
)


def graphics_failure(transcript: str) -> str | None:
    for line in transcript.splitlines():
        if any(signature in line for signature in GRAPHICS_FAILURES):
            return line
    return None


def slug(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def capture_has_demo_pixels(path: Path) -> bool:
    data = path.read_bytes()
    header = re.match(br"P6\s+(\d+)\s+(\d+)\s+255\s", data)
    if not header:
        raise RuntimeError(f"invalid VNC PPM capture: {path}")
    width, height = (int(value) for value in header.groups())
    pixels = memoryview(data)[header.end() :]
    colors: set[tuple[int, int, int]] = set()
    for y in range(100, max(101, height - 60), 4):
        row = y * width * 3
        for x in range(0, max(1, width - 300), 4):
            offset = row + x * 3
            red, green, blue = pixels[offset : offset + 3]
            colors.add((red >> 5, green >> 5, blue >> 5))
            if len(colors) >= 4:
                return True
    return False


def capture_sample(client: Path, password: str, port: int, path: Path) -> None:
    for attempt in range(3):
        try:
            subprocess.run(
                [
                    str(client.resolve()),
                    "--password",
                    password,
                    "--capture",
                    str(path),
                    str(port),
                ],
                check=True,
                timeout=30,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            if not capture_has_demo_pixels(path):
                raise RuntimeError(f"capture is visually blank: {path}")
            return
        except (OSError, RuntimeError, subprocess.SubprocessError):
            if attempt == 2:
                raise
            time.sleep(0.5)


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
        default=REPO_ROOT / ".build/webgl-samples",
    )
    parser.add_argument(
        "--emulator",
        type=Path,
        default=REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin",
    )
    kernarium_common.add_rootfs_arguments(parser)
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument(
        "--engine",
        choices=("jit", "gadgets"),
        default="jit",
    )
    parser.add_argument(
        "--chromium-flag",
        action="append",
        default=[],
        help="append a Chromium flag for diagnostic comparison",
    )
    parser.add_argument(
        "--start-at",
        type=int,
        default=1,
        metavar="INDEX",
        help="begin at the one-based sample index (diagnostic use)",
    )
    parser.add_argument(
        "--end-at",
        type=int,
        default=len(SAMPLES),
        metavar="INDEX",
        help="stop at the one-based sample index (diagnostic use)",
    )
    parser.add_argument(
        "--first-sample-timeout",
        type=float,
        default=300,
        help="bound browser navigation and extension startup",
    )
    parser.add_argument("--timeout", type=float, default=900)
    options = parser.parse_args()
    kernarium_common.finish_rootfs_options(parser, options, identity)

    if not 1 <= options.start_at <= len(SAMPLES):
        parser.error(f"--start-at must be between 1 and {len(SAMPLES)}")
    if not options.start_at <= options.end_at <= len(SAMPLES):
        parser.error(
            f"--end-at must be between --start-at and {len(SAMPLES)}"
        )
    if options.cpus < 1:
        parser.error("--cpus must be positive")
    if options.first_sample_timeout <= 0:
        parser.error("--first-sample-timeout must be positive")
    try:
        deadlines = launcher_deadlines(
            options.first_sample_timeout, options.timeout
        )
    except ValueError as error:
        parser.error(f"--first-sample-timeout: {error}")
    rootfs = options.rootfs.resolve()
    kernarium_common.validate_rootfs(
        rootfs, identity, options.rootfs_contract
    )
    samples = SAMPLES[options.start_at - 1 : options.end_at]
    sample_json = json.dumps(samples, separators=(",", ":"))
    verifier = VERIFY_TEMPLATE.replace("@SAMPLES@", sample_json)
    launcher = build_launcher(
        samples[0][1], options.chromium_flag, deadlines
    )

    port = 0
    password = "webgl"
    launcher_options = boot_kde_vnc.parse_arguments(
        [
            "--display", "vnc",
            "--port", str(port),
            "--password", password,
            "--emulator", str(options.emulator),
            "--rootfs", str(rootfs),
            "--rootfs-type", options.rootfs_contract.filesystem_type,
            # Every image this runner accepts is the unified KDE image or an
            # ext4 derivative sealed from it, never a starter disk.
            "--image-profile", "legacy",
            "--memory", options.memory,
            "--cpus", str(options.cpus),
            "--engine", options.engine,
        ]
    )
    command = boot_kde_vnc.build_harness_command(launcher_options)
    separator = boot_kde_vnc.guest_argument_separator(command)
    overlay_commands = (
        "for unit in docker.service docker.socket containerd.service "
        "packagekit.service apt-daily.service apt-daily.timer "
        "apt-daily-upgrade.service apt-daily-upgrade.timer; do "
        "ln -sf /dev/null /etc/systemd/system/$unit; done",
        install_text(
            "/var/tmp/uml-webgl-samples-extension/manifest.json", MANIFEST
        ),
        install_text(
            "/var/tmp/uml-webgl-samples-extension/service-worker.js",
            SERVICE_WORKER_SCRIPT,
        ),
        install_text(
            "/var/tmp/uml-webgl-samples-extension/reset.html", RESET_HTML
        ),
        install_text(
            "/var/tmp/uml-webgl-samples-extension/reset.js", RESET_SCRIPT
        ),
        install_text(
            "/var/tmp/uml-webgl-samples-extension/probe-main.js", PROBE_SCRIPT
        ),
        install_text(
            "/var/tmp/uml-webgl-samples-extension/verify.js", verifier
        ),
        install_text(
            "/usr/local/libexec/uml-webgl-samples", launcher, "0755"
        ),
        install_text(
            "/etc/systemd/system/uml-webgl-samples.service", SERVICE
        ),
        "install -d -m 0755 /etc/systemd/system/graphical.target.wants; "
        "ln -sf ../uml-webgl-samples.service "
        "/etc/systemd/system/graphical.target.wants/"
        "uml-webgl-samples.service; "
        "systemctl start --no-block uml-webgl-samples.service",
    )
    additions: list[str] = []
    for overlay_command in overlay_commands:
        additions.extend(("--command", overlay_command))
    command[separator:separator] = additions

    artifact = options.artifact_root / time.strftime("%Y%m%d-%H%M%S")
    capture_directory = artifact / "captures"
    capture_directory.mkdir(parents=True)
    log_path = artifact / "console.log"
    run_path = artifact / "run.json"
    run_metadata: dict[str, object] = {
        "schema_version": 1,
        "adapter": "macos-vnc",
        "workload": "webglsamples-functional-v1",
        "state": "running",
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
        "engine": options.engine,
        "cpus": options.cpus,
        "memory": options.memory,
        "start_at": options.start_at,
        "end_at": options.end_at,
        "first_sample_timeout": options.first_sample_timeout,
        "timeout_seconds": options.timeout,
        "launcher_deadlines": deadlines,
        "samples": [name for name, _ in samples],
        "command": command,
        "shutdown_qualified": False,
    }
    run_path.write_text(
        json.dumps(run_metadata, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    seen: set[int] = set()
    deadline = time.monotonic() + options.timeout
    first_sample_deadline = time.monotonic() + options.first_sample_timeout

    with log_path.open("wb") as log:
        process = subprocess.Popen(
            command,
            stdout=log,
            stderr=subprocess.STDOUT,
        )
        try:
            while time.monotonic() < deadline:
                text = log_path.read_text(encoding="utf-8", errors="replace")
                if "UML_WEBGL_SAMPLE_ERROR|" in text:
                    marker = next(
                        line
                        for line in text.splitlines()
                        if "UML_WEBGL_SAMPLE_ERROR|" in line
                    )
                    raise RuntimeError(marker)
                if "UML_WEBGL_RUNNER_ERROR:" in text:
                    marker = next(
                        line[line.index("UML_WEBGL_RUNNER_ERROR:") :].strip()
                        for line in text.splitlines()
                        if "UML_WEBGL_RUNNER_ERROR:" in line
                    )
                    raise RuntimeError(marker)
                failure = graphics_failure(text)
                if failure is not None:
                    raise RuntimeError(
                        "Virgl rejected a WebGL command: " + failure
                    )
                if process.poll() is not None:
                    raise RuntimeError(
                        "emulator exited during WebGL samples run "
                        f"(status {process.returncode})"
                    )

                for match in OK_PATTERN.finditer(text):
                    index = int(match.group(1))
                    total = int(match.group(2))
                    name = match.group(3)
                    renderer = match.group(6)
                    if index in seen:
                        continue
                    uses_offscreen = match.group(4) == "offscreen"
                    if total != len(samples) or (
                        "virgl" not in renderer.lower() and not uses_offscreen
                    ):
                        raise RuntimeError(
                            f"{name} did not use the expected Virgl renderer: "
                            f"{renderer}"
                        )
                    if uses_offscreen and "Device: virgl" not in text:
                        raise RuntimeError(
                            f"{name} used OffscreenCanvas without a verified "
                            "Virgl display device"
                        )
                    capture = (
                        capture_directory
                        / f"{index:02d}-{slug(name)}.ppm"
                    )
                    capture_sample(
                        options.client,
                        password,
                        announced_vnc_port(text),
                        capture,
                    )
                    seen.add(index)
                    print(f"PASS {index:02d}/{total}: {name}")

                if not seen and time.monotonic() >= first_sample_deadline:
                    raise TimeoutError(
                        "first WebGL sample did not render within "
                        f"{options.first_sample_timeout:g} seconds"
                    )

                if "UML_WEBGL_ALL_OK|" in text:
                    if len(seen) != len(samples):
                        raise RuntimeError(
                            f"captured {len(seen)} of {len(samples)} demos"
                        )
                    print(
                        f"All {len(samples)} WebGL demos passed; "
                        f"artifacts: {artifact}"
                    )
                    run_metadata.update({
                        "state": "complete",
                        "passed_samples": len(seen),
                    })
                    run_path.write_text(
                        json.dumps(
                            run_metadata, indent=2, sort_keys=True
                        ) + "\n",
                        encoding="utf-8",
                    )
                    return 0
                time.sleep(0.25)
            raise TimeoutError(
                f"WebGL samples run timed out after {options.timeout:g} seconds"
            )
        except KeyboardInterrupt:
            run_metadata.update({
                "state": "interrupted",
                "passed_samples": len(seen),
            })
            run_path.write_text(
                json.dumps(run_metadata, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            raise
        except (
            OSError,
            RuntimeError,
            TimeoutError,
            subprocess.SubprocessError,
        ) as error:
            run_metadata.update({
                "state": "failed",
                "passed_samples": len(seen),
                "error": str(error),
            })
            run_path.write_text(
                json.dumps(run_metadata, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            raise
        finally:
            stop_guest(process)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(
            "run_webgl_samples.py: interrupted; guest stopped",
            file=sys.stderr,
        )
        raise SystemExit(130) from None
    except (OSError, RuntimeError, TimeoutError, subprocess.SubprocessError) as error:
        print(f"run_webgl_samples.py: {error}", file=sys.stderr)
        raise SystemExit(1)
