#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a bounded VK-GL-CTS shard in the ordinary rendered guest.

The authenticated rootfs owns the CTS executable, resources, Mesa, libdrm,
and WSI.  This runner injects only a first-party launcher and transient output
into the disposable OverlayFS upper layer.  It deliberately refuses the old
macOS CGL renderer and requires the common ANGLE/Metal product.

Official must-pass qualification remains fail-closed while the frozen identity
records no authoritative CTS build receipt or complete display-backed caselist.
Developer shards are still useful for bring-up and failure localization.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import binascii
from dataclasses import dataclass, field
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shlex
import signal
import stat
import subprocess
import sys
import time
from typing import Any, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parent))
import virgl_capset  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]
IDENTITY_PATH = Path(__file__).with_name("vk_gl_cts_identity.json")
APPLE_CAMPAIGNS_PATH = Path(__file__).with_name(
    "vk_gl_cts_apple_campaigns.json"
)
DEFAULT_EMULATOR = REPO_ROOT / ".build/presets/macos-integration/bin/uml-darwin"
DEFAULT_ROOTFS = REPO_ROOT / "rootfs.erofs"
DEFAULT_ARTIFACT_ROOT = REPO_ROOT / ".build/vk-gl-cts"

sys.path.insert(0, str(REPO_ROOT / "scripts"))
import boot_kde_vnc  # noqa: E402


MAX_COMPRESSED_ARTIFACT = 64 * 1024 * 1024
MAX_UNCOMPRESSED_ARTIFACT = 512 * 1024 * 1024
SHARD_ARTIFACTS = frozenset(
    {"renderer.txt", "results.qpa", "selected.txt", "summary.txt"}
)
INVENTORY_ARTIFACTS = frozenset(
    {"inventory.txt", "renderer.txt", "results.qpa", "summary.txt"}
)
ALLOWED_ARTIFACTS = SHARD_ARTIFACTS | INVENTORY_ARTIFACTS
CASE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
APPLE_CAMPAIGN_ID = re.compile(r"[a-z0-9][a-z0-9-]*\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
UUID = re.compile(
    r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\Z"
)
FORBIDDEN_RENDERERS = re.compile(
    r"(?:llvmpipe|softpipe|swrast|software rasterizer|\bzink\b|xvfb)",
    re.IGNORECASE,
)
BEGIN_ARTIFACT = re.compile(
    r"UML_CTS_ARTIFACT_BEGIN\|([^|]+)\|gzip\+base64\|([0-9]+)\|([0-9a-f]{64})\Z"
)
CHUNK_ARTIFACT = re.compile(
    r"UML_CTS_ARTIFACT_CHUNK\|([^|]+)\|([0-9]+)\|([A-Za-z0-9+/=]+)\Z"
)
END_ARTIFACT = re.compile(
    r"UML_CTS_ARTIFACT_END\|([^|]+)\|([0-9]+)\Z"
)
HOST_CONTEXT = re.compile(
    r"UML_GPU_HOST_CONTEXT\|requested=([0-9]+)\.([0-9]+)\|"
    r"created=([01])\|egl_error=([0-9]+)\|actual=([0-9]+)\.([0-9]+)\|"
    r"vendor_hex=([0-9a-f]*)\|renderer_hex=([0-9a-f]*)\|"
    r"version_hex=([0-9a-f]*)\|glsl_hex=([0-9a-f]*)\|"
    r"extensions=([0-9]+)\Z"
)
HOST_EXTENSION = re.compile(
    r"UML_GPU_HOST_EXTENSION\|requested=([0-9]+)\.([0-9]+)\|"
    r"name_hex=([0-9a-f]+)\Z"
)
CAPSET_BEGIN = re.compile(
    r"UML_GPU_CAPSET_BEGIN\|id=([0-9]+)\|version=([0-9]+)\|"
    r"size=([0-9]+)\|chunks=([0-9]+)\Z"
)
CAPSET_CHUNK = re.compile(
    r"UML_GPU_CAPSET_CHUNK\|id=([0-9]+)\|index=([0-9]+)\|"
    r"hex=([0-9a-f]+)\Z"
)
CAPSET_END = re.compile(
    r"UML_GPU_CAPSET_END\|id=([0-9]+)\|chunks=([0-9]+)\Z"
)
NATIVE_DISPLAY_FINAL = re.compile(
    r"Native display final: guest ([0-9]+) presented ([0-9]+) "
    r"dropped ([0-9]+) cpu-fallback ([0-9]+)"
)
RESULT_STATUS = re.compile(r"<Result\s+StatusCode=\"([^\"]+)\"")
RESULT_CASE = re.compile(r"<TestCaseResult\b[^>]*\bCasePath=\"([^\"]+)\"")
GUEST_ERROR = re.compile(r"UML_CTS_ERROR: ([^\r\n]+)")
HOST_BACKEND_MARKER = "Virgl ANGLE/Metal provider:"
RETIRED_BACKEND_MARKER = "Virgl CGL"
DARWIN_CLEAN_STOP = "Darwin UML stopped: result=0 exitcode=0"
DIRTY_DRM_SHUTDOWN = "Memory manager not clean during takedown."


class CampaignError(RuntimeError):
    """A campaign input or result violated a fail-closed contract."""


@dataclass(frozen=True)
class LeafIdentity:
    identifier: str
    path: str
    case_count: int
    sha256: str


@dataclass(frozen=True)
class Selection:
    leaf: LeafIdentity
    exact_cases: tuple[str, ...]
    fraction_index: int
    fraction_count: int

    @property
    def expected_count(self) -> int:
        if self.exact_cases:
            return len(self.exact_cases)
        remaining = self.leaf.case_count - self.fraction_index
        if remaining <= 0:
            return 0
        return (remaining + self.fraction_count - 1) // self.fraction_count


@dataclass(frozen=True)
class AppleCampaign:
    identifier: str
    api: str
    leaf: LeafIdentity
    cases: tuple[str, ...]
    seed: int
    shard_timeout: int

    @property
    def selection(self) -> Selection:
        return Selection(self.leaf, self.cases, 0, 1)


@dataclass
class EncodedArtifact:
    compressed_size: int
    sha256: str
    chunks: dict[int, str] = field(default_factory=dict)
    expected_chunks: int | None = None


@dataclass(frozen=True)
class RootfsContract:
    filesystem_type: str
    size: int
    sha256: str
    glcts_sha256: str
    filesystem_uuid: str | None = None
    guest_receipt_path: str | None = None
    guest_receipt_sha256: str | None = None


def reject_duplicate_keys(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    value: dict[str, Any] = {}
    for key, item in pairs:
        if key in value:
            raise CampaignError(f"duplicate JSON key: {key}")
        value[key] = item
    return value


def load_identity(path: Path = IDENTITY_PATH) -> dict[str, Any]:
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(f"cannot read CTS identity {path}: {error}") from error
    if not isinstance(value, dict) or value.get("schema_version") != 1:
        raise CampaignError("unsupported CTS identity schema")
    return value


def cts_binary_identity(identity: dict[str, Any], path: str) -> dict[str, Any]:
    try:
        rows = identity["cts"]["binaries"]
    except (KeyError, TypeError) as error:
        raise CampaignError("CTS identity has no binary inventory") from error
    if not isinstance(rows, list):
        raise CampaignError("CTS binary inventory is not a list")
    matches = [
        row
        for row in rows
        if isinstance(row, dict) and row.get("path") == path
    ]
    if len(matches) != 1:
        raise CampaignError(f"CTS identity does not uniquely identify {path}")
    row = matches[0]
    if (
        set(row) != {"path", "size", "sha256", "build_id"}
        or type(row["size"]) is not int
        or row["size"] < 1
        or not isinstance(row["sha256"], str)
        or SHA256.fullmatch(row["sha256"]) is None
        or not isinstance(row["build_id"], str)
        or re.fullmatch(r"[0-9a-f]+", row["build_id"]) is None
    ):
        raise CampaignError(f"invalid CTS binary identity for {path}")
    return row


def exclusion_digest(paths: Sequence[str]) -> str:
    payload = "".join(f"{path}\n" for path in paths).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def load_derivative_receipt(
    path: Path,
    base_identity: dict[str, Any],
) -> tuple[dict[str, Any], RootfsContract]:
    try:
        receipt = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read derivative rootfs receipt {path}: {error}"
        ) from error
    if not isinstance(receipt, dict) or set(receipt) != {
        "schema_version",
        "artifact",
        "image",
        "base_rootfs",
        "clone",
        "guest_receipt",
        "cts",
    }:
        raise CampaignError("invalid derivative rootfs receipt")
    if (
        receipt["schema_version"] != 1
        or receipt["artifact"] != "uml-gl-gles-rootfs"
    ):
        raise CampaignError("unsupported derivative rootfs receipt")

    image = receipt["image"]
    base = receipt["base_rootfs"]
    clone = receipt["clone"]
    guest_receipt = receipt["guest_receipt"]
    cts = receipt["cts"]
    sections = (image, base, clone, guest_receipt, cts)
    if not all(isinstance(value, dict) for value in sections):
        raise CampaignError("derivative rootfs receipt sections are invalid")
    if set(image) != {
        "filesystem",
        "filesystem_uuid",
        "size",
        "sha256",
        "read_only",
    }:
        raise CampaignError("invalid derivative image identity")
    if (
        image["filesystem"] != "ext4"
        or not isinstance(image["filesystem_uuid"], str)
        or UUID.fullmatch(image["filesystem_uuid"]) is None
        or type(image["size"]) is not int
        or image["size"] != 8 * 1024 * 1024 * 1024
        or not isinstance(image["sha256"], str)
        or SHA256.fullmatch(image["sha256"]) is None
        or image["read_only"] is not True
    ):
        raise CampaignError("invalid derivative image identity")
    if set(base) != {"size", "sha256"} or base != {
        "size": base_identity["rootfs"]["size"],
        "sha256": base_identity["rootfs"]["sha256"],
    }:
        raise CampaignError("derivative receipt names a different base rootfs")
    if set(clone) != {"excluded_paths", "excluded_paths_sha256"}:
        raise CampaignError("invalid derivative clone identity")
    excluded = clone["excluded_paths"]
    if (
        not isinstance(excluded, list)
        or not excluded
        or any(
            not isinstance(item, str)
            or not item.startswith("/")
            or item.endswith("/")
            for item in excluded
        )
        or excluded != sorted(set(excluded))
        or clone["excluded_paths_sha256"] != exclusion_digest(excluded)
    ):
        raise CampaignError("invalid derivative exclusion identity")
    if set(guest_receipt) != {"path", "sha256"} or (
        not isinstance(guest_receipt["path"], str)
        or not guest_receipt["path"].startswith("/usr/share/uml-rootfs/")
        or not isinstance(guest_receipt["sha256"], str)
        or SHA256.fullmatch(guest_receipt["sha256"]) is None
    ):
        raise CampaignError("invalid in-guest build receipt identity")
    if (
        set(cts)
        != {"glcts_path", "glcts_size", "glcts_sha256", "glcts_build_id"}
        or cts["glcts_path"] != "/opt/vk-gl-cts/openglcts/glcts"
        or type(cts["glcts_size"]) is not int
        or cts["glcts_size"] < 1
        or not isinstance(cts["glcts_sha256"], str)
        or SHA256.fullmatch(cts["glcts_sha256"]) is None
        or not isinstance(cts["glcts_build_id"], str)
        or re.fullmatch(r"[0-9a-f]+", cts["glcts_build_id"]) is None
    ):
        raise CampaignError("invalid derivative CTS identity")
    return receipt, RootfsContract(
        filesystem_type="ext4",
        size=image["size"],
        sha256=image["sha256"],
        glcts_sha256=cts["glcts_sha256"],
        filesystem_uuid=image["filesystem_uuid"],
        guest_receipt_path=guest_receipt["path"],
        guest_receipt_sha256=guest_receipt["sha256"],
    )


def select_rootfs_contract(
    identity: dict[str, Any],
    filesystem_type: str,
    receipt_path: Path | None,
) -> tuple[dict[str, Any] | None, RootfsContract]:
    if filesystem_type == "erofs":
        if receipt_path is not None:
            raise CampaignError("a derivative receipt requires an ext4 rootfs")
        glcts = cts_binary_identity(
            identity, "/opt/vk-gl-cts/openglcts/glcts"
        )
        return None, RootfsContract(
            filesystem_type="erofs",
            size=identity["rootfs"]["size"],
            sha256=identity["rootfs"]["sha256"],
            glcts_sha256=glcts["sha256"],
        )
    if filesystem_type != "ext4":
        raise CampaignError(f"unsupported root filesystem: {filesystem_type}")
    if receipt_path is None:
        raise CampaignError("an ext4 rootfs requires a derivative receipt")
    return load_derivative_receipt(receipt_path.resolve(), identity)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def validate_rootfs_file(path: Path, contract: RootfsContract) -> None:
    if not path.is_file():
        raise CampaignError(f"rootfs is not a regular file: {path}")
    details = path.stat()
    if details.st_size != contract.size:
        raise CampaignError("rootfs size does not match its frozen identity")
    if contract.filesystem_type == "ext4" and details.st_mode & (
        stat.S_IWUSR | stat.S_IWGRP | stat.S_IWOTH
    ):
        raise CampaignError("derivative ext4 rootfs still has host write permission")
    if sha256_file(path) != contract.sha256:
        raise CampaignError("rootfs digest does not match its frozen identity")


def leaf_identities(identity: dict[str, Any]) -> dict[str, LeafIdentity]:
    result: dict[str, LeafIdentity] = {}
    try:
        rows = identity["mustpass"]["leaves"]
    except (KeyError, TypeError) as error:
        raise CampaignError("CTS identity has no must-pass leaves") from error
    if not isinstance(rows, list):
        raise CampaignError("CTS must-pass leaves are not a list")
    for row in rows:
        try:
            leaf = LeafIdentity(
                identifier=row["id"],
                path=row["path"],
                case_count=row["case_count"],
                sha256=row["sha256"],
            )
        except (KeyError, TypeError) as error:
            raise CampaignError("invalid CTS leaf identity") from error
        if (
            not isinstance(leaf.identifier, str)
            or not isinstance(leaf.path, str)
            or not isinstance(leaf.case_count, int)
            or leaf.case_count <= 0
            or not isinstance(leaf.sha256, str)
            or not SHA256.fullmatch(leaf.sha256)
        ):
            raise CampaignError(f"invalid CTS leaf identity: {leaf.identifier!r}")
        if leaf.identifier in result:
            raise CampaignError(f"duplicate CTS leaf identity: {leaf.identifier}")
        result[leaf.identifier] = leaf
    return result


def load_apple_campaigns(
    identity: dict[str, Any] | None = None,
    path: Path = APPLE_CAMPAIGNS_PATH,
) -> dict[str, AppleCampaign]:
    if identity is None:
        identity = load_identity()
    leaves = leaf_identities(identity)
    try:
        value = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read Apple CTS campaigns {path}: {error}"
        ) from error
    if not isinstance(value, dict) or set(value) != {
        "schema_version",
        "campaigns",
    }:
        raise CampaignError("invalid Apple CTS campaign document")
    if value["schema_version"] != 2 or not isinstance(
        value["campaigns"], list
    ):
        raise CampaignError("unsupported Apple CTS campaign schema")

    result: dict[str, AppleCampaign] = {}
    required_keys = {
        "id",
        "api",
        "leaf",
        "cases",
        "seed",
        "shard_timeout",
    }
    for row in value["campaigns"]:
        if not isinstance(row, dict) or set(row) != required_keys:
            raise CampaignError("invalid Apple CTS campaign row")
        identifier = row["id"]
        api = row["api"]
        leaf_identifier = row["leaf"]
        cases = row["cases"]
        seed = row["seed"]
        shard_timeout = row["shard_timeout"]
        if (
            not isinstance(identifier, str)
            or not APPLE_CAMPAIGN_ID.fullmatch(identifier)
            or not isinstance(api, str)
            or not isinstance(leaf_identifier, str)
            or not isinstance(cases, list)
            or not 1 <= len(cases) <= 64
            or any(
                not isinstance(case, str) or not CASE_NAME.fullmatch(case)
                for case in cases
            )
            or len(set(cases)) != len(cases)
            or type(seed) is not int
            or seed < 0
            or seed > 0xFFFFFFFF
            or type(shard_timeout) is not int
            or shard_timeout < 1
            or shard_timeout > 3600
        ):
            raise CampaignError(f"invalid Apple CTS campaign: {identifier!r}")
        leaf = leaves.get(leaf_identifier)
        if leaf is None or api != leaf.identifier.split("-", 1)[0]:
            raise CampaignError(f"invalid Apple CTS campaign: {identifier!r}")
        campaign = AppleCampaign(
            identifier=identifier,
            api=api,
            leaf=leaf,
            cases=tuple(cases),
            seed=seed,
            shard_timeout=shard_timeout,
        )
        if campaign.identifier in result:
            raise CampaignError(
                f"duplicate Apple CTS campaign: {campaign.identifier}"
            )
        result[campaign.identifier] = campaign
    if not result:
        raise CampaignError("Apple CTS campaign allow-list is empty")
    return result


def parse_fraction(value: str) -> tuple[int, int]:
    match = re.fullmatch(r"([0-9]+),([0-9]+)", value)
    if match is None:
        raise argparse.ArgumentTypeError("must be N,M with zero-based N")
    index, count = (int(item) for item in match.groups())
    if count < 1 or index >= count:
        raise argparse.ArgumentTypeError("fraction requires 0 <= N < M")
    return index, count


def exact_case(value: str) -> str:
    if CASE_NAME.fullmatch(value) is None:
        raise argparse.ArgumentTypeError(
            "must be one exact CTS case name without wildcards or commas"
        )
    return value


def positive_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed < 1:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def parse_case_list(text: str) -> list[str]:
    cases: list[str] = []
    seen: set[str] = set()
    for line_number, raw_line in enumerate(text.splitlines(), 1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        if CASE_NAME.fullmatch(line) is None:
            raise CampaignError(f"invalid case at line {line_number}: {line!r}")
        if line in seen:
            raise CampaignError(f"duplicate case at line {line_number}: {line}")
        seen.add(line)
        cases.append(line)
    if not cases:
        raise CampaignError("selected caselist is empty")
    return cases


def select_fraction(cases: Sequence[str], index: int, count: int) -> list[str]:
    if count < 1 or index < 0 or index >= count:
        raise CampaignError("invalid fraction")
    selected = list(cases[index::count])
    if not selected:
        raise CampaignError("fraction selects no cases")
    return selected


def validate_inventory_report(
    payload: bytes,
    identity: dict[str, Any],
) -> tuple[int, str, list[str], int]:
    text = payload.decode("utf-8", errors="strict")
    values: dict[str, str] = {}
    modules: list[str] = []
    for line_number, line in enumerate(text.splitlines(), 1):
        key, separator, value = line.partition("=")
        if not separator or not value:
            raise CampaignError(f"invalid CTS inventory row at line {line_number}")
        if key == "registered_module":
            if CASE_NAME.fullmatch(value) is None or "." in value:
                raise CampaignError(f"invalid registered module at line {line_number}")
            modules.append(value)
            continue
        if key not in {
            "schema_version",
            "complete",
            "exported_package_count",
            "complete_caselist_sha256",
            "complete_caselist_case_count",
        }:
            raise CampaignError(f"unknown CTS inventory key: {key}")
        if key in values:
            raise CampaignError(f"duplicate CTS inventory key: {key}")
        values[key] = value
    required = {
        "schema_version",
        "complete",
        "exported_package_count",
        "complete_caselist_sha256",
        "complete_caselist_case_count",
    }
    if set(values) != required:
        raise CampaignError("CTS inventory report is incomplete")
    if values["schema_version"] != "1" or values["complete"] != "1":
        raise CampaignError("CTS inventory report is not complete")
    if not values["exported_package_count"].isdigit():
        raise CampaignError("CTS inventory package count is invalid")
    package_count = int(values["exported_package_count"])
    if not values["complete_caselist_case_count"].isdigit():
        raise CampaignError("CTS inventory case count is invalid")
    case_count = int(values["complete_caselist_case_count"])
    digest = values["complete_caselist_sha256"]
    if package_count < 1 or case_count < 1 or SHA256.fullmatch(digest) is None:
        raise CampaignError("CTS inventory identity is invalid")
    if not modules or modules != sorted(set(modules)):
        raise CampaignError("registered CTS module set is not sorted and unique")
    frozen_digest = identity["cts"]["complete_caselist_sha256"]
    frozen_count = identity["cts"]["complete_caselist_case_count"]
    frozen_modules = identity["cts"]["registered_modules"]
    if frozen_digest is not None and frozen_digest != digest:
        raise CampaignError("complete CTS caselist identity changed")
    if frozen_count is not None and frozen_count != case_count:
        raise CampaignError("complete CTS caselist count changed")
    if frozen_modules is not None and frozen_modules != modules:
        raise CampaignError("registered CTS module set changed")
    return case_count, digest, modules, package_count


def parse_qpa(text: str) -> dict[str, str]:
    if text.count("#beginSession") != 1:
        raise CampaignError("QPA does not contain exactly one session start")
    if text.count("#endSession") != 1:
        raise CampaignError("QPA is truncated or has multiple session trailers")

    results: dict[str, str] = {}
    current_case: str | None = None
    body: list[str] = []
    ended_session = False
    for raw_line in text.splitlines(keepends=True):
        line = raw_line.strip()
        if line.startswith("#beginTestCaseResult "):
            if current_case is not None or ended_session:
                raise CampaignError("nested or post-session QPA case")
            current_case = line.removeprefix("#beginTestCaseResult ")
            if CASE_NAME.fullmatch(current_case) is None:
                raise CampaignError(f"invalid QPA case name: {current_case!r}")
            if current_case in results:
                raise CampaignError(f"duplicate QPA case: {current_case}")
            body = []
            continue
        if line == "#endTestCaseResult":
            if current_case is None:
                raise CampaignError("QPA case trailer has no case")
            payload = "".join(body)
            statuses = RESULT_STATUS.findall(payload)
            if len(statuses) != 1:
                raise CampaignError(
                    f"QPA case {current_case} has {len(statuses)} result statuses"
                )
            case_paths = RESULT_CASE.findall(payload)
            if case_paths and (len(case_paths) != 1 or case_paths[0] != current_case):
                raise CampaignError(f"QPA case-path mismatch for {current_case}")
            results[current_case] = statuses[0]
            current_case = None
            body = []
            continue
        if line.startswith("#terminateTestCaseResult "):
            if current_case is None:
                raise CampaignError("QPA termination has no case")
            status = line.removeprefix("#terminateTestCaseResult ")
            if not status or not re.fullmatch(r"[A-Za-z]+", status):
                raise CampaignError(f"invalid QPA termination status: {status!r}")
            results[current_case] = status
            current_case = None
            body = []
            continue
        if line == "#endSession":
            if current_case is not None:
                raise CampaignError(f"QPA ended during case {current_case}")
            ended_session = True
            continue
        if current_case is not None:
            body.append(raw_line)

    if current_case is not None:
        raise CampaignError(f"QPA is truncated during case {current_case}")
    if not ended_session:
        raise CampaignError("QPA has no complete session trailer")
    return results


def validate_qpa_union(qpa: str, selected: Sequence[str]) -> dict[str, str]:
    expected = set(selected)
    if len(expected) != len(selected):
        raise CampaignError("expected caselist contains duplicates")
    results = parse_qpa(qpa)
    actual = set(results)
    missing = sorted(expected - actual)
    unexpected = sorted(actual - expected)
    if missing or unexpected:
        details: list[str] = []
        if missing:
            details.append(f"missing={missing[:5]!r}")
        if unexpected:
            details.append(f"unexpected={unexpected[:5]!r}")
        raise CampaignError("incomplete QPA case union: " + ", ".join(details))
    return results


def status_totals(results: dict[str, str]) -> dict[str, int]:
    totals: dict[str, int] = {}
    for status in results.values():
        totals[status] = totals.get(status, 0) + 1
    return totals


def validate_required_passes(results: dict[str, str]) -> None:
    non_pass = {
        case: status for case, status in results.items() if status != "Pass"
    }
    if non_pass:
        first_case = sorted(non_pass)[0]
        raise CampaignError(
            "required CTS case did not pass: "
            f"{first_case}={non_pass[first_case]}"
        )


def validate_renderer_probe(text: str, expected_page_size: int | None = None) -> None:
    lower = text.lower()
    for device in ("/dev/dri/card0", "/dev/dri/renderD128"):
        if f"device={device}" not in text:
            raise CampaignError(f"renderer probe did not prove {device}")
    if "x11_server=xwayland" not in lower:
        raise CampaignError("renderer probe did not prove the real Xwayland WSI")
    forbidden = FORBIDDEN_RENDERERS.search(text)
    if forbidden is not None:
        raise CampaignError(f"software or forbidden renderer: {forbidden.group(0)}")
    if "virgl" not in lower:
        raise CampaignError("renderer probe did not report Virgl")
    if (
        expected_page_size is not None
        and f"guest_page_size={expected_page_size}" not in text
    ):
        raise CampaignError(
            f"renderer probe did not prove {expected_page_size}-byte guest pages"
        )


def validate_host_backend(transcript: str) -> None:
    if RETIRED_BACKEND_MARKER in transcript:
        raise CampaignError("retired CGL renderer was selected")
    if HOST_BACKEND_MARKER not in transcript:
        raise CampaignError("common ANGLE/Metal host-provider marker is missing")


def report_marker(line: str, prefix: str) -> str | None:
    offset = line.find(prefix)
    if offset < 0:
        return None
    return line[offset:].strip()


def decode_report_text(value: str, field: str) -> str:
    if len(value) % 2 != 0:
        raise CampaignError(f"host capability {field} has odd-length hex")
    try:
        decoded = bytes.fromhex(value).decode("utf-8", errors="strict")
    except (ValueError, UnicodeDecodeError) as error:
        raise CampaignError(
            f"host capability {field} is not valid UTF-8 hex"
        ) from error
    if "\x00" in decoded or "\n" in decoded or "\r" in decoded:
        raise CampaignError(f"host capability {field} contains control data")
    return decoded


def parse_gpu_capability_report(transcript: str) -> dict[str, Any]:
    contexts: dict[tuple[int, int], dict[str, Any]] = {}
    context_extensions: dict[tuple[int, int], list[str]] = {}
    capsets: dict[int, dict[str, Any]] = {}
    for raw_line in transcript.replace("\r", "").splitlines():
        marker = report_marker(raw_line, "UML_GPU_HOST_")
        if marker is None:
            marker = report_marker(raw_line, "UML_GPU_CAPSET_")
        if marker is None:
            continue
        match = HOST_CONTEXT.fullmatch(marker)
        if match is not None:
            (
                major_text,
                minor_text,
                created_text,
                error_text,
                actual_major_text,
                actual_minor_text,
                vendor_hex,
                renderer_hex,
                version_hex,
                glsl_hex,
                extension_count_text,
            ) = match.groups()
            requested = (int(major_text), int(minor_text))
            if requested in contexts:
                raise CampaignError(
                    f"duplicate host-context probe: {requested[0]}.{requested[1]}"
                )
            contexts[requested] = {
                "requested": list(requested),
                "created": created_text == "1",
                "egl_error": int(error_text),
                "actual": [int(actual_major_text), int(actual_minor_text)],
                "vendor": decode_report_text(vendor_hex, "vendor"),
                "renderer": decode_report_text(renderer_hex, "renderer"),
                "version": decode_report_text(version_hex, "version"),
                "shading_language_version": decode_report_text(
                    glsl_hex, "shading-language version"
                ),
                "extension_count": int(extension_count_text),
            }
            context_extensions[requested] = []
            continue
        match = HOST_EXTENSION.fullmatch(marker)
        if match is not None:
            major_text, minor_text, extension_hex = match.groups()
            requested = (int(major_text), int(minor_text))
            if requested not in contexts:
                raise CampaignError("host extension preceded its context probe")
            context_extensions[requested].append(
                decode_report_text(extension_hex, "extension")
            )
            continue
        match = CAPSET_BEGIN.fullmatch(marker)
        if match is not None:
            identifier_text, version_text, size_text, chunks_text = match.groups()
            identifier = int(identifier_text)
            if identifier in capsets:
                raise CampaignError(f"duplicate Virgl capset start: {identifier}")
            size = int(size_text)
            chunks = int(chunks_text)
            if size < 1 or size > 64 * 1024 or chunks < 1 or chunks > 256:
                raise CampaignError(f"invalid Virgl capset bounds: {identifier}")
            capsets[identifier] = {
                "maximum_version": int(version_text),
                "size": size,
                "expected_chunks": chunks,
                "chunks": {},
                "ended": False,
            }
            continue
        match = CAPSET_CHUNK.fullmatch(marker)
        if match is not None:
            identifier_text, index_text, payload = match.groups()
            identifier = int(identifier_text)
            if identifier not in capsets:
                raise CampaignError("Virgl capset chunk preceded its header")
            index = int(index_text)
            chunks = capsets[identifier]["chunks"]
            if index in chunks:
                raise CampaignError(
                    f"duplicate Virgl capset chunk: {identifier}[{index}]"
                )
            if len(payload) % 2 != 0:
                raise CampaignError("Virgl capset chunk has odd-length hex")
            try:
                chunks[index] = bytes.fromhex(payload)
            except ValueError as error:
                raise CampaignError("Virgl capset chunk has invalid hex") from error
            continue
        match = CAPSET_END.fullmatch(marker)
        if match is not None:
            identifier_text, chunks_text = match.groups()
            identifier = int(identifier_text)
            if identifier not in capsets or capsets[identifier]["ended"]:
                raise CampaignError("Virgl capset trailer has no unique header")
            if int(chunks_text) != capsets[identifier]["expected_chunks"]:
                raise CampaignError("Virgl capset trailer count changed")
            capsets[identifier]["ended"] = True
            continue
        raise CampaignError(f"malformed GPU capability marker: {marker[:120]}")

    required_contexts = {(3, 0), (3, 1), (3, 2)}
    if set(contexts) != required_contexts:
        raise CampaignError("host capability report omitted an ES 3.x request")
    serialized_contexts: list[dict[str, Any]] = []
    for requested in sorted(contexts):
        context = contexts[requested]
        extensions = context_extensions[requested]
        if extensions != sorted(set(extensions)):
            raise CampaignError("host extension report is not sorted and unique")
        if len(extensions) != context.pop("extension_count"):
            raise CampaignError("host extension count changed during capture")
        context["extensions"] = extensions
        if context["created"]:
            if context["egl_error"] != 0x3000:
                raise CampaignError("successful host context has an EGL error")
            if tuple(context["actual"]) < requested:
                raise CampaignError("host context is below its requested ES version")
            for field in (
                "vendor",
                "renderer",
                "version",
                "shading_language_version",
            ):
                if not context[field]:
                    raise CampaignError(
                        f"successful host context omitted its {field}"
                    )
        elif (
            context["egl_error"] == 0x3000
            or context["actual"] != [0, 0]
            or extensions
        ):
            raise CampaignError("failed host context has contradictory evidence")
        serialized_contexts.append(context)

    if set(capsets) != {1, 2}:
        raise CampaignError("GPU capability report omitted a Virgl capset")
    serialized_capsets: list[dict[str, Any]] = []
    for identifier in sorted(capsets):
        capset = capsets[identifier]
        expected_chunks = capset["expected_chunks"]
        if not capset["ended"] or set(capset["chunks"]) != set(
            range(expected_chunks)
        ):
            raise CampaignError(f"Virgl capset {identifier} is incomplete")
        payload = b"".join(
            capset["chunks"][index] for index in range(expected_chunks)
        )
        if len(payload) != capset["size"]:
            raise CampaignError(f"Virgl capset {identifier} size changed")
        try:
            decoded = virgl_capset.decode_capset(identifier, payload)
        except ValueError as error:
            raise CampaignError(str(error)) from error
        serialized_capsets.append(
            {
                "id": identifier,
                "maximum_version": capset["maximum_version"],
                "size": len(payload),
                "sha256": hashlib.sha256(payload).hexdigest(),
                "data_hex": payload.hex(),
                "decoded": decoded,
            }
        )
    return {
        "host_contexts": serialized_contexts,
        "virgl_capsets": serialized_capsets,
    }


def parse_native_display_final(transcript: str) -> dict[str, int]:
    if "Native display: direct virgl/ANGLE/Metal presentation" not in transcript:
        raise CampaignError("native display never accepted direct Metal presentation")
    matches = NATIVE_DISPLAY_FINAL.findall(transcript)
    if len(matches) != 1:
        raise CampaignError("native display did not emit one final frame report")
    guest, presented, dropped, cpu_fallback = (
        int(value) for value in matches[0]
    )
    if guest < 1 or presented < 1:
        raise CampaignError("native display final frame totals are empty")
    if cpu_fallback != 0:
        raise CampaignError("native display used CPU scanout fallback")
    return {
        "guest_frames": guest,
        "presented_frames": presented,
        "dropped_frames": dropped,
        "cpu_fallback_frames": cpu_fallback,
    }


def validate_guest_shutdown(transcript: str) -> None:
    if DIRTY_DRM_SHUTDOWN in transcript:
        raise CampaignError(
            "Virtio-GPU memory manager was not clean during guest shutdown"
        )
    if DARWIN_CLEAN_STOP not in transcript:
        raise CampaignError("campaign omitted the clean Darwin guest stop marker")


def artifact_marker(line: str) -> str | None:
    offset = line.find("UML_CTS_ARTIFACT_")
    if offset < 0:
        return None
    if offset != 0:
        prefix = line[:offset]
        if not prefix[-1].isspace() or not prefix.rstrip().endswith(":"):
            return None
    return line[offset:].strip()


def decode_artifacts(transcript: str) -> dict[str, bytes]:
    encoded: dict[str, EncodedArtifact] = {}
    for raw_line in transcript.replace("\r", "").splitlines():
        marker = artifact_marker(raw_line)
        if marker is None:
            continue
        begin = BEGIN_ARTIFACT.fullmatch(marker)
        if begin is not None:
            name, size_text, digest = begin.groups()
            if name not in ALLOWED_ARTIFACTS:
                raise CampaignError(f"unsafe or unexpected artifact name: {name}")
            if name in encoded:
                raise CampaignError(f"duplicate artifact start: {name}")
            size = int(size_text)
            if size < 1 or size > MAX_COMPRESSED_ARTIFACT:
                raise CampaignError(f"artifact {name} has invalid compressed size")
            encoded[name] = EncodedArtifact(size, digest)
            continue
        chunk = CHUNK_ARTIFACT.fullmatch(marker)
        if chunk is not None:
            name, index_text, payload = chunk.groups()
            if name not in encoded:
                raise CampaignError(f"artifact chunk precedes start: {name}")
            index = int(index_text)
            if index in encoded[name].chunks:
                raise CampaignError(f"duplicate artifact chunk: {name}[{index}]")
            encoded[name].chunks[index] = payload
            continue
        end = END_ARTIFACT.fullmatch(marker)
        if end is not None:
            name, count_text = end.groups()
            if name not in encoded:
                raise CampaignError(f"artifact end precedes start: {name}")
            if encoded[name].expected_chunks is not None:
                raise CampaignError(f"duplicate artifact end: {name}")
            encoded[name].expected_chunks = int(count_text)
            continue
        raise CampaignError(f"malformed artifact marker: {marker[:120]}")

    if not encoded:
        raise CampaignError("campaign emitted no artifacts")
    decoded: dict[str, bytes] = {}
    for name, artifact in encoded.items():
        count = artifact.expected_chunks
        if count is None or count < 1:
            raise CampaignError(f"artifact {name} is missing its trailer")
        if set(artifact.chunks) != set(range(count)):
            raise CampaignError(f"artifact {name} has an incomplete chunk union")
        encoded_size = sum(len(chunk) for chunk in artifact.chunks.values())
        if encoded_size > ((MAX_COMPRESSED_ARTIFACT + 2) // 3) * 4:
            raise CampaignError(f"artifact {name} exceeds the encoded-data bound")
        try:
            compressed = b"".join(
                base64.b64decode(artifact.chunks[index], validate=True)
                for index in range(count)
            )
        except (ValueError, binascii.Error) as error:
            raise CampaignError(f"artifact {name} has invalid base64") from error
        if len(compressed) != artifact.compressed_size:
            raise CampaignError(f"artifact {name} compressed size mismatch")
        if hashlib.sha256(compressed).hexdigest() != artifact.sha256:
            raise CampaignError(f"artifact {name} checksum mismatch")
        try:
            with gzip.GzipFile(fileobj=io.BytesIO(compressed)) as stream:
                payload = stream.read(MAX_UNCOMPRESSED_ARTIFACT + 1)
        except (OSError, EOFError) as error:
            raise CampaignError(f"artifact {name} is not complete gzip data") from error
        if len(payload) > MAX_UNCOMPRESSED_ARTIFACT:
            raise CampaignError(f"artifact {name} exceeds the output bound")
        decoded[name] = payload
    return decoded


def write_artifacts(artifacts: dict[str, bytes], directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=False)
    root = directory.resolve()
    for name, payload in artifacts.items():
        if name not in ALLOWED_ARTIFACTS:
            raise CampaignError(f"refusing unsafe artifact name: {name}")
        destination = (root / name).resolve()
        if destination.parent != root:
            raise CampaignError(f"artifact escapes output directory: {name}")
        destination.write_bytes(payload)


def install_text(path: str, content: str, mode: str = "0644") -> str:
    encoded = base64.b64encode(content.encode("utf-8")).decode("ascii")
    parent = str(Path(path).parent)
    return (
        f"install -d -m 0755 {shlex.quote(parent)}; "
        f"printf '%s' {shlex.quote(encoded)} | base64 -d > {shlex.quote(path)}; "
        f"chmod {mode} {shlex.quote(path)}"
    )


def build_guest_launcher(
    selection: Selection | None,
    seed: int,
    timeout: int,
    guest_page_size: int,
    rootfs_contract: RootfsContract,
) -> str:
    glcts = "/opt/vk-gl-cts/openglcts/glcts"
    archive = "/opt/vk-gl-cts/openglcts"
    output = "/run/uml-cts"
    selected = f"{output}/selected.txt"
    qpa = f"{output}/results.qpa"
    renderer = f"{output}/renderer.txt"
    summary = f"{output}/summary.txt"
    inventory_report = f"{output}/inventory.txt"
    inventory = selection is None
    derivative_checks = ""
    if rootfs_contract.filesystem_type == "ext4":
        derivative_checks = f"""
actual_lower_uuid=$(findmnt -n -o UUID -T /.uml-lower)
[ "$actual_lower_uuid" = {shlex.quote(rootfs_contract.filesystem_uuid)} ] || {{
    echo 'UML_CTS_ERROR: derivative filesystem UUID changed'
    exit 1
}}
actual_receipt_sha=$(sha256sum \
    {shlex.quote(rootfs_contract.guest_receipt_path)} | cut -d ' ' -f 1)
[ "$actual_receipt_sha" = \
    {shlex.quote(rootfs_contract.guest_receipt_sha256)} ] || {{
    echo 'UML_CTS_ERROR: in-guest derivative receipt changed'
    exit 1
}}
"""
    if inventory:
        export_pattern = f"{output}/${{packageName}}-cases.${{typeExtension}}"
        command = [
            glcts,
            "--deqp-runmode=txt-caselist",
            f"--deqp-caselist-export-file={export_pattern}",
            f"--deqp-archive-dir={archive}",
            "--deqp-gl-context-type=egl",
            "--deqp-duplicate-case-name-check=enable",
            f"--deqp-log-filename={qpa}",
        ]
    else:
        command = [
            glcts,
            "--deqp-gl-context-type=egl",
            f"--deqp-caselist-file={selected}",
            f"--deqp-archive-dir={archive}",
            "--deqp-watchdog=disable",
            "--deqp-crashhandler=disable",
            "--deqp-test-oom=disable",
            "--deqp-log-images=disable",
            "--deqp-log-shader-sources=disable",
            "--deqp-log-flush=enable",
            "--deqp-terminate-on-fail=disable",
            f"--deqp-base-seed={seed}",
            f"--deqp-log-filename={qpa}",
        ]
    quoted_command = " \\\n    ".join(shlex.quote(argument) for argument in command)
    leaf_path = "" if inventory else selection.leaf.path
    leaf_identity_check = ""
    prepare_command = ""
    post_run_command = ""
    if inventory:
        post_run_command = """
LC_ALL=C
export LC_ALL
: > "$selected"
exported_packages=0
for package_caselist in "$output"/*-cases.txt; do
    [ -f "$package_caselist" ] || continue
    cat "$package_caselist" >> "$selected"
    exported_packages=$((exported_packages + 1))
done
if [ "$status" -eq 0 ] && { [ "$exported_packages" -eq 0 ] || [ ! -s "$selected" ]; }; then
    echo 'UML_CTS_ERROR: complete text caselist export is empty'
    status=1
fi
complete=0
[ "$status" -ne 0 ] || complete=1
caselist_digest=$(sha256sum "$selected" | cut -d ' ' -f 1)
caselist_count=$(awk '$1 == "TEST:" { count++ } END { print count + 0 }' "$selected")
{
    printf 'schema_version=1\n'
    printf 'complete=%s\n' "$complete"
    printf 'exported_package_count=%s\n' "$exported_packages"
    printf 'complete_caselist_sha256=%s\n' "$caselist_digest"
    printf 'complete_caselist_case_count=%s\n' "$caselist_count"
    awk '$1 == "TEST:" || $1 == "GROUP:" { split($2, part, "."); print part[1] }' "$selected" |
        sort -u | sed 's/^/registered_module=/'
} > "$inventory_report"
chown uml:uml "$inventory_report" "$renderer"
"""
    elif not selection.exact_cases:
        selection_command = (
            "awk -v index="
            f"{selection.fraction_index} -v count={selection.fraction_count} "
            "'NF && $1 !~ /^#/ { if ((seen % count) == index) print; ++seen }' "
            '"$leaf" > "$selected"'
        )
    else:
        selection_lines = [': > "$selected"']
        selection_lines.extend(
            f"if ! grep -Fx -- {shlex.quote(case)} \"$leaf\" >> "
            '"$selected"; then '
            "echo 'UML_CTS_ERROR: exact case is absent from selected "
            "must-pass leaf'; exit 1; fi"
            for case in selection.exact_cases
        )
        selection_command = "\n".join(selection_lines)
    if not inventory:
        leaf_identity_check = f"""
actual_leaf_sha=$(sha256sum "$leaf" | cut -d ' ' -f 1)
[ "$actual_leaf_sha" = {shlex.quote(selection.leaf.sha256)} ] || {{
    echo 'UML_CTS_ERROR: selected must-pass leaf identity changed'
    exit 1
}}
"""
        prepare_command = f"""
{selection_command}
selected_count=$(grep -Evc '^[[:space:]]*(#|$)' "$selected")
[ "$selected_count" -eq {selection.expected_count} ] || {{
    echo "UML_CTS_ERROR: selected $selected_count cases, expected {selection.expected_count}"
    exit 1
}}
chown uml:uml "$selected" "$renderer"
"""

    return f"""#!/bin/sh
set -eu
output={shlex.quote(output)}
leaf={shlex.quote(leaf_path)}
selected={shlex.quote(selected)}
qpa={shlex.quote(qpa)}
renderer={shlex.quote(renderer)}
summary={shlex.quote(summary)}
inventory_report={shlex.quote(inventory_report)}
inventory_mode={1 if inventory else 0}
status=1

emit_artifact()
{{
    name="$1"
    source="$2"
    [ -f "$source" ] || return 0
    compressed="$output/$name.gz"
    encoded="$compressed.b64"
    gzip -c "$source" > "$compressed"
    base64 -w 1024 "$compressed" > "$encoded"
    size=$(wc -c < "$compressed" | tr -d ' ')
    digest=$(sha256sum "$compressed" | cut -d ' ' -f 1)
    chunks=$(wc -l < "$encoded" | tr -d ' ')
    printf 'UML_CTS_ARTIFACT_BEGIN|%s|gzip+base64|%s|%s\n' \
        "$name" "$size" "$digest"
    index=0
    while IFS= read -r chunk; do
        printf 'UML_CTS_ARTIFACT_CHUNK|%s|%s|%s\n' \
            "$name" "$index" "$chunk"
        index=$((index + 1))
    done < "$encoded"
    printf 'UML_CTS_ARTIFACT_END|%s|%s\n' "$name" "$chunks"
    rm -f "$compressed" "$encoded"
}}

finish()
{{
    trap - EXIT HUP INT TERM
    printf 'cts_rc=%s\n' "$status" > "$summary"
    printf 'UML_CTS_STATUS|rc=%s\n' "$status"
    emit_artifact renderer.txt "$renderer"
    if [ "$inventory_mode" -eq 1 ]; then
        emit_artifact inventory.txt "$inventory_report"
    else
        emit_artifact selected.txt "$selected"
    fi
    emit_artifact results.qpa "$qpa"
    emit_artifact summary.txt "$summary"
    sync
    systemctl poweroff --no-wall --no-block || kill -RTMIN+4 1
}}
trap finish EXIT HUP INT TERM

install -d -m 0755 -o uml -g uml "$output"
actual_page_size=$(getconf PAGESIZE)
[ "$actual_page_size" -eq {guest_page_size} ] || {{
    echo "UML_CTS_ERROR: guest page size $actual_page_size does not match {guest_page_size}"
    exit 1
}}
printf 'guest_page_size=%s\n' "$actual_page_size" >> "$renderer"
{derivative_checks}
{leaf_identity_check}
actual_glcts_sha=$(sha256sum {shlex.quote(glcts)} | cut -d ' ' -f 1)
[ "$actual_glcts_sha" = {shlex.quote(rootfs_contract.glcts_sha256)} ] || {{
    echo 'UML_CTS_ERROR: glcts identity changed'
    exit 1
}}

uml_uid=$(id -u uml)
runtime="/run/user/$uml_uid"
xdisplay=
xauthority=

find_xwayland()
{{
    candidates=$(pgrep -u "$uml_uid" -x Xwayland 2>/dev/null || true)
    for candidate_pid in $candidates; do
        command_line="/proc/$candidate_pid/cmdline"
        [ -r "$command_line" ] || continue
        candidate_display=$(tr '\\000' '\\n' < "$command_line" |
            grep -E '^:[0-9]+$' | head -n 1 || true)
        candidate_xauthority=$(tr '\\000' '\\n' < "$command_line" |
            awk 'previous == "-auth" {{ print; exit }} {{ previous = $0 }}')
        [ -n "$candidate_display" ] || continue
        case "$candidate_xauthority" in
        "$runtime"/xauth_*) ;;
        *) continue ;;
        esac
        xsocket="/tmp/.X11-unix/X${{candidate_display#:}}"
        [ -S "$xsocket" ] && [ -f "$candidate_xauthority" ] || continue
        xdisplay=$candidate_display
        xauthority=$candidate_xauthority
        return 0
    done
    return 1
}}

remaining=600
while [ ! -S "$runtime/bus" ] || ! find_xwayland; do
    [ "$remaining" -gt 0 ] || {{
        echo 'UML_CTS_ERROR: graphical Xwayland session did not become ready'
        exit 1
    }}
    remaining=$((remaining - 1))
    sleep 1
done
if pgrep -x Xvfb >/dev/null; then
    echo 'UML_CTS_ERROR: Xvfb is forbidden'
    exit 1
fi
for device in /dev/dri/card0 /dev/dri/renderD128; do
    [ -e "$device" ] || {{
        echo "UML_CTS_ERROR: missing DRM device $device"
        exit 1
    }}
    printf 'device=%s\n' "$device" >> "$renderer"
done
printf 'x11_server=xwayland\n' >> "$renderer"
runuser -u uml -- env \
    HOME=/home/uml DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    XDG_RUNTIME_DIR="$runtime" glxinfo -B >> "$renderer" 2>&1 || {{
        echo 'UML_CTS_ERROR: glxinfo failed'
        exit 1
    }}
runuser -u uml -- env \
    HOME=/home/uml DISPLAY="$xdisplay" XAUTHORITY="$xauthority" \
    XDG_RUNTIME_DIR="$runtime" eglinfo -B -p gbm >> "$renderer" 2>&1 || true
grep -Eiq 'virgl' "$renderer" || {{
    echo 'UML_CTS_ERROR: renderer is not Virgl'
    exit 1
}}
if grep -Eiq 'llvmpipe|softpipe|swrast|software rasterizer|zink|xvfb' "$renderer"; then
    echo 'UML_CTS_ERROR: software or forbidden renderer selected'
    exit 1
fi

{prepare_command}

set +e
runuser -u uml -- env \
    HOME=/home/uml DISPLAY="$xdisplay" WAYLAND_DISPLAY=wayland-0 \
    XAUTHORITY="$xauthority" XDG_RUNTIME_DIR="$runtime" \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
    timeout --signal=TERM --kill-after=30 {timeout} \
    {quoted_command}
status=$?
set -e
{post_run_command}
exit "$status"
"""


CTS_SERVICE = """[Unit]
Description=Darwin UML VK-GL-CTS developer shard
After=display-manager.service
Wants=display-manager.service

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/uml-vk-gl-cts
StandardInput=null
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=graphical.target
"""


def build_harness_command(
    options: argparse.Namespace,
    selection: Selection | None,
    artifact_directory: Path,
) -> list[str]:
    launcher_arguments = [
            "--display",
            options.display,
            "--emulator",
            str(options.emulator),
            "--rootfs",
            str(options.rootfs),
            "--rootfs-type",
            options.rootfs_type,
            "--memory",
            options.memory,
            "--cpus",
            str(options.cpus),
            "--engine",
            options.engine,
            "--width",
            str(options.width),
            "--height",
            str(options.height),
        ]
    if options.display == "vnc":
        launcher_arguments.extend(("--port", "0"))
    launcher_options = boot_kde_vnc.parse_arguments(launcher_arguments)
    command = boot_kde_vnc.build_harness_command(launcher_options)
    separator = boot_kde_vnc.guest_argument_separator(command)
    overlay_commands = (
        install_text(
            "/usr/local/libexec/uml-vk-gl-cts",
            build_guest_launcher(
                selection,
                options.seed,
                options.shard_timeout,
                16384 if options.page_profile == "16k" else 4096,
                options.rootfs_contract,
            ),
            "0755",
        ),
        install_text(
            "/etc/systemd/system/uml-vk-gl-cts.service",
            CTS_SERVICE,
        ),
        "systemctl start --no-block uml-vk-gl-cts.service",
    )
    additions = ["--artifact-root", str(artifact_directory / "guest")]
    for overlay_command in overlay_commands:
        additions.extend(("--command", overlay_command))
    command[separator:separator] = additions
    launch_environment = ["UML_GPU_CAPABILITY_REPORT=1"]
    if options.display == "native":
        launch_environment.append("UML_GPU_FRAME_STATS=1")
    return ["env", *launch_environment, *command]


def stop_guest(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT)
    try:
        process.wait(timeout=45)
    except subprocess.TimeoutExpired:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def wait_for_guest(
    process: subprocess.Popen[bytes],
    log_path: Path,
    timeout: int,
) -> str:
    deadline = time.monotonic() + timeout
    next_heartbeat = time.monotonic() + 30
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        now = time.monotonic()
        if now >= next_heartbeat:
            size = log_path.stat().st_size if log_path.exists() else 0
            print(f"VK-GL-CTS guest is running; console_bytes={size}", flush=True)
            next_heartbeat = now + 30
        time.sleep(0.5)
    else:
        raise TimeoutError(f"VK-GL-CTS guest exceeded {timeout} seconds")
    process.wait(timeout=5)
    return log_path.read_text(encoding="utf-8", errors="replace")


def git_state() -> dict[str, Any]:
    revision = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    dirty = subprocess.run(
        ["git", "status", "--porcelain=v1"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    return {"revision": revision, "dirty_paths": dirty}


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def api_for_leaf(identifier: str) -> str:
    return identifier.split("-", 1)[0]


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    identity = load_identity()
    leaves = leaf_identities(identity)
    campaigns = load_apple_campaigns(identity)
    parser = argparse.ArgumentParser(
        description="run a bounded in-image VK-GL-CTS shard through Virgl"
    )
    parser.add_argument(
        "--inventory",
        action="store_true",
        help="capture the complete display-backed glcts text caselist",
    )
    parser.add_argument("--campaign", choices=sorted(campaigns))
    parser.add_argument("--leaf", choices=sorted(leaves))
    parser.add_argument(
        "--api",
        choices=sorted({api_for_leaf(identifier) for identifier in leaves}),
    )
    parser.add_argument("--context", choices=("egl",), default="egl")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--case", type=exact_case)
    selection.add_argument("--fraction", type=parse_fraction)
    parser.add_argument("--seed", type=int)
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--display", choices=("vnc", "native"), default="vnc")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=positive_integer, default=10)
    parser.add_argument("--width", type=positive_integer, default=1280)
    parser.add_argument("--height", type=positive_integer, default=720)
    parser.add_argument("--shard-timeout", type=positive_integer)
    parser.add_argument("--guest-timeout", type=positive_integer, default=1200)
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument(
        "--rootfs-type",
        choices=("erofs", "ext4"),
        default="erofs",
        help="authenticated lower filesystem type (default: erofs)",
    )
    parser.add_argument(
        "--rootfs-receipt",
        type=Path,
        help="required identity receipt for a derivative ext4 rootfs",
    )
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument(
        "--qualification",
        action="store_true",
        help="require a frozen build receipt, caselist, and official matrix",
    )
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args(arguments)
    if options.inventory:
        if (
            options.campaign is not None
            or options.leaf is not None
            or options.api is not None
        ):
            parser.error("--inventory does not accept a campaign, leaf, or API")
        if options.case is not None or options.fraction is not None:
            parser.error("--inventory does not accept a case or fraction")
        if options.qualification:
            parser.error("inventory capture is diagnostic, not qualification")
    elif options.campaign is not None:
        if (
            options.leaf is not None
            or options.api is not None
            or options.case is not None
            or options.fraction is not None
        ):
            parser.error(
                "--campaign does not accept a leaf, API, case, or fraction"
            )
        if options.seed is not None or options.shard_timeout is not None:
            parser.error("--campaign owns its seed and shard timeout")
        campaign = campaigns[options.campaign]
        options.seed = campaign.seed
        options.shard_timeout = campaign.shard_timeout
    else:
        if options.leaf is None or options.api is None:
            parser.error("a developer shard requires --leaf and --api")
        if api_for_leaf(options.leaf) != options.api:
            parser.error(
                f"--leaf {options.leaf} does not belong to --api {options.api}"
            )
    if options.seed is None:
        options.seed = 0
    if options.shard_timeout is None:
        options.shard_timeout = 900
    if options.fraction is None:
        options.fraction = (0, 1)
    if options.seed < 0 or options.seed > 0xFFFFFFFF:
        parser.error("--seed must be between 0 and 4294967295")
    if options.qualification and not identity["cts"]["qualification_ready"]:
        parser.error(identity["cts"]["qualification_blocker"])
    if options.emulator is None:
        options.emulator = (
            DEFAULT_EMULATOR
            if options.page_profile == "16k"
            else REPO_ROOT / ".build/presets/macos-integration-4k/bin/uml-darwin"
        )
    try:
        receipt, contract = select_rootfs_contract(
            identity, options.rootfs_type, options.rootfs_receipt
        )
    except CampaignError as error:
        parser.error(str(error))
    options.derivative_receipt = receipt
    options.rootfs_contract = contract
    options.identity = identity
    options.leaves = leaves
    options.campaigns = campaigns
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    fraction_index, fraction_count = options.fraction
    leaf: LeafIdentity | None = None
    selection: Selection | None = None
    suffix = "inventory"
    if not options.inventory:
        if options.campaign is not None:
            campaign = options.campaigns[options.campaign]
            leaf = campaign.leaf
            selection = campaign.selection
            api_identifier = campaign.api
            leaf_identifier = campaign.leaf.identifier
            suffix = campaign.identifier
        else:
            leaf = options.leaves[options.leaf]
            exact_cases = (options.case,) if options.case is not None else ()
            selection = Selection(
                leaf, exact_cases, fraction_index, fraction_count
            )
            api_identifier = options.api
            leaf_identifier = options.leaf
            suffix = options.leaf
        if selection.expected_count < 1:
            raise CampaignError("selected fraction contains no cases")

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact_directory = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-{options.page_profile}-{suffix}"
    )
    command = build_harness_command(options, selection, artifact_directory)
    if options.dry_run:
        print(shlex.join(command))
        return 0

    emulator = options.emulator.resolve()
    rootfs = options.rootfs.resolve()
    if not emulator.is_file() or not os.access(emulator, os.X_OK):
        raise CampaignError(f"emulator is not executable: {emulator}")
    emulator_bytes = emulator.read_bytes()
    if RETIRED_BACKEND_MARKER.encode() in emulator_bytes:
        raise CampaignError("emulator contains the retired CGL provider")
    if HOST_BACKEND_MARKER.encode() not in emulator_bytes:
        raise CampaignError("emulator does not contain the ANGLE/Metal provider marker")
    rootfs_contract = options.rootfs_contract
    if rootfs_contract.filesystem_type == "erofs":
        if not rootfs.is_file() or rootfs.stat().st_size != rootfs_contract.size:
            raise CampaignError("rootfs size does not match the frozen CTS identity")
        subprocess.run(
            [
                sys.executable,
                str(REPO_ROOT / "tools/rootfs/verify.py"),
                "--image",
                str(rootfs),
                "--manifest",
                str(REPO_ROOT / "tools/rootfs/manifest.json"),
                "--quiet",
            ],
            check=True,
        )
    else:
        validate_rootfs_file(rootfs, rootfs_contract)

    artifact_directory.mkdir(parents=True, exist_ok=False)
    metadata = {
        "schema_version": 1,
        "git": git_state(),
        "adapter": "macos",
        "backend": "angle-metal",
        "page_profile": options.page_profile,
        "engine": options.engine,
        "memory": options.memory,
        "cpus": options.cpus,
        "display": options.display,
        "surface": [options.width, options.height],
        "context": options.context,
        "mode": "inventory" if options.inventory else "shard",
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "qualification": options.qualification,
        "launch_environment": {
            "UML_GPU_CAPABILITY_REPORT": "1",
            **(
                {"UML_GPU_FRAME_STATS": "1"}
                if options.display == "native"
                else {}
            ),
        },
        "command": command,
    }
    if options.derivative_receipt is not None:
        metadata["rootfs_receipt"] = options.derivative_receipt
    if options.inventory:
        metadata["expected_artifacts"] = [
            "inventory.txt",
            "renderer.txt",
            "results.qpa",
            "summary.txt",
        ]
    else:
        metadata.update(
            {
                "api": api_identifier,
                "leaf": leaf_identifier,
                "leaf_sha256": leaf.sha256,
                "fraction": [fraction_index, fraction_count],
                "exact_cases": list(selection.exact_cases),
                "expected_cases": selection.expected_count,
                "seed": options.seed,
            }
        )
        if options.campaign is not None:
            metadata["campaign"] = options.campaign
    write_json(artifact_directory / "run.json", metadata)
    log_path = artifact_directory / "console.log"
    with log_path.open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            transcript = wait_for_guest(process, log_path, options.guest_timeout)
        finally:
            stop_guest(process)

    if process.returncode != 0:
        raise CampaignError(f"guest harness exited with status {process.returncode}")
    validate_host_backend(transcript)
    validate_guest_shutdown(transcript)
    metadata.update(parse_gpu_capability_report(transcript))
    if options.display == "native":
        metadata["native_display_final"] = parse_native_display_final(transcript)
    write_json(artifact_directory / "run.json", metadata)
    artifacts = decode_artifacts(transcript)
    required_artifacts = INVENTORY_ARTIFACTS if options.inventory else SHARD_ARTIFACTS
    required = required_artifacts - set(artifacts)
    if required:
        guest_errors = GUEST_ERROR.findall(transcript)
        if guest_errors:
            raise CampaignError(f"guest campaign failed: {guest_errors[-1]}")
        raise CampaignError(f"campaign omitted artifacts: {sorted(required)!r}")
    unexpected = set(artifacts) - required_artifacts
    if unexpected:
        raise CampaignError(
            f"campaign emitted unexpected artifacts: {sorted(unexpected)!r}"
        )
    output_directory = artifact_directory / "artifacts"
    write_artifacts(artifacts, output_directory)
    metadata["renderer_sha256"] = hashlib.sha256(
        artifacts["renderer.txt"]
    ).hexdigest()
    renderer_text = artifacts["renderer.txt"].decode("utf-8", errors="strict")
    summary_text = artifacts["summary.txt"].decode("utf-8", errors="strict")
    validate_renderer_probe(
        renderer_text,
        16384 if options.page_profile == "16k" else 4096,
    )
    if summary_text.strip() != "cts_rc=0":
        raise CampaignError(f"CTS process failed: {summary_text.strip()}")
    if options.inventory:
        case_count, caselist_digest, modules, package_count = validate_inventory_report(
            artifacts["inventory.txt"], options.identity
        )
        metadata.update(
            {
                "emulator_return_code": process.returncode,
                "complete_caselist_case_count": case_count,
                "complete_caselist_sha256": caselist_digest,
                "exported_package_count": package_count,
                "registered_modules": modules,
            }
        )
        write_json(artifact_directory / "run.json", metadata)
        print(
            f"VK-GL-CTS inventory captured {case_count} cases "
            f"from {len(modules)} modules"
        )
        print(f"Artifacts: {artifact_directory}")
        return 0

    selected_text = artifacts["selected.txt"].decode("utf-8", errors="strict")
    selected_cases = parse_case_list(selected_text)
    selected_digest = hashlib.sha256(artifacts["selected.txt"]).hexdigest()
    if len(selected_cases) != selection.expected_count:
        raise CampaignError("egressed caselist count changed")
    qpa_text = artifacts["results.qpa"].decode("utf-8", errors="strict")
    results = validate_qpa_union(qpa_text, selected_cases)
    totals = status_totals(results)
    metadata.update(
        {
            "emulator_return_code": process.returncode,
            "completed_cases": len(results),
            "status_totals": totals,
            "selected_caselist_sha256": selected_digest,
            "qpa_sha256": hashlib.sha256(artifacts["results.qpa"]).hexdigest(),
        }
    )
    write_json(artifact_directory / "run.json", metadata)
    validate_required_passes(results)
    print(f"VK-GL-CTS completed {len(results)} cases: {totals}")
    print(f"Artifacts: {artifact_directory}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print("run_vk_gl_cts.py: interrupted; guest stopped", file=sys.stderr)
        raise SystemExit(130) from None
    except (CampaignError, OSError, subprocess.SubprocessError, TimeoutError) as error:
        print(f"run_vk_gl_cts.py: {error}", file=sys.stderr)
        raise SystemExit(1) from error
