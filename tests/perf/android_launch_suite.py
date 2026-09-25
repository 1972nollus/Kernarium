#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Time cold application launches in one Android guest boot.

The guest boots from a copy-on-write clone of a source disk, so every run
starts from the same installed and optimized state and the source is never
written. Each round launches every selected application from cold (force
stopped, after an idle interval) and times it from the activity manager's
START line to the first logcat line matching the application's completion
pattern, both by the guest's own logcat timestamps. The patterns live in android_launch_apps.tsv; pick one
that marks the end of the launch's CPU work (a game's first scene, not its
splash screen). Records, one JSON object per launch, are appended to
--output; `--summary FILE...` prints each label's per-application medians.

The caller serializes guests: wrap the command in the host's GPU lock and
guest slot. Environment for one arm of an A/B goes in with --env, and
--guest-samples profiles the whole boot with DARWIN_UML_GUEST_SAMPLES.
"""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import dataclasses
import json
import os
import pathlib
import re
import shutil
import statistics
import subprocess
import sys
import time

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
BOOT = REPOSITORY / "scripts" / "boot_android.py"
MANIFEST = pathlib.Path(__file__).resolve().parent / "android_launch_apps.tsv"
# `logcat -v epoch`: seconds.fraction, then the rest of the line.
EPOCH_LINE = re.compile(r"^\s*(\d+\.\d+)\s+(.*)$")
TOTAL_TIME = re.compile(r"^TotalTime:\s*(\d+)", re.MULTILINE)
# `ps -M`: a thread's scheduling priority precedes its two CPU time columns.
THREAD_PRIORITY = re.compile(r"\s(\d+)[A-Z]*\s+[\d:.]+\s+[\d:.]+\s*")


@dataclasses.dataclass(frozen=True)
class App:
    name: str
    component: str
    done: re.Pattern[str]
    limit: int

    @property
    def package(self) -> str:
        return self.component.split("/", 1)[0]

    @property
    def launched(self) -> re.Pattern[str]:
        return re.compile(r"START u0 .*cmp=" + re.escape(self.package) + "/")


def read_manifest(path: pathlib.Path) -> dict[str, App]:
    lines = path.read_text().splitlines()
    if not lines or lines[0].split("\t") != [
            "name", "component", "done", "limit_seconds"]:
        raise ValueError(f"{path}: unexpected header")
    apps = {}
    for number, line in enumerate(lines[1:], 2):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 4 or "/" not in fields[1]:
            raise ValueError(f"{path}:{number}: malformed row")
        apps[fields[0]] = App(fields[0], fields[1], re.compile(fields[2]),
                              int(fields[3]))
    return apps


def first_match(logcat: str, pattern: re.Pattern[str],
                since: float) -> float | None:
    """Timestamp of the first line at or after `since` matching `pattern`."""
    for line in logcat.splitlines():
        match = EPOCH_LINE.match(line)
        if match and float(match.group(1)) >= since and \
                pattern.search(match.group(2)):
            return float(match.group(1))
    return None


def launch_seconds(logcat: str, app: App) -> float | None:
    """Seconds from the activity manager's START of `app` to its marker.

    Both ends are the guest's own logcat timestamps, so the host and guest
    clocks never mix.
    """
    started = first_match(logcat, app.launched, 0.0)
    if started is None:
        return None
    finished = first_match(logcat, app.done, started)
    return None if finished is None else round(finished - started, 3)


def cpu_seconds(text: str) -> float:
    """`ps -o time=` as seconds: [[dd-]hh:]mm:ss.ss."""
    days, _, clock = text.strip().rpartition("-")
    seconds = 0.0
    for part in clock.split(":"):
        seconds = seconds * 60 + float(part)
    return seconds + (int(days) * 86400 if days else 0)


def thread_priorities(ps_threads: str) -> dict[str, int]:
    """Threads per scheduling priority in `ps -M` output."""
    counts: dict[str, int] = {}
    for line in ps_threads.splitlines()[1:]:
        match = THREAD_PRIORITY.search(line)
        if match:
            counts[match.group(1)] = counts.get(match.group(1), 0) + 1
    return counts


def summarize(records: list[dict]) -> dict[str, dict[str, dict[str, float]]]:
    """Per label and application: completed launches and median seconds."""
    grouped: dict[str, dict[str, list[dict]]] = {}
    for record in records:
        grouped.setdefault(record["label"], {}).setdefault(
            record["app"], []).append(record)
    summary = {}
    for label, apps in grouped.items():
        summary[label] = {}
        for app, runs in apps.items():
            done = [run for run in runs if run["seconds"] is not None]
            row = {"launches": len(runs), "completed": len(done)}
            if done:
                row["median_seconds"] = statistics.median(
                    run["seconds"] for run in done)
                row["median_emulator_cpu"] = statistics.median(
                    run["emulator_cpu"] for run in done)
            summary[label][app] = row
    return summary


class Guest:
    def __init__(self, arguments: argparse.Namespace, environment: dict):
        self.arguments = arguments
        self.serial = f"127.0.0.1:{arguments.adb_port}"
        self.console = open(arguments.console, "ab")
        command = [sys.executable, str(BOOT),
                   "--emulator", str(arguments.emulator),
                   "--release", str(arguments.release),
                   "--disk", str(arguments.work_disk),
                   "--adb-port", str(arguments.adb_port),
                   "--adb-timeout", "900",
                   "--cpus", str(arguments.cpus),
                   "--memory", arguments.memory]
        self.process = subprocess.Popen(
            command, cwd=REPOSITORY, env=environment, stdin=subprocess.DEVNULL,
            stdout=self.console, stderr=subprocess.STDOUT)

    def adb(self, *command: str, timeout: int = 60) -> str:
        result = subprocess.run(
            ["adb", "-s", self.serial, *command], stdin=subprocess.DEVNULL,
            capture_output=True, text=True, timeout=timeout)
        return result.stdout.replace("\r", "")

    def shell(self, command: str, timeout: int = 60) -> str:
        return self.adb("shell", command, timeout=timeout)

    def emulator_pid(self) -> int | None:
        """The launcher starts the emulator as its only child."""
        result = subprocess.run(
            ["pgrep", "-P", str(self.process.pid)], capture_output=True,
            text=True, stdin=subprocess.DEVNULL)
        pids = result.stdout.split()
        return int(pids[0]) if len(pids) == 1 else None

    def wait_booted(self, deadline: float) -> bool:
        while time.monotonic() < deadline and self.process.poll() is None:
            try:
                if self.shell("getprop sys.boot_completed",
                              timeout=20).strip() == "1":
                    return True
            except subprocess.TimeoutExpired:
                pass
            time.sleep(10)
        return False

    def power_off(self) -> int | None:
        try:
            self.shell("reboot -p", timeout=30)
        except subprocess.TimeoutExpired:
            pass
        try:
            return self.process.wait(timeout=300)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                return self.process.wait(timeout=60)
            except subprocess.TimeoutExpired:
                return None
        finally:
            self.console.close()


def launch(guest: Guest, app: App, emulator_pid: int) -> dict:
    guest.shell(f"am force-stop {app.package}")
    time.sleep(3)
    guest.adb("logcat", "-c")
    logcat_path = guest.arguments.output.with_suffix(".logcat")
    with open(logcat_path, "w") as logcat_file:
        reader = subprocess.Popen(
            ["adb", "-s", guest.serial, "logcat", "-v", "epoch"],
            stdin=subprocess.DEVNULL, stdout=logcat_file,
            stderr=subprocess.DEVNULL)
        try:
            cpu0 = cpu_seconds(subprocess.run(
                ["ps", "-o", "time=", "-p", str(emulator_pid)],
                capture_output=True, text=True).stdout)
            deadline = time.monotonic() + app.limit
            try:
                start = guest.shell(f"am start -W -n {app.component}",
                                    timeout=app.limit)
            except subprocess.TimeoutExpired:
                start = ""
            seconds = None
            while seconds is None and time.monotonic() < deadline:
                time.sleep(1)
                seconds = launch_seconds(
                    logcat_path.read_text(errors="replace"), app)
            cpu1 = cpu_seconds(subprocess.run(
                ["ps", "-o", "time=", "-p", str(emulator_pid)],
                capture_output=True, text=True).stdout)
        finally:
            reader.terminate()
            reader.wait(timeout=30)
    total = TOTAL_TIME.search(start)
    # A napped emulator runs its threads at priority 4 instead of 31, which
    # alone makes launches several times slower.
    priorities = thread_priorities(subprocess.run(
        ["ps", "-M", "-p", str(emulator_pid)], capture_output=True,
        text=True).stdout)
    time.sleep(10)
    guest.shell(f"am force-stop {app.package}")
    return {
        "seconds": seconds,
        "displayed_ms": int(total.group(1)) if total else None,
        "emulator_cpu": round(cpu1 - cpu0, 2),
        "thread_priorities": priorities,
        "load": os.getloadavg()[0],
    }


def run(arguments: argparse.Namespace) -> int:
    apps = read_manifest(MANIFEST)
    selected = [apps[name] for name in arguments.apps.split(",")]
    environment = dict(os.environ)
    for item in arguments.env:
        key, _, value = item.partition("=")
        environment[key] = value
    if arguments.guest_samples:
        environment["DARWIN_UML_GUEST_SAMPLES"] = arguments.guest_samples
    # The launcher re-creates a disk whose release identity differs, which
    # would silently measure a fresh install.
    identity = json.loads((arguments.release / "release-manifest.json")
                          .read_text())["release_id"]
    release_id = arguments.source_disk.with_name(
        arguments.source_disk.name + ".release-id")
    if not release_id.exists() or release_id.read_text().strip() != identity:
        print(f"{release_id} does not name release {identity}",
              file=sys.stderr)
        return 1
    # A clone shares the source's blocks until the guest writes, so each run
    # starts from the source's state without copying it.
    work = arguments.work_disk
    staging = work.with_name(work.name + ".new")
    subprocess.run(["cp", "-c", str(arguments.source_disk), str(staging)],
                   check=True, stdin=subprocess.DEVNULL)
    staging.replace(work)
    shutil.copyfile(release_id, work.with_name(work.name + ".release-id"))

    guest = Guest(arguments, environment)
    booted = guest.wait_booted(time.monotonic() + 1200)
    emulator_pid = guest.emulator_pid()
    status = None
    try:
        if not booted or emulator_pid is None:
            print(f"guest did not boot (pid {emulator_pid})", file=sys.stderr)
            return 1
        time.sleep(arguments.settle)
        installed = guest.shell("pm list packages")
        for round_index in range(arguments.launches):
            for app in selected:
                record = {"label": arguments.label, "app": app.name,
                          "round": round_index}
                if f"package:{app.package}\n" not in installed + "\n":
                    record.update(seconds=None, missing=True)
                else:
                    record.update(launch(guest, app, emulator_pid))
                with arguments.output.open("a") as handle:
                    handle.write(json.dumps(record) + "\n")
                print(json.dumps(record), flush=True)
    finally:
        status = guest.power_off()
        print(f"guest exit status {status}", flush=True)
    return 0 if status == 0 else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--summary", nargs="+", type=pathlib.Path,
                        help="print the medians of these record files and exit")
    parser.add_argument("--emulator", type=pathlib.Path)
    parser.add_argument("--release", type=pathlib.Path)
    parser.add_argument("--source-disk", type=pathlib.Path)
    parser.add_argument("--work-disk", type=pathlib.Path)
    parser.add_argument("--adb-port", type=int, default=5601)
    parser.add_argument("--apps", default="settings,3dmark,ab2")
    parser.add_argument("--launches", type=int, default=2)
    parser.add_argument("--label", default="run")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--console", type=pathlib.Path)
    parser.add_argument("--env", action="append", default=[])
    parser.add_argument("--guest-samples")
    parser.add_argument("--cpus", type=int, default=10)
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--settle", type=int, default=45)
    arguments = parser.parse_args()
    if arguments.summary:
        records = [json.loads(line) for path in arguments.summary
                   for line in path.read_text().splitlines() if line]
        print(json.dumps(summarize(records), indent=2))
        return 0
    for required in ("emulator", "release", "source_disk", "work_disk",
                     "output"):
        if getattr(arguments, required) is None:
            parser.error(f"--{required.replace('_', '-')} is required")
    if arguments.work_disk.resolve() == arguments.source_disk.resolve():
        parser.error("--work-disk must differ from --source-disk")
    if arguments.console is None:
        arguments.console = arguments.output.with_suffix(".console.log")
    return run(arguments)


if __name__ == "__main__":
    raise SystemExit(main())
