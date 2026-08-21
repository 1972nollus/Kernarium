#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run a bounded VK-GL-CTS shard in the ordinary rendered guest.

The authenticated rootfs owns the CTS executable, resources, Mesa, libdrm,
and WSI.  This runner injects only a first-party launcher and transient output
into the disposable OverlayFS upper layer.  It deliberately refuses the old
macOS CGL renderer and requires the common ANGLE/Metal product.

Official must-pass qualification remains fail-closed while the original image
has no authoritative CTS build receipt and the complete matrices have not run.
Developer shards are still useful for bring-up and failure localization.
"""

from __future__ import annotations

# Standalone runners must not leave source-adjacent bytecode caches.
__import__("sys").dont_write_bytecode = True

import argparse
import base64
import binascii
from dataclasses import dataclass, field
import errno
import gzip
import hashlib
import io
import json
import os
from pathlib import Path
import re
import select
import shlex
import signal
import socket
import stat
import subprocess
import sys
import time
from typing import Any, Sequence
from xml.etree import ElementTree

sys.path.insert(0, str(Path(__file__).resolve().parent))
import virgl_capset  # noqa: E402


REPO_ROOT = Path(__file__).resolve().parents[3]
IDENTITY_PATH = Path(__file__).with_name("vk_gl_cts_identity.json")
APPLE_CAMPAIGNS_PATH = Path(__file__).with_name(
    "vk_gl_cts_apple_campaigns.json"
)
GLES_MUSTPASS_PATH = Path(__file__).with_name(
    "vk_gl_cts_gles_mustpass.xml"
)
RUNNER_ORACLE_PATH = Path(__file__).with_name(
    "vk_gl_cts_runner_oracle.json"
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
CONFIG_INVENTORY_ARTIFACTS = frozenset(
    {"renderer.txt", "results.qpa", "summary.txt"}
)
ALLOWED_ARTIFACTS = (
    SHARD_ARTIFACTS | INVENTORY_ARTIFACTS | CONFIG_INVENTORY_ARTIFACTS
)
GLES_INVENTORY_SCOPE = "gles2-gles3"
GLES_INVENTORY_MODULES = ("KHR-GLES2", "KHR-GLES3")
BASELINE_GLES_APIS = ("gles2", "gles3")
CONFIG_CASES = {
    "gles2": "CTS-Configs.es2",
    "gles3": "CTS-Configs.es3",
}
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
ACK_ARTIFACT = re.compile(
    r"UML_CTS_ARTIFACT_ACK\|([^|]+)\|(begin|end|[0-9]+)\Z"
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
EGL_CONFIG_MESSAGE = re.compile(
    r"EGL\(([1-9][0-9]*)\): "
    r"((?:window|pixmap|pbuffer)"
    r"(?:, (?:window|pixmap|pbuffer))*)\Z"
)
GUEST_ERROR = re.compile(r"UML_CTS_ERROR: ([^\r\n]+)")
HOST_BACKEND_MARKER = "Virgl ANGLE/Metal provider:"
RETIRED_BACKEND_MARKER = "Virgl CGL"
DARWIN_CLEAN_STOP = "Darwin UML stopped: result=0 exitcode=0"
DIRTY_DRM_SHUTDOWN = "Memory manager not clean during takedown."
MAX_ARTIFACT_TRANSPORT_LINE = 2048
VIRTIO_SERIAL_PORT_NAME = "org.uml.host-serial.0"
VIRTIO_SERIAL_GUEST_DEVICE = (
    f"/dev/virtio-ports/{VIRTIO_SERIAL_PORT_NAME}"
)
ARTIFACT_TRANSPORT_LOG = "artifact-serial.log"


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
class MustpassConfiguration:
    api: str
    leaf: LeafIdentity
    name: str
    use_for_first_egl_config: bool
    screen_rotation: str
    surface_width: int
    surface_height: int
    base_seed: int
    watchdog: str
    fbo_config: str | None
    xml_arguments: tuple[str, ...]


@dataclass(frozen=True)
class EglConfig:
    identifier: int
    surface_types: tuple[str, ...]

    @property
    def selected_surface_type(self) -> str:
        for surface_type in ("window", "pbuffer", "pixmap"):
            if surface_type in self.surface_types:
                return surface_type
        raise CampaignError(
            f"EGL config {self.identifier} has no supported surface type"
        )


@dataclass(frozen=True)
class MatrixRun:
    configuration: MustpassConfiguration
    egl_config: EglConfig
    egl_config_index: int
    config_inventory_sha256: str
    selection: Selection
    mustpass_xml_path: str
    mustpass_xml_size: int
    mustpass_xml_sha256: str


@dataclass(frozen=True)
class RunnerOracle:
    path: str
    size: int
    sha256: str
    fixture_configs: tuple[EglConfig, ...]
    successful_case_statuses: tuple[str, ...]


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


class AcknowledgedArtifactSocket:
    """Drain the guest Virtio serial stream and acknowledge every frame."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.connection: socket.socket | None = None
        self.input = bytearray()
        self.output = bytearray()
        self.lines: list[str] = []
        self.connected = False
        self.closed = False

    def try_connect(self) -> None:
        if self.connection is not None or self.connected:
            return
        try:
            details = self.path.lstat()
        except FileNotFoundError:
            return
        if stat.S_ISLNK(details.st_mode) or not stat.S_ISSOCK(details.st_mode):
            raise CampaignError(
                f"artifact transport path is not a Unix socket: {self.path}"
            )
        if (
            stat.S_IMODE(details.st_mode) != 0o600
            or details.st_uid != os.getuid()
        ):
            raise CampaignError("artifact transport socket ownership changed")
        connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connection.settimeout(0.1)
        try:
            connection.connect(str(self.path))
        except OSError as error:
            connection.close()
            if error.errno in {errno.ENOENT, errno.ECONNREFUSED}:
                return
            raise CampaignError(
                f"cannot connect to artifact transport {self.path}: {error}"
            ) from error
        connection.setblocking(False)
        self.connection = connection
        self.connected = True

    def service(self) -> None:
        self.try_connect()
        if self.connection is None:
            return
        readable, writable, _ = select.select(
            [self.connection],
            [self.connection] if self.output else [],
            [],
            0,
        )
        if self.connection in readable:
            try:
                payload = self.connection.recv(64 * 1024)
            except BlockingIOError:
                payload = None
            except OSError as error:
                raise CampaignError(
                    f"artifact transport receive failed: {error}"
                ) from error
            if payload == b"":
                self.connection.close()
                self.connection = None
                self.closed = True
                if self.input or self.output:
                    raise CampaignError(
                        "artifact transport closed with an incomplete frame"
                    )
            elif payload:
                self.input.extend(payload)
                self._parse_input()
        if self.connection is not None and self.connection in writable:
            try:
                written = self.connection.send(self.output)
            except BlockingIOError:
                written = 0
            except OSError as error:
                raise CampaignError(
                    f"artifact transport acknowledgement failed: {error}"
                ) from error
            if written > 0:
                del self.output[:written]

    def _parse_input(self) -> None:
        while b"\n" in self.input:
            raw_line, _, remainder = self.input.partition(b"\n")
            self.input = bytearray(remainder)
            if (
                not raw_line
                or len(raw_line) > MAX_ARTIFACT_TRANSPORT_LINE
                or raw_line.endswith(b"\r")
            ):
                raise CampaignError("artifact transport emitted an invalid line")
            try:
                line = raw_line.decode("ascii", errors="strict")
            except UnicodeDecodeError as error:
                raise CampaignError(
                    "artifact transport emitted a non-ASCII line"
                ) from error
            acknowledgement = artifact_acknowledgement(line)
            self.lines.append(line + "\n")
            self.output.extend((acknowledgement + "\n").encode("ascii"))
        if len(self.input) > MAX_ARTIFACT_TRANSPORT_LINE:
            raise CampaignError("artifact transport line exceeds its bound")

    def finish(self) -> str:
        if not self.connected:
            raise CampaignError("guest never connected the artifact transport")
        if self.input or self.output:
            raise CampaignError("artifact transport ended with pending bytes")
        return "".join(self.lines)

    def close(self) -> None:
        if self.connection is not None:
            self.connection.close()
            self.connection = None


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


def artifact_socket_path(artifact_directory: Path) -> Path:
    identity = hashlib.sha256(
        os.fsencode(artifact_directory.resolve())
    ).hexdigest()[:20]
    path = Path("/private/tmp") / f"uml-cts-artifact-{identity}.sock"
    if len(os.fsencode(path)) >= 104:
        raise CampaignError("CTS artifact socket path is too long")
    return path


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


def parse_mustpass_command(arguments: tuple[str, ...]) -> dict[str, Any]:
    values: dict[str, str] = {}
    allowed = {
        "deqp-screen-rotation",
        "deqp-surface-width",
        "deqp-surface-height",
        "deqp-base-seed",
        "deqp-gl-config-name",
        "deqp-surface-type",
        "deqp-watchdog",
    }
    for argument in arguments:
        match = re.fullmatch(r"--([a-z0-9-]+)=([^\s]+)", argument)
        if match is None or match.group(1) not in allowed:
            raise CampaignError(
                f"unsupported GLES must-pass argument: {argument!r}"
            )
        key, value = match.groups()
        if key in values:
            raise CampaignError(f"duplicate GLES must-pass argument: {key}")
        values[key] = value

    required = {
        "deqp-screen-rotation",
        "deqp-surface-width",
        "deqp-surface-height",
        "deqp-base-seed",
        "deqp-watchdog",
    }
    if not required.issubset(values):
        raise CampaignError("GLES must-pass command is incomplete")
    if values["deqp-screen-rotation"] != "unspecified":
        raise CampaignError("unsupported GLES must-pass screen rotation")
    if values["deqp-watchdog"] != "disable":
        raise CampaignError("unsupported GLES must-pass watchdog policy")
    try:
        surface_width = int(values["deqp-surface-width"])
        surface_height = int(values["deqp-surface-height"])
        base_seed = int(values["deqp-base-seed"])
    except ValueError as error:
        raise CampaignError("GLES must-pass numeric argument is invalid") from error
    if (
        surface_width == 0
        or surface_width < -1
        or surface_height == 0
        or surface_height < -1
        or base_seed < 0
        or base_seed > 0xFFFFFFFF
    ):
        raise CampaignError("GLES must-pass numeric argument is out of range")

    gl_config = values.get("deqp-gl-config-name")
    surface_type = values.get("deqp-surface-type")
    if (gl_config is None) != (surface_type is None):
        raise CampaignError("GLES must-pass FBO configuration is incomplete")
    if gl_config is not None and (
        re.fullmatch(r"[a-z0-9]+", gl_config) is None
        or surface_type != "fbo"
    ):
        raise CampaignError("unsupported GLES must-pass FBO configuration")
    return {
        "screen_rotation": values["deqp-screen-rotation"],
        "surface_width": surface_width,
        "surface_height": surface_height,
        "base_seed": base_seed,
        "watchdog": values["deqp-watchdog"],
        "fbo_config": gl_config,
    }


def load_gles_mustpass(
    identity: dict[str, Any] | None = None,
    path: Path = GLES_MUSTPASS_PATH,
) -> dict[str, tuple[MustpassConfiguration, ...]]:
    if identity is None:
        identity = load_identity()
    try:
        xml_identity = identity["mustpass"]["gles_xml"]
    except (KeyError, TypeError) as error:
        raise CampaignError("CTS identity has no GLES must-pass XML") from error
    if (
        not isinstance(xml_identity, dict)
        or set(xml_identity) != {"path", "size", "sha256"}
        or type(xml_identity["size"]) is not int
        or xml_identity["size"] < 1
        or not isinstance(xml_identity["sha256"], str)
        or SHA256.fullmatch(xml_identity["sha256"]) is None
    ):
        raise CampaignError("invalid GLES must-pass XML identity")
    try:
        payload = path.read_bytes()
    except OSError as error:
        raise CampaignError(f"cannot read GLES must-pass XML {path}: {error}") from error
    if len(payload) != xml_identity["size"]:
        raise CampaignError("repository GLES must-pass XML size changed")
    if hashlib.sha256(payload).hexdigest() != xml_identity["sha256"]:
        raise CampaignError("repository GLES must-pass XML identity changed")
    try:
        root = ElementTree.fromstring(payload)
    except ElementTree.ParseError as error:
        raise CampaignError("repository GLES must-pass XML is invalid") from error
    if root.tag != "Mustpass" or root.attrib != {"version": "main"}:
        raise CampaignError("unsupported GLES must-pass document")
    packages = list(root)
    if len(packages) != 1:
        raise CampaignError("GLES must-pass document has an unexpected package set")
    package = packages[0]
    if package.tag != "TestPackage" or package.attrib != {
        "name": "Khronos Mustpass ES"
    }:
        raise CampaignError("unsupported GLES must-pass package")

    leaves = leaf_identities(identity)
    leaves_by_name = {Path(leaf.path).name: leaf for leaf in leaves.values()}
    rows: dict[str, list[MustpassConfiguration]] = {}
    required_attributes = {
        "caseListFile",
        "commandLine",
        "name",
        "os",
        "useForFirstEGLConfig",
    }
    for element in package:
        if (
            element.tag != "Configuration"
            or set(element.attrib) != required_attributes
            or list(element)
            or (element.text is not None and element.text.strip())
        ):
            raise CampaignError("invalid GLES must-pass configuration")
        case_list_file = element.attrib["caseListFile"]
        if Path(case_list_file).name != case_list_file:
            raise CampaignError("unsafe GLES must-pass caselist path")
        leaf = leaves_by_name.get(case_list_file)
        if leaf is None:
            raise CampaignError(
                f"unidentified GLES must-pass caselist: {case_list_file}"
            )
        if element.attrib["os"] != "any" or re.fullmatch(
            r"[a-z0-9-]+", element.attrib["name"]
        ) is None:
            raise CampaignError("unsupported GLES must-pass configuration scope")
        first_text = element.attrib["useForFirstEGLConfig"]
        if first_text not in {"True", "False"}:
            raise CampaignError("invalid GLES first-config selector")
        try:
            xml_arguments = tuple(shlex.split(element.attrib["commandLine"]))
        except ValueError as error:
            raise CampaignError("invalid GLES must-pass command line") from error
        values = parse_mustpass_command(xml_arguments)
        configuration = MustpassConfiguration(
            api=leaf.identifier,
            leaf=leaf,
            name=element.attrib["name"],
            use_for_first_egl_config=first_text == "True",
            xml_arguments=xml_arguments,
            **values,
        )
        rows.setdefault(leaf.identifier, []).append(configuration)

    result = {api: tuple(configurations) for api, configurations in rows.items()}
    for api in BASELINE_GLES_APIS:
        configurations = result.get(api, ())
        if len(configurations) != 2 or {
            item.use_for_first_egl_config for item in configurations
        } != {True, False}:
            raise CampaignError(
                f"GLES must-pass XML does not define the exact {api} split"
            )
    return result


def baseline_mustpass_configuration(
    configurations: dict[str, tuple[MustpassConfiguration, ...]],
    api: str,
    first: bool,
) -> MustpassConfiguration:
    matches = [
        item
        for item in configurations.get(api, ())
        if item.use_for_first_egl_config is first
    ]
    if len(matches) != 1:
        raise CampaignError(f"ambiguous {api} must-pass configuration split")
    return matches[0]


def matrix_runner_arguments(
    matrix: MatrixRun,
    caselist_path: str,
) -> list[str]:
    configuration = matrix.configuration
    arguments = [
        f"--deqp-caselist-file={caselist_path}",
        f"--deqp-screen-rotation={configuration.screen_rotation}",
        f"--deqp-surface-width={configuration.surface_width}",
        f"--deqp-surface-height={configuration.surface_height}",
        f"--deqp-watchdog={configuration.watchdog}",
        f"--deqp-base-seed={configuration.base_seed}",
    ]
    if configuration.fbo_config is not None:
        arguments.extend(
            (
                f"--deqp-gl-config-name={configuration.fbo_config}",
                "--deqp-surface-type=fbo",
            )
        )
    else:
        arguments.append(
            f"--deqp-surface-type={matrix.egl_config.selected_surface_type}"
        )
    arguments.extend(
        (
            f"--deqp-gl-config-id={matrix.egl_config.identifier}",
            "--deqp-gl-context-type=egl",
        )
    )
    return arguments


def load_runner_oracle(
    identity: dict[str, Any] | None = None,
    configurations: dict[
        str, tuple[MustpassConfiguration, ...]
    ] | None = None,
    path: Path = RUNNER_ORACLE_PATH,
) -> RunnerOracle:
    if identity is None:
        identity = load_identity()
    if configurations is None:
        configurations = load_gles_mustpass(identity)
    try:
        payload = path.read_bytes()
        value = json.loads(
            payload.decode("utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read CTS runner oracle {path}: {error}"
        ) from error
    if not isinstance(value, dict) or set(value) != {
        "schema_version",
        "cts_release",
        "mustpass_xml_sha256",
        "unwaived_successful_case_statuses",
        "oracle_build",
        "fixture_egl_configs",
        "summaries",
    }:
        raise CampaignError("invalid CTS runner oracle")
    if (
        value["schema_version"] != 1
        or value["cts_release"] != identity["cts"]["release_label"]
        or value["mustpass_xml_sha256"]
        != identity["mustpass"]["gles_xml"]["sha256"]
    ):
        raise CampaignError("CTS runner oracle names another release")
    successful_case_statuses = value["unwaived_successful_case_statuses"]
    if successful_case_statuses != [
        "Pass",
        "NotSupported",
        "QualityWarning",
        "CompatibilityWarning",
    ]:
        raise CampaignError("CTS runner oracle has invalid success statuses")

    build = value["oracle_build"]
    build_keys = {
        "platform",
        "build_type",
        "target",
        "fixture_scope",
        "original_runner_sha256",
        "fixture_runner_sha256",
        "generated_gles_header_sha256",
        "binary_sha256",
        "omitted_unrelated_subdirectories",
    }
    if not isinstance(build, dict) or set(build) != build_keys:
        raise CampaignError("invalid CTS runner oracle build identity")
    digests = (
        build["original_runner_sha256"],
        build["fixture_runner_sha256"],
        build["generated_gles_header_sha256"],
        build["binary_sha256"],
    )
    omitted = build["omitted_unrelated_subdirectories"]
    if (
        build["platform"] != "macos-arm64"
        or build["build_type"] != "Release"
        or build["target"] != "cts-runner"
        or build["fixture_scope"]
        != "replace-platform-config-enumeration-only"
        or any(
            not isinstance(item, str) or SHA256.fullmatch(item) is None
            for item in digests
        )
        or digests[0] == digests[1]
        or not isinstance(omitted, list)
        or omitted != sorted(set(omitted))
        or any(
            not isinstance(item, str)
            or item.startswith("/")
            or ".." in Path(item).parts
            for item in omitted
        )
    ):
        raise CampaignError("invalid CTS runner oracle build identity")

    fixture_configs = egl_configs_from_json(value["fixture_egl_configs"])
    if len(fixture_configs) < 3:
        raise CampaignError("CTS runner oracle has too few EGL fixtures")
    summaries = value["summaries"]
    if not isinstance(summaries, dict) or set(summaries) != set(
        BASELINE_GLES_APIS
    ):
        raise CampaignError("CTS runner oracle has an invalid API set")
    for api in BASELINE_GLES_APIS:
        summary = summaries[api]
        if not isinstance(summary, dict) or set(summary) != {
            "runner_type",
            "exit_code",
            "summary_sha256",
            "configuration_roles",
            "runner_arguments",
        }:
            raise CampaignError(f"invalid CTS runner oracle summary for {api}")
        roles = summary["configuration_roles"]
        rows = summary["runner_arguments"]
        if (
            summary["runner_type"] != api.replace("gles", "es")
            or summary["exit_code"] != 1
            or not isinstance(summary["summary_sha256"], str)
            or SHA256.fullmatch(summary["summary_sha256"]) is None
            or roles != ["first"] + ["other"] * (len(fixture_configs) - 1)
            or not isinstance(rows, list)
            or len(rows) != len(fixture_configs)
        ):
            raise CampaignError(f"invalid CTS runner oracle summary for {api}")
        relative_caselist = (
            "gl_cts/data/mustpass/gles/khronos_mustpass/main/"
            f"{Path(leaf_identities(identity)[api].path).name}"
        )
        for index, (config, arguments) in enumerate(
            zip(fixture_configs, rows, strict=True)
        ):
            if (
                not isinstance(arguments, list)
                or not arguments
                or any(not isinstance(argument, str) for argument in arguments)
            ):
                raise CampaignError(
                    f"invalid CTS runner oracle arguments for {api}"
                )
            configuration = baseline_mustpass_configuration(
                configurations, api, index == 0
            )
            matrix = MatrixRun(
                configuration=configuration,
                egl_config=config,
                egl_config_index=index,
                config_inventory_sha256="0" * 64,
                selection=Selection(configuration.leaf, (), 0, 1),
                mustpass_xml_path=identity["mustpass"]["gles_xml"]["path"],
                mustpass_xml_size=identity["mustpass"]["gles_xml"]["size"],
                mustpass_xml_sha256=identity["mustpass"]["gles_xml"][
                    "sha256"
                ],
            )
            expected = matrix_runner_arguments(matrix, relative_caselist)
            if arguments != expected:
                raise CampaignError(
                    f"CTS runner oracle expansion changed for {api} "
                    f"config index {index}"
                )
    resolved_path = path.resolve()
    try:
        recorded_path = str(resolved_path.relative_to(REPO_ROOT))
    except ValueError:
        recorded_path = str(resolved_path)
    return RunnerOracle(
        path=recorded_path,
        size=len(payload),
        sha256=hashlib.sha256(payload).hexdigest(),
        fixture_configs=fixture_configs,
        successful_case_statuses=tuple(successful_case_statuses),
    )


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


def nonnegative_integer(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("must be an integer") from error
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be nonnegative")
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
            "scope",
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
        "scope",
        "complete",
        "exported_package_count",
        "complete_caselist_sha256",
        "complete_caselist_case_count",
    }
    if set(values) != required:
        raise CampaignError("CTS inventory report is incomplete")
    if values["schema_version"] != "2" or values["complete"] != "1":
        raise CampaignError("CTS inventory report is not complete")
    if values["scope"] != GLES_INVENTORY_SCOPE:
        raise CampaignError("CTS inventory scope is invalid")
    if not values["exported_package_count"].isdigit():
        raise CampaignError("CTS inventory package count is invalid")
    package_count = int(values["exported_package_count"])
    if not values["complete_caselist_case_count"].isdigit():
        raise CampaignError("CTS inventory case count is invalid")
    case_count = int(values["complete_caselist_case_count"])
    digest = values["complete_caselist_sha256"]
    if (
        package_count != len(GLES_INVENTORY_MODULES)
        or case_count < 1
        or SHA256.fullmatch(digest) is None
    ):
        raise CampaignError("CTS inventory identity is invalid")
    if modules != list(GLES_INVENTORY_MODULES):
        raise CampaignError("registered CTS modules do not match the GLES scope")
    frozen_scope = identity["cts"].get("inventory_scope")
    frozen_digest = identity["cts"]["complete_caselist_sha256"]
    frozen_count = identity["cts"]["complete_caselist_case_count"]
    frozen_modules = identity["cts"]["registered_modules"]
    if frozen_scope != GLES_INVENTORY_SCOPE:
        raise CampaignError("frozen CTS inventory scope changed")
    if frozen_digest != digest:
        raise CampaignError("complete CTS caselist identity changed")
    if frozen_count != case_count:
        raise CampaignError("complete CTS caselist count changed")
    if frozen_modules != modules:
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
    if list(results) != list(selected):
        raise CampaignError("QPA case order does not match the selected caselist")
    return results


def qpa_case_xml(qpa: str, case: str) -> ElementTree.Element:
    begin = f"#beginTestCaseResult {case}"
    end = "#endTestCaseResult"
    lines = qpa.splitlines()
    starts = [index for index, line in enumerate(lines) if line == begin]
    if len(starts) != 1:
        raise CampaignError(f"QPA does not contain one XML payload for {case}")
    start = starts[0] + 1
    try:
        finish = lines.index(end, start)
    except ValueError as error:
        raise CampaignError(f"QPA XML payload is truncated for {case}") from error
    payload = "\n".join(lines[start:finish]).strip()
    if not payload:
        raise CampaignError(f"QPA XML payload is empty for {case}")
    try:
        root = ElementTree.fromstring(payload)
    except ElementTree.ParseError as error:
        raise CampaignError(f"QPA XML payload is invalid for {case}") from error
    if root.tag != "TestCaseResult" or root.attrib != {
        "CasePath": case,
        "Version": "0.3.4",
        "CaseType": "SelfValidate",
    }:
        raise CampaignError(f"QPA XML payload names another case for {case}")
    return root


def egl_config_digest(configs: Sequence[EglConfig]) -> str:
    payload = json.dumps(
        [
            {
                "id": config.identifier,
                "surface_types": list(config.surface_types),
            }
            for config in configs
        ],
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def parse_egl_config_qpa(qpa: str, api: str) -> tuple[EglConfig, ...]:
    case = CONFIG_CASES.get(api)
    if case is None:
        raise CampaignError(f"unsupported EGL config inventory API: {api}")
    results = validate_qpa_union(qpa, [case])
    validate_required_passes(results)
    root = qpa_case_xml(qpa, case)
    sections = [
        element
        for element in root.findall("Section")
        if element.attrib.get("Name") == "Configs"
    ]
    if len(sections) != 1:
        raise CampaignError("CTS config QPA has no unique Configs section")
    section = sections[0]
    if section.attrib != {"Name": "Configs", "Description": "Configs"}:
        raise CampaignError("CTS config QPA has an invalid Configs section")
    configs: list[EglConfig] = []
    seen: set[int] = set()
    for element in section:
        if element.tag != "Text" or element.attrib or element.text is None:
            raise CampaignError("CTS config QPA has an invalid config row")
        match = EGL_CONFIG_MESSAGE.fullmatch(element.text)
        if match is None:
            raise CampaignError(
                f"CTS config QPA has an unsupported row: {element.text!r}"
            )
        identifier = int(match.group(1))
        surface_types = tuple(match.group(2).split(", "))
        if (
            identifier in seen
            or len(surface_types) != len(set(surface_types))
        ):
            raise CampaignError("CTS config QPA has a duplicate config entry")
        seen.add(identifier)
        config = EglConfig(identifier, surface_types)
        config.selected_surface_type
        configs.append(config)
    if not configs:
        raise CampaignError("CTS config QPA enumerated no usable configs")
    return tuple(configs)


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


def validate_official_runner_results(
    results: dict[str, str],
    successful_statuses: Sequence[str],
) -> None:
    accepted = set(successful_statuses)
    nonconformant = {
        case: status for case, status in results.items() if status not in accepted
    }
    if nonconformant:
        first_case = sorted(nonconformant)[0]
        raise CampaignError(
            "official CTS runner rejects case result: "
            f"{first_case}={nonconformant[first_case]}"
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


def artifact_acknowledgement(line: str) -> str:
    begin = BEGIN_ARTIFACT.fullmatch(line)
    if begin is not None:
        return f"UML_CTS_ARTIFACT_ACK|{begin.group(1)}|begin"
    chunk = CHUNK_ARTIFACT.fullmatch(line)
    if chunk is not None:
        return f"UML_CTS_ARTIFACT_ACK|{chunk.group(1)}|{chunk.group(2)}"
    end = END_ARTIFACT.fullmatch(line)
    if end is not None:
        return f"UML_CTS_ARTIFACT_ACK|{end.group(1)}|end"
    raise CampaignError("artifact transport emitted a non-frame line")


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
    *,
    config_inventory_api: str | None = None,
    matrix: MatrixRun | None = None,
    artifact_transport: str = "console",
) -> str:
    if config_inventory_api is not None and config_inventory_api not in CONFIG_CASES:
        raise CampaignError(
            f"unsupported EGL config inventory API: {config_inventory_api}"
        )
    if config_inventory_api is not None and selection is not None:
        raise CampaignError("config inventory cannot carry a must-pass selection")
    if matrix is not None and (
        selection is None or selection != matrix.selection
    ):
        raise CampaignError("matrix run does not match its selected shard")
    if matrix is not None and config_inventory_api is not None:
        raise CampaignError("matrix and config inventory modes are exclusive")
    if artifact_transport not in {"console", "virtio-serial"}:
        raise CampaignError("unsupported CTS artifact transport")
    glcts = "/opt/vk-gl-cts/openglcts/glcts"
    archive = "/opt/vk-gl-cts/openglcts"
    output = "/run/uml-cts"
    selected = f"{output}/selected.txt"
    qpa = f"{output}/results.qpa"
    renderer = f"{output}/renderer.txt"
    summary = f"{output}/summary.txt"
    inventory_report = f"{output}/inventory.txt"
    hierarchy_inventory = selection is None and config_inventory_api is None
    config_inventory = config_inventory_api is not None
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
    if hierarchy_inventory:
        export_pattern = f"{output}/${{packageName}}-cases.${{typeExtension}}"
        inventory_commands = []
        for module in GLES_INVENTORY_MODULES:
            inventory_commands.append(
                shlex.join(
                    [
                        glcts,
                        "--deqp-runmode=txt-caselist",
                        f"--deqp-case={module}.*",
                        f"--deqp-caselist-export-file={export_pattern}",
                        f"--deqp-archive-dir={archive}",
                        "--deqp-gl-context-type=egl",
                        "--deqp-duplicate-case-name-check=enable",
                        f"--deqp-log-filename={qpa}",
                    ]
                )
            )
        command = ["/bin/sh", "-c", " &&\n".join(inventory_commands)]
    elif config_inventory:
        command = [
            glcts,
            f"--deqp-case={CONFIG_CASES[config_inventory_api]}",
            "--deqp-gl-context-type=egl",
            f"--deqp-archive-dir={archive}",
            "--deqp-log-flush=enable",
            f"--deqp-log-filename={qpa}",
        ]
    elif matrix is not None:
        command = [
            glcts,
            *matrix_runner_arguments(matrix, selected),
            f"--deqp-archive-dir={archive}",
            "--deqp-crashhandler=disable",
            "--deqp-test-oom=disable",
            "--deqp-log-images=disable",
            "--deqp-log-shader-sources=disable",
            "--deqp-log-flush=enable",
            "--deqp-terminate-on-fail=disable",
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
    leaf_path = "" if selection is None else selection.leaf.path
    leaf_identity_check = ""
    mustpass_identity_check = ""
    prepare_command = ""
    post_run_command = ""
    if hierarchy_inventory:
        post_run_command = """
LC_ALL=C
export LC_ALL
: > "$selected"
exported_packages=0
for package_caselist in "$output"/*-cases.txt; do
    [ -f "$package_caselist" ] || continue
    exported_packages=$((exported_packages + 1))
done
if [ "$exported_packages" -ne 2 ]; then
    echo "UML_CTS_ERROR: GLES inventory exported $exported_packages packages, expected 2"
    status=1
fi
for inventory_package in KHR-GLES2 KHR-GLES3; do
    package_caselist="$output/$inventory_package-cases.txt"
    if [ ! -s "$package_caselist" ]; then
        echo "UML_CTS_ERROR: GLES inventory package $inventory_package is empty"
        status=1
        continue
    fi
    if ! awk -v expected="$inventory_package" '
        $1 == "TEST:" || $1 == "GROUP:" {
            split($2, part, ".")
            if (part[1] != expected) exit 1
            seen = 1
        }
        END { if (!seen) exit 1 }
    ' "$package_caselist"; then
        echo "UML_CTS_ERROR: GLES inventory package $inventory_package contains another module"
        status=1
    fi
    cat "$package_caselist" >> "$selected"
done
complete=0
[ "$status" -ne 0 ] || complete=1
caselist_digest=$(sha256sum "$selected" | cut -d ' ' -f 1)
caselist_count=$(awk '$1 == "TEST:" { count++ } END { print count + 0 }' "$selected")
{
    printf 'schema_version=2\n'
    printf 'scope=gles2-gles3\n'
    printf 'complete=%s\n' "$complete"
    printf 'exported_package_count=%s\n' "$exported_packages"
    printf 'complete_caselist_sha256=%s\n' "$caselist_digest"
    printf 'complete_caselist_case_count=%s\n' "$caselist_count"
    printf 'registered_module=KHR-GLES2\n'
    printf 'registered_module=KHR-GLES3\n'
} > "$inventory_report"
chown uml:uml "$inventory_report" "$renderer"
"""
    elif config_inventory:
        pass
    elif not selection.exact_cases:
        selection_command = (
            "awk -v shard_index="
            f"{selection.fraction_index} "
            f"-v shard_count={selection.fraction_count} "
            "'NF && $1 !~ /^#/ { "
            "if ((seen % shard_count) == shard_index) print; ++seen }' "
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
    if selection is not None:
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
    if matrix is not None:
        mustpass_identity_check = f"""
actual_mustpass_size=$(wc -c < {shlex.quote(matrix.mustpass_xml_path)} | tr -d ' ')
[ "$actual_mustpass_size" -eq {matrix.mustpass_xml_size} ] || {{
    echo 'UML_CTS_ERROR: GLES must-pass XML size changed'
    exit 1
}}
actual_mustpass_sha=$(sha256sum \
    {shlex.quote(matrix.mustpass_xml_path)} | cut -d ' ' -f 1)
[ "$actual_mustpass_sha" = {shlex.quote(matrix.mustpass_xml_sha256)} ] || {{
    echo 'UML_CTS_ERROR: GLES must-pass XML identity changed'
    exit 1
}}
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
hierarchy_inventory_mode={1 if hierarchy_inventory else 0}
selected_mode={1 if selection is not None else 0}
artifact_transport={shlex.quote(artifact_transport)}
artifact_channel={shlex.quote(VIRTIO_SERIAL_GUEST_DEVICE)}
artifact_fd_ready=0
status=1

prepare_artifact_transport()
{{
    [ "$artifact_transport" = virtio-serial ] || return 0
    remaining=60
    while [ ! -c "$artifact_channel" ]; do
        [ "$remaining" -gt 0 ] || return 1
        remaining=$((remaining - 1))
        sleep 1
    done
    exec 3<> "$artifact_channel" || return 1
    artifact_fd_ready=1
}}

emit_frame()
{{
    frame="$1"
    acknowledgement="$2"
    if [ "$artifact_transport" = console ]; then
        printf '%s\n' "$frame"
        return 0
    fi
    [ "$artifact_fd_ready" -eq 1 ] || return 1
    printf '%s\n' "$frame" >&3 || return 1
    actual_ack=$(timeout --signal=TERM 30 head -n 1 <&3) || return 1
    [ "$actual_ack" = "$acknowledgement" ] || {{
        echo 'UML_CTS_ERROR: artifact acknowledgement changed'
        return 1
    }}
}}

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
    frame=$(printf 'UML_CTS_ARTIFACT_BEGIN|%s|gzip+base64|%s|%s' \
        "$name" "$size" "$digest")
    emit_frame "$frame" "UML_CTS_ARTIFACT_ACK|$name|begin" || return 1
    index=0
    while IFS= read -r chunk; do
        frame=$(printf 'UML_CTS_ARTIFACT_CHUNK|%s|%s|%s' \
            "$name" "$index" "$chunk")
        emit_frame "$frame" "UML_CTS_ARTIFACT_ACK|$name|$index" || return 1
        index=$((index + 1))
    done < "$encoded"
    frame=$(printf 'UML_CTS_ARTIFACT_END|%s|%s' "$name" "$chunks")
    emit_frame "$frame" "UML_CTS_ARTIFACT_ACK|$name|end" || return 1
    rm -f "$compressed" "$encoded"
}}

finish()
{{
    trap - EXIT HUP INT TERM
    printf 'cts_rc=%s\n' "$status" > "$summary"
    printf 'UML_CTS_STATUS|rc=%s\n' "$status"
    transfer_status=0
    if ! prepare_artifact_transport; then
        echo 'UML_CTS_ERROR: artifact transport did not become ready'
        transfer_status=1
    fi
    if [ "$transfer_status" -eq 0 ] && \
        ! emit_artifact renderer.txt "$renderer"; then
        transfer_status=1
    fi
    if [ "$transfer_status" -eq 0 ] && \
        [ "$hierarchy_inventory_mode" -eq 1 ]; then
        emit_artifact inventory.txt "$inventory_report" || transfer_status=1
    elif [ "$transfer_status" -eq 0 ] && [ "$selected_mode" -eq 1 ]; then
        emit_artifact selected.txt "$selected" || transfer_status=1
    fi
    if [ "$transfer_status" -eq 0 ]; then
        emit_artifact results.qpa "$qpa" || transfer_status=1
    fi
    if [ "$transfer_status" -eq 0 ]; then
        emit_artifact summary.txt "$summary" || transfer_status=1
    fi
    if [ "$artifact_fd_ready" -eq 1 ]; then
        exec 3>&-
    fi
    if [ "$transfer_status" -ne 0 ]; then
        echo 'UML_CTS_ERROR: acknowledged artifact transfer failed'
    fi
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
{mustpass_identity_check}
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
Description=Darwin UML VK-GL-CTS campaign
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
    artifact_socket = artifact_socket_path(artifact_directory)
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
                config_inventory_api=options.config_inventory,
                matrix=options.matrix_run,
                artifact_transport="virtio-serial",
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
    command.append(f"--virtio-serial-socket={artifact_socket}")
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
    artifact_socket_path: Path,
) -> tuple[str, str]:
    deadline = time.monotonic() + timeout
    next_heartbeat = time.monotonic() + 30
    transport = AcknowledgedArtifactSocket(artifact_socket_path)
    try:
        while time.monotonic() < deadline:
            transport.service()
            if process.poll() is not None:
                break
            now = time.monotonic()
            if now >= next_heartbeat:
                size = log_path.stat().st_size if log_path.exists() else 0
                print(
                    "VK-GL-CTS guest is running; "
                    f"console_bytes={size} "
                    f"artifact_frames={len(transport.lines)}",
                    flush=True,
                )
                next_heartbeat = now + 30
            time.sleep(0.05)
        else:
            raise TimeoutError(f"VK-GL-CTS guest exceeded {timeout} seconds")
        process.wait(timeout=5)
        drain_deadline = time.monotonic() + 2
        while transport.connection is not None and time.monotonic() < drain_deadline:
            transport.service()
            if transport.connection is not None:
                time.sleep(0.01)
        artifact_transcript = transport.finish()
    finally:
        transport.close()
    return (
        log_path.read_text(encoding="utf-8", errors="replace"),
        artifact_transcript,
    )


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


def egl_configs_from_json(value: Any) -> tuple[EglConfig, ...]:
    if not isinstance(value, list) or not value:
        raise CampaignError("config-inventory evidence has no EGL configs")
    configs: list[EglConfig] = []
    seen: set[int] = set()
    for row in value:
        if not isinstance(row, dict) or set(row) != {
            "id",
            "surface_types",
            "selected_surface_type",
        }:
            raise CampaignError("config-inventory evidence has an invalid row")
        identifier = row["id"]
        surfaces = row["surface_types"]
        if (
            type(identifier) is not int
            or identifier < 1
            or identifier in seen
            or not isinstance(surfaces, list)
            or not surfaces
            or any(
                surface not in {"window", "pbuffer", "pixmap"}
                for surface in surfaces
            )
            or len(surfaces) != len(set(surfaces))
        ):
            raise CampaignError("config-inventory evidence has an invalid row")
        config = EglConfig(identifier, tuple(surfaces))
        if row["selected_surface_type"] != config.selected_surface_type:
            raise CampaignError(
                "config-inventory evidence changed its selected surface type"
            )
        seen.add(identifier)
        configs.append(config)
    return tuple(configs)


def load_config_inventory_evidence(
    path: Path,
    api: str,
    page_profile: str,
    display: str,
    rootfs_contract: RootfsContract,
) -> tuple[tuple[EglConfig, ...], str]:
    try:
        metadata = json.loads(
            path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_keys,
        )
    except (OSError, json.JSONDecodeError) as error:
        raise CampaignError(
            f"cannot read config-inventory evidence {path}: {error}"
        ) from error
    if not isinstance(metadata, dict) or metadata.get("schema_version") != 1:
        raise CampaignError("unsupported config-inventory evidence")
    expected = {
        "adapter": "macos",
        "backend": "angle-metal",
        "mode": "config-inventory",
        "api": api,
        "page_profile": page_profile,
        "display": display,
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "glcts_sha256": rootfs_contract.glcts_sha256,
        "emulator_return_code": 0,
        "completed_cases": 1,
        "status_totals": {"Pass": 1},
    }
    for key, value in expected.items():
        if metadata.get(key) != value:
            raise CampaignError(
                f"config-inventory evidence does not match {key}"
            )
    configs = egl_configs_from_json(metadata.get("egl_configs"))
    digest = metadata.get("egl_config_sha256")
    if (
        not isinstance(digest, str)
        or SHA256.fullmatch(digest) is None
        or digest != egl_config_digest(configs)
    ):
        raise CampaignError("config-inventory evidence digest changed")
    qpa_digest = metadata.get("qpa_sha256")
    if not isinstance(qpa_digest, str) or SHA256.fullmatch(qpa_digest) is None:
        raise CampaignError("config-inventory QPA identity is invalid")
    qpa_path = path.parent / "artifacts" / "results.qpa"
    try:
        qpa_payload = qpa_path.read_bytes()
    except OSError as error:
        raise CampaignError(
            f"cannot read config-inventory QPA {qpa_path}: {error}"
        ) from error
    if hashlib.sha256(qpa_payload).hexdigest() != qpa_digest:
        raise CampaignError("config-inventory QPA identity changed")
    try:
        qpa_text = qpa_payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise CampaignError("config-inventory QPA is not UTF-8") from error
    parsed = parse_egl_config_qpa(qpa_text, api)
    if parsed != configs:
        raise CampaignError("config-inventory QPA and metadata disagree")
    return configs, digest


def api_for_leaf(identifier: str) -> str:
    return identifier.split("-", 1)[0]


def parse_arguments(arguments: Sequence[str] | None = None) -> argparse.Namespace:
    identity = load_identity()
    leaves = leaf_identities(identity)
    campaigns = load_apple_campaigns(identity)
    mustpass = load_gles_mustpass(identity)
    parser = argparse.ArgumentParser(
        description="run a bounded in-image VK-GL-CTS shard through Virgl"
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--inventory",
        action="store_true",
        help="capture the complete display-backed GLES 2/3 text caselist",
    )
    mode.add_argument(
        "--config-inventory",
        choices=BASELINE_GLES_APIS,
        help="enumerate the ordered conformant EGL configs for one GLES API",
    )
    mode.add_argument(
        "--matrix",
        choices=BASELINE_GLES_APIS,
        help="run one exact official must-pass matrix shard",
    )
    mode.add_argument("--campaign", choices=sorted(campaigns))
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
    parser.add_argument("--config-inventory-run", type=Path)
    parser.add_argument("--egl-config-index", type=nonnegative_integer)
    parser.add_argument("--page-profile", choices=("16k", "4k"), default="16k")
    parser.add_argument("--engine", choices=("jit", "gadgets"), default="jit")
    parser.add_argument("--display", choices=("vnc", "native"), default="vnc")
    parser.add_argument("--memory", default="8G")
    parser.add_argument("--cpus", type=positive_integer, default=10)
    parser.add_argument("--width", type=positive_integer, default=1280)
    parser.add_argument("--height", type=positive_integer, default=720)
    parser.add_argument("--shard-timeout", type=positive_integer)
    parser.add_argument("--guest-timeout", type=positive_integer)
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
        help="mark an authenticated official-matrix candidate shard",
    )
    parser.add_argument("--dry-run", action="store_true")
    options = parser.parse_args(arguments)
    matrix_only_arguments = (
        options.config_inventory_run is not None
        or options.egl_config_index is not None
    )
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
        if matrix_only_arguments or options.seed is not None:
            parser.error("--inventory does not accept matrix or seed options")
    elif options.config_inventory is not None:
        if options.leaf is not None or options.api is not None:
            parser.error("--config-inventory does not accept a leaf or --api")
        if options.case is not None or options.fraction is not None:
            parser.error("--config-inventory does not accept a case or fraction")
        if matrix_only_arguments or options.seed is not None:
            parser.error(
                "--config-inventory does not accept matrix or seed options"
            )
        if options.qualification:
            parser.error(
                "config discovery is an input to, not itself, qualification"
            )
    elif options.matrix is not None:
        if options.leaf is not None or options.api is not None:
            parser.error("--matrix owns its exact leaf and API")
        if options.case is not None:
            parser.error("--matrix does not accept an exact-case selector")
        if options.seed is not None:
            parser.error("--matrix uses the seed from mustpass.xml")
        if options.config_inventory_run is None or options.egl_config_index is None:
            parser.error(
                "--matrix requires --config-inventory-run and "
                "--egl-config-index"
            )
        if not options.qualification:
            parser.error("--matrix requires --qualification")
        if options.display != "vnc":
            parser.error("a macOS qualification matrix requires --display vnc")
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
        if matrix_only_arguments:
            parser.error("--campaign does not accept matrix options")
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
        if matrix_only_arguments:
            parser.error("a developer shard does not accept matrix options")
    if options.seed is None:
        options.seed = 0
    if options.shard_timeout is None:
        options.shard_timeout = 1800 if options.matrix is not None else 900
    if options.guest_timeout is None:
        options.guest_timeout = options.shard_timeout + 600
    if options.fraction is None:
        options.fraction = (0, 1)
    if options.seed < 0 or options.seed > 0xFFFFFFFF:
        parser.error("--seed must be between 0 and 4294967295")
    if (
        options.qualification
        and options.matrix is None
        and not identity["cts"]["qualification_ready"]
    ):
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
    options.matrix_run = None
    options.runner_oracle = None
    if options.matrix is not None:
        if contract.filesystem_type != "ext4" or receipt is None:
            parser.error(
                "an official matrix requires the authenticated derivative ext4 rootfs"
            )
        try:
            options.runner_oracle = load_runner_oracle(identity, mustpass)
            configs, config_digest = load_config_inventory_evidence(
                options.config_inventory_run.resolve(),
                options.matrix,
                options.page_profile,
                options.display,
                contract,
            )
        except (CampaignError, UnicodeDecodeError) as error:
            parser.error(str(error))
        if options.egl_config_index >= len(configs):
            parser.error(
                f"--egl-config-index exceeds the {len(configs)} discovered configs"
            )
        fraction_index, fraction_count = options.fraction
        leaf = leaves[options.matrix]
        selected = Selection(leaf, (), fraction_index, fraction_count)
        xml_identity = identity["mustpass"]["gles_xml"]
        options.matrix_run = MatrixRun(
            configuration=baseline_mustpass_configuration(
                mustpass, options.matrix, options.egl_config_index == 0
            ),
            egl_config=configs[options.egl_config_index],
            egl_config_index=options.egl_config_index,
            config_inventory_sha256=config_digest,
            selection=selected,
            mustpass_xml_path=xml_identity["path"],
            mustpass_xml_size=xml_identity["size"],
            mustpass_xml_sha256=xml_identity["sha256"],
        )
    options.identity = identity
    options.leaves = leaves
    options.campaigns = campaigns
    options.mustpass = mustpass
    return options


def main(arguments: Sequence[str] | None = None) -> int:
    options = parse_arguments(arguments)
    fraction_index, fraction_count = options.fraction
    leaf: LeafIdentity | None = None
    selection: Selection | None = None
    suffix = "inventory"
    if options.config_inventory is not None:
        api_identifier = options.config_inventory
        suffix = f"config-inventory-{api_identifier}"
    elif options.matrix is not None:
        matrix = options.matrix_run
        leaf = matrix.configuration.leaf
        selection = matrix.selection
        api_identifier = matrix.configuration.api
        leaf_identifier = leaf.identifier
        suffix = (
            f"matrix-{api_identifier}-cfg{matrix.egl_config_index:02d}-"
            f"{fraction_index:03d}of{fraction_count:03d}"
        )
    elif options.campaign is not None:
        campaign = options.campaigns[options.campaign]
        leaf = campaign.leaf
        selection = campaign.selection
        api_identifier = campaign.api
        leaf_identifier = campaign.leaf.identifier
        suffix = campaign.identifier
    elif not options.inventory:
        leaf = options.leaves[options.leaf]
        exact_cases = (options.case,) if options.case is not None else ()
        selection = Selection(
            leaf, exact_cases, fraction_index, fraction_count
        )
        api_identifier = options.api
        leaf_identifier = options.leaf
        suffix = options.leaf
    if selection is not None:
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
        "mode": (
            "inventory"
            if options.inventory
            else "config-inventory"
            if options.config_inventory is not None
            else "mustpass-matrix-shard"
            if options.matrix is not None
            else "shard"
        ),
        "rootfs_type": rootfs_contract.filesystem_type,
        "rootfs_sha256": rootfs_contract.sha256,
        "glcts_sha256": rootfs_contract.glcts_sha256,
        "emulator_sha256": hashlib.sha256(emulator_bytes).hexdigest(),
        "qualification": options.qualification,
        "artifact_transport": {
            "type": "virtio-serial-unix-socket",
            "guest_device": VIRTIO_SERIAL_GUEST_DEVICE,
            "port_name": VIRTIO_SERIAL_PORT_NAME,
            "acknowledged_frames": True,
        },
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
    elif options.config_inventory is not None:
        metadata.update(
            {
                "api": api_identifier,
                "config_case": CONFIG_CASES[api_identifier],
                "expected_artifacts": [
                    "renderer.txt",
                    "results.qpa",
                    "summary.txt",
                ],
            }
        )
    else:
        metadata.update(
            {
                "api": api_identifier,
                "leaf": leaf_identifier,
                "leaf_sha256": leaf.sha256,
                "fraction": [fraction_index, fraction_count],
                "exact_cases": list(selection.exact_cases),
                "expected_cases": selection.expected_count,
                "seed": (
                    options.matrix_run.configuration.base_seed
                    if options.matrix is not None
                    else options.seed
                ),
            }
        )
        if options.campaign is not None:
            metadata["campaign"] = options.campaign
        if options.matrix is not None:
            matrix = options.matrix_run
            metadata["matrix"] = {
                "mustpass_xml_path": matrix.mustpass_xml_path,
                "mustpass_xml_size": matrix.mustpass_xml_size,
                "mustpass_xml_sha256": matrix.mustpass_xml_sha256,
                "runner_oracle_path": options.runner_oracle.path,
                "runner_oracle_size": options.runner_oracle.size,
                "runner_oracle_sha256": options.runner_oracle.sha256,
                "successful_case_statuses": list(
                    options.runner_oracle.successful_case_statuses
                ),
                "configuration_name": matrix.configuration.name,
                "configuration_role": (
                    "first"
                    if matrix.configuration.use_for_first_egl_config
                    else "other"
                ),
                "xml_arguments": list(matrix.configuration.xml_arguments),
                "runner_arguments": matrix_runner_arguments(
                    matrix, "/run/uml-cts/selected.txt"
                ),
                "egl_config_index": matrix.egl_config_index,
                "egl_config_id": matrix.egl_config.identifier,
                "egl_surface_types": list(matrix.egl_config.surface_types),
                "egl_surface_type": matrix.egl_config.selected_surface_type,
                "config_inventory_sha256": matrix.config_inventory_sha256,
                "config_inventory_run": str(
                    options.config_inventory_run.resolve()
                ),
            }
    write_json(artifact_directory / "run.json", metadata)
    log_path = artifact_directory / "console.log"
    artifact_socket = artifact_socket_path(artifact_directory)
    artifact_log_path = artifact_directory / ARTIFACT_TRANSPORT_LOG
    with log_path.open("wb") as log:
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            transcript, artifact_transcript = wait_for_guest(
                process,
                log_path,
                options.guest_timeout,
                artifact_socket,
            )
        finally:
            stop_guest(process)

    if process.returncode != 0:
        raise CampaignError(f"guest harness exited with status {process.returncode}")
    if os.path.lexists(artifact_socket):
        raise CampaignError("emulator did not remove the artifact socket")
    artifact_log_path.write_text(artifact_transcript, encoding="ascii")
    metadata["artifact_transport_sha256"] = sha256_file(artifact_log_path)
    validate_host_backend(transcript)
    validate_guest_shutdown(transcript)
    metadata.update(parse_gpu_capability_report(transcript))
    if options.display == "native":
        metadata["native_display_final"] = parse_native_display_final(transcript)
    write_json(artifact_directory / "run.json", metadata)
    artifacts = decode_artifacts(artifact_transcript)
    required_artifacts = (
        INVENTORY_ARTIFACTS
        if options.inventory
        else CONFIG_INVENTORY_ARTIFACTS
        if options.config_inventory is not None
        else SHARD_ARTIFACTS
    )
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

    if options.config_inventory is not None:
        qpa_payload = artifacts["results.qpa"]
        qpa_text = qpa_payload.decode("utf-8", errors="strict")
        configs = parse_egl_config_qpa(qpa_text, options.config_inventory)
        totals = {"Pass": 1}
        metadata.update(
            {
                "emulator_return_code": process.returncode,
                "completed_cases": 1,
                "status_totals": totals,
                "qpa_sha256": hashlib.sha256(qpa_payload).hexdigest(),
                "egl_config_sha256": egl_config_digest(configs),
                "egl_configs": [
                    {
                        "id": config.identifier,
                        "surface_types": list(config.surface_types),
                        "selected_surface_type": config.selected_surface_type,
                    }
                    for config in configs
                ],
            }
        )
        write_json(artifact_directory / "run.json", metadata)
        print(
            f"VK-GL-CTS {options.config_inventory} config inventory "
            f"captured {len(configs)} EGL configs"
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
    if options.matrix is not None:
        validate_official_runner_results(
            results, options.runner_oracle.successful_case_statuses
        )
    else:
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
