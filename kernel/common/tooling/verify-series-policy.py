#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Verify one explicitly selected kernel line's series and local policy."""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

sys.dont_write_bytecode = True


def fail(message: str) -> None:
    raise SystemExit(
        f"kernel/common/tooling/verify-series-policy.py: {message}"
    )


def run(*arguments: str, input_bytes: bytes | None = None) -> str:
    result = subprocess.run(
        arguments,
        input=input_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        detail = result.stderr.decode(errors="replace").strip()
        fail(f"command failed ({' '.join(arguments)}): {detail}")
    return result.stdout.decode(errors="strict")


def patch_parse_root(line_root: Path) -> Path:
    """Use the outer worktree root so Git does not subdirectory-filter paths."""
    result = subprocess.run(
        ["git", "-C", str(line_root), "rev-parse", "--show-toplevel"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        check=False,
        text=True,
    )
    if result.returncode == 0:
        candidate = Path(result.stdout.strip()).resolve()
        if candidate.is_dir():
            return candidate
    # Temporary policy fixtures need not themselves be Git repositories.
    return line_root


def table(path: Path, fields: int) -> list[tuple[str, ...]]:
    rows: list[tuple[str, ...]] = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        values = tuple(line.split("\t"))
        if values[0] in {"order", "path", "pattern"}:
            continue
        if len(values) != fields or any(not value.strip() for value in values):
            fail(f"{path}:{number}: expected {fields} nonempty tab-separated fields")
        rows.append(values)
    return rows


def metadata_value(path: Path, name: str) -> str:
    match = re.search(
        rf"^{re.escape(name)}=(?:'([^']*)'|([^\n#]+))$",
        path.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if match is None:
        fail(f"cannot parse {name} from {path}")
    return (match.group(1) or match.group(2)).strip()


@dataclass(frozen=True)
class ManifestRow:
    order: int
    filename: str
    patch_id: str
    digest: str
    subject: str
    paths: frozenset[str]


def patch_mail_subject(contents: bytes) -> str:
    header = contents.split(b"\n\n", 1)[0].decode("utf-8")
    lines = header.splitlines()
    subject = ""
    collecting = False
    for line in lines:
        if line.startswith("Subject: "):
            subject = line.removeprefix("Subject: ")
            collecting = True
        elif collecting and line.startswith((" ", "\t")):
            subject += " " + line.strip()
        elif collecting:
            break
    if not subject:
        fail("patch mail has no subject")
    return subject


def patch_subject(contents: bytes) -> str:
    subject = re.sub(
        r"^\[PATCH[^]]*\]\s*", "", patch_mail_subject(contents)
    )
    if not subject:
        fail("patch mail has no subject after its prefix")
    return subject


HUNK_HEADER = re.compile(
    rb"^@@ -[0-9]+(?:,([0-9]+))? \+[0-9]+(?:,([0-9]+))? @@(?: .*)?$"
)


def verify_added_source_whitespace(filename: str, contents: bytes) -> None:
    """Reject whitespace errors in source lines added by a mail patch."""
    old_remaining = 0
    new_remaining = 0
    in_diff = False
    in_hunk = False
    for number, line in enumerate(contents.splitlines(), 1):
        if line.startswith(b"diff --git "):
            if in_hunk:
                fail(f"{filename}:{number}: truncated unified-diff hunk")
            in_diff = True
            continue
        if not in_diff:
            continue
        if not in_hunk and line == b"-- ":
            in_diff = False
            continue
        if line.startswith(b"@@ "):
            match = HUNK_HEADER.fullmatch(line)
            if match is None:
                fail(f"{filename}:{number}: malformed unified-diff hunk header")
            old_remaining = int(match.group(1) or b"1")
            new_remaining = int(match.group(2) or b"1")
            in_hunk = old_remaining != 0 or new_remaining != 0
            continue
        if not in_hunk:
            continue
        if line.startswith(b"\\ No newline at end of file"):
            continue
        if not line:
            fail(f"{filename}:{number}: malformed empty unified-diff line")

        marker = line[:1]
        source = line[1:]
        if marker == b" ":
            old_remaining -= 1
            new_remaining -= 1
        elif marker == b"-":
            old_remaining -= 1
        elif marker == b"+":
            new_remaining -= 1
            if source.endswith((b" ", b"\t")):
                fail(
                    f"{filename}:{number}: added source line has trailing whitespace"
                )
            indentation = source[:len(source) - len(source.lstrip(b" \t"))]
            if b" \t" in indentation:
                fail(
                    f"{filename}:{number}: added source indentation has space before tab"
                )
        else:
            fail(f"{filename}:{number}: malformed unified-diff hunk line")

        if old_remaining < 0 or new_remaining < 0:
            fail(f"{filename}:{number}: unified-diff hunk exceeds declared size")
        in_hunk = old_remaining != 0 or new_remaining != 0

    if in_hunk:
        fail(f"{filename}: truncated unified-diff hunk")


def verify_topic_path_overlaps(root: Path, rows: list[ManifestRow]) -> None:
    policy_path = root / "patches/topic-path-overlaps.tsv"
    declared: dict[str, tuple[str, ...]] = {}
    if policy_path.is_file():
        for path, topic_text, _ in table(policy_path, 3):
            if (path.startswith("/") or "\\" in path or
                    ".." in path.split("/")):
                fail(f"unsafe topic-overlap path: {path}")
            if (path != "arch/arm64/Makefile.um" and
                    not path.startswith(("arch/arm64/um/", "arch/um/"))):
                fail(f"topic overlap is outside the port-owned paths: {path}")
            if path in declared:
                fail(f"duplicate topic-overlap policy path: {path}")
            if not re.fullmatch(r"[0-9]{4}(,[0-9]{4})+", topic_text):
                fail(f"invalid topic-overlap order list for {path}: {topic_text}")
            topics = tuple(topic_text.split(","))
            if tuple(sorted(set(topics))) != topics:
                fail(f"topic-overlap orders are not unique and sorted: {path}")
            declared[path] = topics

    owners: dict[str, list[str]] = {}
    rows_by_order = {f"{row.order:04d}": row for row in rows}
    for row in rows:
        order = f"{row.order:04d}"
        for path in row.paths:
            owners.setdefault(path, []).append(order)
    actual = {
        path: tuple(topic_orders)
        for path, topic_orders in owners.items()
        if len(topic_orders) > 1
    }

    for path, topics in declared.items():
        for topic in topics:
            row = rows_by_order.get(topic)
            if row is None or path not in row.paths:
                fail(f"topic-overlap policy names a non-owner: {path} {topic}")
    if actual != declared:
        missing = sorted(set(actual) - set(declared))
        stale = sorted(set(declared) - set(actual))
        mismatched = sorted(
            path for path in set(actual) & set(declared)
            if actual[path] != declared[path]
        )
        fail("topic-path overlaps differ from policy; "
             f"undeclared={missing}, stale={stale}, mismatched={mismatched}")


def manifest(root: Path) -> list[ManifestRow]:
    series = root / "patches/series"
    manifest_path = series / "manifest.tsv"
    metadata = root / "patches/metadata.env"
    expected_count = int(metadata_value(metadata, "UML_KERNEL_PATCH_COUNT"))
    rows: list[ManifestRow] = []
    apply_root = patch_parse_root(root)
    seen_files: set[str] = set()
    seen_ids: set[str] = set()
    for expected_order, values in enumerate(table(manifest_path, 5), 1):
        order_text, filename, patch_id, digest, subject = values
        if not order_text.isdecimal() or int(order_text) != expected_order:
            fail(f"manifest order {order_text} is not {expected_order}")
        if not re.fullmatch(r"[A-Za-z0-9._-]+", filename) or ".." in filename:
            fail(f"unsafe patch filename: {filename}")
        if filename in seen_files or patch_id in seen_ids:
            fail(f"duplicate patch filename or ID: {filename}")
        patch = series / filename
        if not patch.is_file():
            fail(f"manifest patch is missing: {patch}")
        contents = patch.read_bytes()
        verify_added_source_whitespace(filename, contents)
        prefix_width = len(str(expected_count))
        expected_prefix = "[PATCH] " if expected_count == 1 else (
            f"[PATCH {expected_order:0{prefix_width}d}/{expected_count}] "
        )
        if not patch_mail_subject(contents).startswith(expected_prefix):
            fail(
                f"patch mail Subject prefix mismatch for {filename}; "
                f"expected {expected_prefix.rstrip()}"
            )
        actual_digest = hashlib.sha256(contents).hexdigest()
        if actual_digest != digest:
            fail(f"patch hash mismatch for {filename}")
        patch_ids = [line.split()[0] for line in run(
            "git", "patch-id", "--stable", input_bytes=contents
        ).splitlines() if line.strip()]
        if patch_ids != [patch_id]:
            fail(f"{filename} must have exactly its one manifest patch ID")
        if patch_subject(contents) != subject:
            fail(f"patch subject mismatch for {filename}")
        paths: set[str] = set()
        for line in run(
            "git", "-C", str(apply_root), "apply", "--numstat", str(patch)
        ).splitlines():
            values = line.split("\t")
            if len(values) != 3 or not values[2]:
                fail(f"cannot parse changed path in {filename}")
            paths.add(values[2])
        rows.append(ManifestRow(expected_order, filename, patch_id, digest,
                                subject, frozenset(paths)))
        seen_files.add(filename)
        seen_ids.add(patch_id)
    if len(rows) != expected_count:
        fail(f"manifest has {len(rows)} patches; metadata requires {expected_count}")
    inventory = {
        path.relative_to(series).as_posix()
        for path in series.rglob("*.patch")
        if path.is_file()
    }
    if inventory != seen_files:
        missing = sorted(seen_files - inventory)
        unlisted = sorted(inventory - seen_files)
        fail(f"series patch inventory differs from manifest; "
             f"missing={missing}, unlisted={unlisted}")
    verify_topic_path_overlaps(root, rows)
    return rows


def verify_topology(root: Path, source: Path, base: str, tip: str) -> None:
    rows = manifest(root)
    commits = run("git", "-C", str(source), "rev-list", "--reverse",
                  f"{base}..{tip}").splitlines()
    if len(commits) != len(rows):
        fail(f"history has {len(commits)} commits; expected {len(rows)}")
    for commit, row in zip(commits, rows, strict=True):
        parents = run("git", "-C", str(source), "show", "-s",
                      "--format=%P", commit).split()
        if len(parents) != 1:
            fail(f"commit {commit} is a merge or root commit")
        subject = run("git", "-C", str(source), "show", "-s",
                      "--format=%s", commit).rstrip("\n")
        if subject != row.subject:
            fail(f"commit {commit} subject/order differs from manifest")
        patch = subprocess.run(
            ["git", "-C", str(source), "show", "--pretty=format:", commit],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        if patch.returncode != 0:
            fail(f"cannot inspect commit {commit}")
        ids = [line.split()[0] for line in run(
            "git", "patch-id", "--stable", input_bytes=patch.stdout
        ).splitlines() if line.strip()]
        if ids != [row.patch_id]:
            fail(f"commit {commit} lacks its one exact manifest patch ID")


def owner_for(path: str, ownership: list[tuple[str, ...]]) -> tuple[str, str] | None:
    for pattern, owner, disposition, _ in ownership:
        if fnmatch.fnmatchcase(path, pattern):
            return owner, disposition
    return None


def verify_policy(root: Path, source: Path | None, base: str, tip: str) -> None:
    rows = manifest(root)
    paths_by_order = {f"{row.order:04d}": row.paths for row in rows}
    all_paths = set().union(*(row.paths for row in rows))
    ownership = table(root / "patches/path-ownership.tsv", 4)
    extraction_rows = table(root / "patches/extraction-decisions.tsv", 6)
    outside_rows = table(root / "patches/outside-port-justification.tsv", 6)

    extraction: dict[str, tuple[str, ...]] = {}
    for row in extraction_rows:
        path, owner, decision, boundary, action, rationale = row
        if path in extraction:
            fail(f"duplicate extraction decision: {path}")
        classified = owner_for(path, ownership)
        if classified is None or classified[0] != owner:
            fail(f"extraction owner does not match path ownership: {path}")
        if decision not in {"keep-kernel", "keep-build"}:
            fail(f"invalid extraction decision for {path}: {decision}")
        if action != "carry-locally":
            fail(f"invalid extraction next action for {path}: {action}")
        if not boundary.strip() or not rationale.strip():
            fail(f"incomplete extraction decision: {path}")
        extraction[path] = row
    extraction_scope = {
        path for path in all_paths
        if path.startswith("arch/um/drivers/") or path.startswith("scripts/")
    }
    if set(extraction) != extraction_scope:
        missing = sorted(extraction_scope - set(extraction))
        stale = sorted(set(extraction) - extraction_scope)
        fail(f"extraction decisions differ from series paths; missing={missing}, stale={stale}")

    outside: dict[str, tuple[str, ...]] = {}
    for row in outside_rows:
        path, owner, disposition, origin, next_policy, _ = row
        if path in outside:
            fail(f"duplicate outside-port policy: {path}")
        classified = owner_for(path, ownership)
        if classified != (owner, disposition):
            fail(f"outside-port ownership mismatch: {path}")
        if origin not in paths_by_order or path not in paths_by_order[origin]:
            fail(f"outside-port origin does not own path: {path}")
        allowed = {
            ("carry-locally", "carry-locally"),
            ("drop-locally-if-contained", "drop-locally-if-contained"),
        }
        if (disposition, next_policy) not in allowed:
            fail(f"invalid next-version policy for {path}: {next_policy}")
        outside[path] = row
    outside_scope = {
        path for path in all_paths
        if not path.startswith("arch/arm64/um/") and not path.startswith("arch/um/")
    }
    if set(outside) != outside_scope:
        missing = sorted(outside_scope - set(outside))
        stale = sorted(set(outside) - outside_scope)
        fail(f"outside-port policy differs from series paths; missing={missing}, stale={stale}")

    if source is None:
        return
    changed = set(run("git", "-C", str(source), "diff", "--name-only",
                      base, tip).splitlines())
    for path in changed:
        if owner_for(path, ownership) is None:
            fail(f"next-version delta has no owner: {path}")
        if ((path.startswith("arch/um/drivers/") or path.startswith("scripts/"))
                and path not in extraction):
            fail(f"next-version delta lacks extraction decision: {path}")
        if (not path.startswith("arch/arm64/um/") and
                not path.startswith("arch/um/") and path not in outside):
            fail(f"next-version delta lacks per-path policy: {path}")


def recorded_path_policy(root: Path, path: str) -> str:
    outside = {
        row[0]: row for row in table(
            root / "patches/outside-port-justification.tsv", 6
        )
    }
    if path in outside:
        return f"outside/{outside[path][4]}"

    extraction = {
        row[0]: row for row in table(
            root / "patches/extraction-decisions.tsv", 6
        )
    }
    if path in extraction:
        return f"extraction/{extraction[path][4]}"

    ownership = table(root / "patches/path-ownership.tsv", 4)
    classified = owner_for(path, ownership)
    if classified is None:
        fail(f"next-version path has no recorded policy: {path}")
    _, disposition = classified
    if disposition in {"carry-locally", "drop-locally-if-contained"}:
        return f"ownership/{disposition}"
    fail(f"unsupported next-base disposition for {path}: {disposition}")


def required_next_action(state: str, policy: str) -> str:
    name = policy.rsplit("/", 1)[-1]
    if state == "contained":
        if name == "drop-locally-if-contained":
            return "drop-locally-and-retest"
        return "review-contained-change-before-carrying-locally-and-retest"
    if state == "conflict":
        if name == "drop-locally-if-contained":
            return (
                "check-for-equivalent-base-change-then-drop-locally-or-"
                "rebase-locally-and-retest"
            )
        return "rebase-locally-and-retest"
    if state != "applied":
        fail(f"unknown next-version state: {state}")
    if name == "drop-locally-if-contained":
        return "carry-locally-until-contained-and-retest"
    return "carry-locally-and-retest"


def emit_next_actions(root: Path, row: ManifestRow, state: str) -> None:
    for path in sorted(row.paths):
        policy = recorded_path_policy(root, path)
        action = required_next_action(state, policy)
        print(
            f"{row.order:04d}\t{path}\t{state}\t{policy}\t{action}",
            flush=True,
        )


def git_check(source: Path, *arguments: str) -> bool:
    result = subprocess.run(
        ["git", "-C", str(source), *arguments],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def verify_next_version(root: Path, source: Path, base: str) -> None:
    rows = manifest(root)
    verify_policy(root, None, "", "HEAD")
    actual_head = run("git", "-C", str(source), "rev-parse", "HEAD").strip()
    expected_base = run(
        "git", "-C", str(source), "rev-parse", base
    ).strip()
    if actual_head != expected_base:
        fail(
            "next-version source must be detached at the requested base "
            f"({actual_head} != {expected_base})"
        )

    print("patch\tpath\tstate\trecorded_policy\trequired_action", flush=True)
    for row in rows:
        patch = root / "patches/series" / row.filename
        if git_check(source, "apply", "--reverse", "--check", str(patch)):
            emit_next_actions(root, row, "contained")
            fail(
                f"patch {row.order:04d} ({row.filename}) is already "
                "contained by the selected base; follow the emitted actions"
            )

        applied = git_check(
            source,
            "-c", "user.name=Yi Xie",
            "-c", "user.email=imxieyi@users.noreply.github.com",
            "-c", "core.hooksPath=/dev/null",
            "am", "--3way", "--no-gpg-sign", str(patch),
        )
        if not applied:
            subprocess.run(
                ["git", "-C", str(source), "am", "--abort"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
            )
            emit_next_actions(root, row, "conflict")
            fail(
                f"patch {row.order:04d} ({row.filename}) genuinely "
                "conflicts with the selected base; follow the emitted actions"
            )
        emit_next_actions(root, row, "applied")

    verify_topology(root, source, expected_base, "HEAD")
    verify_policy(root, source, expected_base, "HEAD")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--line-root", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--base")
    parser.add_argument("--tip", default="HEAD")
    parser.add_argument(
        "action", choices=("manifest", "topology", "policy", "next-version")
    )
    options = parser.parse_args()
    root = options.line_root.resolve()
    if not root.is_dir():
        fail(f"kernel line root is not a directory: {root}")
    if options.action == "manifest":
        manifest(root)
    elif options.action == "topology":
        if options.source is None or not options.base:
            fail("topology requires --source and --base")
        verify_topology(root, options.source.resolve(), options.base, options.tip)
    elif options.action == "policy":
        if (options.source is None) != (options.base is None):
            fail("policy requires both --source and --base when either is used")
        verify_policy(root, options.source.resolve() if options.source else None,
                      options.base or "", options.tip)
    else:
        if options.source is None or not options.base:
            fail("next-version requires --source and --base")
        verify_next_version(root, options.source.resolve(), options.base)
    print(f"verified {root.name} Linux series {options.action}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as error:
        fail(str(error))
