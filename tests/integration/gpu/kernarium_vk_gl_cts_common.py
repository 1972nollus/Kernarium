# SPDX-License-Identifier: GPL-2.0-or-later
"""Common validation for VK-GL-CTS campaigns run through Kernarium."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

from pathlib import Path
import plistlib
import re
import subprocess
import sys
import time
from typing import Any

import run_vk_gl_cts as cts


REPO_ROOT = Path(__file__).resolve().parents[3]
BUNDLE_ID = "com.imxieyi.Kernarium"
RUNTIME_RESULT = re.compile(r"Kernarium: runtime returned (-?[0-9]+)")


def validate_app(app: Path) -> None:
    info_path = app / "Info.plist"
    executable = app / "Kernarium"
    if not app.is_dir() or not executable.is_file() or not info_path.is_file():
        raise cts.CampaignError(f"Kernarium app is incomplete: {app}")
    try:
        with info_path.open("rb") as stream:
            info = plistlib.load(stream)
    except (OSError, plistlib.InvalidFileException) as error:
        raise cts.CampaignError(f"cannot read app identity: {error}") from error
    if info.get("CFBundleIdentifier") != BUNDLE_ID:
        raise cts.CampaignError("Kernarium app has the wrong bundle identifier")


def validate_rootfs(rootfs: Path, identity: dict[str, Any]) -> None:
    frozen = identity["rootfs"]
    if not rootfs.is_file() or rootfs.stat().st_size != frozen["size"]:
        raise cts.CampaignError("rootfs size does not match the frozen identity")
    subprocess.run(
        [
            sys.executable,
            "-B",
            str(REPO_ROOT / "tools/rootfs/verify.py"),
            "--image",
            str(rootfs),
            "--manifest",
            str(REPO_ROOT / "tools/rootfs/manifest.json"),
            "--quiet",
        ],
        check=True,
    )


def wait_for_runtime(
    process: subprocess.Popen[bytes],
    log_path: Path,
    timeout: int,
    platform: str,
) -> tuple[str, int]:
    deadline = time.monotonic() + timeout
    next_heartbeat = time.monotonic() + 30
    while time.monotonic() < deadline:
        transcript = log_path.read_text(encoding="utf-8", errors="replace")
        matches = RUNTIME_RESULT.findall(transcript)
        if matches:
            return transcript, int(matches[-1])
        if process.poll() is not None:
            return transcript, -1
        now = time.monotonic()
        if now >= next_heartbeat:
            print(
                f"{platform} VK-GL-CTS guest is running; "
                f"console_bytes={log_path.stat().st_size}",
                flush=True,
            )
            next_heartbeat = now + 30
        time.sleep(0.5)
    raise TimeoutError(f"{platform} VK-GL-CTS guest exceeded {timeout} seconds")


def validate_transcript(
    transcript: str,
    campaign: cts.AppleCampaign,
    guest_page_size: int,
) -> tuple[dict[str, bytes], dict[str, str], dict[str, int]]:
    cts.validate_host_backend(transcript)
    artifacts = cts.decode_artifacts(transcript)
    required = cts.ALLOWED_ARTIFACTS - set(artifacts)
    if required:
        raise cts.CampaignError(
            f"Kernarium campaign omitted artifacts: {sorted(required)!r}"
        )
    renderer_text = artifacts["renderer.txt"].decode("utf-8", errors="strict")
    selected_text = artifacts["selected.txt"].decode("utf-8", errors="strict")
    qpa_text = artifacts["results.qpa"].decode("utf-8", errors="strict")
    summary_text = artifacts["summary.txt"].decode("utf-8", errors="strict")
    cts.validate_renderer_probe(renderer_text, guest_page_size)
    selected_cases = cts.parse_case_list(selected_text)
    if selected_cases != [campaign.exact_case]:
        raise cts.CampaignError("Kernarium campaign selected the wrong exact case")
    results = cts.validate_qpa_union(qpa_text, selected_cases)
    if summary_text.strip() != "cts_rc=0":
        raise cts.CampaignError(f"CTS process failed: {summary_text.strip()}")
    return artifacts, results, cts.status_totals(results)
