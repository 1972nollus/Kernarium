#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later

from __future__ import annotations

__import__("sys").dont_write_bytecode = True

import argparse
import csv
import hashlib
from pathlib import Path
import re
import subprocess
import sys


SOURCE_HEADER = (
    "component",
    "repository",
    "base_commit",
    "base_tree",
    "release_ref",
    "release_object",
    "series",
)
SERIES_HEADER = ("order", "patch", "stable_patch_id", "sha256", "subject")
REQUIRED_COMPONENTS = {
    "mesa",
    "venus-protocol",
    "vk-gl-cts",
    "vulkan-headers",
    "vulkan-loader",
}
UNPATCHED_COMPONENTS = {
    "vulkan-headers",
    "vulkan-loader",
}
HEX40 = re.compile(r"[0-9a-f]{40}")
HEX64 = re.compile(r"[0-9a-f]{64}")


class VerificationError(RuntimeError):
    pass


def fail(message: str) -> None:
    raise VerificationError(message)


def read_tsv(path: Path) -> tuple[tuple[str, ...], list[dict[str, str]]]:
    try:
        lines = [
            line
            for line in path.read_text(encoding="utf-8").splitlines()
            if line and not line.startswith("#")
        ]
    except OSError as error:
        fail(f"cannot read {path}: {error}")
    if not lines:
        fail(f"TSV is empty: {path}")
    reader = csv.DictReader(lines, delimiter="\t")
    header = tuple(reader.fieldnames or ())
    rows = list(reader)
    if any(None in row or None in row.values() for row in rows):
        fail(f"TSV has a malformed row: {path}")
    return header, rows


def safe_relative(root: Path, value: str, label: str) -> Path:
    relative = Path(value)
    if relative.is_absolute() or not value or "\x00" in value:
        fail(f"{label} is not a safe relative path: {value!r}")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root.resolve())
    except ValueError:
        fail(f"{label} escapes its root: {value!r}")
    return candidate


def patch_id(path: Path) -> str:
    result = subprocess.run(
        ("git", "patch-id", "--stable"),
        input=path.read_bytes(),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        fail(f"git patch-id failed for {path}: {result.stderr.decode().strip()}")
    words = result.stdout.decode("ascii", errors="strict").split()
    if len(words) < 2 or HEX40.fullmatch(words[0]) is None:
        fail(f"git patch-id returned malformed output for {path}")
    return words[0]


def verify_series(metadata_root: Path, series_value: str) -> str:
    series = safe_relative(metadata_root, series_value, "series path")
    header, rows = read_tsv(series)
    if header != SERIES_HEADER:
        fail(f"series header changed in {series}: {header}")
    if not rows:
        fail(f"patch series is empty: {series}")

    declared: set[str] = set()
    payloads: list[str] = []
    for expected_order, row in enumerate(rows, start=1):
        if row["order"] != str(expected_order):
            fail(f"patch order changed in {series}: {row['order']}")
        filename = row["patch"]
        if Path(filename).name != filename or not filename.endswith(".patch"):
            fail(f"unsafe patch filename in {series}: {filename!r}")
        if filename in declared:
            fail(f"duplicate patch in {series}: {filename}")
        declared.add(filename)
        if HEX40.fullmatch(row["stable_patch_id"]) is None:
            fail(f"malformed patch ID in {series}: {filename}")
        if HEX64.fullmatch(row["sha256"]) is None:
            fail(f"malformed patch digest in {series}: {filename}")
        if not row["subject"] or "\t" in row["subject"]:
            fail(f"malformed patch subject in {series}: {filename}")

        patch = safe_relative(series.parent, filename, "patch path")
        payload = patch.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        if digest != row["sha256"]:
            fail(f"patch digest changed for {patch}")
        if patch_id(patch) != row["stable_patch_id"]:
            fail(f"stable patch ID changed for {patch}")
        text = payload.decode("utf-8", errors="strict")
        # Git folds long Subject headers. Use the same parser as the owning
        # format-patch generator rather than searching raw mail bytes.
        mail = subprocess.run(
            ("git", "mailinfo", "/dev/null", "/dev/null"),
            input=payload,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        subjects = [line.removeprefix("Subject: ") for line in
                    mail.stdout.decode("utf-8", errors="strict").splitlines()
                    if line.startswith("Subject: ")]
        if mail.returncode or "Subject: [PATCH" not in text or subjects != [row["subject"]]:
            fail(f"patch subject does not match its mail file: {patch}")
        payloads.append(text)

    actual = {path.name for path in series.parent.glob("*.patch")}
    if actual != declared:
        fail(f"patch inventory differs from {series}: {sorted(actual ^ declared)}")
    return "\n".join(payloads)


def require_landmarks(payload: str, landmarks: tuple[str, ...], label: str) -> None:
    missing = [landmark for landmark in landmarks if landmark not in payload]
    if missing:
        fail(f"{label} is missing required landmarks: {missing}")


def verify_generated_ownership(
    payload: str, label: str, forbidden_prefixes: tuple[str, ...] = (),
) -> None:
    """Keep generated imports out and retained shader binaries in one topic."""
    generated: set[str] = set()
    for line in payload.splitlines():
        if not line.startswith("diff "):
            continue
        # Only unquoted two-path Git headers are supported. Reject other forms
        # explicitly instead of silently leaving their paths unaudited.
        match = re.fullmatch(r'diff --git a/([^\s"\\]+) b/([^\s"\\]+)', line)
        if match is None:
            fail(f"{label} has an unsupported or quoted diff header: {line}")
        # A rename affects both paths; an ordinary edit names the same path
        # twice and must count only once for generated-file ownership.
        for path in sorted(set(match.groups())):
            if path.startswith(forbidden_prefixes):
                fail(f"{label} imports a build-only generated input: {path}")
            if path.endswith((".spv", "_spirv.h", "_spirv.hpp")):
                if path in generated:
                    fail(f"{label} repeats a generated file across topics: {path}")
                generated.add(path)


def verify(metadata_root: Path, outer_root: Path) -> None:
    header, rows = read_tsv(metadata_root / "sources.tsv")
    if header != SOURCE_HEADER:
        fail(f"source-lock header changed: {header}")
    components = [row["component"] for row in rows]
    if set(components) != REQUIRED_COMPONENTS or len(components) != len(set(components)):
        fail(f"source-lock components changed: {components}")

    series_payloads: dict[str, str] = {}
    for row in rows:
        component = row["component"]
        if not row["repository"].startswith("https://"):
            fail(f"{component} repository is not HTTPS")
        for field in ("base_commit", "base_tree"):
            if HEX40.fullmatch(row[field]) is None:
                fail(f"{component} {field} is not a full Git object ID")
        release_ref = row["release_ref"]
        release_object = row["release_object"]
        if (release_ref == "-") != (release_object == "-"):
            fail(f"{component} release ref/object pair is incomplete")
        if release_object != "-" and HEX40.fullmatch(release_object) is None:
            fail(f"{component} release object is not a full Git object ID")
        if row["series"] == "-":
            if component not in UNPATCHED_COMPONENTS:
                fail(f"component must carry a generated patch series: {component}")
        else:
            series_payloads[component] = verify_series(
                metadata_root, row["series"]
            )

    # Only what the local series still adds. venus-protocol v1.1.3 serializes
    # VK_KHR_maintenance8, VK_KHR_maintenance10 and vkCmdEndRendering2KHR
    # itself, so those moved from this series to the pinned base.
    require_landmarks(
        series_payloads["venus-protocol"],
        (
            "VK_KHR_device_address_commands",
            "VK_COMMAND_TYPE_vkCmdCopyMemoryKHR_EXT",
        ),
        "Venus protocol series",
    )
    verify_generated_ownership(
        series_payloads["venus-protocol"], "Venus protocol series",
        ("include/vulkan/", "include/vk_video/", "xmls/vk.xml"),
    )
    verify_generated_ownership(
        series_payloads["mesa"], "guest Mesa series", ("src/virtio/venus-protocol/",),
    )
    provider_payload = verify_series(outer_root / "dependencies", "patches/mesa/series.tsv")
    verify_generated_ownership(provider_payload, "KosmicKrisp series")
    cts_payload = series_payloads["vk-gl-cts"]
    require_landmarks(
        cts_payload,
        (
            "m_bufferSize - r.bufferImageCopy.bufferOffset",
            "Exclusive compute queue not supported.",
            "5e8b107ad4461730c208c4cc69f08a965c70ceea",
        ),
        "VK-GL-CTS series",
    )

    mesa_payload = series_payloads["mesa"]
    require_landmarks(
        mesa_payload,
        (
            "Always observe the renderer's creation result",
            "vn_host_copy_blocksize",
            "venus: stop advertising DRM display acquisition",
            "venus: preserve renderer conformance version",
            "zink_use_venus_implicit_dmabuf",
        ),
        "Mesa series",
    )
    for obsolete in (
        "VK_KHR_portability_subset",
        "is_portability_driver",
        "MoltenVK",
    ):
        if obsolete in mesa_payload:
            fail(f"Mesa series retains obsolete portability path: {obsolete}")

    renderer_series = (
        outer_root / "dependencies/patches/virglrenderer/series.tsv"
    )
    _, renderer_rows = read_tsv(renderer_series)
    renderer_payload = verify_series(
        outer_root / "dependencies", "patches/virglrenderer/series.tsv"
    )
    verify_generated_ownership(
        renderer_payload, "renderer series", ("src/venus/venus-protocol/",),
    )
    require_landmarks(
        renderer_payload,
        (
            "vulkan-library-file",
            "VK_EXT_external_memory_host",
            "vkr_pipeline_add_array",
            "host image copy wrote",
        ),
        "host renderer series",
    )
    obsolete_renderer_topics = (
        "venus-transport-portability-subset-structures",
        "venus-cap-Metal-renderer-at-Vulkan-1.3",
    )
    for row in renderer_rows:
        if any(topic in row["patch"] for topic in obsolete_renderer_topics):
            fail(f"host renderer retains obsolete topic: {row['patch']}")


def parse_arguments(argv: list[str]) -> argparse.Namespace:
    script_root = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--metadata-root", type=Path, default=script_root)
    parser.add_argument("--outer-root", type=Path, default=script_root.parents[2])
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    options = parse_arguments(argv)
    try:
        verify(options.metadata_root.resolve(), options.outer_root.resolve())
    except (OSError, UnicodeError, VerificationError) as error:
        print(f"verify-inputs.py: {error}", file=sys.stderr)
        return 1
    print("verified Vulkan guest source locks and patch series")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
