#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pin the shared patch-series contract.

A series exports to the same bytes every time, changes only where its topics
change, replays with ``git am`` to the tree it records, and rejects anything
that did not come from its topic branch.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))

import git_identity  # noqa: E402
import patch_series  # noqa: E402

SeriesError = patch_series.SeriesError
TOOL = ROOT / "scripts" / "patch_series.py"


def git(repository: Path, *arguments: str, date: str | None = None) -> str:
    environment = dict(os.environ)
    environment["GIT_CONFIG_NOSYSTEM"] = "1"
    environment["GIT_CONFIG_GLOBAL"] = os.devnull
    if date is not None:
        environment["GIT_AUTHOR_DATE"] = date
        environment["GIT_COMMITTER_DATE"] = date
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=environment,
        check=False,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"git {' '.join(arguments)}: {result.stderr.decode(errors='replace')}"
        )
    return result.stdout.decode()


def commit(repository: Path, subject: str, files: dict[str, bytes | None], day: int) -> str:
    for relative, content in files.items():
        path = repository / relative
        if content is None:
            path.unlink()
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    git(repository, "add", "-A")
    git(repository, "commit", "-q", "-m", subject, "-m", f"Body of {subject}.",
        date=f"2026-01-{day:02d}T12:00:00+0900")
    return git(repository, "rev-parse", "HEAD").strip()


def expect_error(label: str, fragment: str, action) -> None:
    try:
        action()
    except SeriesError as error:
        if fragment not in str(error):
            raise AssertionError(f"{label}: unexpected error: {error}") from error
        return
    raise AssertionError(f"{label}: accepted")


def snapshot(directory: Path) -> dict[str, bytes]:
    return {entry.name: entry.read_bytes() for entry in sorted(directory.iterdir())
            if entry.is_file()}


def export(source: Path, base: str, head: str, directory: Path, **options) -> str:
    directory.mkdir(parents=True, exist_ok=True)
    return patch_series.export_series(source, base, head, directory, **options)


def main() -> int:
    git_identity.require_git_identity("patch-series contract")
    scratch = Path(tempfile.mkdtemp(prefix="uml-patch-series."))
    try:
        run(scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("patch-series contract passed")
    return 0


def run(scratch: Path) -> None:
    source = scratch / "source"
    source.mkdir()
    git(source, "init", "-q", "-b", "base")
    base = commit(source, "base", {
        "a.txt": b"alpha\n" * 20,
        "b.txt": b"bravo\n" * 20,
        "crlf.txt": b"one\r\ntwo\r\nthree\r\n",
        "sub/c.txt": b"".join(b"charlie %d\n" % index for index in range(40)),
    }, 1)
    git(source, "checkout", "-q", "-b", "topic")
    commit(source, "fix: change a", {"a.txt": b"alpha\n" * 19 + b"ALPHA\n"}, 2)
    commit(source, "feat: add binary", {"bin.dat": bytes(range(256)) * 4}, 3)
    commit(source, "fix: keep carriage returns", {"crlf.txt": b"one\r\nTWO\r\nthree\r\n"}, 4)
    commit(source, "refactor: move c", {
        "sub/c.txt": None,
        "sub/d.txt": b"".join(b"charlie %d\n" % index for index in range(40)) + b"delta\n",
    }, 5)
    head_tree = git(source, "rev-parse", "topic^{tree}").strip()

    # Export: slug names in order, no ordinals or commit IDs, full index lines.
    series = scratch / "series"
    tree = export(source, base, "topic", series, record_tree=True)
    assert tree == head_tree
    parsed = patch_series.read_series(series)
    assert parsed.tree == head_tree
    assert parsed.names == (
        "fix-change-a.patch",
        "feat-add-binary.patch",
        "fix-keep-carriage-returns.patch",
        "refactor-move-c.patch",
    ), parsed.names
    first = (series / "fix-change-a.patch").read_text()
    assert first.startswith(patch_series.ZERO_FROM.decode())
    assert "Subject: [PATCH] fix: change a\n" in first
    index_lines = [line for line in first.splitlines() if line.startswith("index ")]
    assert index_lines and all(len(line.split()[1]) == 82 for line in index_lines), index_lines
    assert "GIT binary patch" in (series / "feat-add-binary.patch").read_text()
    assert b"\r\n" in (series / "fix-keep-carriage-returns.patch").read_bytes()
    assert "rename from sub/c.txt" in (series / "refactor-move-c.patch").read_text()

    # Exporting unchanged commits reproduces every byte.
    before = snapshot(series)
    export(source, base, "topic", series, record_tree=True)
    assert snapshot(series) == before
    export(source, base, "topic", series, record_tree=True, check=True)

    # Inserting one topic changes that topic's file and series.tsv only.
    git(source, "checkout", "-q", "-b", "inserted", base)
    git(source, "cherry-pick", "topic~3")
    commit(source, "fix: change b", {"b.txt": b"bravo\n" * 19 + b"BRAVO\n"}, 6)
    git(source, "cherry-pick", "topic~2", "topic~1", "topic")
    widened = scratch / "widened"
    shutil.copytree(series, widened)
    export(source, base, "inserted", widened, record_tree=True)
    after = snapshot(widened)
    changed = sorted(name for name in set(before) | set(after)
                     if before.get(name) != after.get(name))
    assert changed == ["fix-change-b.patch", "series.tsv"], changed
    assert patch_series.read_series(widened).names[1] == "fix-change-b.patch"

    # A replay reproduces the tree, and exporting the replay reproduces the bytes.
    replay = scratch / "replay"
    git(scratch, "clone", "-q", "--no-hardlinks", str(source), str(replay))
    git(replay, "checkout", "-q", "-b", "replayed", base)
    assert patch_series.apply_series(replay, [series], expected_tree=head_tree) == head_tree
    export(replay, base, "replayed", series, record_tree=True, check=True)
    assert patch_series.series_tree(source, base, [series]) == head_tree

    # A common series plus an overlay compose in order.
    common, overlay = scratch / "common", scratch / "overlay"
    export(source, base, "topic~2", common)
    export(source, "topic~2", "topic", overlay, record_tree=True)
    assert patch_series.read_series(common).tree is None
    assert patch_series.series_tree(source, base, [common, overlay]) == head_tree
    composed = scratch / "composed"
    git(scratch, "clone", "-q", "--no-hardlinks", str(source), str(composed))
    git(composed, "checkout", "-q", "-b", "composed", base)
    patch_series.apply_series(composed, [common, overlay], expected_tree=head_tree)

    # Anything that did not come from the topic branch is rejected.
    tampered = scratch / "tampered"
    shutil.copytree(series, tampered)
    (tampered / "stray.patch").write_bytes(patch_series.ZERO_FROM)
    expect_error("unlisted patch", "unlisted=['stray.patch']",
                 lambda: patch_series.read_series(tampered))
    (tampered / "stray.patch").unlink()
    edited = tampered / "fix-change-a.patch"
    edited.write_text(edited.read_text().replace("From 0000000000", "From 1111111111", 1))
    expect_error("rewritten header", "was not exported",
                 lambda: patch_series.read_series(tampered))
    edited.write_text(first.replace("+ALPHA", "+OMEGA"))
    expect_error("hand edit", "changed fix-change-a.patch",
                 lambda: export(source, base, "topic", tampered, record_tree=True, check=True))
    assert patch_series.series_tree(source, base, [tampered]) != head_tree
    (tampered / "refactor-move-c.patch").unlink()
    expect_error("missing patch", "missing=['refactor-move-c.patch']",
                 lambda: patch_series.read_series(tampered))

    # Topic branches the series cannot represent are refused.
    git(source, "checkout", "-q", "topic")
    (source / "a.txt").write_text("dirty\n")
    expect_error("dirty checkout", "not clean",
                 lambda: export(source, base, "topic", scratch / "dirty"))
    git(source, "checkout", "-q", "--", "a.txt")
    expect_error("unrelated base", "is not based on",
                 lambda: export(source, "inserted", "topic", scratch / "unrelated"))
    git(source, "checkout", "-q", "-b", "merged", base)
    commit(source, "side", {"e.txt": b"echo\n"}, 7)
    git(source, "merge", "-q", "--no-edit", "--no-ff", "topic", date="2026-01-08T12:00:00+0900")
    expect_error("merge", "contains a merge",
                 lambda: export(source, base, "merged", scratch / "merge"))
    git(source, "checkout", "-q", "-b", "empty", "topic")
    git(source, "commit", "-q", "--allow-empty", "-m", "chore: nothing",
        date="2026-01-09T12:00:00+0900")
    expect_error("empty topic", "changes nothing",
                 lambda: export(source, base, "empty", scratch / "empty"))
    git(source, "checkout", "-q", "-b", "twins", "topic")
    commit(source, "fix: change a", {"a.txt": b"ALPHA\n" * 20}, 10)
    expect_error("colliding subjects", "two topics export to fix-change-a.patch",
                 lambda: export(source, base, "twins", scratch / "twins"))

    # The command line reports tree mismatches with a non-zero status.
    verify = [sys.executable, "-B", str(TOOL), "verify", "--series-dir", str(series),
              "--repo", str(source), "--base", base]
    assert subprocess.run(verify, capture_output=True, check=False).returncode == 0
    wrong = subprocess.run(verify + ["--expect-tree", base], capture_output=True, check=False)
    assert wrong.returncode == 1 and b"expected" in wrong.stderr, wrong.stderr


if __name__ == "__main__":
    raise SystemExit(main())
