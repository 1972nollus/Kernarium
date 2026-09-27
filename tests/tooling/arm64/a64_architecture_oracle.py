#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Arm A64 XML form/allocation access for exact instruction evidence.

This module deliberately reads Arm's machine-readable release rather than
deriving architectural forms from the local inventory or execution bindings.
Only information present in the XML is exposed here; semantic result contracts
remain a separate, explicitly incomplete review artifact.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


RELEASE = "2026-06"
COMMIT_ID = "2026-06_rel"
ARCHIVE_NAME = f"ISA_A64_xml_A_profile-{RELEASE}.tar.gz"
ARCHIVE_SHA256 = "63a01a1696483bbe2edfef9e0f0cd053d6c1c619ec0587876cb7a60bb344f354"
SOURCE_ORACLE = f"arm-a-profile-{RELEASE}-xml-{COMMIT_ID}"
INDEXES = {"Base": ("index.xml", 516), "FP&SIMD": ("fpsimdindex.xml", 467)}
STABLE_ID_PREFIXES = {"Base": "BASE", "FP&SIMD": "FPSIMD"}
SIMPLE_CONSTRAINT = re.compile(r"(!=|==) ([01x]+)\Z")
FEATURE_EXPRESSION = re.compile(
    r"FEAT_[A-Za-z0-9_]+(?: (?:&&|\|\|) FEAT_[A-Za-z0-9_]+)*\Z"
)
FEATURE_PREDICATE = re.compile(
    r"IsFeatureImplemented\((FEAT_[A-Za-z0-9_]+)\)"
)
FEATURE_NAME = re.compile(r"\b(FEAT_[A-Za-z0-9_]+)\b")
FEATURE_NOP_GUARD = re.compile(
    r"if\s+!IsFeatureImplemented\((FEAT_[A-Za-z0-9_]+)\)\s+then\s+"
    r"EndOfDecode\(Decode_NOP\);\s+end;"
)


@dataclass(frozen=True)
class FieldConstraint:
    name: str
    low: int
    width: int
    expression: str

    def matches(self, word: int) -> bool:
        match = SIMPLE_CONSTRAINT.fullmatch(self.expression)
        if match is None or len(match.group(2)) != self.width:
            raise ValueError(
                f"unsupported Arm XML constraint {self.name} "
                f"{self.expression!r}"
            )
        actual = f"{(word >> self.low) & ((1 << self.width) - 1):0{self.width}b}"
        equal = all(
            expected == "x" or expected == observed
            for expected, observed in zip(match.group(2), actual)
        )
        return equal if match.group(1) == "==" else not equal

    def text(self) -> str:
        return (
            f"{self.name}@{self.low}:{self.width}"
            f"{self.expression.replace(' ', '')}"
        )


@dataclass(frozen=True)
class ArchitectureVariant:
    section: str
    form: str
    file: str
    encoding: str
    mask: int
    value: int
    constraints: tuple[FieldConstraint, ...]
    feature_expressions: tuple[str, ...]
    feature_rules: tuple[str, ...]
    feature_fallbacks: tuple[str, ...]
    feature_fallback_rules: tuple[str, ...]
    fields: tuple[tuple[str, int, int], ...]

    def matches(self, word: int) -> bool:
        return word & self.mask == self.value and all(
            constraint.matches(word) for constraint in self.constraints
        )


@dataclass(frozen=True)
class DecodePartition:
    """One exact predicate from the official encoding-index decode tables."""

    layer: str
    kind: str
    decode_class: str
    name: str
    file: str
    mask: int
    value: int
    constraints: tuple[FieldConstraint, ...]
    feature_expressions: tuple[str, ...]

    def matches(self, word: int) -> bool:
        return word & self.mask == self.value and all(
            constraint.matches(word) for constraint in self.constraints
        )


def _box_bits(box: ET.Element) -> list[tuple[int, str]]:
    width = int(box.get("width", "1"))
    cells: list[str] = []
    for cell in box.findall("c"):
        cells.extend(
            [(cell.text or "").strip()] * int(cell.get("colspan", "1"))
        )
    if len(cells) != width:
        raise ValueError(
            f"Arm XML box at bit {box.get('hibit')} declares width {width} "
            f"but contains {len(cells)} bits"
        )
    high = int(box.get("hibit", "-1"))
    if high < 0 or high - width + 1 < 0 or high > 31:
        raise ValueError("Arm XML contains an out-of-range encoding box")
    return [(high - index, setting) for index, setting in enumerate(cells)]


def _add_boxes(
    mask: int, value: int, boxes: list[ET.Element]
) -> tuple[int, int, dict[str, tuple[int, int]], list[FieldConstraint]]:
    fields: dict[str, tuple[int, int]] = {}
    constraints: list[FieldConstraint] = []
    for box in boxes:
        high = int(box.get("hibit", "-1"))
        width = int(box.get("width", "1"))
        name = box.get("name", "")
        if name:
            fields[name] = (high - width + 1, width)
        expression = box.get("constraint")
        if expression is not None:
            if not name:
                raise ValueError("Arm XML constraint has no named bit field")
            constraints.append(
                FieldConstraint(name, high - width + 1, width, expression)
            )
        for bit, setting in _box_bits(box):
            if setting not in {"0", "1"}:
                continue
            bit_mask = 1 << bit
            wanted = bit_mask if setting == "1" else 0
            if mask & bit_mask and value & bit_mask != wanted:
                raise ValueError("Arm XML contains conflicting fixed bits")
            mask |= bit_mask
            value = (value & ~bit_mask) | wanted
    return mask, value, fields, constraints


def _declared_features(element: ET.Element) -> list[str]:
    result: list[str] = []
    for variant in element.findall("./arch_variants/arch_variant"):
        feature = variant.get("feature")
        if feature:
            if FEATURE_EXPRESSION.fullmatch(feature) is None:
                raise ValueError(
                    f"unsupported Arm XML feature expression {feature!r}"
                )
            result.append(feature)
    return result


def _decode_box_patterns(box: ET.Element) -> tuple[str, str | None]:
    """Return a normalized decode pattern and optional != expression."""
    width = int(box.get("width", "1"))
    pieces: list[str] = []
    inequality: str | None = None
    for cell in box.findall("c"):
        span = int(cell.get("colspan", "1"))
        text = (cell.text or "").strip()
        if text.startswith("!= "):
            if len(box.findall("c")) != 1 or span != width:
                raise ValueError("Arm XML != decode cell is not a whole box")
            inequality = text
            pieces.append("x" * span)
        elif not text:
            pieces.append("x" * span)
        elif re.fullmatch(r"[01x]+", text) is not None and len(text) == span:
            pieces.append(text)
        else:
            raise ValueError(f"unsupported Arm XML decode cell {text!r}")
    pattern = "".join(pieces)
    if len(pattern) != width:
        raise ValueError("Arm XML decode box width does not match its cells")
    return pattern, inequality


def _add_decode_boxes(
    mask: int,
    value: int,
    constraints: list[FieldConstraint],
    boxes: list[ET.Element],
) -> tuple[int, int, list[FieldConstraint]]:
    """Conjoin hierarchy decode boxes with an accumulated predicate."""
    result = list(constraints)
    for box in boxes:
        high = int(box.get("hibit", "-1"))
        width = int(box.get("width", "1"))
        low = high - width + 1
        pattern, inequality = _decode_box_patterns(box)
        name = box.get("name", f"bits_{high}_{low}")
        if inequality is not None:
            result.append(FieldConstraint(name, low, width, inequality))
        for offset, setting in enumerate(reversed(pattern)):
            if setting == "x":
                continue
            bit_mask = 1 << (low + offset)
            wanted = bit_mask if setting == "1" else 0
            if mask & bit_mask and value & bit_mask != wanted:
                raise ValueError("Arm XML contains conflicting decode bits")
            mask |= bit_mask
            value = (value & ~bit_mask) | wanted
    return mask, value, result


def _add_table_cells(
    mask: int,
    value: int,
    constraints: list[FieldConstraint],
    fields: dict[str, tuple[int, int]],
    names: list[str],
    cells: list[ET.Element],
) -> tuple[int, int, list[FieldConstraint]]:
    result = list(constraints)
    for name, cell in zip(names, cells):
        if name not in fields:
            raise ValueError(f"encoding-index table uses unknown field {name!r}")
        low, width = fields[name]
        text = "".join(cell.itertext()).strip()
        if not text:
            continue
        if text.startswith("!= "):
            pattern = text[3:]
            if len(pattern) != width or re.fullmatch(r"[01x]+", pattern) is None:
                raise ValueError(f"unsupported table constraint {text!r}")
            result.append(FieldConstraint(name, low, width, text))
            continue
        if len(text) != width or re.fullmatch(r"[01x]+", text) is None:
            raise ValueError(f"unsupported encoding-index pattern {text!r}")
        for offset, setting in enumerate(reversed(text)):
            if setting == "x":
                continue
            bit_mask = 1 << (low + offset)
            wanted = bit_mask if setting == "1" else 0
            if mask & bit_mask and value & bit_mask != wanted:
                raise ValueError("encoding-index row conflicts with its diagram")
            mask |= bit_mask
            value = (value & ~bit_mask) | wanted
    return mask, value, result


def _matching_table_features(
    root: ET.Element, fields: dict[str, tuple[int, int]], word: int
) -> list[str]:
    """Return feature expressions selected by word-indexed value tables."""
    result: list[str] = []
    for table in root.findall(".//table[@class='valuetable']"):
        header = table.find("./tgroup/thead/row")
        body = table.find("./tgroup/tbody")
        if header is None or body is None:
            continue
        headers = header.findall("entry")
        names = ["".join(entry.itertext()).strip() for entry in headers]
        bit_columns = [
            (index, names[index])
            for index, entry in enumerate(headers)
            if entry.get("class") == "bitfield" and names[index] in fields
        ]
        if not bit_columns:
            continue
        for table_row in body.findall("row"):
            entries = table_row.findall("entry")
            if len(entries) != len(names):
                continue
            matched = True
            for index, name in bit_columns:
                low, width = fields[name]
                pattern = "".join(entries[index].itertext()).strip()
                if len(pattern) != width or re.fullmatch(r"[01x]+", pattern) is None:
                    matched = False
                    break
                actual = (
                    f"{(word >> low) & ((1 << width) - 1):0{width}b}"
                )
                if any(
                    expected != "x" and expected != observed
                    for expected, observed in zip(pattern, actual)
                ):
                    matched = False
                    break
            if not matched:
                continue
            for entry in entries:
                for variant in entry.findall(".//arch_variant"):
                    feature = variant.get("feature")
                    if feature:
                        if FEATURE_EXPRESSION.fullmatch(feature) is None:
                            raise ValueError(
                                "unsupported Arm XML feature expression "
                                f"{feature!r}"
                            )
                        result.append(feature)
    return result


def _table_feature_rules(
    root: ET.Element, fields: dict[str, tuple[int, int]]
) -> list[str]:
    """Return canonical word-dependent feature rules from Arm value tables."""
    result: list[str] = []
    for table in root.findall(".//table[@class='valuetable']"):
        header = table.find("./tgroup/thead/row")
        body = table.find("./tgroup/tbody")
        if header is None or body is None:
            continue
        headers = header.findall("entry")
        names = ["".join(entry.itertext()).strip() for entry in headers]
        bit_columns = [
            (index, names[index])
            for index, entry in enumerate(headers)
            if entry.get("class") == "bitfield" and names[index] in fields
        ]
        if not bit_columns:
            continue
        for table_row in body.findall("row"):
            entries = table_row.findall("entry")
            if len(entries) != len(names):
                continue
            conditions: list[str] = []
            valid = True
            for index, name in bit_columns:
                low, width = fields[name]
                pattern = "".join(entries[index].itertext()).strip()
                if len(pattern) != width or re.fullmatch(r"[01x]+", pattern) is None:
                    valid = False
                    break
                conditions.append(f"{name}@{low}:{width}={pattern}")
            if not valid:
                continue
            for entry in entries:
                for variant in entry.findall(".//arch_variant"):
                    feature = variant.get("feature")
                    if feature:
                        if FEATURE_EXPRESSION.fullmatch(feature) is None:
                            raise ValueError(
                                "unsupported Arm XML feature expression "
                                f"{feature!r}"
                            )
                        result.append("&".join(conditions) + "=>" + feature)
    return list(dict.fromkeys(result))


def _feature_names(expression: str) -> set[str]:
    return set(FEATURE_NAME.findall(expression))


def _decode_feature_policy(
    instruction_class: ET.Element,
) -> tuple[set[str], set[str]]:
    """Return direct feature predicates and architected NOP fallbacks.

    A feature mentioned by instruction metadata is not necessarily required
    for safe native execution.  The official decode ASL explicitly turns a
    small family of optional instructions into NOP when their feature is
    absent.  Keep that outcome distinct from Decode_UNDEF feature guards.
    """
    text = "\n".join(
        "".join(element.itertext())
        for element in instruction_class.findall(
            ".//pstext[@section='Decode']"
        )
    )
    predicates = set(FEATURE_PREDICATE.findall(text))
    nop_fallbacks = set(FEATURE_NOP_GUARD.findall(text))
    return predicates, nop_fallbacks


class ArchitectureCatalog:
    """Validated index and instruction forms from the pinned Arm XML release."""

    def __init__(self, root: Path):
        self.root = root
        self._files: dict[tuple[str, str], str] = {}
        self._stable_forms: dict[str, tuple[str, str, str]] = {}
        self._trees: dict[str, ET.Element] = {}
        self._shared_feature_dependencies: dict[str, frozenset[str]] | None = None
        self._semantic_features: dict[str, tuple[str, ...]] = {}
        for section, (index_name, expected_count) in INDEXES.items():
            index_path = root / index_name
            if not index_path.is_file():
                raise ValueError(f"Arm XML release lacks {index_name}")
            entries = ET.parse(index_path).getroot().findall(".//iform")
            if len(entries) != expected_count:
                raise ValueError(
                    f"{index_path}: contains {len(entries)} forms, "
                    f"expected {expected_count} for {RELEASE}"
                )
            for ordinal, entry in enumerate(entries, start=1):
                heading = entry.get("heading")
                filename = entry.get("iformfile")
                if not heading or not filename:
                    raise ValueError(f"{index_path}: malformed form index entry")
                key = (section, heading)
                if key in self._files:
                    raise ValueError(f"{index_path}: duplicate form {heading!r}")
                self._files[key] = filename
                stable_id = (
                    f"{STABLE_ID_PREFIXES[section]}-{ordinal:04d}"
                )
                self._stable_forms[stable_id] = (
                    section,
                    heading,
                    filename,
                )

    @property
    def forms(self) -> set[tuple[str, str]]:
        return set(self._files)

    @property
    def stable_forms(self) -> dict[str, tuple[str, str, str]]:
        """Return stable IDs derived solely from official index order."""
        return dict(self._stable_forms)

    def stable_form(self, stable_id: str) -> tuple[str, str, str]:
        try:
            return self._stable_forms[stable_id]
        except KeyError as error:
            raise ValueError(
                f"Arm {RELEASE} XML has no indexed stable ID {stable_id!r}"
            ) from error

    def _tree(self, filename: str) -> ET.Element:
        cached = self._trees.get(filename)
        if cached is not None:
            return cached
        path = self.root / filename
        if not path.is_file() or path.parent != self.root:
            raise ValueError(f"Arm XML release lacks indexed file {filename}")
        root = ET.parse(path).getroot()
        commit_ids = {item.text for item in root.findall(".//commit_id")}
        if commit_ids != {COMMIT_ID}:
            raise ValueError(
                f"{path}: commit IDs are {sorted(str(x) for x in commit_ids)!r}, "
                f"expected {COMMIT_ID!r}"
            )
        self._trees[filename] = root
        return root

    def encoding_count(self, section: str, form: str) -> int:
        filename = self._files.get((section, form))
        if filename is None:
            raise ValueError(
                f"Arm {RELEASE} XML has no {section} form {form!r}"
            )
        count = len(self._tree(filename).findall("./classes/iclass/encoding"))
        if count == 0:
            raise ValueError(f"{filename}: form contains no encodings")
        return count

    def all_variants(self, section: str, form: str) -> list[ArchitectureVariant]:
        filename = self._files.get((section, form))
        if filename is None:
            raise ValueError(
                f"Arm {RELEASE} XML has no {section} form {form!r}"
            )
        root = self._tree(filename)
        result: list[ArchitectureVariant] = []
        is_alias = root.find("./aliasto") is not None
        for instruction_class in root.findall("./classes/iclass"):
            diagram = instruction_class.find("regdiagram")
            if diagram is None:
                continue
            mask, value, fields, constraints = _add_boxes(
                0, 0, diagram.findall("box")
            )
            class_features = _declared_features(instruction_class)
            decode_features, nop_fallbacks = _decode_feature_policy(
                instruction_class
            )
            for encoding in instruction_class.findall("encoding"):
                (
                    encoding_mask,
                    encoding_value,
                    encoding_fields,
                    encoding_constraints,
                ) = _add_boxes(mask, value, encoding.findall("box"))
                all_fields = dict(fields)
                all_fields.update(encoding_fields)
                all_constraints = tuple(constraints + encoding_constraints)
                features = tuple(
                    dict.fromkeys(class_features + _declared_features(encoding))
                )
                feature_rules = tuple(_table_feature_rules(root, all_fields))
                feature_fallbacks = tuple(sorted(
                    feature
                    for expression in features
                    for feature in _feature_names(expression)
                    if feature in nop_fallbacks
                ))
                feature_fallback_rules: tuple[str, ...] = ()
                if not is_alias:
                    feature_fallback_rules = tuple(
                        rule
                        for rule in feature_rules
                        if _feature_names(rule.split("=>", 1)[1]).isdisjoint(
                            decode_features - nop_fallbacks
                        )
                    )
                variant = ArchitectureVariant(
                    section=section,
                    form=form,
                    file=filename,
                    encoding=encoding.get("name", ""),
                    mask=encoding_mask,
                    value=encoding_value,
                    constraints=all_constraints,
                    feature_expressions=features,
                    feature_rules=feature_rules,
                    feature_fallbacks=feature_fallbacks,
                    feature_fallback_rules=feature_fallback_rules,
                    fields=tuple(
                        sorted(
                            (name, low, width)
                            for name, (low, width) in all_fields.items()
                        )
                    ),
                )
                if not variant.encoding:
                    raise ValueError(f"{filename}: encoding has no name")
                result.append(variant)
        return result

    def variants(self, section: str, form: str, word: int) -> list[ArchitectureVariant]:
        result: list[ArchitectureVariant] = []
        root = self._tree(self._files[(section, form)])
        for variant in self.all_variants(section, form):
            if not variant.matches(word):
                continue
            dynamic = _matching_table_features(
                root,
                {name: (low, width) for name, low, width in variant.fields},
                word,
            )
            result.append(
                ArchitectureVariant(
                    section=variant.section,
                    form=variant.form,
                    file=variant.file,
                    encoding=variant.encoding,
                    mask=variant.mask,
                    value=variant.value,
                    constraints=variant.constraints,
                    feature_expressions=tuple(
                        dict.fromkeys(
                            list(variant.feature_expressions) + dynamic
                        )
                    ),
                    feature_rules=variant.feature_rules,
                    feature_fallbacks=variant.feature_fallbacks,
                    feature_fallback_rules=variant.feature_fallback_rules,
                    fields=variant.fields,
                )
            )
        return result

    def _shared_features(self) -> dict[str, frozenset[str]]:
        """Return transitive feature predicates for shared ASL definitions."""
        if self._shared_feature_dependencies is not None:
            return self._shared_feature_dependencies
        path = self.root / "shared_pseudocode.xml"
        if not path.is_file():
            raise ValueError("Arm XML release lacks shared_pseudocode.xml")
        root = ET.parse(path).getroot()
        direct: dict[str, set[str]] = {}
        calls: dict[str, set[str]] = {}
        for pseudocode in root.findall(".//ps"):
            definitions = {
                anchor.get("link", "")
                for anchor in pseudocode.findall(".//anchor")
                if anchor.get("link")
            }
            if not definitions:
                continue
            text = "".join(pseudocode.itertext())
            features = {
                _host_feature(match)
                for match in FEATURE_PREDICATE.findall(text)
            }
            dependencies = {
                link.get("link", "")
                for link in pseudocode.findall(".//a")
                if link.get("file") == "shared_pseudocode.xml"
                and link.get("link")
            }
            for definition in definitions:
                direct.setdefault(definition, set()).update(features)
                calls.setdefault(definition, set()).update(dependencies)

        closure = {name: set(features) for name, features in direct.items()}
        changed = True
        while changed:
            changed = False
            for name, dependencies in calls.items():
                expanded = set(closure.get(name, set()))
                for dependency in dependencies:
                    expanded.update(closure.get(dependency, set()))
                if expanded != closure.get(name, set()):
                    closure[name] = expanded
                    changed = True
        self._shared_feature_dependencies = {
            name: frozenset(features) for name, features in closure.items()
        }
        return self._shared_feature_dependencies

    def form_semantic_features(
        self, section: str, form: str
    ) -> tuple[str, ...]:
        """Return feature predicates reachable from a form's official ASL.

        This is deliberately a conservative dependency set, not an assertion
        that every feature affects every encoding or operand value.  It is
        suitable for proving that an extra native gate is specification-
        related, while allocation requirements remain exact and separate.
        """
        filename = self._files.get((section, form))
        if filename is None:
            raise ValueError(f"Arm {RELEASE} XML has no {section} form {form!r}")
        cached = self._semantic_features.get(filename)
        if cached is not None:
            return cached
        root = self._tree(filename)
        sections = root.findall(".//ps_section")
        text = "\n".join("".join(element.itertext()) for element in sections)
        features = {
            _host_feature(match) for match in FEATURE_PREDICATE.findall(text)
        }
        # Feature metadata and surrounding architectural prose can affect an
        # operand's semantics even when the decode ASL does not make the raw
        # encoding UNDEFINED (PRFMSLC and MTE2 are examples).  This remains a
        # conservative relevance set, never an allocation requirement.
        document_text = "\n".join(root.itertext())
        features.update(
            _host_feature(match) for match in FEATURE_NAME.findall(document_text)
        )
        shared = self._shared_features()
        for section_element in sections:
            for link in section_element.findall(".//a"):
                if link.get("file") == "shared_pseudocode.xml":
                    features.update(shared.get(link.get("link", ""), ()))
        alias = self.alias_target(section, form)
        if alias and alias != filename:
            alias_key = next(
                (
                    key
                    for key, candidate in self._files.items()
                    if candidate == alias
                ),
                None,
            )
            if alias_key is not None:
                features.update(self.form_semantic_features(*alias_key))
        result = tuple(sorted(features))
        self._semantic_features[filename] = result
        return result

    def alias_target(self, section: str, form: str) -> str:
        """Return the official target file for an alias form, if any."""
        filename = self._files.get((section, form))
        if filename is None:
            raise ValueError(f"Arm {RELEASE} XML has no {section} form {form!r}")
        element = self._tree(filename).find("./aliasto")
        return "" if element is None else element.get("refiform", "")

    def resolve(self, section: str, form: str, word: int) -> ArchitectureVariant:
        matches = self.variants(section, form, word)
        if not matches:
            raise ValueError(
                f"{section} {form}: {word:08x} matches no Arm {RELEASE} "
                "XML encoding"
            )
        specificity = max(variant.mask.bit_count() for variant in matches)
        matches = [
            variant for variant in matches if variant.mask.bit_count() == specificity
        ]
        identities = {
            (
                variant.encoding,
                variant.mask,
                variant.value,
                variant.constraints,
                variant.feature_expressions,
            )
            for variant in matches
        }
        if len(identities) != 1:
            names = ", ".join(sorted(variant.encoding for variant in matches))
            raise ValueError(
                f"{section} {form}: {word:08x} ambiguously matches Arm XML "
                f"encodings {names}"
            )
        return matches[0]


class DecodeCatalog:
    """Official A64 decode hierarchy and terminal instruction-table rows."""

    def __init__(self, root: Path):
        path = root / "encodingindex.xml"
        if not path.is_file():
            raise ValueError("Arm XML release lacks encodingindex.xml")
        self.root = ET.parse(path).getroot()
        if self.root.get("instructionset") != "A64":
            raise ValueError("encodingindex.xml is not the A64 index")

    def _hierarchy_partitions(self) -> list[DecodePartition]:
        hierarchy = self.root.find("./hierarchy")
        if hierarchy is None:
            raise ValueError("encodingindex.xml lacks its decode hierarchy")
        result: list[DecodePartition] = []

        def visit(
            node: ET.Element,
            mask: int,
            value: int,
            constraints: list[FieldConstraint],
        ) -> None:
            decode = node.find("decode")
            if decode is None:
                raise ValueError("decode hierarchy node lacks decode boxes")
            mask, value, constraints = _add_decode_boxes(
                mask, value, constraints, decode.findall("box")
            )
            children = node.findall("node")
            if children:
                for child in children:
                    visit(child, mask, value, constraints)
                return
            name = node.get("iclass", "")
            if not name:
                raise ValueError("decode hierarchy leaf has no identity")
            kind = (
                "unallocated"
                if node.get("unallocated") == "1"
                else "instruction-class"
            )
            result.append(
                DecodePartition(
                    layer="hierarchy",
                    kind=kind,
                    decode_class=name,
                    name=name,
                    file="none",
                    mask=mask,
                    value=value,
                    constraints=tuple(constraints),
                    feature_expressions=(),
                )
            )

        for node in hierarchy.findall("node"):
            visit(node, 0, 0, [])
        return result

    def _table_partitions(self) -> list[DecodePartition]:
        result: list[DecodePartition] = []
        for section in self.root.findall("./iclass_sect"):
            decode_class = section.get("id", "")
            diagram = section.find("regdiagram")
            table = section.find("instructiontable")
            if not decode_class or diagram is None or table is None:
                raise ValueError("malformed encoding-index instruction class")
            mask, value, fields, constraints = _add_boxes(
                0, 0, diagram.findall("box")
            )
            headings = table.findall(
                "./thead/tr[@id='heading2']/th[@class='bitfields']"
            )
            names = ["".join(item.itertext()).strip() for item in headings]
            for row in table.findall("./tbody/tr"):
                cells = row.findall("td[@class='bitfield']")
                if len(cells) != len(names):
                    raise ValueError(
                        f"{decode_class}: row has {len(cells)} fields, "
                        f"expected {len(names)}"
                    )
                row_mask, row_value, row_constraints = _add_table_cells(
                    mask, value, constraints, fields, names, cells
                )
                name = row.get("encname", "")
                if not name:
                    raise ValueError(f"{decode_class}: table row has no name")
                filename = row.get("iformfile", "")
                if filename:
                    kind = "allocated"
                elif row.get("undef") == "1":
                    kind = "unallocated"
                    filename = "none"
                else:
                    raise ValueError(
                        f"{decode_class} {name}: unclassified table row"
                    )
                feature = row.get("arch_version", "")
                if feature and any(character in feature for character in "\t\r\n"):
                    raise ValueError(
                        f"unsupported table feature expression {feature!r}"
                    )
                result.append(
                    DecodePartition(
                        layer="instruction-table",
                        kind=kind,
                        decode_class=decode_class,
                        name=name,
                        file=filename,
                        mask=row_mask,
                        value=row_value,
                        constraints=tuple(dict.fromkeys(row_constraints)),
                        feature_expressions=(feature,) if feature else (),
                    )
                )
        return result

    def partitions(self) -> list[DecodePartition]:
        result = self._hierarchy_partitions() + self._table_partitions()
        identities = {
            (
                item.layer,
                item.decode_class,
                item.name,
                item.file,
                item.mask,
                item.value,
                item.constraints,
            )
            for item in result
        }
        if len(identities) != len(result):
            raise ValueError("encoding-index contains duplicate decode partitions")
        return result


def feature_alternatives(
    expressions: tuple[str, ...], architecture_file: str = ""
) -> set[frozenset[str]]:
    """Convert the conjunction of Arm feature expressions to host alternatives."""
    alternatives: set[frozenset[str]] = {frozenset()}
    for expression in expressions:
        disjunction = expression.split(" || ")
        terms: list[frozenset[str]] = []
        for term in disjunction:
            names = term.split(" && ")
            terms.append(frozenset(_host_feature(name) for name in names))
        alternatives = {
            left | right for left in alternatives for right in terms
        }
    return {
        _runtime_feature_closure(features, architecture_file)
        for features in alternatives
    }


def _host_feature(feature: str) -> str:
    if not feature.startswith("FEAT_"):
        raise ValueError(f"invalid Arm feature name {feature!r}")
    name = feature[5:]
    exceptions = {
        "AdvSIMD": "advanced_simd",
        "DotProd": "dotprod",
        "FP": "floating_point",
        "FPMR": "fp8",
        "FlagM": "flag_m",
        "FlagM2": "flag_m2",
        "PAuth": "pauth",
        "PAuth_LR": "pauth_lr",
        "WFxT": "wfxt",
    }
    return exceptions.get(name, name.lower())


def _runtime_feature_closure(
    features: frozenset[str], architecture_file: str
) -> frozenset[str]:
    """Map architectural extension requirements to exact runtime feature gates.

    These additions are reviewed host-gating dependencies. They are fixed code,
    independent of the inventory, bindings, and emitted execution evidence.
    """
    result = set(features)
    if "aes" in result:
        result.add("advanced_simd")
    if "rdm" in result:
        result.add("advanced_simd")
    if "sha3" in result:
        result.add("advanced_simd")
    if "fcma" in result:
        result.add("advanced_simd")
    if "fp16" in result:
        if "_advsimd" in architecture_file:
            result.add("advanced_simd")
        elif "_float" in architecture_file:
            result.add("floating_point")
        else:
            raise ValueError(
                "FEAT_FP16 form has no reviewed runtime parent: "
                f"{architecture_file}"
            )
    if "frintts" in result:
        if architecture_file.endswith("_advsimd.xml"):
            result.add("advanced_simd")
        elif architecture_file.endswith("_float.xml"):
            result.add("floating_point")
        else:
            raise ValueError(
                "FEAT_FRINTTS form has no reviewed runtime parent: "
                f"{architecture_file}"
            )
    return frozenset(result)
