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
import subprocess
import sys
import time
from typing import Any, Sequence


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
ALLOWED_ARTIFACTS = frozenset(
    {"renderer.txt", "results.qpa", "selected.txt", "summary.txt"}
)
CASE_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.:-]*\Z")
APPLE_CAMPAIGN_ID = re.compile(r"[a-z0-9][a-z0-9-]*\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
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
RESULT_STATUS = re.compile(r"<Result\s+StatusCode=\"([^\"]+)\"")
RESULT_CASE = re.compile(r"<TestCaseResult\b[^>]*\bCasePath=\"([^\"]+)\"")
GUEST_ERROR = re.compile(r"UML_CTS_ERROR: ([^\r\n]+)")
HOST_BACKEND_MARKER = "Virgl ANGLE/Metal provider:"
RETIRED_BACKEND_MARKER = "Virgl CGL"


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
    exact_case: str | None
    fraction_index: int
    fraction_count: int

    @property
    def expected_count(self) -> int:
        if self.exact_case is not None:
            return 1
        remaining = self.leaf.case_count - self.fraction_index
        if remaining <= 0:
            return 0
        return (remaining + self.fraction_count - 1) // self.fraction_count


@dataclass(frozen=True)
class AppleCampaign:
    identifier: str
    api: str
    leaf: LeafIdentity
    exact_case: str
    seed: int
    shard_timeout: int

    @property
    def selection(self) -> Selection:
        return Selection(self.leaf, self.exact_case, 0, 1)


@dataclass
class EncodedArtifact:
    compressed_size: int
    sha256: str
    chunks: dict[int, str] = field(default_factory=dict)
    expected_chunks: int | None = None


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
    if value["schema_version"] != 1 or not isinstance(
        value["campaigns"], list
    ):
        raise CampaignError("unsupported Apple CTS campaign schema")

    result: dict[str, AppleCampaign] = {}
    required_keys = {
        "id",
        "api",
        "leaf",
        "exact_case",
        "seed",
        "shard_timeout",
    }
    for row in value["campaigns"]:
        if not isinstance(row, dict) or set(row) != required_keys:
            raise CampaignError("invalid Apple CTS campaign row")
        identifier = row["id"]
        api = row["api"]
        leaf_identifier = row["leaf"]
        exact = row["exact_case"]
        seed = row["seed"]
        shard_timeout = row["shard_timeout"]
        if (
            not isinstance(identifier, str)
            or not APPLE_CAMPAIGN_ID.fullmatch(identifier)
            or not isinstance(api, str)
            or not isinstance(leaf_identifier, str)
            or not isinstance(exact, str)
            or not CASE_NAME.fullmatch(exact)
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
            exact_case=exact,
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


def artifact_marker(line: str) -> str | None:
    offset = line.find("UML_CTS_ARTIFACT_")
    if offset < 0:
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
    selection: Selection,
    seed: int,
    timeout: int,
    guest_page_size: int,
) -> str:
    glcts = "/opt/vk-gl-cts/openglcts/glcts"
    archive = "/opt/vk-gl-cts/openglcts"
    output = "/run/uml-cts"
    selected = f"{output}/selected.txt"
    qpa = f"{output}/results.qpa"
    renderer = f"{output}/renderer.txt"
    summary = f"{output}/summary.txt"
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
    if selection.exact_case is None:
        selection_command = (
            "awk -v index="
            f"{selection.fraction_index} -v count={selection.fraction_count} "
            "'NF && $1 !~ /^#/ { if ((seen % count) == index) print; ++seen }' "
            '"$leaf" > "$selected"'
        )
    else:
        selection_command = (
            f"if ! grep -Fx -- {shlex.quote(selection.exact_case)} "
            '"$leaf" > "$selected"; then '
            "echo 'UML_CTS_ERROR: exact case is absent from selected must-pass leaf'; "
            "exit 1; fi"
        )

    return f"""#!/bin/sh
set -eu
output={shlex.quote(output)}
leaf={shlex.quote(selection.leaf.path)}
selected={shlex.quote(selected)}
qpa={shlex.quote(qpa)}
renderer={shlex.quote(renderer)}
summary={shlex.quote(summary)}
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
    emit_artifact selected.txt "$selected"
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
actual_leaf_sha=$(sha256sum "$leaf" | cut -d ' ' -f 1)
[ "$actual_leaf_sha" = {shlex.quote(selection.leaf.sha256)} ] || {{
    echo 'UML_CTS_ERROR: selected must-pass leaf identity changed'
    exit 1
}}
actual_glcts_sha=$(sha256sum {shlex.quote(glcts)} | cut -d ' ' -f 1)
[ "$actual_glcts_sha" = b74aa4337f7225bceb0f81ccad1da96f391efde5591f18030b614b8f5380c664 ] || {{
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

{selection_command}
selected_count=$(grep -Evc '^[[:space:]]*(#|$)' "$selected")
[ "$selected_count" -eq {selection.expected_count} ] || {{
    echo "UML_CTS_ERROR: selected $selected_count cases, expected {selection.expected_count}"
    exit 1
}}
chown uml:uml "$selected" "$renderer"

set +e
runuser -u uml -- env \
    HOME=/home/uml DISPLAY="$xdisplay" WAYLAND_DISPLAY=wayland-0 \
    XAUTHORITY="$xauthority" XDG_RUNTIME_DIR="$runtime" \
    DBUS_SESSION_BUS_ADDRESS="unix:path=$runtime/bus" \
    timeout --signal=TERM --kill-after=30 {timeout} \
    {quoted_command}
status=$?
set -e
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
    selection: Selection,
    artifact_directory: Path,
) -> list[str]:
    launcher_arguments = [
            "--display",
            options.display,
            "--emulator",
            str(options.emulator),
            "--rootfs",
            str(options.rootfs),
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
    return command


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
    parser = argparse.ArgumentParser(
        description="run a bounded in-image VK-GL-CTS shard through Virgl"
    )
    parser.add_argument("--leaf", required=True, choices=sorted(leaves))
    parser.add_argument(
        "--api",
        required=True,
        choices=sorted({api_for_leaf(identifier) for identifier in leaves}),
    )
    parser.add_argument("--context", choices=("egl",), default="egl")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--case", type=exact_case)
    selection.add_argument("--fraction", type=parse_fraction, default=(0, 1))
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--display", choices=("vnc", "native"), default="vnc")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=positive_integer, default=10)
    parser.add_argument("--width", type=positive_integer, default=1280)
    parser.add_argument("--height", type=positive_integer, default=720)
    parser.add_argument("--shard-timeout", type=positive_integer, default=900)
    parser.add_argument("--guest-timeout", type=positive_integer, default=1200)
    parser.add_argument("--emulator", type=Path)
    parser.add_argument("--rootfs", type=Path, default=DEFAULT_ROOTFS)
    parser.add_argument("--artifact-root", type=Path, default=DEFAULT_ARTIFACT_ROOT)
    parser.add_argument(
        "--qualification",
        action="store_true",
        help="require a frozen build receipt, caselist, and official matrix",
    )
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args(arguments)
    if api_for_leaf(options.leaf) != options.api:
        parser.error(f"--leaf {options.leaf} does not belong to --api {options.api}")
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
    options.identity = identity
    options.leaves = leaves
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    fraction_index, fraction_count = options.fraction
    leaf = options.leaves[options.leaf]
    selection = Selection(leaf, options.case, fraction_index, fraction_count)
    if selection.expected_count < 1:
        raise CampaignError("selected fraction contains no cases")

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    artifact_directory = options.artifact_root.resolve() / (
        f"{timestamp}-{os.getpid()}-{options.page_profile}-{options.leaf}"
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
    if not rootfs.is_file():
        raise CampaignError(f"rootfs is not a regular file: {rootfs}")
    rootfs_identity = options.identity["rootfs"]
    if rootfs.stat().st_size != rootfs_identity["size"]:
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
        "api": options.api,
        "context": options.context,
        "leaf": options.leaf,
        "leaf_sha256": leaf.sha256,
        "fraction": [fraction_index, fraction_count],
        "exact_case": options.case,
        "expected_cases": selection.expected_count,
        "seed": options.seed,
        "rootfs_sha256": rootfs_identity["sha256"],
        "qualification": options.qualification,
        "command": command,
    }
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
    artifacts = decode_artifacts(transcript)
    required = ALLOWED_ARTIFACTS - set(artifacts)
    if required:
        guest_errors = GUEST_ERROR.findall(transcript)
        if guest_errors:
            raise CampaignError(f"guest campaign failed: {guest_errors[-1]}")
        raise CampaignError(f"campaign omitted artifacts: {sorted(required)!r}")
    output_directory = artifact_directory / "artifacts"
    write_artifacts(artifacts, output_directory)
    renderer_text = artifacts["renderer.txt"].decode("utf-8", errors="strict")
    selected_text = artifacts["selected.txt"].decode("utf-8", errors="strict")
    qpa_text = artifacts["results.qpa"].decode("utf-8", errors="strict")
    summary_text = artifacts["summary.txt"].decode("utf-8", errors="strict")
    validate_renderer_probe(
        renderer_text,
        16384 if options.page_profile == "16k" else 4096,
    )
    selected_cases = parse_case_list(selected_text)
    if len(selected_cases) != selection.expected_count:
        raise CampaignError("egressed caselist count changed")
    results = validate_qpa_union(qpa_text, selected_cases)
    if summary_text.strip() != "cts_rc=0":
        raise CampaignError(f"CTS process failed: {summary_text.strip()}")
    totals = status_totals(results)
    metadata.update(
        {
            "emulator_return_code": process.returncode,
            "completed_cases": len(results),
            "status_totals": totals,
            "selected_caselist_sha256": hashlib.sha256(
                artifacts["selected.txt"]
            ).hexdigest(),
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
