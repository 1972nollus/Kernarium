#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Canonical generation identity for the bound A64 ASLRef corpus trio."""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import csv
import hashlib
import io
import json
import re
from pathlib import Path
from typing import Callable, Mapping, Sequence

from a64_raw_evidence_files import freeze_read_inputs, recheck_read_inputs


IDENTITY_SCHEMA = "uml.a64.aslref-corpus-identity/v1"
CORPUS_SCHEMA = "uml.a64.aslref-allocation-corpus/v1"
QUEUE_SCHEMA = "uml.a64.aslref-interface-queue/v1"
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
ROLES = ("allocation-corpus", "interface-queue")


def canonical_json(value: object) -> str:
    return json.dumps(
        value,
        ensure_ascii=True,
        sort_keys=True,
        separators=(",", ":"),
    )


def columns_sha256(columns: Sequence[str]) -> str:
    return hashlib.sha256(("\t".join(columns) + "\n").encode("ascii")).hexdigest()


def _tsv_rows(
    payload: bytes,
    columns: Sequence[str],
    file_schema: str,
) -> list[dict[str, str]]:
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ValueError("corpus member is not valid UTF-8") from error
    if "\r" in text or not text.endswith("\n"):
        raise ValueError("corpus member is not canonical LF-terminated TSV")
    reader = csv.DictReader(io.StringIO(text, newline=""), delimiter="\t")
    if reader.fieldnames != list(columns):
        raise ValueError(
            f"corpus member columns are {reader.fieldnames!r}, "
            f"expected {list(columns)!r}"
        )
    rows = list(reader)
    if not rows:
        raise ValueError("corpus member contains no rows")
    expected_keys = set(columns)
    for row in rows:
        if set(row) != expected_keys or any(
            not isinstance(value, str) for value in row.values()
        ):
            raise ValueError("corpus member row shape differs")
        if "schema" in row and row["schema"] != file_schema:
            raise ValueError("corpus member row schema differs")
    canonical = io.StringIO(newline="")
    writer = csv.DictWriter(
        canonical,
        fieldnames=list(columns),
        delimiter="\t",
        lineterminator="\n",
        extrasaction="raise",
    )
    writer.writeheader()
    writer.writerows(rows)
    if canonical.getvalue() != text:
        raise ValueError("corpus member TSV is not canonical")
    return rows


def _member(
    role: str,
    file_name: str,
    file_schema: str,
    columns: Sequence[str],
    payload: bytes,
) -> dict[str, object]:
    if Path(file_name).name != file_name or file_name in {"", ".", ".."}:
        raise ValueError(f"corpus member has an invalid filename: {file_name!r}")
    return {
        "columns_sha256": columns_sha256(columns),
        "file_name": file_name,
        "file_schema": file_schema,
        "file_sha256": hashlib.sha256(payload).hexdigest(),
        "role": role,
        "row_count": len(_tsv_rows(payload, columns, file_schema)),
    }


def corpus_identity(
    corpus_name: str,
    corpus_payload: bytes,
    corpus_columns: Sequence[str],
    queue_name: str,
    queue_payload: bytes,
    queue_columns: Sequence[str],
) -> dict[str, object]:
    members = [
        _member(
            ROLES[0], corpus_name, CORPUS_SCHEMA,
            corpus_columns, corpus_payload,
        ),
        _member(
            ROLES[1], queue_name, QUEUE_SCHEMA,
            queue_columns, queue_payload,
        ),
    ]
    generation = hashlib.sha256(
        (IDENTITY_SCHEMA + "\0" + canonical_json(members)).encode("utf-8")
    ).hexdigest()
    return {
        "generation_sha256": generation,
        "members": members,
        "schema": IDENTITY_SCHEMA,
    }


def corpus_identity_bytes(
    corpus_name: str,
    corpus_payload: bytes,
    corpus_columns: Sequence[str],
    queue_name: str,
    queue_payload: bytes,
    queue_columns: Sequence[str],
) -> bytes:
    value = corpus_identity(
        corpus_name,
        corpus_payload,
        corpus_columns,
        queue_name,
        queue_payload,
        queue_columns,
    )
    return (canonical_json(value) + "\n").encode("ascii")


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"corpus identity repeats field {key!r}")
        result[key] = value
    return result


def read_identity_bytes(payload: bytes) -> dict[str, object]:
    try:
        text = payload.decode("ascii", errors="strict")
        value = json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ValueError("corpus identity is not canonical JSON") from error
    if not isinstance(value, dict) or set(value) != {
        "generation_sha256", "members", "schema"
    }:
        raise ValueError("corpus identity fields differ")
    if value["schema"] != IDENTITY_SCHEMA:
        raise ValueError("corpus identity schema differs")
    generation = value["generation_sha256"]
    if not isinstance(generation, str) or SHA256.fullmatch(generation) is None:
        raise ValueError("corpus identity generation digest is invalid")
    members = value["members"]
    if not isinstance(members, list) or len(members) != len(ROLES):
        raise ValueError("corpus identity member set differs")
    expected_schemas = (CORPUS_SCHEMA, QUEUE_SCHEMA)
    for index, member in enumerate(members):
        if not isinstance(member, dict) or set(member) != {
            "columns_sha256", "file_name", "file_schema", "file_sha256",
            "role", "row_count",
        }:
            raise ValueError("corpus identity member fields differ")
        if member["role"] != ROLES[index]:
            raise ValueError("corpus identity member order/role differs")
        if member["file_schema"] != expected_schemas[index]:
            raise ValueError("corpus identity member schema differs")
        name = member["file_name"]
        if (
            not isinstance(name, str)
            or Path(name).name != name
            or name in {"", ".", ".."}
        ):
            raise ValueError("corpus identity member filename is invalid")
        if type(member["row_count"]) is not int or member["row_count"] <= 0:
            raise ValueError("corpus identity member row count is invalid")
        for field in ("columns_sha256", "file_sha256"):
            digest = member[field]
            if not isinstance(digest, str) or SHA256.fullmatch(digest) is None:
                raise ValueError(f"corpus identity member {field} is invalid")
    observed_generation = hashlib.sha256(
        (IDENTITY_SCHEMA + "\0" + canonical_json(members)).encode("utf-8")
    ).hexdigest()
    if generation != observed_generation:
        raise ValueError("corpus identity generation digest differs")
    if canonical_json(value) + "\n" != text:
        raise ValueError("corpus identity is not canonical")
    return value


def validate_corpus_payloads(
    corpus_name: str,
    corpus_payload: bytes,
    corpus_columns: Sequence[str],
    queue_name: str,
    queue_payload: bytes,
    queue_columns: Sequence[str],
    identity_payload: bytes,
    *,
    corpus_validator: Callable[[Sequence[Mapping[str, str]]], None] | None = None,
    queue_validator: Callable[[Sequence[Mapping[str, str]]], None] | None = None,
) -> None:
    corpus_rows = _tsv_rows(corpus_payload, corpus_columns, CORPUS_SCHEMA)
    queue_rows = _tsv_rows(queue_payload, queue_columns, QUEUE_SCHEMA)
    if corpus_validator is not None:
        corpus_validator(corpus_rows)
    if queue_validator is not None:
        queue_validator(queue_rows)
    observed = read_identity_bytes(identity_payload)
    expected = corpus_identity(
        corpus_name,
        corpus_payload,
        corpus_columns,
        queue_name,
        queue_payload,
        queue_columns,
    )
    if observed != expected:
        raise ValueError("corpus files differ from their bound generation identity")


def validate_corpus_trio(
    corpus_path: Path,
    corpus_columns: Sequence[str],
    queue_path: Path,
    queue_columns: Sequence[str],
    identity_path: Path,
    *,
    corpus_validator: Callable[[Sequence[Mapping[str, str]]], None] | None = None,
    queue_validator: Callable[[Sequence[Mapping[str, str]]], None] | None = None,
) -> None:
    frozen = freeze_read_inputs((corpus_path, queue_path, identity_path))
    corpus_payload = corpus_path.read_bytes()
    queue_payload = queue_path.read_bytes()
    identity_payload = identity_path.read_bytes()
    recheck_read_inputs(frozen)
    validate_corpus_payloads(
        corpus_path.name,
        corpus_payload,
        corpus_columns,
        queue_path.name,
        queue_payload,
        queue_columns,
        identity_payload,
        corpus_validator=corpus_validator,
        queue_validator=queue_validator,
    )
