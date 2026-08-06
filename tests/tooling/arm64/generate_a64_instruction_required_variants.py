#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate the official allocated A64 variant model from pinned Arm XML."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import itertools
import sys
from pathlib import Path

from a64_architecture_oracle import (
    SOURCE_ORACLE,
    ArchitectureCatalog,
    ArchitectureVariant,
    DecodeCatalog,
    feature_alternatives,
)
from a64_bounded_generation import (
    add_bounded_generation_arguments,
    workspace_from_options,
)
from a64_one_shot_publication import OneShotPublication
from generate_a64_instruction_allocations import archive_root
from a64_system_register_oracle import (
    SOURCE_ORACLE as SYSTEM_REGISTER_ORACLE,
    SystemRegisterCatalog,
)


COLUMNS = [
    "stable_id",
    "section",
    "architectural_form",
    "architecture_file",
    "architecture_encoding",
    "form_encoding_count",
    "architecture_mask",
    "architecture_value",
    "architecture_constraints",
    "decode_class",
    "decode_encoding",
    "decode_architecture_file",
    "decode_mask",
    "decode_value",
    "decode_constraints",
    "decode_feature_condition",
    "allocation_kind",
    "static_feature_expressions",
    "static_native_feature_alternatives",
    "static_native_fallback_features",
    "word_feature_rules",
    "word_feature_fallback_rules",
    "word_semantic_feature_rules",
    "word_semantic_feature_oracle",
    "semantic_native_features",
    "operation_digest",
    "alias_digest",
    "architecture_oracle",
]


def arguments() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--architecture-archive", type=Path, required=True)
    parser.add_argument("--system-register-archive", type=Path, required=True)
    parser.add_argument(
        "--output",
        type=Path,
        default=(
            repository
            / "tests/unit/arm64/a64_instruction_required_variants.tsv"
        ),
    )
    add_bounded_generation_arguments(parser)
    return parser.parse_args()


def format_alternatives(alternatives: set[frozenset[str]]) -> str:
    values = [
        "baseline" if not features else "+".join(sorted(features))
        for features in alternatives
    ]
    return "|".join(sorted(values))


def format_feature_conjunction(
    expressions: tuple[str, ...], architecture_file: str
) -> str:
    if not expressions:
        return "baseline"
    alternatives = feature_alternatives(expressions, architecture_file)
    features = set().union(*alternatives)
    return "+".join(sorted(features)) or "baseline"


def intersection_witness(variant: ArchitectureVariant, decode) -> int | None:
    if (variant.value ^ decode.value) & variant.mask & decode.mask:
        return None
    mask = variant.mask | decode.mask
    value = (variant.value & variant.mask) | (decode.value & decode.mask)
    constraints = variant.constraints + decode.constraints
    free_bits = sorted({
        bit
        for constraint in constraints
        for bit in range(constraint.low, constraint.low + constraint.width)
        if not mask & (1 << bit)
    })
    for settings in itertools.product((0, 1), repeat=len(free_bits)):
        word = value
        for bit, setting in zip(free_bits, settings):
            if setting:
                word |= 1 << bit
        if variant.matches(word) and decode.matches(word):
            return word
    return None


def generate(
    catalog: ArchitectureCatalog,
    system_register_catalog: SystemRegisterCatalog,
) -> list[dict[str, str]]:
    decode_rows = [
        row
        for row in DecodeCatalog(catalog.root).partitions()
        if row.kind == "allocated"
    ]
    decode_by_file: dict[str, list] = {}
    for row in decode_rows:
        decode_by_file.setdefault(row.file, []).append(row)
    rows: list[dict[str, str]] = []
    for stable_id, (section, form, filename) in sorted(
        catalog.stable_forms.items()
    ):
        variants = catalog.all_variants(section, form)
        operation_digest, alias_digest = catalog.form_metadata(section, form)
        semantic_features = catalog.form_semantic_features(section, form)
        alias_target = catalog.alias_target(section, form)
        for variant in variants:
            word_semantic_rules: tuple[str, ...] = ()
            word_semantic_oracle = "none"
            if filename in {"mrs.xml", "msr_reg.xml"}:
                word_semantic_rules = (
                    system_register_catalog.word_semantic_feature_rules()
                )
                word_semantic_oracle = SYSTEM_REGISTER_ORACLE
            decodes = [
                row
                for row in decode_by_file.get(filename, [])
                if row.name == variant.encoding
                and intersection_witness(variant, row) is not None
            ]
            allocation_kind = "primary"
            if not decodes:
                allocation_kind = "alias"
                decodes = [
                    row
                    for row in decode_by_file.get(alias_target, [])
                    if intersection_witness(variant, row) is not None
                ]
            if not decodes:
                raise ValueError(
                    f"{stable_id} {variant.encoding}: maps to no official "
                    "decode-table partition"
                )
            constraints = ";".join(
                constraint.text() for constraint in variant.constraints
            )
            for decode in decodes:
                feature_expressions = tuple(dict.fromkeys(
                    variant.feature_expressions + decode.feature_expressions
                ))
                rows.append(
                {
                    "stable_id": stable_id,
                    "section": section,
                    "architectural_form": form,
                    "architecture_file": filename,
                    "architecture_encoding": variant.encoding,
                    "form_encoding_count": str(len(variants)),
                    "architecture_mask": f"{variant.mask:08x}",
                    "architecture_value": f"{variant.value:08x}",
                    "architecture_constraints": constraints or "none",
                    "decode_class": decode.decode_class,
                    "decode_encoding": decode.name,
                    "decode_architecture_file": decode.file,
                    "decode_mask": f"{decode.mask:08x}",
                    "decode_value": f"{decode.value:08x}",
                    "decode_constraints": ";".join(
                        constraint.text() for constraint in decode.constraints
                    )
                    or "none",
                    "decode_feature_condition": ";".join(
                        decode.feature_expressions
                    )
                    or "baseline",
                    "allocation_kind": allocation_kind,
                    "static_feature_expressions": ";".join(
                        feature_expressions
                    )
                    or "baseline",
                    "static_native_feature_alternatives": format_alternatives(
                        feature_alternatives(
                            feature_expressions, variant.file
                        )
                    ),
                    "static_native_fallback_features": (
                        format_feature_conjunction(
                            variant.feature_fallbacks, variant.file
                        )
                    ),
                    "word_feature_rules": ";".join(variant.feature_rules)
                    or "none",
                    "word_feature_fallback_rules": ";".join(
                        variant.feature_fallback_rules
                    )
                    or "none",
                    "word_semantic_feature_rules": ";".join(
                        word_semantic_rules
                    )
                    or "none",
                    "word_semantic_feature_oracle": word_semantic_oracle,
                    "semantic_native_features": "+".join(
                        semantic_features
                    )
                    or "baseline",
                    "operation_digest": operation_digest,
                    "alias_digest": alias_digest,
                    "architecture_oracle": SOURCE_ORACLE,
                }
            )
    rows.sort(
        key=lambda row: (
            row["stable_id"],
            row["architecture_encoding"],
            row["architecture_mask"],
            row["architecture_value"],
            row["architecture_constraints"],
        )
    )
    return rows


def main() -> int:
    options = arguments()
    publication = OneShotPublication.prepare(
        [options.output],
        [options.architecture_archive, options.system_register_archive],
    )
    with workspace_from_options(
        options,
        source_root=Path(__file__).resolve().parents[3],
        publication_targets=publication.targets,
        prefix="a64-required-variants-",
    ) as workspace:
        catalog = ArchitectureCatalog(
            archive_root(
                options.architecture_archive,
                workspace.require_path() / "archive",
                workspace,
            )
        )
        system_register_catalog = SystemRegisterCatalog(
            options.system_register_archive
        )
        rows = generate(catalog, system_register_catalog)
    if len({row["stable_id"] for row in rows}) != 983:
        raise ValueError("official required model does not cover 983 stable IDs")
    publication.publish_tsvs([(publication.targets[0], COLUMNS, rows)])
    print(
        f"A64 official required variants generated: {len(rows)} allocated "
        f"encodings across 983 stable IDs; wrote {options.output}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        sys.exit(1)
