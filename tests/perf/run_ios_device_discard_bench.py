#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure batched and fragmented UBD discard on a physical iPad."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import json
import os
from pathlib import Path
import re
import signal
import statistics
import subprocess
import time


REPOSITORY = Path(__file__).resolve().parents[2]
DEFAULT_APP = (
    REPOSITORY
    / ".build/kernarium-device-derived/Build/Products/Release-iphoneos"
    / "Kernarium.app"
)
DEFAULT_ARTIFACT_ROOT = REPOSITORY / ".build/ios-device-discard-bench"
BUNDLE_ID = "com.imxieyi.Kernarium"
MODES = ("batched-128m", "fragmented-32k")
EXPECTED_REQUESTS = {"batched-128m": 4, "fragmented-32k": 16384}
SAMPLE = re.compile(
    r"KERNARIUM_DISCARD_SAMPLE:"
    r"(batched-128m|fragmented-32k):"
    r"([1-7]):([0-9]+):([0-9]+):([0-9]+):([0-9]+)"
)
READY = re.compile(
    r"KERNARIUM_DISCARD_READY:"
    r"(batched-128m|fragmented-32k):([0-9]+)"
)
RUNTIME_RESULT = re.compile(r"Kernarium: runtime returned (-?[0-9]+)")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--app", type=Path, default=DEFAULT_APP)
    parser.add_argument("--rootfs-filename", default="rootfs.erofs")
    parser.add_argument("--cpus", type=int, default=4)
    parser.add_argument("--memory-mib", type=int, default=1024)
    parser.add_argument("--timeout", type=int, default=600)
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument("--maximum-batched-ms", type=float, default=500.0)
    parser.add_argument("--minimum-speedup", type=float, default=2.0)
    return parser.parse_args()


def stop_console(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.terminate()
        process.wait(timeout=5)


def wait_for_runtime(
    process: subprocess.Popen[bytes], log: Path, timeout: int, mode: str
) -> str:
    deadline = time.monotonic() + timeout
    next_update = time.monotonic() + 30
    while time.monotonic() < deadline:
        transcript = log.read_text(encoding="utf-8", errors="replace")
        if RUNTIME_RESULT.search(transcript) is not None:
            return transcript
        if process.poll() is not None:
            raise RuntimeError(
                f"device console exited before {mode} completed; see {log}"
            )
        now = time.monotonic()
        if now >= next_update:
            print(
                f"iPad discard benchmark {mode} is running; "
                f"console_bytes={log.stat().st_size}",
                flush=True,
            )
            next_update = now + 30
        time.sleep(0.5)
    raise TimeoutError(f"iPad discard benchmark {mode} exceeded {timeout} seconds")


def launch(
    arguments: argparse.Namespace, mode: str, log: Path
) -> list[dict[str, int | str]]:
    environment = os.environ.copy()
    for name in (
        "DEVICECTL_CHILD_KERNARIUM_CTS_CAMPAIGN",
        "DEVICECTL_CHILD_KERNARIUM_GEEKBENCH",
        "DEVICECTL_CHILD_KERNARIUM_GEEKBENCH_WORKLOADS",
        "DEVICECTL_CHILD_KERNARIUM_WEBGL_AQUARIUM",
    ):
        environment.pop(name, None)
    environment.update(
        {
            "DEVICECTL_CHILD_KERNARIUM_DISCARD_BENCHMARK": mode,
            "DEVICECTL_CHILD_KERNARIUM_EXECUTION_ENGINE": "gadgets",
            "DEVICECTL_CHILD_KERNARIUM_GUEST_MEMORY_MIB": str(
                arguments.memory_mib
            ),
            "DEVICECTL_CHILD_KERNARIUM_KERNEL_PAGE_SIZE": "16384",
            "DEVICECTL_CHILD_KERNARIUM_OVERLAY_SIZE_GIB": "1",
            "DEVICECTL_CHILD_KERNARIUM_ROOTFS": arguments.rootfs_filename,
            "DEVICECTL_CHILD_KERNARIUM_VCPU_COUNT": str(arguments.cpus),
        }
    )
    command = [
        "xcrun",
        "devicectl",
        "device",
        "process",
        "launch",
        "--device",
        arguments.device,
        "--console",
        "--terminate-existing",
        BUNDLE_ID,
    ]
    with log.open("wb") as output:
        process = subprocess.Popen(
            command, stdout=output, stderr=subprocess.STDOUT, env=environment
        )
        try:
            transcript = wait_for_runtime(
                process, log, arguments.timeout, mode
            )
        finally:
            stop_console(process)

    runtime_results = RUNTIME_RESULT.findall(transcript)
    if not runtime_results or int(runtime_results[-1]) != 0:
        raise RuntimeError(f"{mode} runtime did not return zero; see {log}")
    ready = READY.findall(transcript)
    if len(ready) != 1 or ready[0][0] != mode:
        raise RuntimeError(f"{mode} emitted an invalid ready record; see {log}")
    samples = [
        {
            "mode": match.group(1),
            "run": int(match.group(2)),
            "nanoseconds": int(match.group(3)),
            "requests": int(match.group(4)),
            "sectors": int(match.group(5)),
            "ticks_ms": int(match.group(6)),
        }
        for match in SAMPLE.finditer(transcript)
        if match.group(1) == mode
    ]
    if [sample["run"] for sample in samples] != list(range(1, 8)):
        raise RuntimeError(f"{mode} emitted an incomplete sample set; see {log}")
    for sample in samples:
        if (
            sample["requests"] != EXPECTED_REQUESTS[mode]
            or sample["sectors"] != 1048576
        ):
            raise RuntimeError(
                f"{mode} emitted unexpected block statistics: {sample}"
            )
    return samples


def main() -> int:
    arguments = parse_arguments()
    if arguments.cpus < 1 or arguments.memory_mib < 512:
        raise ValueError("guest CPU and memory settings are invalid")
    if arguments.timeout < 1:
        raise ValueError("--timeout must be positive")
    if arguments.maximum_batched_ms <= 0 or arguments.minimum_speedup <= 1:
        raise ValueError("performance thresholds are invalid")
    app = arguments.app.resolve()
    if not app.is_dir():
        raise ValueError(f"Kernarium app is unavailable: {app}")
    subprocess.run(
        ["codesign", "--verify", "--deep", "--strict", str(app)], check=True
    )
    subprocess.run(
        [
            "xcrun",
            "devicectl",
            "device",
            "install",
            "app",
            "--device",
            arguments.device,
            str(app),
            "--quiet",
            "--timeout",
            "300",
        ],
        check=True,
    )

    artifact = arguments.artifact_root.resolve() / time.strftime(
        "%Y%m%d-%H%M%S"
    )
    artifact.mkdir(parents=True, exist_ok=False)
    results: dict[str, list[dict[str, int | str]]] = {}
    for mode in MODES:
        results[mode] = launch(arguments, mode, artifact / f"{mode}.log")

    medians_ms = {
        mode: statistics.median(
            sample["nanoseconds"] for sample in samples
        )
        / 1_000_000
        for mode, samples in results.items()
    }
    speedup = medians_ms["fragmented-32k"] / medians_ms["batched-128m"]
    summary = {
        "schema_version": 1,
        "device": arguments.device,
        "app": str(app),
        "guest": {
            "cpus": arguments.cpus,
            "memory_mib": arguments.memory_mib,
            "overlay_gib": 1,
            "page_size": 16384,
            "engine": "gadgets",
            "discard_bytes": 536870912,
        },
        "medians_ms": medians_ms,
        "speedup": speedup,
        "samples": results,
    }
    (artifact / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"batched-128m median: {medians_ms['batched-128m']:.3f} ms; "
        f"fragmented-32k median: {medians_ms['fragmented-32k']:.3f} ms; "
        f"speedup: {speedup:.2f}x",
        flush=True,
    )
    print(f"Artifacts: {artifact}", flush=True)
    if medians_ms["batched-128m"] > arguments.maximum_batched_ms:
        raise RuntimeError("batched discard exceeded the wall-time threshold")
    if speedup < arguments.minimum_speedup:
        raise RuntimeError("batching did not meet the minimum speedup")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
