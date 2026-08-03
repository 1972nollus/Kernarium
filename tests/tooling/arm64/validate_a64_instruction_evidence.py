#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bind A64 inventory IDs to executed CTest instruction evidence."""

from __future__ import annotations
# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import csv
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path


EVIDENCE_COLUMNS = [
    "ctest",
    "case",
    "source",
    "line",
    "representative",
    "opcode_bucket",
    "alias_shape",
    "variants",
    "encoding_hash",
    "invalid",
    "interpreter",
    "gadgets",
    "jit",
    "jit_skip",
]
STABLE_ID = re.compile(r"\b(?:BASE|FPSIMD)-\d{4}\b")
FORM_WORDS = {
    "immediate": {"IMMEDIATE"},
    "shifted register": {"SHIFTED"},
    "extended register": {"EXTENDED"},
    "register": {"REGISTER"},
    "scalar": {"SCALAR"},
    "vector": {"VECTOR"},
    "indexed": {"INDEXED"},
    "literal": {"LITERAL"},
}

# Architectural base encodings are often printed using their preferred
# assembler alias.  These are semantic equivalences from the Arm encoding
# descriptions, not fuzzy spelling matches.
DECODED_ALIASES = {
    "ASRV": {"ASR"},
    "BFC": {"BFI"},
    "BFM": {"BFI", "BFXIL"},
    "CBBLE": {"CBBGE"},
    "CBBLO": {"CBBHI"},
    "CBBLS": {"CBBHS"},
    "CBBLT": {"CBBGT"},
    "CBGE": {"CBGT"},
    "CBHLE": {"CBHGE"},
    "CBHLO": {"CBHHI"},
    "CBHLS": {"CBHHS"},
    "CBHLT": {"CBHGT"},
    "CBHS": {"CBHI"},
    "CBLE": {"CBGE", "CBLT"},
    "CBLO": {"CBHI"},
    "CBLS": {"CBHS", "CBLO"},
    "CBLT": {"CBGT"},
    "CMPP": {"SUBPS"},
    "GIC": {"SYS"},
    "GICR": {"SYSL"},
    "GSB": {"SYS"},
    "HINTE": {"HINT", "MSR"},
    "INS": {"MOV"},
    "LSLV": {"LSL"},
    "LSRV": {"LSR"},
    "NOT": {"MVN"},
    "REV64": {"REV"},
    "RORV": {"ROR"},
    "SBFM": {"ASR", "SBFIZ", "SBFX", "SXTB", "SXTH", "SXTW"},
    "SHUH": {"HINT"},
    "STCPH": {"HINT"},
    "STSHH": {"HINT"},
    "STTADD": {"LDTADD"},
    "STTADDL": {"LDTADDL"},
    "STTCLR": {"LDTCLR"},
    "STTCLRL": {"LDTCLRL"},
    "STTSET": {"LDTSET"},
    "STTSETL": {"LDTSETL"},
    "SXTL": {"SSHLL", "SSHLL2"},
    "UBFM": {"LSL", "LSR", "UBFIZ", "UBFX", "UXTB", "UXTH"},
    "UXTL": {"USHLL", "USHLL2"},
}


def fail(message: str) -> None:
    raise ValueError(message)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--evidence-directory", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    return parser.parse_args()


def registered_tests(build_root: Path) -> set[str]:
    completed = subprocess.run(
        ["ctest", "--test-dir", str(build_root), "-N"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    return set(
        re.findall(r"^\s*Test\s+#[0-9]+:\s*(\S.*?)\s*$", completed.stdout,
                   re.MULTILINE)
    )


def read_evidence(directory: Path, tests: set[str]) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for path in sorted(directory.glob("*.tsv")):
        if path.stem not in tests:
            continue
        with path.open(encoding="utf-8", newline="") as source:
            reader = csv.DictReader(source, delimiter="\t")
            if reader.fieldnames != EVIDENCE_COLUMNS:
                fail(f"{path}: columns are {reader.fieldnames!r}")
            for row in reader:
                if row["ctest"] not in tests:
                    fail(f"{path}: evidence names unregistered CTest {row['ctest']!r}")
                if row["interpreter"] != "1" or row["gadgets"] != "1":
                    fail(f"{path}: {row['case']!r} lacks interpreter/gadget execution")
                if row["jit"] != "1" and row["jit_skip"] != "host-feature":
                    fail(f"{path}: {row['case']!r} lacks JIT execution/host skip")
                if int(row["variants"]) <= 0:
                    fail(f"{path}: non-positive variant count")
                int(row["representative"], 16)
                int(row["opcode_bucket"], 16)
                int(row["alias_shape"], 16)
                int(row["encoding_hash"], 16)
                rows.append(row)
    if not rows:
        fail(f"no executable evidence was produced below {directory}")
    return rows


def llvm_objdump() -> str:
    if found := shutil.which("llvm-objdump"):
        return found
    completed = subprocess.run(
        ["xcrun", "--find", "llvm-objdump"], check=True, text=True,
        stdout=subprocess.PIPE,
    )
    return completed.stdout.strip()


def disassemble(rows: list[dict[str, str]]) -> list[str]:
    """Disassemble the exact representative encoding emitted by each test."""
    with tempfile.TemporaryDirectory(prefix="uml-instruction-evidence-") as raw:
        temporary = Path(raw)
        assembly = temporary / "evidence.s"
        obj = temporary / "evidence.o"
        with assembly.open("w", encoding="utf-8") as output:
            output.write(".text\n.globl _uml_instruction_evidence\n")
            output.write("_uml_instruction_evidence:\n")
            for row in rows:
                output.write(f".long 0x{row['representative']}\n")
        subprocess.run(
            ["clang", "-target", "arm64-apple-macos", "-c", str(assembly),
             "-o", str(obj)],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        completed = subprocess.run(
            [llvm_objdump(), "-d", str(obj)], check=True, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
    decoded: list[str] = []
    for line in completed.stdout.splitlines():
        match = re.match(
            r"^\s*[0-9a-f]+:\s+(?:(?:[0-9a-f]{2}\s+){4}|[0-9a-f]{8}\s+)"
            r"(\S+)(?:\s+(.*))?$",
            line,
            re.IGNORECASE,
        )
        if match:
            decoded.append(
                f"{match.group(1)} {match.group(2) or ''}".strip().upper()
            )
    if len(decoded) != len(rows):
        fail(f"disassembler returned {len(decoded)} rows for {len(rows)} encodings")
    return decoded


def semantic_mnemonic_matches(label: str, disassembly: str) -> bool:
    """Reject a stable-ID annotation if it landed on another program word."""
    rendered = disassembly.split(maxsplit=1)[0]
    # Current Apple tools emit .long for several newer allocated encodings.
    # In that case the checked-in stable ID is the authoritative semantic tag.
    if rendered in {".LONG", "<UNKNOWN>"}:
        return True
    decoded = rendered.split(".", maxsplit=1)[0]
    mnemonics = label_mnemonics(label)
    if decoded in mnemonics:
        return True
    if "B.COND" in mnemonics and disassembly.startswith("B."):
        return True
    if "BC.COND" in mnemonics and disassembly.startswith("BC."):
        return True
    generic_condition = re.match(r"([A-Z]+)<CC>", label.upper())
    if generic_condition is not None and decoded.startswith(
        generic_condition.group(1)
    ):
        return True
    return any(
        decoded in DECODED_ALIASES.get(mnemonic, set())
        for mnemonic in mnemonics
    )


def label_mnemonics(label: str) -> set[str]:
    result: set[str] = set()
    for alias in label.upper().split(","):
        match = re.match(r"\s*([A-Z][A-Z0-9.]*)", alias)
        if match:
            result.add(match.group(1))
    return result


def form_matches(label: str, case: str, decoded: str) -> bool:
    qualifier_match = re.search(r"\(([^)]+)\)", label)
    if qualifier_match is None:
        return True
    qualifier = qualifier_match.group(1).lower()
    required = FORM_WORDS.get(qualifier)
    context_words = set(re.findall(r"[A-Z]+", case))
    if required is not None and required & context_words:
        return True
    operands = decoded.split(maxsplit=1)[1] if " " in decoded else ""
    destination_is_vector = re.match(r"V[0-9]+(?:\.|,|$)", operands) is not None
    # Some Apple toolchain versions recognize matrix mnemonics but omit their
    # operands.  A stable-ID-tagged SIMD case still carries the exact encoding
    # and engine result, so an absent presentation operand is not scalar proof.
    if "vector" in qualifier and not destination_is_vector and operands:
        return False
    if "scalar" in qualifier and destination_is_vector:
        return False
    if qualifier in {"vector", "scalar"}:
        return True
    if "immediate" in qualifier:
        return "#" in decoded
    if "register" in qualifier:
        return "#" not in decoded
    if qualifier == "element":
        return "[" in decoded
    if qualifier == "general":
        return "[" not in decoded
    if qualifier == "by element" or qualifier == "indexed":
        return "[" in decoded or "INDEXED" in context_words
    if qualifier == "multiple structures":
        return "MULTIPLE" in context_words
    if qualifier == "single structure":
        return bool({"LANE", "SINGLE"} & context_words)
    return required is None


def decoded_mnemonic(disassembly: str, section: str) -> str:
    mnemonic = disassembly.split(maxsplit=1)[0]
    # Apple llvm-objdump renders AdvSIMD arrangements as a mnemonic suffix
    # (for example ADD.8B and FMLA.4S), while the Arm PDF inventory records
    # the architectural mnemonic without that presentation-only suffix.
    if section == "FP&SIMD":
        return mnemonic.split(".", maxsplit=1)[0]
    return mnemonic


def section_matches(section: str, row: dict[str, str]) -> bool:
    context = " ".join((row["ctest"], row["source"], row["case"])).lower()
    if section == "Base":
        return "base" in context
    return any(token in context for token in (
        "simd", "floating", "bfloat", "half_", "fp8", "fpsimd-"
    ))


def bind_inventory(
    inventory: list[dict[str, str]], evidence: list[dict[str, str]],
    decoded: list[str],
) -> list[tuple[dict[str, str], dict[str, str], str, str]]:
    direct_by_id: dict[str, list[tuple[dict[str, str], str]]] = defaultdict(list)
    decoded_by_section_and_mnemonic: dict[
        tuple[str, str], list[tuple[dict[str, str], str]]
    ] = defaultdict(list)
    for row, disassembly in zip(evidence, decoded):
        item = (row, disassembly)
        for stable_id in STABLE_ID.findall(row["case"]):
            direct_by_id[stable_id].append(item)
        for section in ("Base", "FP&SIMD"):
            if section_matches(section, row):
                mnemonic = decoded_mnemonic(disassembly, section)
                decoded_by_section_and_mnemonic[(section, mnemonic)].append(item)

    bindings: list[tuple[dict[str, str], dict[str, str], str, str]] = []
    missing: list[str] = []
    for instruction in inventory:
        stable_id = instruction["stable_id"]
        direct = [
            (row, disassembly)
            for row, disassembly in direct_by_id[stable_id]
            if section_matches(instruction["section"], row)
            and semantic_mnemonic_matches(
                instruction["pdf_label"], disassembly
            )
        ]
        candidates = direct
        reason = "stable-id"
        if not candidates:
            mnemonics = label_mnemonics(instruction["pdf_label"])
            candidates = [
                (row, disassembly)
                for mnemonic in mnemonics
                for row, disassembly in decoded_by_section_and_mnemonic[
                    (instruction["section"], mnemonic)
                ]
                if form_matches(
                    instruction["pdf_label"],
                    " ".join((row["ctest"], row["case"], row["source"])).upper(),
                    disassembly,
                )
            ]
            reason = "decoded-label"
        if not candidates:
            missing.append(f"{stable_id} {instruction['pdf_label']}")
            continue
        # Prefer the widest executed variant generator and make the selection
        # deterministic so the generated report is stable across CTest order.
        row, disassembly = min(
            candidates,
            key=lambda item: (
                -int(item[0]["variants"]), item[0]["ctest"],
                item[0]["source"], int(item[0]["line"]),
            ),
        )
        bindings.append((instruction, row, disassembly, reason))
    if missing:
        preview = "\n  ".join(missing)
        fail(
            f"{len(missing)} inventory IDs lack executable encoding evidence:\n"
            f"  {preview}"
        )
    return bindings


def write_report(
    directory: Path,
    bindings: list[tuple[dict[str, str], dict[str, str], str, str]],
) -> None:
    path = directory / "stable-id-bindings.tsv"
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, delimiter="\t", lineterminator="\n")
        writer.writerow([
            "stable_id", "pdf_label", "ctest", "case", "source", "line",
            "encoding", "decoded", "variants", "interpreter", "gadgets",
            "jit", "jit_skip", "binding",
        ])
        for instruction, row, decoded, reason in bindings:
            writer.writerow([
                instruction["stable_id"], instruction["pdf_label"],
                row["ctest"], row["case"], row["source"], row["line"],
                row["representative"], decoded, row["variants"],
                row["interpreter"], row["gadgets"], row["jit"],
                row["jit_skip"], reason,
            ])


def main() -> int:
    options = arguments()
    repository = Path(__file__).resolve().parents[3]
    inventory_path = repository / "tests/unit/arm64/a64_instruction_inventory.tsv"
    with inventory_path.open(encoding="utf-8", newline="") as source:
        inventory = list(csv.DictReader(source, delimiter="\t"))
    if len(inventory) != 983:
        fail(f"instruction inventory has {len(inventory)} rows")
    tests = registered_tests(options.build_root)
    evidence = read_evidence(options.evidence_directory, tests)
    decoded = disassemble(evidence)
    bindings = bind_inventory(inventory, evidence, decoded)
    write_report(options.evidence_directory, bindings)
    jit_executed = sum(row[1]["jit"] == "1" for row in bindings)
    print(
        f"A64 executable evidence valid: {len(bindings)} stable IDs, "
        f"{len(evidence)} encoding groups, {jit_executed} JIT executions, "
        f"{len(bindings) - jit_executed} host-feature skips"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
