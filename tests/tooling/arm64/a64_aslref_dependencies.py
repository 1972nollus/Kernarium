#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exact dependency and deterministic-interface queues for official A64 ASL.

Arm's XML links most references to definitions in shared_pseudocode.xml.  A
small set of implementation-defined procedures (notably system-register
access) is intentionally unlinked; those are resolved by their unique shared
definition name after lexical ASL call extraction.  The result is a finite,
per-form dependency closure.  It is not a behavioral-completeness assertion.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import re
import xml.etree.ElementTree as ET
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


SHARED_FILE = "shared_pseudocode.xml"
CALL = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:\{[^{}]*\})?\s*\(")
DECLARATION = re.compile(
    r"\b(?:impdef\s+)?(?:readonly\s+)?(?:noreturn\s+)?"
    r"(?:func|accessor)\s+([A-Za-z_][A-Za-z0-9_]*)"
)
TOP_LEVEL_VARIABLE = re.compile(r"(?m)^var\s+([A-Za-z_][A-Za-z0-9_]*)\s*:")
TOP_LEVEL_CONSTANT = re.compile(
    r"(?m)^(?:constant|config|let)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*:"
)
TYPE_DECLARATION = re.compile(
    r"(?m)^type\s+([A-Za-z_][A-Za-z0-9_]*)\s+(?:of\b|=)"
)
LINE_COMMENT = re.compile(r"//[^\n]*")
BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
STRING = re.compile(r'"(?:[^"\\]|\\.)*"')
IDENTIFIER = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
FEATURE_ENUM_VALUE = re.compile(r"\bFEAT_[A-Za-z0-9_]+\b")
PMU_CONSTANT = re.compile(r"\bPMU_[A-Za-z0-9_]+\b")
FIELD_SELECTOR = re.compile(
    r"(?<!\.)\.\s*(?:"
    r"\[\s*[A-Za-z_][A-Za-z0-9_]*"
    r"(?:\s*,\s*[A-Za-z_][A-Za-z0-9_]*)*\s*\]"
    r"|[A-Za-z_][A-Za-z0-9_]*)"
)
IMPDEF = re.compile(r"(?m)^\s*impdef\s+(?:readonly\s+)?(?:noreturn\s+)?func\b")

# These accessors are supplied by a64_aslref_user_state_interface.asl today.
# Raw PC/NZCV/FP storage in that file deliberately does not masquerade as the
# typed architectural interfaces used by the official shared pseudocode.
PROVIDED_INTERFACE_SYMBOLS = frozenset({"X", "SP", "V"})

STATE_BOUNDARY_SYMBOLS = frozenset(
    {
        "_PC",
        "PSTATE",
        "Mem",
        "MemA",
        "MemO",
        "MemS",
        "MemU",
        "MemU_unpriv",
        "AArch32_MemSingle",
        "AArch64_MemSingle",
        "AArch64_MemTag",
        "MemAttrHints",
        "PrefetchHint",
        "NVMem",
        "NVMem128",
        "FPCR",
        "FPSR",
        "FPMR",
        "Exception",
        "ExceptionRecord",
        "ExceptionalOccurrenceTargetState",
        "BranchTo",
        "BranchNotTaken",
        "Z",
        "P",
        "FFR",
        "_Z",
        "_P",
        "_FFR",
        "ZAhslice",
        "ZAslice",
        "ZAtile",
        "ZAvector",
        "ZAvslice",
        "ZT0",
        "_ZA",
        "_ZT0",
        "SVCR",
    }
)

EXTERNAL_INTERFACE_PREFIXES = (
    "IsFeatureImplemented",
    "ImpDef",
    "Unimplemented",
    "ReservedEncoding",
    "AArch64_SysReg",
    "AArch32_SysReg",
    "PhysMem",
    "Hint_",
)

ASL_STANDARD_CALLS = frozenset(
    {
        "Abs",
        "AlignDownP2",
        "AlignDownSize",
        "AlignUpSize",
        "AND",
        "ASR",
        "ASR_C",
        "BitCount",
        "bits",
        "CeilLog2",
        "CountLeadingSignBits",
        "CountLeadingZeroBits",
        "DIV",
        "DIVRM",
        "Extend",
        "FloorLog2",
        "FloorPow2",
        "HighestSetBit",
        "HighestSetBitNZ",
        "ILog2",
        "IsAlignedP2",
        "IsAlignedSize",
        "IsEven",
        "IsOnes",
        "IsPow2",
        "IsZero",
        "LowestSetBit",
        "LowestSetBitNZ",
        "LSL",
        "LSL_C",
        "LSR",
        "LSR_C",
        "Max",
        "Min",
        "MOD",
        "NOT",
        "OR",
        "Replicate",
        "Real",
        "ROL",
        "ROR",
        "RoundDown",
        "RoundUp",
        "SignExtend",
        "SInt",
        "SqrtRounded",
        "UInt",
        "XOR",
        "ZeroExtend",
        "assert",
        "case",
        "do",
        "else",
        "elsif",
        "if",
        "let",
        "looplimit",
        "of",
        "return",
        "then",
        "to",
        "var",
        "while",
    }
)


def _asl_without_comments(text: str) -> str:
    return STRING.sub('""', BLOCK_COMMENT.sub("", LINE_COMMENT.sub("", text)))


def _calls(text: str) -> set[str]:
    return set(CALL.findall(_asl_without_comments(text)))


def _identifiers(text: str) -> set[str]:
    """Return code identifiers, excluding comments and string payloads."""
    return set(IDENTIFIER.findall(_asl_without_comments(text)))


def _global_identifiers(text: str) -> set[str]:
    """Return identifiers after removing record and collection selectors."""
    source = FIELD_SELECTOR.sub("", _asl_without_comments(text))
    return set(IDENTIFIER.findall(source))


def _declared_symbols(text: str, fallback: str) -> frozenset[str]:
    """Index every replaceable top-level declaration in one shared chunk."""
    source = _asl_without_comments(text)
    declarations = frozenset(
        DECLARATION.findall(source)
        + TOP_LEVEL_VARIABLE.findall(source)
        + TOP_LEVEL_CONSTANT.findall(source)
        + TYPE_DECLARATION.findall(source)
    )
    return declarations or frozenset({fallback})


def interface_declarations(text: str) -> frozenset[str]:
    """Return top-level state, operation, and type names supplied by an adapter."""
    source = _asl_without_comments(text)
    return frozenset(
        DECLARATION.findall(source)
        + TOP_LEVEL_VARIABLE.findall(source)
        + TOP_LEVEL_CONSTANT.findall(source)
        + TYPE_DECLARATION.findall(source)
    )


def interface_family(symbol: str, definition_name: str = "") -> str:
    """Assign an exact unresolved symbol to one independently owned module."""
    value = f"{symbol} {definition_name}".lower()
    if re.search(r"\b(za|zt0|svcr|sme|streaming)", value):
        return "sme-state"
    if re.search(r"\b(ffr|sve|predicate|predicated|currentvl|max_vl)\b", value) or symbol in {
        "Z", "P", "_Z", "_P", "_FFR", "FFR"
    }:
        return "sve-state"
    if re.search(r"sysreg|systemregister", value):
        return "system-register-state"
    if re.search(r"fpcr|fpsr|fpmr|floatingpoint|fp(exception|trap)", value):
        return "fp-state-and-traps"
    if re.search(
        r"mem|address|exclusive|atomic|cache|translation|tlbi|tag|prefetch",
        value,
    ):
        return "memory-and-ordering"
    if re.search(
        r"exception|abort|fault|trap|endofdecode|branchto|branchnottaken|"
        r"breakpoint|barrier|event|hint_|wfi|wfe",
        value,
    ):
        return "control-and-events"
    if re.search(
        r"feature|pstate|currentel|usingaarch|elusing|haveel|secure|endian|"
        r"impdef|constrainunpredictable|implementation",
        value,
    ):
        return "execution-environment"
    if symbol == "_PC":
        return "core-state"
    return "implementation-choice"


@dataclass(frozen=True)
class Definition:
    name: str
    symbols: frozenset[str]
    links: frozenset[str]
    linked_dependencies: frozenset[str]
    calls: frozenset[str]
    identifiers: frozenset[str]
    text: str

    @property
    def is_impdef(self) -> bool:
        return IMPDEF.search(self.text) is not None

    @property
    def symbol(self) -> str:
        return sorted(self.symbols)[0]


@dataclass(frozen=True)
class DependencyStatus:
    direct_definitions: frozenset[str]
    transitive_definitions: frozenset[str]
    dependency_digest: str
    impdef_symbols: frozenset[str]
    external_symbols: frozenset[str]
    missing_interfaces: tuple[tuple[str, str], ...]
    official_operation_present: bool


class AslDependencyCatalog:
    """Dependency closure over one pinned A64 shared-pseudocode release."""

    def __init__(self, root: Path):
        self.root = root
        path = root / SHARED_FILE
        if not path.is_file():
            raise ValueError(f"official A64 XML lacks {SHARED_FILE}")
        tree = ET.parse(path).getroot()
        definitions: dict[str, Definition] = {}
        links: dict[str, set[str]] = defaultdict(set)
        symbols: dict[str, set[str]] = defaultdict(set)
        type_symbols: dict[str, set[str]] = defaultdict(set)
        constant_symbols: dict[str, set[str]] = defaultdict(set)
        variable_symbols: dict[str, set[str]] = defaultdict(set)
        for ps in tree.findall(".//ps"):
            name = ps.get("name", "")
            if not name or name in definitions:
                raise ValueError("shared pseudocode has a missing/duplicate definition name")
            node_links = frozenset(
                anchor.get("link", "")
                for anchor in ps.findall(".//anchor")
                if anchor.get("link")
            )
            linked_dependencies = frozenset(
                link.get("link", "")
                for link in ps.findall(".//a")
                if link.get("file") == SHARED_FILE and link.get("link")
            )
            text = "\n".join("".join(item.itertext()) for item in ps.findall("pstext"))
            node_symbols = _declared_symbols(
                text, name.rsplit("/", 1)[-1]
            )
            declared_types = frozenset(
                TYPE_DECLARATION.findall(_asl_without_comments(text))
            )
            declared_constants = frozenset(
                TOP_LEVEL_CONSTANT.findall(_asl_without_comments(text))
            )
            declared_variables = frozenset(
                TOP_LEVEL_VARIABLE.findall(_asl_without_comments(text))
            )
            definition = Definition(
                name=name,
                symbols=node_symbols,
                links=node_links,
                linked_dependencies=linked_dependencies,
                calls=frozenset(_calls(text) - set(node_symbols)),
                identifiers=frozenset(
                    _global_identifiers(text) - set(node_symbols)
                ),
                text=text,
            )
            definitions[name] = definition
            for symbol in node_symbols:
                symbols[symbol].add(name)
            for symbol in declared_types:
                type_symbols[symbol].add(name)
            for symbol in declared_constants:
                constant_symbols[symbol].add(name)
            for symbol in declared_variables:
                variable_symbols[symbol].add(name)
            for link in node_links:
                links[link].add(name)
        self.definitions = definitions
        self.links = {key: frozenset(value) for key, value in links.items()}
        self.symbols = {key: frozenset(value) for key, value in symbols.items()}
        self.type_symbols = {
            key: frozenset(value) for key, value in type_symbols.items()
        }
        self.constant_symbols = {
            key: frozenset(value) for key, value in constant_symbols.items()
        }
        self.variable_symbols = {
            key: frozenset(value) for key, value in variable_symbols.items()
        }
        self.lexical_symbols = frozenset(
            self.type_symbols.keys()
            | self.constant_symbols.keys()
            | self.variable_symbols.keys()
        )
        self.feature_symbols = frozenset(
            feature
            for xml_path in root.glob("*.xml")
            for feature in FEATURE_ENUM_VALUE.findall(
                xml_path.read_text(encoding="utf-8")
            )
        )
        self.pmu_symbols = frozenset(
            pmu
            for definition in self.definitions.values()
            for pmu in PMU_CONSTANT.findall(definition.text)
        )
        self._cache: dict[str, DependencyStatus] = {}

    def _feature_type_declaration(
        self, additional: frozenset[str] = frozenset()
    ) -> str:
        """Build ASLRef's missing Feature enumeration from the pinned XML."""
        if "Feature" in self.symbols:
            return ""
        features = sorted(self.feature_symbols | additional)
        if not features:
            raise ValueError("official A64 XML exposes no FEAT_ identifiers")
        members = "".join(f"    {feature},\n" for feature in features)
        return f"type Feature of enumeration {{\n{members}}};"

    def _pmu_typechecking_declarations(self) -> str:
        """Mirror ASLRef's generated zero-valued PMU typecheck constants."""
        return "\n".join(
            f"constant {symbol} = Zeros{{16}};"
            for symbol in sorted(self.pmu_symbols)
        )

    def _operation_source_chain(self, filename: str) -> tuple[str, ...]:
        """Resolve a blank alias form to the XML file carrying its operation."""
        chain: list[str] = []
        current = filename
        while True:
            if current in chain:
                cycle = " -> ".join([*chain, current])
                raise ValueError(f"official A64 XML alias cycle: {cycle}")
            chain.append(current)
            path = self.root / current
            if not path.is_file() or path.parent != self.root:
                raise ValueError(f"official A64 XML lacks indexed file {current}")
            root = ET.parse(path).getroot()
            form_text = "\n".join(
                "".join(item.itertext())
                for item in root.findall(".//ps_section//pstext")
            )
            alias = root.find("./aliasto")
            alias_target = "" if alias is None else alias.get("refiform", "")
            if form_text.strip() or not alias_target:
                return tuple(chain)
            current = alias_target

    def operation_source_file(self, filename: str) -> str:
        """Return the concrete XML source bundled for an instruction form."""
        return self._operation_source_chain(filename)[-1]

    def _definition_references(
        self, definition: Definition
    ) -> tuple[set[str], set[str]]:
        nodes: set[str] = set()
        external: set[str] = set()
        for link in definition.linked_dependencies:
            matches = self.links.get(link)
            if not matches:
                raise ValueError(f"shared dependency link {link!r} has no definition")
            nodes.update(matches)
        for call in definition.calls:
            matches = self.symbols.get(call)
            if matches:
                nodes.update(matches)
            elif call not in ASL_STANDARD_CALLS:
                external.add(call)
        for symbol in definition.identifiers & self.lexical_symbols:
            nodes.update(self.symbols[symbol])
        return nodes, external

    def shared_subset(
        self,
        status: DependencyStatus,
        supplied_interfaces: frozenset[str] = frozenset(),
        supplemental_sources: tuple[str, ...] = (),
    ) -> str:
        """Render official dependencies of the form and selected adapters."""
        excluded = PROVIDED_INTERFACE_SYMBOLS | supplied_interfaces
        reachable: set[str] = set()
        pending = list(status.direct_definitions)
        for source in supplemental_sources:
            references = _calls(source) | (
                _global_identifiers(source) & self.lexical_symbols
            )
            for symbol in references - excluded:
                pending.extend(self.symbols.get(symbol, frozenset()))
        while pending:
            name = pending.pop()
            if name in reachable:
                continue
            definition = self.definitions[name]
            if definition.symbols & excluded:
                continue
            reachable.add(name)
            dependencies, _unresolved = self._definition_references(definition)
            pending.extend(dependencies - reachable)
        pieces: list[str] = []
        feature_references = frozenset(
            feature
            for source in supplemental_sources
            for feature in FEATURE_ENUM_VALUE.findall(source)
        ) | frozenset(
            feature
            for name in reachable
            for feature in FEATURE_ENUM_VALUE.findall(
                self.definitions[name].text
            )
        )
        if feature_references or any(
            "Feature" in _global_identifiers(source)
            for source in supplemental_sources
        ):
            feature_type = self._feature_type_declaration(
                feature_references
            )
            if feature_type:
                pieces.append(feature_type)
        if any(
            PMU_CONSTANT.search(self.definitions[name].text)
            for name in reachable
        ):
            pieces.append(self._pmu_typechecking_declarations())
        for name in sorted(reachable):
            definition = self.definitions[name]
            pieces.append(definition.text.strip())
        return "\n\n".join(piece for piece in pieces if piece) + "\n"

    def _boundary_requirements(self, name: str) -> set[tuple[str, str]]:
        """Return independently supplied state/operation declarations."""
        definition = self.definitions[name]
        operation_symbols = frozenset(
            DECLARATION.findall(_asl_without_comments(definition.text))
        )
        result: set[tuple[str, str]] = set()
        for symbol in definition.symbols:
            if symbol in PROVIDED_INTERFACE_SYMBOLS:
                continue
            if definition.is_impdef or symbol in STATE_BOUNDARY_SYMBOLS:
                result.add((interface_family(symbol, name), symbol))
            elif symbol in operation_symbols and re.search(
                r"/(exceptions|memory|system|system128)/", name
            ) and re.search(
                r"(EndOfDecode|BranchTo|Abort|Barrier|Hint|TLBI|Exception|Trap)",
                symbol,
            ):
                result.add((interface_family(symbol, name), symbol))
        return result

    def for_file(self, filename: str) -> DependencyStatus:
        cached = self._cache.get(filename)
        if cached is not None:
            return cached
        path = self.root / filename
        if not path.is_file() or path.parent != self.root:
            raise ValueError(f"official A64 XML lacks indexed file {filename}")
        root = ET.parse(path).getroot()
        form_text = "\n".join(
            "".join(item.itertext())
            for item in root.findall(".//ps_section//pstext")
        )
        source_chain = self._operation_source_chain(filename)
        if len(source_chain) > 1:
            target = self.for_file(source_chain[-1])
            alias_digest = target.dependency_digest
            for alias_source, alias_target in reversed(
                tuple(zip(source_chain, source_chain[1:]))
            ):
                alias_digest = hashlib.sha256(
                    (
                        f"alias:{alias_source}->{alias_target}\0"
                        f"{alias_digest}"
                    ).encode()
                ).hexdigest()
            status = DependencyStatus(
                direct_definitions=target.direct_definitions,
                transitive_definitions=target.transitive_definitions,
                dependency_digest=alias_digest,
                impdef_symbols=target.impdef_symbols,
                external_symbols=target.external_symbols,
                missing_interfaces=target.missing_interfaces,
                official_operation_present=target.official_operation_present,
            )
            self._cache[filename] = status
            return status
        form_links = {
            link.get("link", "")
            for link in root.findall(".//ps_section//a")
            if link.get("file") == SHARED_FILE and link.get("link")
        }
        direct: set[str] = set()
        for link in form_links:
            matches = self.links.get(link)
            if not matches:
                raise ValueError(f"{filename}: dependency link {link!r} is unresolved")
            direct.update(matches)
        direct_external: set[str] = set()
        for call in _calls(form_text):
            matches = self.symbols.get(call)
            if matches:
                direct.update(matches)
            elif call not in ASL_STANDARD_CALLS:
                direct_external.add(call)
        for symbol in _global_identifiers(form_text) & self.lexical_symbols:
            direct.update(self.symbols[symbol])

        closure: set[str] = set()
        closure_external: set[str] = set(direct_external)
        pending = list(direct)
        while pending:
            name = pending.pop()
            if name in closure:
                continue
            closure.add(name)
            dependencies, unresolved = self._definition_references(
                self.definitions[name]
            )
            closure_external.update(unresolved)
            pending.extend(dependencies - closure)

        impdef = {
            symbol
            for name in closure
            if self.definitions[name].is_impdef
            for symbol in self.definitions[name].symbols
        }
        # Build the minimal deterministic-interface frontier. Once an adapter
        # boundary is reached, dependencies behind that boundary belong to the
        # same independently implementable module and are not duplicated in
        # every allocation's queue.
        missing: set[tuple[str, str]] = {
            (interface_family(symbol), symbol) for symbol in direct_external
        }
        frontier_seen: set[str] = set()
        frontier_pending = list(direct)
        while frontier_pending:
            name = frontier_pending.pop()
            if name in frontier_seen:
                continue
            frontier_seen.add(name)
            if self.definitions[name].symbols & PROVIDED_INTERFACE_SYMBOLS:
                continue
            boundary = self._boundary_requirements(name)
            if boundary:
                missing.update(boundary)
                continue
            dependencies, unresolved = self._definition_references(
                self.definitions[name]
            )
            missing.update(
                (interface_family(symbol), symbol) for symbol in unresolved
            )
            frontier_pending.extend(dependencies - frontier_seen)

        operation_present = bool(form_text.strip())
        if not operation_present:
            missing.add(("official-semantic-body", f"operation:{filename}"))

        digest_input = "\n".join(
            f"{name}\0{hashlib.sha256(self.definitions[name].text.encode()).hexdigest()}"
            for name in sorted(closure)
        )
        status = DependencyStatus(
            direct_definitions=frozenset(direct),
            transitive_definitions=frozenset(closure),
            dependency_digest=hashlib.sha256(digest_input.encode()).hexdigest(),
            impdef_symbols=frozenset(impdef),
            external_symbols=frozenset(closure_external),
            missing_interfaces=tuple(sorted(missing)),
            official_operation_present=operation_present,
        )
        self._cache[filename] = status
        return status
