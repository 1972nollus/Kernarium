#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Canonical identity for the frozen A64 aggregate manifest."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import json
import re
from collections import Counter
from pathlib import Path
from typing import Mapping, Sequence


IDENTITY_SCHEMA = "uml.a64.aggregate-manifest-identity/v1"
MANIFEST_SCHEMA = "uml.a64.instruction-aggregate-manifest/v1"
AGGREGATE_ID = re.compile(r"aggregate-[0-9a-f]{20}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")


def canonical_json(value: object) -> str:
    return json.dumps(
        value,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    )


def _framed_update(digest: object, payload: bytes) -> None:
    digest.update(len(payload).to_bytes(8, "big"))  # type: ignore[attr-defined]
    digest.update(payload)  # type: ignore[attr-defined]


def aggregate_manifest_identity(
    rows: Sequence[Mapping[str, str]], columns: Sequence[str]
) -> dict[str, object]:
    """Return the exact canonical identity of a parsed aggregate manifest."""
    if not rows:
        raise ValueError("aggregate manifest is empty")
    identifiers = [row["aggregate_id"] for row in rows]
    if any(AGGREGATE_ID.fullmatch(identifier) is None for identifier in identifiers):
        raise ValueError("aggregate manifest contains an invalid aggregate ID")
    if identifiers != sorted(identifiers):
        raise ValueError("aggregate manifest is not sorted by aggregate ID")
    if len(identifiers) != len(set(identifiers)):
        raise ValueError("aggregate manifest contains duplicate aggregate IDs")
    expected_keys = set(columns)
    for row in rows:
        if set(row) != expected_keys:
            raise ValueError("aggregate manifest row columns differ")

    row_digest = hashlib.sha256()
    identifier_digest = hashlib.sha256()
    for row in rows:
        _framed_update(
            row_digest,
            canonical_json({column: row[column] for column in columns}).encode(
                "utf-8"
            ),
        )
        _framed_update(identifier_digest, row["aggregate_id"].encode("ascii"))
    page_counts = Counter(row["guest_page_size"] for row in rows)
    return {
        "schema": IDENTITY_SCHEMA,
        "manifest_schema": MANIFEST_SCHEMA,
        "columns_sha256": hashlib.sha256(
            ("\t".join(columns) + "\n").encode("ascii")
        ).hexdigest(),
        "row_count": len(rows),
        "row_sha256": row_digest.hexdigest(),
        "aggregate_id_sha256": identifier_digest.hexdigest(),
        "guest_page_size_counts": {
            page: page_counts[page] for page in sorted(page_counts, key=int)
        },
    }


def read_manifest_identity(path: Path) -> Mapping[str, object]:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: aggregate manifest identity is not an object")
    expected_keys = {
        "schema",
        "manifest_schema",
        "columns_sha256",
        "row_count",
        "row_sha256",
        "aggregate_id_sha256",
        "guest_page_size_counts",
    }
    if set(value) != expected_keys:
        raise ValueError(f"{path}: aggregate manifest identity fields differ")
    if value["schema"] != IDENTITY_SCHEMA or value["manifest_schema"] != MANIFEST_SCHEMA:
        raise ValueError(f"{path}: aggregate manifest identity schema differs")
    for name in ("columns_sha256", "row_sha256", "aggregate_id_sha256"):
        if not isinstance(value[name], str) or SHA256.fullmatch(value[name]) is None:
            raise ValueError(f"{path}: invalid {name}")
    if type(value["row_count"]) is not int or value["row_count"] <= 0:
        raise ValueError(f"{path}: invalid row_count")
    counts = value["guest_page_size_counts"]
    if (
        not isinstance(counts, dict)
        or not counts
        or any(
            not isinstance(page, str)
            or not page.isdigit()
            or type(count) is not int
            or count <= 0
            for page, count in counts.items()
        )
        or sum(counts.values()) != value["row_count"]
    ):
        raise ValueError(f"{path}: invalid guest_page_size_counts")
    if canonical_json(value) + "\n" != path.read_text(encoding="utf-8"):
        raise ValueError(f"{path}: aggregate manifest identity is not canonical")
    return value


def validate_manifest_identity(
    rows: Sequence[Mapping[str, str]],
    columns: Sequence[str],
    expected: Mapping[str, object],
) -> None:
    observed = aggregate_manifest_identity(rows, columns)
    if observed != expected:
        changed = sorted(
            name for name in observed if observed[name] != expected.get(name)
        )
        raise ValueError(
            "aggregate manifest differs from its frozen identity: "
            + ",".join(changed)
        )
