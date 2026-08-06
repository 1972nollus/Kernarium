#!/bin/sh
set -eu

DEPENDENCY_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../../../dependencies" && pwd)
HELPER="$DEPENDENCY_DIR/source_archive.py"
PYTHON=${PYTHON:-python3}
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-source-archive.XXXXXX")
trap 'rm -rf "$TEST_ROOT"' EXIT HUP INT TERM

fail()
{
    printf '%s\n' "test-source-archive.sh: $*" >&2
    exit 1
}

"$PYTHON" - "$TEST_ROOT" <<'PY'
import io
from pathlib import Path
import tarfile
import sys


output = Path(sys.argv[1])
root = "package-1.0"
archive_identity = {
    "uid": 424242,
    "gid": 424243,
    "uname": "archive-owner",
    "gname": "archive-group",
    "mtime": 946684800,
}


def info(name, entry_type, mode=0o644):
    member = tarfile.TarInfo(name)
    member.type = entry_type
    member.mode = mode
    for key, value in archive_identity.items():
        setattr(member, key, value)
    return member


def add_file(archive, name, contents, mode=0o644):
    data = contents.encode()
    member = info(name, tarfile.REGTYPE, mode)
    member.size = len(data)
    archive.addfile(member, io.BytesIO(data))


with tarfile.open(output / "valid.tar.gz", "w:gz") as archive:
    archive.addfile(info(root + "/", tarfile.DIRTYPE, 0o700))
    archive.addfile(info(root + "/empty", tarfile.DIRTYPE, 0o701))
    archive.addfile(info(root + "/sub", tarfile.DIRTYPE, 0o777))
    add_file(archive, root + "/plain.txt", "plain contents\n", 0o600)
    add_file(archive, root + "/tool.sh", "#!/bin/sh\nexit 0\n", 0o711)
    link = info(root + "/sub/plain-link", tarfile.SYMTYPE, 0o777)
    link.linkname = "../plain.txt"
    archive.addfile(link)


def one_member_archive(filename, member, contents=b"x"):
    with tarfile.open(output / filename, "w") as archive:
        if member.isreg():
            member.size = len(contents)
            archive.addfile(member, io.BytesIO(contents))
        else:
            archive.addfile(member)


one_member_archive("traversal.tar", info(root + "/../escaped", tarfile.REGTYPE))
one_member_archive("absolute.tar", info("/" + root + "/escaped", tarfile.REGTYPE))
one_member_archive("wrong-root.tar", info("other-root/file", tarfile.REGTYPE))

absolute_link = info(root + "/absolute-link", tarfile.SYMTYPE)
absolute_link.linkname = "/tmp/escaped"
one_member_archive("absolute-link.tar", absolute_link)

escaping_link = info(root + "/sub/escaping-link", tarfile.SYMTYPE)
escaping_link.linkname = "../../outside"
one_member_archive("escaping-link.tar", escaping_link)

with tarfile.open(output / "symlink-parent.tar", "w") as archive:
    link = info(root + "/link-directory", tarfile.SYMTYPE)
    link.linkname = "real-directory"
    archive.addfile(link)
    add_file(archive, root + "/link-directory/payload", "must not extract")

one_member_archive("fifo.tar", info(root + "/named-pipe", tarfile.FIFOTYPE))

device = info(root + "/device", tarfile.CHRTYPE)
device.devmajor = 1
device.devminor = 3
one_member_archive("device.tar", device)

hard_link = info(root + "/hard-link", tarfile.LNKTYPE)
hard_link.linkname = "../../outside"
one_member_archive("hard-link.tar", hard_link)
PY

extract()
{
    "$PYTHON" "$HELPER" extract \
        --archive "$1" \
        --expected-root package-1.0 \
        --destination "$2"
}

tree_hash()
{
    "$PYTHON" "$HELPER" tree-hash --root "$1"
}

tree_hash_excluding()
{
    "$PYTHON" "$HELPER" tree-hash --root "$1" --exclude "$2"
}

extract "$TEST_ROOT/valid.tar.gz" "$TEST_ROOT/tree-a"
extract "$TEST_ROOT/valid.tar.gz" "$TEST_ROOT/tree-b"

"$PYTHON" - "$TEST_ROOT/tree-a" <<'PY'
import os
from pathlib import Path
import stat
import sys


root = Path(sys.argv[1])
assert (root / "plain.txt").read_text() == "plain contents\n"
assert (root / "tool.sh").read_text() == "#!/bin/sh\nexit 0\n"
assert (root / "empty").is_dir()
assert os.readlink(root / "sub/plain-link") == "../plain.txt"
assert stat.S_IMODE((root / "plain.txt").stat().st_mode) == 0o644
assert stat.S_IMODE((root / "tool.sh").stat().st_mode) == 0o755
assert stat.S_IMODE((root / "empty").stat().st_mode) == 0o755
assert (root / "plain.txt").stat().st_uid == os.getuid()
assert int((root / "plain.txt").stat().st_mtime) != 946684800
PY

HASH_A=$(tree_hash "$TEST_ROOT/tree-a")
HASH_B=$(tree_hash "$TEST_ROOT/tree-b")
[ "$HASH_A" = "$HASH_B" ] || fail "equivalent extractions have different hashes"

[ "${#HASH_A}" -eq 64 ] || fail "tree hash is not 64 characters"
case "$HASH_A" in
    *[!0-9a-f]*) fail "tree hash is not lowercase hexadecimal" ;;
esac

"$PYTHON" - "$TEST_ROOT/tree-b" <<'PY'
import os
from pathlib import Path
import sys


root = Path(sys.argv[1])
os.chmod(root / "plain.txt", 0o600)
os.chmod(root / "tool.sh", 0o700)
os.chmod(root / "empty", 0o700)
os.utime(root / "plain.txt", (1700000000, 1700000000))
os.utime(root / "tool.sh", (1800000000, 1800000000))
PY
[ "$(tree_hash "$TEST_ROOT/tree-b")" = "$HASH_A" ] || \
    fail "irrelevant permission or timestamp metadata changed the hash"

chmod +x "$TEST_ROOT/tree-b/plain.txt"
[ "$(tree_hash "$TEST_ROOT/tree-b")" != "$HASH_A" ] || \
    fail "regular-file executable status is missing from the hash"

extract "$TEST_ROOT/valid.tar.gz" "$TEST_ROOT/tree-content"
printf '%s\n' "changed contents" >"$TEST_ROOT/tree-content/plain.txt"
[ "$(tree_hash "$TEST_ROOT/tree-content")" != "$HASH_A" ] || \
    fail "regular-file bytes are missing from the hash"

extract "$TEST_ROOT/valid.tar.gz" "$TEST_ROOT/tree-link"
rm "$TEST_ROOT/tree-link/sub/plain-link"
ln -s ../tool.sh "$TEST_ROOT/tree-link/sub/plain-link"
[ "$(tree_hash "$TEST_ROOT/tree-link")" != "$HASH_A" ] || \
    fail "symbolic-link target is missing from the hash"

extract "$TEST_ROOT/valid.tar.gz" "$TEST_ROOT/tree-path"
mv "$TEST_ROOT/tree-path/plain.txt" "$TEST_ROOT/tree-path/renamed.txt"
[ "$(tree_hash "$TEST_ROOT/tree-path")" != "$HASH_A" ] || \
    fail "relative path is missing from the hash"

EXCLUDED_HASH=$(tree_hash_excluding "$TEST_ROOT/tree-a" plain.txt)
printf '%s\n' "ignored receipt bytes" >"$TEST_ROOT/tree-b/plain.txt"
[ "$(tree_hash_excluding "$TEST_ROOT/tree-b" plain.txt)" = "$EXCLUDED_HASH" ] || \
    fail "an exact excluded file still changed the tree hash"
printf '%s\n' "untracked payload" >"$TEST_ROOT/tree-b/untracked.txt"
[ "$(tree_hash_excluding "$TEST_ROOT/tree-b" plain.txt)" != "$EXCLUDED_HASH" ] || \
    fail "an unexcluded payload file was missing from the tree hash"
if tree_hash_excluding "$TEST_ROOT/tree-a" ../outside \
    >"$TEST_ROOT/unsafe-exclude.out" 2>"$TEST_ROOT/unsafe-exclude.err"; then
    fail "tree hash accepted an unsafe excluded path"
fi

FAILURE_NUMBER=0
expect_extract_failure()
{
    FAILURE_NUMBER=$((FAILURE_NUMBER + 1))
    destination="$TEST_ROOT/rejected-$FAILURE_NUMBER"
    if extract "$1" "$destination" 2>"$TEST_ROOT/rejected-error-$FAILURE_NUMBER"; then
        fail "unsafe archive unexpectedly extracted: $1"
    fi
    [ ! -e "$destination" ] || fail "failed extraction left a destination tree: $1"
    [ -s "$TEST_ROOT/rejected-error-$FAILURE_NUMBER" ] || \
        fail "failed extraction did not report an error: $1"
}

expect_extract_failure "$TEST_ROOT/traversal.tar"
expect_extract_failure "$TEST_ROOT/absolute.tar"
expect_extract_failure "$TEST_ROOT/wrong-root.tar"
expect_extract_failure "$TEST_ROOT/absolute-link.tar"
expect_extract_failure "$TEST_ROOT/escaping-link.tar"
expect_extract_failure "$TEST_ROOT/symlink-parent.tar"
expect_extract_failure "$TEST_ROOT/fifo.tar"
expect_extract_failure "$TEST_ROOT/device.tar"
expect_extract_failure "$TEST_ROOT/hard-link.tar"

mkdir "$TEST_ROOT/unsafe-tree"
ln -s ../outside "$TEST_ROOT/unsafe-tree/escaping-link"
if tree_hash "$TEST_ROOT/unsafe-tree" >"$TEST_ROOT/unsafe-tree.out" 2>"$TEST_ROOT/unsafe-tree.err"; then
    fail "tree hash accepted an escaping symbolic link"
fi

printf '%s\n' "source archive tests passed"
