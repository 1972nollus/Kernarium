#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Derive the public OpenGL ES 3.2 runner scope from authenticated upstream XML.

The collection order and API predicates match getTestRunsForES in the pinned
glcTestRunner.cpp, which for an ES 3.2 context visits the NoContext, Single
Config and Khronos ES must-pass collections in that order and then walks the
EGL configuration list. Text files not referenced by these XML documents do
not define executable coverage. EGL configurations remain an ordered runtime
input.

The two AOSP collections that getTestRunsForES also visits (dEQP-EGL and
dEQP-GLES2/3/31) are outside this scope: the official runner resolves their
surface type from ``ConfigList::aospConfigs``, which the ``CTS-Configs`` case
does not log, so no authenticated runtime input reproduces their command
lines.
"""

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import hashlib
import re
import shlex
from typing import Any, Callable
from xml.etree import ElementTree


ROOT = "/opt/vk-gl-cts/openglcts/gl_cts/data/mustpass/gles"
API = "gles32"
# The run table selects a row when the context supports the row's API
# ("supported"), except the single-configuration collection, which the runner
# admits only on an exact API match ("exact").
COLLECTIONS = (
    ("no-context", "khronos_mustpass_noctx", "Khronos Mustpass ES NoContext", "supported"),
    ("single", "khronos_mustpass_single", "Khronos Mustpass ES Single Config", "exact"),
    ("main", "khronos_mustpass", "Khronos Mustpass ES", "supported"),
)
# ES caselist stems name a version without a separator: 3 is ES 3.0, 31 is
# ES 3.1 and 32 is ES 3.2.
ES_VERSIONS = {"2": (2, 0), "3": (3, 0), "31": (3, 1), "32": (3, 2)}
TARGET_VERSION = ES_VERSIONS["32"]
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
CASELIST = re.compile(r"gles(2|3|31|32)-(khr-[a-z0-9-]+)\.txt\Z")
SCHEMA = 1
RUNNER_SOURCE = {
    "path": "external/openglcts/modules/runner/glcTestRunner.cpp",
    "size": 33982,
    "sha256": "85b56c755c4e615ed8bf8b9fd6ee43abf3dc03a879352a68a83c08b13f950426",
}
LEAF_KEYS = {"id", "path", "size", "case_count", "sha256"}


class PlanError(ValueError):
    """An XML, identity, or runtime configuration does not match the contract."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise PlanError(message)


def digest(payload: bytes) -> str:
    return hashlib.sha256(payload).hexdigest()


def file_identity(path: str, payload: bytes) -> dict[str, Any]:
    return {"path": path, "size": len(payload), "sha256": digest(payload)}


def command_values(arguments: list[str]) -> dict[str, Any]:
    allowed = {
        "deqp-screen-rotation", "deqp-surface-width", "deqp-surface-height",
        "deqp-base-seed", "deqp-gl-config-name", "deqp-surface-type",
        "deqp-watchdog",
    }
    values: dict[str, str] = {}
    for argument in arguments:
        match = re.fullmatch(r"--([a-z0-9-]+)=([^\s]+)", argument)
        require(match is not None, "malformed OpenGL ES must-pass argument")
        key, value = match.groups()
        require(key in allowed and key not in values,
                "unknown or duplicate OpenGL ES must-pass argument")
        values[key] = value
    required = allowed - {"deqp-gl-config-name", "deqp-surface-type"}
    require(required.issubset(values), "incomplete OpenGL ES must-pass command")
    require(values["deqp-screen-rotation"] == "unspecified", "unsupported OpenGL ES rotation")
    require(values["deqp-watchdog"] == "disable", "unsupported OpenGL ES watchdog policy")
    try:
        width = int(values["deqp-surface-width"])
        height = int(values["deqp-surface-height"])
        seed = int(values["deqp-base-seed"])
    except ValueError as error:
        raise PlanError("invalid OpenGL ES must-pass numeric argument") from error
    require(width == -1 or 0 < width <= 0x7FFFFFFF, "invalid OpenGL ES surface width")
    require(height == -1 or 0 < height <= 0x7FFFFFFF, "invalid OpenGL ES surface height")
    require(0 <= seed <= 0xFFFFFFFF, "invalid OpenGL ES base seed")
    config = values.get("deqp-gl-config-name")
    surface = values.get("deqp-surface-type")
    require(config is None or re.fullmatch(r"[a-z0-9]+", config) is not None,
            "invalid OpenGL ES named config")
    require(surface is None or surface == "fbo", "unsupported OpenGL ES XML surface type")
    require((config is None) == (surface is None), "incomplete OpenGL ES FBO configuration")
    return {"surface_width": width, "surface_height": height,
            "base_seed": seed, "fbo_config": config}


def uses_fbo_surface(configuration: dict[str, Any]) -> bool:
    """Whether a run record draws into the framework-owned framebuffer object.

    appendConfigArgs adds ``--deqp-surface-type=fbo`` only where the generated
    RunParams table carries an fboConfig, which it does for the records whose
    command line already names that surface.
    """
    return "--deqp-surface-type=fbo" in configuration["xml_arguments"]


def selected(version: str, predicate: str) -> bool:
    if predicate == "exact":
        return ES_VERSIONS[version] == TARGET_VERSION
    return ES_VERSIONS[version] <= TARGET_VERSION


def parse_documents(documents: Any) -> list[dict[str, Any]]:
    require(isinstance(documents, list) and len(documents) == len(COLLECTIONS),
            "incomplete OpenGL ES XML collection")
    configurations: list[dict[str, Any]] = []
    attributes = {"caseListFile", "commandLine", "name", "os", "useForFirstEGLConfig"}
    for document, (collection, directory, package_name, predicate) in zip(documents, COLLECTIONS):
        path = f"{ROOT}/{directory}/main/mustpass.xml"
        require(isinstance(document, dict)
                and set(document) == {"collection", "path", "size", "sha256", "text"},
                "invalid OpenGL ES XML record")
        require(document["collection"] == collection and document["path"] == path,
                "OpenGL ES XML collection order/path changed")
        require(isinstance(document["text"], str), "invalid OpenGL ES XML payload")
        payload = document["text"].encode("utf-8")
        require(type(document["size"]) is int and 0 < document["size"] <= 64 * 1024,
                "invalid OpenGL ES XML size")
        require(file_identity(path, payload)
                == {key: document[key] for key in ("path", "size", "sha256")},
                "OpenGL ES XML identity changed")
        require(b"<!DOCTYPE" not in payload and b"<!ENTITY" not in payload,
                "unsupported OpenGL ES XML declarations")
        try:
            root = ElementTree.fromstring(payload)
        except ElementTree.ParseError as error:
            raise PlanError("malformed OpenGL ES must-pass XML") from error
        require(root.tag == "Mustpass" and root.attrib == {"version": "main"},
                "unsupported OpenGL ES XML document")
        require(len(root) == 1, "unexpected OpenGL ES XML package set")
        package = root[0]
        require(package.tag == "TestPackage" and package.attrib == {"name": package_name},
                "unexpected OpenGL ES XML package")
        chosen = 0
        for xml_index, element in enumerate(package):
            require(element.tag == "Configuration"
                    and set(element.attrib) == attributes and not list(element),
                    "invalid OpenGL ES XML configuration")
            require(not (element.text or "").strip(),
                    "unexpected OpenGL ES configuration content")
            row = element.attrib
            require(row["os"] == "any" and row["useForFirstEGLConfig"] in {"True", "False"},
                    "unsupported OpenGL ES configuration scope")
            match = CASELIST.fullmatch(row["caseListFile"])
            require(match is not None, "unsafe or unsupported OpenGL ES caselist reference")
            version, name = match.groups()
            require(row["name"] == name, "OpenGL ES configuration name differs from caselist")
            try:
                arguments = shlex.split(row["commandLine"])
            except ValueError as error:
                raise PlanError("malformed OpenGL ES must-pass command") from error
            values = command_values(arguments)
            if not selected(version, predicate):
                continue
            first = row["useForFirstEGLConfig"] == "True"
            require(collection == "main" or first,
                    "one-time OpenGL ES collection has an other-config row")
            configurations.append({
                "index": len(configurations), "collection": collection,
                "xml_index": xml_index, "leaf_id": row["caseListFile"][:-4],
                "leaf_path": f"{ROOT}/{directory}/main/{row['caseListFile']}",
                "name": name, "use_for_first_egl_config": first,
                "xml_arguments": arguments, **values,
            })
            chosen += 1
        require(chosen > 0, f"OpenGL ES XML collection has no ES 3.2 runs: {collection}")
    main = [row for row in configurations if row["collection"] == "main"]
    require(any(row["use_for_first_egl_config"] for row in main)
            and any(not row["use_for_first_egl_config"] for row in main),
            "OpenGL ES main XML lacks first/other roles")
    return configurations


def build_manifest(
    read_document: Callable[[str], bytes],
    leaf_identity: Callable[[str], dict[str, Any]],
) -> dict[str, Any]:
    """Assemble the ES 3.2 manifest from repository XML and pinned leaves.

    ``read_document`` returns the bytes of one collection's ``mustpass.xml``
    and ``leaf_identity`` the pinned identity of one caselist; the caselists
    themselves live only in the guest image, where the shard re-checks the
    digest this manifest carries.
    """
    documents = []
    for collection, directory, _, _ in COLLECTIONS:
        path = f"{ROOT}/{directory}/main/mustpass.xml"
        payload = read_document(collection)
        documents.append({"collection": collection, **file_identity(path, payload),
                          "text": payload.decode("utf-8", errors="strict")})
    configurations = parse_documents(documents)
    leaves: dict[str, dict[str, Any]] = {}
    count = 0
    for row in configurations:
        if row["leaf_id"] in leaves:
            require(leaves[row["leaf_id"]]["path"] == row["leaf_path"],
                    "OpenGL ES leaf identifier names multiple files")
            continue
        leaf = dict(leaf_identity(row["leaf_path"]))
        require(set(leaf) == LEAF_KEYS, "invalid OpenGL ES leaf identity")
        require(leaf["path"] == row["leaf_path"], "OpenGL ES leaf identity names another file")
        leaf["id"] = row["leaf_id"]
        leaves[row["leaf_id"]] = leaf
        count += leaf["case_count"]
    return {"schema_version": SCHEMA, "api": API,
            "runner_source": dict(RUNNER_SOURCE), "documents": documents,
            "configurations": configurations, "leaves": list(leaves.values()),
            "leaf_count": len(leaves), "case_count": count}


def validate_manifest(manifest: Any) -> dict[str, Any]:
    require(isinstance(manifest, dict) and set(manifest) == {
        "schema_version", "api", "runner_source", "documents", "configurations",
        "leaves", "leaf_count", "case_count",
    }, "invalid OpenGL ES must-pass manifest")
    require(type(manifest["schema_version"]) is int
            and manifest["schema_version"] == SCHEMA and manifest["api"] == API,
            "unsupported OpenGL ES must-pass manifest")
    require(manifest["runner_source"] == RUNNER_SOURCE, "OpenGL ES runner scope source changed")
    configurations = parse_documents(manifest["documents"])
    require(manifest["configurations"] == configurations,
            "OpenGL ES run records differ from authenticated XML")
    expected: dict[str, str] = {}
    for row in configurations:
        expected.setdefault(row["leaf_id"], row["leaf_path"])
    leaves = manifest["leaves"]
    require(isinstance(leaves, list) and len(leaves) == len(expected),
            "incomplete OpenGL ES leaf set")
    count = 0
    for leaf, (identifier, path) in zip(leaves, expected.items()):
        require(isinstance(leaf, dict) and set(leaf) == LEAF_KEYS,
                "invalid OpenGL ES leaf identity")
        require(leaf["id"] == identifier and leaf["path"] == path,
                "OpenGL ES leaf order/path differs from XML")
        require(type(leaf["case_count"]) is int and leaf["case_count"] > 0
                and type(leaf["size"]) is int and leaf["size"] > 0,
                "invalid OpenGL ES leaf size/case count")
        require(isinstance(leaf["sha256"], str) and SHA256.fullmatch(leaf["sha256"]) is not None,
                "invalid OpenGL ES leaf digest")
        count += leaf["case_count"]
    for key in ("leaf_count", "case_count"):
        require(type(manifest[key]) is int, "invalid OpenGL ES coverage count")
    require(manifest["leaf_count"] == len(leaves) and manifest["case_count"] == count,
            "OpenGL ES coverage count mismatch")
    return manifest


def expand_runs(manifest: dict[str, Any], configs: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Expand the manifest over the ordered EGL configuration list.

    getTestRunsForES emits the NoContext and Single Config collections against
    the first configuration only, then walks every configuration and emits the
    Khronos ES rows whose first/other role matches.
    """
    validate_manifest(manifest)
    require(isinstance(configs, list) and bool(configs),
            "OpenGL ES run plan has no EGL configurations")
    seen = set()
    for config in configs:
        require(isinstance(config, dict) and set(config) == {"id", "surface_types"},
                "invalid OpenGL ES EGL configuration")
        require(type(config["id"]) is int and config["id"] > 0 and config["id"] not in seen,
                "duplicate or invalid OpenGL ES EGL config ID")
        seen.add(config["id"])
        surfaces = config["surface_types"]
        require(isinstance(surfaces, list) and bool(surfaces)
                and all(value in {"window", "pbuffer", "pixmap"} for value in surfaces)
                and len(surfaces) == len(set(surfaces)),
                "invalid OpenGL ES EGL surface list")
    runs: list[dict[str, Any]] = []

    def append(configuration: dict[str, Any], config_index: int) -> None:
        config = configs[config_index]
        surface = "fbo" if uses_fbo_surface(configuration) else next(
            value for value in ("window", "pbuffer", "pixmap") if value in config["surface_types"]
        )
        runs.append({"index": len(runs), "configuration_index": configuration["index"],
                     "egl_config_index": config_index, "egl_config_id": config["id"],
                     "surface_type": surface, "leaf_id": configuration["leaf_id"]})

    rows = manifest["configurations"]
    for row in rows:
        if row["collection"] != "main":
            append(row, 0)
    for index in range(len(configs)):
        for row in rows:
            if row["collection"] == "main" and row["use_for_first_egl_config"] == (index == 0):
                append(row, index)
    return runs


def runner_arguments(manifest: dict[str, Any], run: dict[str, Any], caselist_path: str) -> list[str]:
    """The official command line for one run, in getBaseOptions order."""
    row = manifest["configurations"][run["configuration_index"]]
    arguments = [
        f"--deqp-caselist-file={caselist_path}",
        "--deqp-screen-rotation=unspecified",
        f"--deqp-surface-width={row['surface_width']}",
        f"--deqp-surface-height={row['surface_height']}",
        "--deqp-watchdog=disable",
        f"--deqp-base-seed={row['base_seed']}",
    ]
    if uses_fbo_surface(row):
        arguments.extend((f"--deqp-gl-config-name={row['fbo_config']}",
                          "--deqp-surface-type=fbo"))
    else:
        arguments.append(f"--deqp-surface-type={run['surface_type']}")
    arguments.extend((f"--deqp-gl-config-id={run['egl_config_id']}",
                      "--deqp-gl-context-type=egl"))
    return arguments
