#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Produce the AArch32 instruction corpus source tables.

  a32-corpus.py observe --archive TAR.GZ --suite alu[,memory...] --host HOST
                        [--runner 'qemu-arm -cpu max'] [--oracle qemu]
                        [--source tests/unit/arm64/aarch32_corpus/source]
  a32-corpus.py finish  --archive TAR.GZ [--directory tests/unit/arm64/aarch32_corpus]
  a32-corpus.py check   --archive TAR.GZ [--source tests/unit/arm64/aarch32_corpus/source]

observe plans the cases of each suite, runs them twice through the oracle on
HOST (tools/arm64-tests/a32-oracle.sh), requires both runs to agree, and
writes the suite's source table beside the state recipes. finish generates
the pack from the source tables (tools/instruction-corpus/source.py does the
same at build time) and writes provenance, manifest and coverage. Decision
D2: records observed under qemu-arm are labelled "qemu"; hardware records
carry the hardware's name. check plans every suite again without an oracle and
requires each source table to hold exactly the planned cases (name, form, program,
starting state), in order: a change to the planner or to the forms it iterates over
that moves a case shows up here, and the suite is observed again.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import hashlib
import json
import pathlib
import shlex
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
REPOSITORY = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPOSITORY / "tests/tooling/arm64"))
sys.path.insert(0, str(REPOSITORY / "tools/instruction-corpus"))

import a32_corpus_armv8 as armv8  # noqa: E402
import a32_corpus_boundary as boundary  # noqa: E402
import a32_corpus_control as control  # noqa: E402
import a32_corpus_memory as memory  # noqa: E402
import a32_corpus_plan as plan  # noqa: E402
import a32_corpus_simd as simd  # noqa: E402
import a32_corpus_system as system  # noqa: E402
import a32_corpus_wire as wire  # noqa: E402
import aarch32 as corpus  # noqa: E402

DIRECTORY = REPOSITORY / "tests/unit/arm64/aarch32_corpus"
INVENTORY = REPOSITORY / "tests/unit/arm64/a32_instruction_inventory.tsv"
CHUNK = 1500
SUITES = {**plan.SUITES, **memory.SUITES, **control.SUITES, **system.SUITES, **simd.SUITES,
          **armv8.SUITES, **boundary.SUITES}
HEADER_DEFAULTS = {"k": 1}
# Suites whose golden only hardware can settle: the multi-word accesses at
# unaligned addresses, which qemu-arm refuses and an arm64 Linux kernel fixes
# up for a compat task, and the FPSCR bits (Stride, Len, FZ16) an
# implementation may or may not hold.
HARDWARE_ONLY = {"unaligned", "fpscr-reserved"}


def json_line(value) -> str:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def run_oracle(arguments, cases, recipes, label: str) -> tuple[dict, list]:
    """Both runs of one chunk; they must be identical."""
    with tempfile.TemporaryDirectory(prefix="a32-corpus-run-") as scratch:
        scratch = pathlib.Path(scratch)
        (scratch / "request").write_bytes(wire.request(cases, recipes))
        responses = []
        for attempt in range(2):
            output = scratch / f"response{attempt}"
            command = [str(HERE / "a32-oracle.sh"), "--prefix", "a32d-oracle"]
            if arguments.runner:
                command += ["--runner", arguments.runner]
            command += [arguments.host, str(scratch / "request"), str(output)]
            subprocess.run(command, check=True, timeout=3600, stdin=subprocess.DEVNULL)
            responses.append(output.read_bytes())
        if responses[0] != responses[1]:
            raise SystemExit(f"{label}: the oracle's two runs disagree")
        return wire.parse_response(responses[0])


def describe_host(arguments) -> str:
    """The oracle's identity: the emulator's version line, or the board and kernel."""
    if arguments.runner:
        script = f"{shlex.split(arguments.runner)[0]} --version | head -1"
    else:
        script = "tr -d '\\0' < /proc/device-tree/model 2>/dev/null; echo ' '$(uname -sr)"
    result = subprocess.run(["ssh", "-o", "ConnectTimeout=15", "-o", "BatchMode=yes", arguments.host, script],
                            check=True, capture_output=True, text=True, timeout=60,
                            stdin=subprocess.DEVNULL)
    return " ".join(result.stdout.split())


def starting_changes(case: wire.Case, recipes) -> dict:
    """The registers a case changes from its recipe, as the source rows spell them."""
    return {key: corpus.hex_value(key, value) for key, value in case.initial.items()
            if value != recipes[case.recipe][key]}


def source_row(case: wire.Case, recipes, observation: wire.Observation) -> dict:
    initial = wire.initial_state(case, recipes)
    row = {"n": case.name, "f": case.form, "i": case.isa, "p": case.program_text(),
           "t": case.target, "s": case.recipe}
    if case.origin:
        row["o"] = case.origin
    changes = starting_changes(case, recipes)
    if changes:
        row["si"] = changes
    if case.fill != corpus.FILL_HASH32:
        row["d"] = case.fill
    if case.data_patches:
        row["dp"] = [[offset, payload.hex()] for offset, payload in case.data_patches]
    if case.path is not None and not observation.signal:
        executed = (observation.state["r11"] - initial["r11"] + observation.state["r10"]
                    - initial["r10"]) & 0xFFFFFFFF
        if executed != case.path:
            raise SystemExit(f"{case.name}: ran {executed} fillers, the plan expects {case.path}")
    if case.expect is not None and not observation.signal:
        for key, increment in case.expect.items():
            if (observation.state[key] - initial[key]) & 0xFFFFFFFF != increment & 0xFFFFFFFF:
                raise SystemExit(f"{case.name}: {key} changed by "
                                 f"{(observation.state[key] - initial[key]) & 0xFFFFFFFF:#x}, "
                                 f"the plan expects {increment & 0xFFFFFFFF:#x}")
    delta = corpus.difference(initial, observation.state)
    if delta:
        row["x"] = delta
    flags = ""
    if case.straight:
        flags += "s"
    if observation.signal:
        row["e"] = observation.signal
        row["fa"] = f"{observation.address:x}"
        half = (observation.pc - corpus.CODE_BASE - case.origin) >> 1
        inside = observation.pc >= corpus.CODE_BASE + case.origin and half < len(case.halfwords())
        row["fi"] = half if inside else 0
        retired = case.retired_before(row["fi"]) if case.straight else case.fault_retired
        if retired is None:
            raise SystemExit(f"{case.name}: a fault in a program with branches needs fault_retired")
        if retired != HEADER_DEFAULTS["k"]:
            row["k"] = retired
    else:
        if observation.exit_kind == 1:
            flags += "x"
        if case.finished() != HEADER_DEFAULTS["k"]:
            row["k"] = case.finished()
    if flags:
        row["fl"] = flags
    if observation.runs:
        row["m"] = [[offset, payload.hex()] for offset, payload in observation.runs]
    return row


def predicted_observation(case: wire.Case, recipes) -> wire.Observation:
    """What the profile defines for a case no oracle runs: the event at the
    instruction under test, the state unchanged."""
    half = case.starts()[case.target]
    pc = corpus.CODE_BASE + case.origin + 2 * half
    return wire.Observation(case.predicted, 0, pc if case.predicted == 4 else 0, pc, 0,
                            wire.initial_data_digest(case), wire.initial_state(case, recipes), [])


def observe(arguments) -> int:
    context = plan.build_context(arguments.archive)
    source = pathlib.Path(arguments.source)
    source.mkdir(parents=True, exist_ok=True)
    identity = describe_host(arguments)
    for suite in arguments.suite.split(","):
        if suite in HARDWARE_ONLY and arguments.oracle == "qemu":
            raise SystemExit(f"{suite}: only hardware records settle this suite")
        cases = SUITES[suite](context)
        if arguments.limit:
            cases = cases[:arguments.limit]
        rows = []
        environment = {}
        oracle = "profile" if suite in system.PREDICTED else arguments.oracle
        if suite in system.PREDICTED:
            rows = [source_row(case, context.recipes, predicted_observation(case, context.recipes))
                    for case in cases]
            cases = []
        for start in range(0, len(cases), CHUNK):
            chunk = cases[start:start + CHUNK]
            meta, observations = run_oracle(arguments, chunk, context.recipes,
                                            f"{suite} {start}")
            environment = {"runner": arguments.runner or "native", "host": identity,
                           "hwcap": f"{meta['hwcap']:x}", "hwcap2": f"{meta['hwcap2']:x}"}
            for case, observation in zip(chunk, observations):
                if wire.initial_data_digest(case) != observation.digest:
                    raise SystemExit(f"{case.name}: the initial data differs from the oracle's")
                rows.append(source_row(case, context.recipes, observation))
            print(f"{suite}: {start + len(chunk)}/{len(cases)}", flush=True)
        name = f"{suite}-{oracle}"
        header = {"record": name, "oracle": oracle, "defaults": HEADER_DEFAULTS,
                  "environment": environment}
        (source / f"{name}.jsonl").write_text(
            json_line(header) + "\n" + "".join(json_line(row) + "\n" for row in rows),
            encoding="utf-8")
    write_states(source, context.recipes)
    write_index(source)
    return 0


def write_states(source: pathlib.Path, recipes) -> None:
    lines = [json_line(corpus.recipe_row(name, state)) for name, state in sorted(recipes.items())]
    (source / "states.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_index(source: pathlib.Path) -> None:
    records = sorted(path.name for path in source.glob("*.jsonl") if path.name != "states.jsonl")
    index = {"schema": 1, "kind": "aarch32", "files": ["states.jsonl", *records],
             "records": [name.removesuffix(".jsonl") for name in records]}
    (source / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")


def finish(arguments) -> int:
    directory = pathlib.Path(arguments.directory)
    source = directory / "source"
    index = json.loads((source / "index.json").read_text())
    pack = corpus.generate(source, index)
    parsed = corpus.parse_pack(pack)
    environments = {}
    cases = 0
    per_form: dict[str, list[int]] = {}
    manifest = []
    for file in index["files"][1:]:
        rows = corpus.read_jsonl(source / file)
        header = rows[0]
        environments[header["oracle"]] = header.get("environment", {})
        for row in rows[1:]:
            counts = per_form.setdefault(row["f"], [0, 0])
            counts[0] += 1
            counts[1] += row.get("e", 0) == 4
        cases += len(rows) - 1
        manifest.append((header["record"], header["oracle"], len(rows) - 1))
    provenance = {
        "schema": 1, "kind": "aarch32", "cases": cases,
        "architecture_archive_sha256": plan.oracle.ARCHIVE_SHA256,
        "oracles": environments, "pack_sha256": hashlib.sha256(pack).hexdigest(),
        "records": {name: count for name, _oracle, count in manifest},
    }
    assert len(parsed["records"]) == len(manifest)
    (directory / "provenance.json").write_text(
        json.dumps(provenance, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    (directory / "manifest.tsv").write_text(
        "record\toracle\tcases\n" + "".join(f"{n}\t{o}\t{c}\n" for n, o, c in manifest),
        encoding="utf-8")
    with INVENTORY.open(encoding="utf-8") as table:
        rows = [line.rstrip("\n").split("\t") for line in table][1:]
    covered, excluded = [], []
    for row in rows:
        encoding, profile = row[3], row[8]
        counts = per_form.get(encoding, [0, 0])
        covered.append(f"{row[0]}\t{encoding}\t{counts[0]}\t{counts[1]}\n")
        if not counts[0]:
            excluded.append(f"{row[0]}\t{encoding}\t{profile}\n")
    (directory / "coverage.tsv").write_text(
        "stable_id\tencoding\tcases\tillegal_cases\n" + "".join(covered), encoding="utf-8")
    (directory / "excluded.tsv").write_text(
        "stable_id\tencoding\tprofile\n" + "".join(excluded), encoding="utf-8")
    print(f"{cases} cases, pack sha256 {provenance['pack_sha256']}")
    return 0


def check(arguments) -> int:
    """Every suite's plan against the source table that records it; 1 on any difference."""
    context = plan.build_context(arguments.archive)
    source = pathlib.Path(arguments.source)
    drifted = 0
    for suite in sorted(SUITES):
        oracle = "profile" if suite in system.PREDICTED else "qemu"
        table = source / f"{suite}-{oracle}.jsonl"
        if not table.exists():
            print(f"{suite}: no table (hardware only)")
            continue
        rows = corpus.read_jsonl(table)[1:]
        cases = SUITES[suite](context)
        planned = [(case.name, case.form, case.isa, case.program_text(), case.recipe, case.origin,
                    starting_changes(case, context.recipes)) for case in cases]
        recorded = [(row["n"], row["f"], row["i"], row["p"], row["s"], row.get("o", 0), row.get("si", {}))
                    for row in rows]
        first = next((index for index, (left, right) in enumerate(zip(planned, recorded)) if left != right),
                     None if len(planned) == len(recorded) else min(len(planned), len(recorded)))
        if first is None:
            print(f"{suite}: {len(planned)} cases reproduce")
            continue
        drifted += 1
        print(f"{suite}: the plan has {len(planned)} cases, the table {len(recorded)}; "
              f"they first differ at {first}:")
        print(f"  plan:  {planned[first] if first < len(planned) else None}")
        print(f"  table: {recorded[first] if first < len(recorded) else None}")
    if drifted:
        print(f"{drifted} suite(s) no longer reproduce from the planner: observe them again "
              "(see the corpus README)", file=sys.stderr)
    return 1 if drifted else 0


def compare(arguments) -> int:
    """Differences between two source directories' observations of the same cases."""
    def load(directory: pathlib.Path) -> dict[str, dict[str, dict]]:
        records = {}
        for path in sorted(directory.glob("*.jsonl")):
            if path.name != "states.jsonl":
                records[path.name.rsplit("-", 1)[0]] = {
                    row["n"]: row for row in corpus.read_jsonl(path)[1:]}
        return records
    left, right = load(pathlib.Path(arguments.left)), load(pathlib.Path(arguments.right))
    keys = ("e", "fi", "fa", "k", "x", "m")
    different = 0
    for suite in sorted(left.keys() & right.keys()):
        shown = 0
        differing, same = [], 0
        for name, row in left[suite].items():
            other = right[suite].get(name)
            if other is None:
                continue
            if row["p"] != other["p"] or any(row.get(key) != other.get(key) for key in keys):
                differing.append((name, [key for key in keys if row.get(key) != other.get(key)]))
            else:
                same += 1
        different += len(differing)
        print(f"{suite}: {same} agree, {len(differing)} differ, "
              f"{len(left[suite].keys() - right[suite].keys())} only left, "
              f"{len(right[suite].keys() - left[suite].keys())} only right")
        for name, fields in differing[:arguments.show]:
            print(f"  {name}: {','.join(fields)}")
            shown += 1
    return 1 if different else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("observe", "finish", "check"):
        command = commands.add_parser(name)
        command.add_argument("--archive", required=name != "finish", type=pathlib.Path)
        if name == "observe":
            command.add_argument("--suite", required=True)
            command.add_argument("--host", required=True)
            command.add_argument("--runner", default="")
            command.add_argument("--oracle", default="qemu")
            command.add_argument("--source", default=str(DIRECTORY / "source"))
            command.add_argument("--limit", type=int, default=0)
        elif name == "check":
            command.add_argument("--source", default=str(DIRECTORY / "source"))
        else:
            command.add_argument("--directory", default=str(DIRECTORY))
    command = commands.add_parser("compare")
    command.add_argument("--left", default=str(DIRECTORY / "source"))
    command.add_argument("--right", required=True)
    command.add_argument("--show", type=int, default=20)
    arguments = parser.parse_args()
    return {"observe": observe, "finish": finish, "check": check,
            "compare": compare}[arguments.command](arguments)


if __name__ == "__main__":
    sys.exit(main())
