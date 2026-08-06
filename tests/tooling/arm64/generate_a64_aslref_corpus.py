#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate a finite per-allocation official-ASL work queue.

Every official allocation receives a concrete encoding witness and an exact
shared-pseudocode dependency digest.  Missing deterministic interfaces are
kept separate from typechecking, execution, native observation, and semantic
partition completeness.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
import itertools
import subprocess
import sys
from collections import defaultdict
from pathlib import Path
from typing import Mapping, Sequence

from a64_architecture_oracle import SOURCE_ORACLE
from a64_bounded_generation import (
    BoundedCommandRunner,
    BoundedWorkspace,
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_aslref_corpus_identity import (
    corpus_identity_bytes,
    validate_corpus_payloads,
)
from a64_aslref_dependencies import (
    AslDependencyCatalog,
    interface_declarations,
)
from a64_one_shot_publication import OneShotPublication, render_tsv
from aggregate_a64_raw_instruction_evidence import parse_constraints
from generate_a64_aslref_behavior_status import (
    COLUMNS as BEHAVIOR_COLUMNS,
    SPECIMEN,
    status_id,
)
from generate_a64_instruction_allocations import archive_root
from generate_a64_instruction_required_variants import COLUMNS as REQUIRED_COLUMNS
from probe_a64_aslref_batch import COLUMNS as BATCH_COLUMNS, SUPPORT_DIGESTS, sha256
from run_a64_aslref_specimen import ASLREF_VERSION


COLUMNS = [
    "oracle_case_id",
    "allocation_status_id",
    "stable_id",
    "section",
    "architectural_form",
    "architecture_file",
    "architecture_encoding",
    "decode_class",
    "decode_encoding",
    "allocation_kind",
    "witness_encoding",
    "candidate_input_partition",
    "official_operation_status",
    "direct_dependency_count",
    "transitive_dependency_count",
    "dependency_digest",
    "impdef_dependency_count",
    "external_dependency_count",
    "missing_interface_count",
    "missing_interface_digest",
    "missing_interface_families",
    "batch_composition_status",
    "per_form_typecheck_status",
    "execution_status",
    "independent_native_status",
    "semantic_partition_status",
    "allocation_behavior_complete",
    "unresolved_requirements",
    "official_semantics_source",
]
QUEUE_COLUMNS = [
    "interface_family",
    "interface_symbol",
    "owner_module",
    "affected_allocation_count",
    "affected_stable_id_count",
    "affected_form_count",
    "implementation_status",
]
OWNER_MODULES = {
    "core-state": "a64_aslref_state_core.asl",
    "memory-and-ordering": "a64_aslref_state_memory.asl",
    "fp-state-and-traps": "a64_aslref_state_fp.asl",
    "system-register-state": "a64_aslref_state_sysreg.asl",
    "sve-state": "a64_aslref_state_sve.asl",
    "sme-state": "a64_aslref_state_sme.asl",
    "execution-environment": "a64_aslref_state_environment.asl",
    "control-and-events": "a64_aslref_state_events.asl",
    "implementation-choice": "a64_aslref_state_environment.asl",
    "official-semantic-body": "official-source-gap",
}
STATE_MODULE_FILENAMES = tuple(
    sorted(
        {
            owner
            for owner in OWNER_MODULES.values()
            if owner != "official-source-gap"
        }
    )
)
OWNER_DEPENDENCIES = {
    "a64_aslref_state_events.asl": frozenset(
        {
            "a64_aslref_state_core.asl",
            "a64_aslref_state_environment.asl",
        }
    ),
    "a64_aslref_state_fp.asl": frozenset(
        {"a64_aslref_state_events.asl"}
    ),
    "a64_aslref_state_sme.asl": frozenset({"a64_aslref_state_sve.asl"}),
    "a64_aslref_state_sysreg.asl": frozenset(
        {"a64_aslref_state_fp.asl"}
    ),
}


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument("--aslref", type=Path, required=True)
    parser.add_argument("--bundler", type=Path, required=True)
    parser.add_argument(
        "--interface",
        type=Path,
        default=Path(__file__).with_name("a64_aslref_user_state_interface.asl"),
    )
    parser.add_argument(
        "--state-module",
        type=Path,
        action="append",
        default=None,
        help=(
            "ASL state adapter module; repeat to select an explicit set. "
            "By default, every known module present beside this script is used."
        ),
    )
    parser.add_argument(
        "--required-variants",
        type=Path,
        default=repository / "tests/unit/arm64/a64_instruction_required_variants.tsv",
    )
    parser.add_argument(
        "--batch-status",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_batch_status.tsv",
    )
    parser.add_argument(
        "--behavior-status",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_behavior_status.tsv",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_allocation_corpus.tsv",
    )
    parser.add_argument(
        "--interface-queue-output",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_interface_queue.tsv",
    )
    parser.add_argument(
        "--identity-output",
        type=Path,
        default=repository / "tests/unit/arm64/a64_aslref_corpus.identity.json",
    )
    parser.add_argument("--timeout-seconds", type=int, default=600)
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def read_tsv(path: Path, columns: list[str]) -> list[dict[str, str]]:
    with path.open(encoding="utf-8", newline="") as source:
        reader = csv.DictReader(source, delimiter="\t")
        if reader.fieldnames != columns:
            raise ValueError(f"{path}: unexpected columns {reader.fieldnames!r}")
        rows = list(reader)
    if not rows:
        raise ValueError(f"{path}: contains no rows")
    return rows


def load_interface_modules(
    base_interface: Path,
    selected: list[Path] | None,
) -> tuple[dict[str, Path], dict[str, frozenset[str]], dict[str, str]]:
    """Load deterministic owner modules and reject ambiguous declarations."""
    if not base_interface.is_file():
        raise ValueError(f"base ASL interface is missing: {base_interface}")
    if selected is None:
        selected = [
            Path(__file__).with_name(filename)
            for filename in STATE_MODULE_FILENAMES
            if Path(__file__).with_name(filename).is_file()
        ]
    expected_names = set(STATE_MODULE_FILENAMES)
    paths: dict[str, Path] = {}
    declarations: dict[str, frozenset[str]] = {}
    owner_for_symbol: dict[str, str] = {}
    base_symbols = interface_declarations(base_interface.read_text(encoding="utf-8"))
    for path in selected:
        if not path.is_file():
            raise ValueError(f"ASL state module is missing: {path}")
        name = path.name
        if name not in expected_names:
            raise ValueError(f"unrecognized ASL state module owner: {name}")
        if name in paths:
            raise ValueError(f"duplicate ASL state module: {name}")
        paths[name] = path
        symbols = interface_declarations(path.read_text(encoding="utf-8"))
        if not symbols:
            raise ValueError(f"ASL state module declares no interfaces: {path}")
        overlap = base_symbols & symbols
        if overlap:
            raise ValueError(
                f"{name}: duplicates base interface declarations: "
                + ",".join(sorted(overlap))
            )
        declarations[name] = symbols
        for symbol in symbols:
            previous = owner_for_symbol.setdefault(symbol, name)
            if previous != name:
                raise ValueError(
                    f"ASL interface {symbol} is declared by both "
                    f"{previous} and {name}"
                )
    return paths, declarations, owner_for_symbol


def resolved_interface_map(
    frontier: set[tuple[str, str]],
    owner_for_symbol: dict[str, str],
) -> dict[tuple[str, str], bool]:
    resolved: dict[tuple[str, str], bool] = {}
    for family, symbol in sorted(frontier):
        expected_owner = OWNER_MODULES[family]
        actual_owner = owner_for_symbol.get(symbol)
        if actual_owner is not None and actual_owner != expected_owner:
            raise ValueError(
                f"ASL interface {family}:{symbol} is declared by "
                f"{actual_owner}, expected {expected_owner}"
            )
        resolved[(family, symbol)] = actual_owner == expected_owner
    return resolved


def selected_owner_modules(
    frontier: tuple[tuple[str, str], ...],
    resolved_interfaces: dict[tuple[str, str], bool],
) -> list[str]:
    """Select interface owners plus their deterministic module dependencies."""
    selected = {
        OWNER_MODULES[family]
        for family, symbol in frontier
        if resolved_interfaces.get((family, symbol), False)
    }
    pending = list(selected)
    while pending:
        owner = pending.pop()
        for dependency in OWNER_DEPENDENCIES.get(owner, frozenset()):
            if dependency not in selected:
                selected.add(dependency)
                pending.append(dependency)
    return sorted(selected)


def allocation_witness(row: dict[str, str]) -> int:
    mask = int(row["architecture_mask"], 16)
    value = int(row["architecture_value"], 16)
    decode_mask = int(row["decode_mask"], 16)
    decode_value = int(row["decode_value"], 16)
    if (value ^ decode_value) & mask & decode_mask:
        raise ValueError(f"{row['stable_id']}: conflicting allocation predicates")
    value = (value & mask) | (decode_value & decode_mask)
    mask |= decode_mask
    constraints = parse_constraints(
        row["architecture_constraints"], row["decode_constraints"]
    )
    free_bits = sorted(
        {
            bit
            for low, width, _operator, _expected in constraints
            for bit in range(low, low + width)
            if not mask & (1 << bit)
        }
    )
    for settings in itertools.product((0, 1), repeat=len(free_bits)):
        word = value
        for bit, setting in zip(free_bits, settings):
            if setting:
                word |= 1 << bit
        valid = True
        for low, width, operator, expected in constraints:
            actual = f"{(word >> low) & ((1 << width) - 1):0{width}b}"
            equal = all(
                wanted == "x" or wanted == observed
                for wanted, observed in zip(expected, actual)
            )
            if (operator == "==") != equal:
                valid = False
                break
        if valid:
            return word
    raise ValueError(f"{row['stable_id']}: allocation has no witness")


def batch_status(rows: list[dict[str, str]]) -> str:
    matches = [row for row in rows if row["component"] == "full-composition"]
    if len(matches) != 1:
        raise ValueError("batch status lacks one full-composition row")
    row = matches[0]
    return f"{row['status']}:{row['detail']}"


def validate_corpus_rows(rows: Sequence[Mapping[str, str]]) -> None:
    if len(rows) != 2396:
        raise ValueError("ASLRef allocation corpus does not contain 2,396 rows")
    for column in ("oracle_case_id", "allocation_status_id"):
        values = [row[column] for row in rows]
        if len(values) != len(set(values)):
            raise ValueError(f"ASLRef allocation corpus repeats {column}")
    order = [(row["stable_id"], row["allocation_status_id"]) for row in rows]
    if order != sorted(order):
        raise ValueError("ASLRef allocation corpus is not canonically sorted")
    if any(row["allocation_behavior_complete"] != "0" for row in rows):
        raise ValueError("ASLRef allocation corpus infers behavioral completion")


def validate_queue_rows(rows: Sequence[Mapping[str, str]]) -> None:
    identities = [
        (row["interface_family"], row["interface_symbol"]) for row in rows
    ]
    if not identities or len(identities) != len(set(identities)):
        raise ValueError("ASLRef interface queue identities are empty or duplicate")
    if identities != sorted(identities):
        raise ValueError("ASLRef interface queue is not canonically sorted")
    for row in rows:
        if OWNER_MODULES.get(row["interface_family"]) != row["owner_module"]:
            raise ValueError("ASLRef interface queue owner differs")
        for column in (
            "affected_allocation_count",
            "affected_stable_id_count",
            "affected_form_count",
        ):
            if not row[column].isdigit() or int(row[column]) <= 0:
                raise ValueError(f"ASLRef interface queue {column} is invalid")
        if row["implementation_status"] not in {"implemented", "unimplemented"}:
            raise ValueError("ASLRef interface queue status is invalid")


def diagnostic_digest(
    diagnostic: str, replacements: tuple[tuple[Path, str], ...]
) -> str:
    """Hash a diagnostic after replacing machine/run-specific paths."""
    canonical = diagnostic.replace("\r\n", "\n")
    expanded = {
        str(path): label
        for path, label in replacements
    }
    expanded.update(
        (str(path.resolve()), label)
        for path, label in replacements
    )
    for value, label in sorted(
        expanded.items(), key=lambda item: len(item[0]), reverse=True
    ):
        canonical = canonical.replace(value, label)
    return hashlib.sha256(canonical.encode()).hexdigest()[:16]


def per_form_typechecks(
    files: list[str],
    catalog: AslDependencyCatalog,
    root: Path,
    aslref: Path,
    bundler: Path,
    base_interface: Path,
    module_paths: dict[str, Path],
    module_declarations: dict[str, frozenset[str]],
    resolved_interfaces: dict[tuple[str, str], bool],
    workspace: BoundedWorkspace,
    runner: BoundedCommandRunner,
) -> dict[str, str]:
    results: dict[str, str] = {}
    directory = workspace.require_path() / "forms"
    directory.mkdir()
    for index, filename in enumerate(files):
        status = catalog.for_file(filename)
        unresolved = [
            interface
            for interface in status.missing_interfaces
            if not resolved_interfaces.get(interface, False)
        ]
        if unresolved:
            results[filename] = "blocked-missing-interface-frontier"
            continue
        selected_owners = selected_owner_modules(
            status.missing_interfaces, resolved_interfaces
        )
        supplied_symbols = frozenset(
            symbol
            for owner in selected_owners
            for symbol in module_declarations[owner]
        )
        form = directory / f"form-{index}"
        form.mkdir()
        instructions = form / "instructions.asl"
        shared = form / "shared_subset.asl"
        generated = runner.run(
            [
                sys.executable, "-B", str(bundler), "--make-funs", "-j", "1",
                "-o", str(form),
                str(root / catalog.operation_source_file(filename)),
            ],
            capture_output=True,
            text=True,
            timeout=min(60, runner.timeout_seconds),
        )
        if generated.returncode != 0 or not instructions.is_file():
            digest = diagnostic_digest(
                generated.stderr + generated.stdout,
                (
                    (form, "<form>"),
                    (directory, "<forms>"),
                    (root, "<architecture-root>"),
                    (bundler, "<bundler>"),
                ),
            )
            results[filename] = f"bundle-failed:{digest}"
            continue
        workspace.write_text(
            shared,
            catalog.shared_subset(
                status,
                supplied_symbols,
                tuple(
                    module_paths[owner].read_text(encoding="utf-8")
                    for owner in selected_owners
                ),
            ),
        )
        interface_arguments = ["-1", str(base_interface)]
        for owner in selected_owners:
            interface_arguments.extend(["-1", str(module_paths[owner])])
        checked = runner.run(
            [
                str(aslref), "--type-check-strict", "--no-exec",
                "-1", str(instructions), "-1", str(shared),
                *interface_arguments,
            ],
            capture_output=True,
            text=True,
            timeout=min(60, runner.timeout_seconds),
        )
        if checked.returncode == 0:
            results[filename] = "strict-pass"
        else:
            diagnostic = checked.stderr + checked.stdout
            results[filename] = (
                "strict-failed:"
                + diagnostic_digest(
                    diagnostic,
                    (
                        (form, "<form>"),
                        (directory, "<forms>"),
                        (root, "<architecture-root>"),
                        (base_interface, "<base-interface>"),
                        *tuple(
                            (
                                module_paths[owner],
                                f"<state-module:{owner}>",
                            )
                            for owner in selected_owners
                        ),
                    ),
                )
            )
    return results


def main() -> int:
    options = arguments()
    selected_modules = options.state_module
    if selected_modules is None:
        selected_modules = [
            Path(__file__).with_name(filename)
            for filename in STATE_MODULE_FILENAMES
            if Path(__file__).with_name(filename).is_file()
        ]
    options.aslref = options.aslref.resolve(strict=True)
    options.bundler = options.bundler.resolve(strict=True)
    options.state_module = selected_modules
    publication = OneShotPublication.prepare(
        [
            options.output,
            options.interface_queue_output,
            options.identity_output,
        ],
        [
            options.architecture_archive,
            options.aslref,
            options.bundler,
            options.interface,
            *selected_modules,
            options.required_variants,
            options.batch_status,
            options.behavior_status,
        ],
    )
    with workspace_from_options(
        options,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=publication.targets,
        prefix="a64-aslref-corpus-",
    ) as workspace:
        runner = BoundedCommandRunner(workspace, options.timeout_seconds)
        if sha256(options.bundler) != SUPPORT_DIGESTS["bundler.py"]:
            raise ValueError("bundler is not the pinned ASLRef support version")
        version = runner.run(
            [str(options.aslref), "--version"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
        if version != ASLREF_VERSION:
            raise ValueError(
                f"ASLRef version is {version!r}, expected {ASLREF_VERSION!r}"
            )
        required = read_tsv(options.required_variants, REQUIRED_COLUMNS)
        if len(required) != 2396:
            raise ValueError("required allocation model does not contain 2,396 rows")
        composed = batch_status(read_tsv(options.batch_status, BATCH_COLUMNS))
        behavior_rows = read_tsv(options.behavior_status, BEHAVIOR_COLUMNS)
        behavior = {row["allocation_status_id"]: row for row in behavior_rows}
        if len(behavior) != len(required):
            raise ValueError("behavior status does not cover every allocation")

        root = archive_root(
            options.architecture_archive,
            workspace.require_path() / "archive",
            workspace,
        )
        catalog = AslDependencyCatalog(root)
        files = sorted({row["architecture_file"] for row in required})
        frontier = {
            interface
            for filename in files
            for interface in catalog.for_file(filename).missing_interfaces
        }
        module_paths, module_declarations, owner_for_symbol = (
            load_interface_modules(options.interface, options.state_module)
        )
        resolved_interfaces = resolved_interface_map(frontier, owner_for_symbol)
        form_typechecks = per_form_typechecks(
            files, catalog, root, options.aslref, options.bundler,
            options.interface, module_paths, module_declarations,
            resolved_interfaces,
            workspace,
            runner,
        )

        queue_allocations: dict[tuple[str, str], set[str]] = defaultdict(set)
        queue_ids: dict[tuple[str, str], set[str]] = defaultdict(set)
        queue_forms: dict[tuple[str, str], set[str]] = defaultdict(set)
        output_rows: list[dict[str, str]] = []
        specimen_rows = 0
        for row in required:
            dependency = catalog.for_file(row["architecture_file"])
            unresolved_interfaces = tuple(
                interface
                for interface in dependency.missing_interfaces
                if not resolved_interfaces.get(interface, False)
            )
            witness = allocation_witness(row)
            is_specimen = all(
                row[column] == SPECIMEN[column]
                for column in (
                    "stable_id", "architecture_file", "architecture_encoding"
                )
            )
            if is_specimen:
                witness = int(SPECIMEN["encoding"], 16)
                specimen_rows += 1
            identifier = status_id(row)
            joined = behavior.get(identifier)
            if joined is None:
                raise ValueError(f"missing behavior status {identifier}")
            for interface in dependency.missing_interfaces:
                queue_allocations[interface].add(identifier)
                queue_ids[interface].add(row["stable_id"])
                queue_forms[interface].add(row["architecture_file"])
            interface_text = "\n".join(
                f"{family}:{symbol}"
                for family, symbol in unresolved_interfaces
            )
            missing_digest = hashlib.sha256(interface_text.encode()).hexdigest()
            families = sorted(
                {family for family, _symbol in unresolved_interfaces}
            )
            unresolved = set(families)
            unresolved.add("remaining-semantic-partitions")
            if not unresolved_interfaces:
                unresolved.add("deterministic-case-inputs")
            if is_specimen:
                unresolved.discard("deterministic-case-inputs")
            output_rows.append(
                {
                    "oracle_case_id": (
                        "BASE-0005/ADD_64_addsub_imm"
                        if is_specimen
                        else f"{identifier}/{witness:08x}"
                    ),
                    "allocation_status_id": identifier,
                    "stable_id": row["stable_id"],
                    "section": row["section"],
                    "architectural_form": row["architectural_form"],
                    "architecture_file": row["architecture_file"],
                    "architecture_encoding": row["architecture_encoding"],
                    "decode_class": row["decode_class"],
                    "decode_encoding": row["decode_encoding"],
                    "allocation_kind": row["allocation_kind"],
                    "witness_encoding": f"{witness:08x}",
                    "candidate_input_partition": (
                        "x5=0000000000000025+nzcv=a0000000"
                        if is_specimen else "candidate-zero-state"
                    ),
                    "official_operation_status": (
                        "present" if dependency.official_operation_present
                        else "missing-from-isa-form"
                    ),
                    "direct_dependency_count": str(
                        len(dependency.direct_definitions)
                    ),
                    "transitive_dependency_count": str(
                        len(dependency.transitive_definitions)
                    ),
                    "dependency_digest": dependency.dependency_digest,
                    "impdef_dependency_count": str(len(dependency.impdef_symbols)),
                    "external_dependency_count": str(
                        len(dependency.external_symbols)
                    ),
                    "missing_interface_count": str(
                        len(unresolved_interfaces)
                    ),
                    "missing_interface_digest": missing_digest,
                    "missing_interface_families": "+".join(families) or "none",
                    "batch_composition_status": composed,
                    "per_form_typecheck_status": form_typechecks[
                        row["architecture_file"]
                    ],
                    "execution_status": (
                        "strict-pass" if is_specimen else "not-run"
                    ),
                    "independent_native_status": (
                        joined["independent_native_hardware_oracle"]
                        if is_specimen else "not-run"
                    ),
                    "semantic_partition_status": (
                        "one-reviewed-partition-pass"
                        if is_specimen else "incomplete"
                    ),
                    "allocation_behavior_complete": "0",
                    "unresolved_requirements": "+".join(sorted(unresolved)),
                    "official_semantics_source": (
                        f"{row['architecture_file']}#{row['operation_digest']}#"
                        f"{SOURCE_ORACLE}"
                    ),
                }
            )
    if specimen_rows != 1:
        raise ValueError(f"corpus contains {specimen_rows} ADD specimens")
    output_rows.sort(key=lambda item: (item["stable_id"], item["allocation_status_id"]))
    queue_rows = [
        {
            "interface_family": family,
            "interface_symbol": symbol,
            "owner_module": OWNER_MODULES[family],
            "affected_allocation_count": str(len(queue_allocations[(family, symbol)])),
            "affected_stable_id_count": str(len(queue_ids[(family, symbol)])),
            "affected_form_count": str(len(queue_forms[(family, symbol)])),
            "implementation_status": (
                "implemented"
                if resolved_interfaces[(family, symbol)]
                else "unimplemented"
            ),
        }
        for family, symbol in sorted(queue_allocations)
    ]
    validate_corpus_rows(output_rows)
    validate_queue_rows(queue_rows)
    corpus_payload = render_tsv(COLUMNS, output_rows)
    queue_payload = render_tsv(QUEUE_COLUMNS, queue_rows)
    identity_payload = corpus_identity_bytes(
        publication.targets[0].name,
        corpus_payload,
        COLUMNS,
        publication.targets[1].name,
        queue_payload,
        QUEUE_COLUMNS,
    )

    def validate_bound_set(_observed: bytes) -> None:
        validate_corpus_payloads(
            publication.targets[0].name,
            corpus_payload,
            COLUMNS,
            publication.targets[1].name,
            queue_payload,
            QUEUE_COLUMNS,
            identity_payload,
            corpus_validator=validate_corpus_rows,
            queue_validator=validate_queue_rows,
        )

    publication.publish_payloads(
        [
            (publication.targets[0], corpus_payload, validate_bound_set),
            (publication.targets[1], queue_payload, validate_bound_set),
            (publication.targets[2], identity_payload, validate_bound_set),
        ]
    )
    typechecks = defaultdict(int)
    for value in form_typechecks.values():
        typechecks[value] += 1
    implemented_interfaces = sum(resolved_interfaces.values())
    print(
        f"A64 ASLRef corpus: {len(output_rows)} allocation witnesses, "
        f"{implemented_interfaces}/{len(queue_rows)} interface symbols implemented, "
        f"{len(queue_rows) - implemented_interfaces} unresolved, "
        f"{typechecks['strict-pass']} per-form strict passes, "
        "1 executed specimen, 0/2396 behavior-complete; wrote "
        f"{options.output}, {options.interface_queue_output}, and "
        f"{options.identity_output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (
        OSError,
        ValueError,
        subprocess.CalledProcessError,
        subprocess.TimeoutExpired,
    ) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
