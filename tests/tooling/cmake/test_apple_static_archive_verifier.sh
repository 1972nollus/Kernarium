#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
set -eu

VERIFIER=${1:?verify-apple-static-archive.py is required}
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/uml-apple-archive.XXXXXX")
trap 'rm -rf -- "$TEST_ROOT"' EXIT HUP INT TERM
TEST_ROOT=$(CDPATH= cd -- "$TEST_ROOT" && pwd -P)
failure_index=0

fail()
{
	printf '%s\n' "test_apple_static_archive_verifier.sh: $*" >&2
	exit 1
}

expect_failure()
{
	pattern=$1
	shift
	failure_index=$((failure_index + 1))
	log="$TEST_ROOT/failure-$failure_index.log"
	if "$@" >"$log" 2>&1; then
		fail "unexpected success: $*"
	fi
	grep -F "$pattern" "$log" >/dev/null || {
		cat "$log" >&2
		fail "failure lacked expected diagnostic: $pattern"
	}
}

compile_object()
{
	sdk=$1
	target=$2
	mode=$3
	output=$4
	source=$5
	mkdir -p -- "$(dirname -- "$output")"
	if [ "$mode" = link-time ]; then
		xcrun --sdk "$sdk" clang -target "$target" -flto=thin -c \
			"$source" -o "$output"
	else
		xcrun --sdk "$sdk" clang -target "$target" -c \
			"$source" -o "$output"
	fi
}

verify_macos()
{
	archive=$1
	expected_root=$2
	python3 -B "$VERIFIER" --archive "$archive" --platform macos \
		--expected-link-time-object-root "$expected_root" \
		--forbidden-path "$TEST_ROOT"
}

append_raw_member()
{
	archive=$1
	name=$2
	payload=$3
	python3 -B - "$archive" "$name" "$payload" <<'PY'
from pathlib import Path
import sys

archive = Path(sys.argv[1])
name = sys.argv[2]
payload = Path(sys.argv[3]).read_bytes()
if len(name) > 15:
    raise SystemExit("fixture member name is too long")
header = (
    f"{name + '/':<16}"
    f"{0:<12}"
    f"{0:<6}"
    f"{0:<6}"
    f"{'100644':<8}"
    f"{len(payload):<10}"
    "`\n"
).encode("ascii")
with archive.open("ab") as stream:
    stream.write(header)
    stream.write(payload)
    if len(payload) & 1:
        stream.write(b"\n")
PY
}

append_extended_member()
{
	archive=$1
	name=$2
	payload=$3
	python3 -B - "$archive" "$name" "$payload" <<'PY'
from pathlib import Path
import sys

archive = Path(sys.argv[1])
name = sys.argv[2].encode("utf-8")
payload = Path(sys.argv[3]).read_bytes()
stored_size = len(name) + len(payload)
name_field = f"#1/{len(name)}"
if len(name_field) > 16:
    raise SystemExit("fixture extended-name header is too long")
header = (
    f"{name_field:<16}"
    f"{0:<12}"
    f"{0:<6}"
    f"{0:<6}"
    f"{'100644':<8}"
    f"{stored_size:<10}"
    "`\n"
).encode("ascii")
with archive.open("ab") as stream:
    stream.write(header)
    stream.write(name)
    stream.write(payload)
    if stored_size & 1:
        stream.write(b"\n")
PY
}

cat >"$TEST_ROOT/ordinary.c" <<'EOF'
int uml_archive_ordinary(void) { return 1; }
EOF
cat >"$TEST_ROOT/link_time.c" <<'EOF'
int uml_archive_link_time(void) { return 2; }
EOF
cat >"$TEST_ROOT/alternate.c" <<'EOF'
int uml_archive_alternate(void) { return 3; }
EOF

EXPECTED_ROOT="$TEST_ROOT/expected/uml_arm64_gadgets.dir"
ORDINARY="$TEST_ROOT/objects/ordinary.o"
EXPECTED_LINK="$EXPECTED_ROOT/gadgets/expected_link.cpp.o"
compile_object macosx arm64-apple-macos14.0 ordinary "$ORDINARY" \
	"$TEST_ROOT/ordinary.c"
compile_object macosx arm64-apple-macos14.0 link-time "$EXPECTED_LINK" \
	"$TEST_ROOT/link_time.c"

POSITIVE="$TEST_ROOT/positive.a"
xcrun ar -rcs "$POSITIVE" "$ORDINARY" "$EXPECTED_LINK"
verify_macos "$POSITIVE" "$EXPECTED_ROOT" >"$TEST_ROOT/positive.log"
grep -F '1 arm64 Mach-O members, 1 target-checked gadget link-time members' \
	"$TEST_ROOT/positive.log" >/dev/null ||
	fail "positive archive summary did not classify both object forms"

FOREIGN_ROOT="$TEST_ROOT/foreign/uml_arm64_gadgets.dir"
FOREIGN_LINK="$FOREIGN_ROOT/gadgets/expected_link.cpp.o"
compile_object iphoneos arm64-apple-ios16.0 link-time "$FOREIGN_LINK" \
	"$TEST_ROOT/link_time.c"
FOREIGN_ARCHIVE="$TEST_ROOT/foreign.a"
xcrun ar -rcs "$FOREIGN_ARCHIVE" "$ORDINARY" "$FOREIGN_LINK"
expect_failure 'does not target arm64-apple-macos14.0' \
	verify_macos "$FOREIGN_ARCHIVE" "$FOREIGN_ROOT"

MALFORMED="$TEST_ROOT/objects/malformed.o"
printf 'not an object file from %s\n' "$TEST_ROOT" >"$MALFORMED"
MALFORMED_ARCHIVE="$TEST_ROOT/malformed.a"
xcrun ar -rcs "$MALFORMED_ARCHIVE" "$ORDINARY" "$EXPECTED_LINK"
append_raw_member "$MALFORMED_ARCHIVE" malformed.o "$MALFORMED"
expect_failure 'is neither a 64-bit Mach-O object nor LLVM bitcode' \
	verify_macos "$MALFORMED_ARCHIVE" "$EXPECTED_ROOT"

X86_OBJECT="$TEST_ROOT/objects/x86_64.o"
compile_object macosx x86_64-apple-macos14.0 ordinary "$X86_OBJECT" \
	"$TEST_ROOT/alternate.c"
MIXED_ARCHIVE="$TEST_ROOT/mixed-architecture.a"
cp "$POSITIVE" "$MIXED_ARCHIVE"
append_raw_member "$MIXED_ARCHIVE" x86_64.o "$X86_OBJECT"
expect_failure 'expected arm64' \
	verify_macos "$MIXED_ARCHIVE" "$EXPECTED_ROOT"

OLD_OBJECT="$TEST_ROOT/objects/old-minimum.o"
compile_object macosx arm64-apple-macos13.0 ordinary "$OLD_OBJECT" \
	"$TEST_ROOT/alternate.c"
OLD_ARCHIVE="$TEST_ROOT/old-minimum.a"
xcrun ar -rcs "$OLD_ARCHIVE" "$OLD_OBJECT" "$EXPECTED_LINK"
expect_failure 'platform 1 minimum 13.0, expected platform 1 minimum 14.0' \
	verify_macos "$OLD_ARCHIVE" "$EXPECTED_ROOT"

UNAUDITED_LINK="$TEST_ROOT/objects/unaudited_target.cpp.o"
compile_object macosx arm64-apple-macos14.0 link-time "$UNAUDITED_LINK" \
	"$TEST_ROOT/alternate.c"
UNAUDITED_ARCHIVE="$TEST_ROOT/unaudited-target.a"
xcrun ar -rcs "$UNAUDITED_ARCHIVE" "$ORDINARY" "$EXPECTED_LINK" \
	"$UNAUDITED_LINK"
expect_failure "unexpected=['unaudited_target.cpp.o']" \
	verify_macos "$UNAUDITED_ARCHIVE" "$EXPECTED_ROOT"

IMPOSTOR_DIR="$TEST_ROOT/impostor"
IMPOSTOR_LINK="$IMPOSTOR_DIR/expected_link.cpp.o"
compile_object macosx arm64-apple-macos14.0 link-time "$IMPOSTOR_LINK" \
	"$TEST_ROOT/alternate.c"
IMPOSTOR_ARCHIVE="$TEST_ROOT/impostor.a"
xcrun ar -rcs "$IMPOSTOR_ARCHIVE" "$ORDINARY" "$IMPOSTOR_LINK"
expect_failure 'does not match its expected gadget-target object' \
	verify_macos "$IMPOSTOR_ARCHIVE" "$EXPECTED_ROOT"

cat >"$TEST_ROOT/leaked_path.c" <<EOF
const char *uml_archive_leaked_path = "$TEST_ROOT";
EOF
LEAKED_OBJECT="$TEST_ROOT/objects/leaked-path.o"
compile_object macosx arm64-apple-macos14.0 ordinary "$LEAKED_OBJECT" \
	"$TEST_ROOT/leaked_path.c"
LEAKED_ARCHIVE="$TEST_ROOT/leaked-path.a"
xcrun ar -rcs "$LEAKED_ARCHIVE" "$LEAKED_OBJECT" "$EXPECTED_LINK"
expect_failure "contains forbidden path: $TEST_ROOT" \
	verify_macos "$LEAKED_ARCHIVE" "$EXPECTED_ROOT"

# A BSD extended archive name can carry an absolute path. Ensure it is
# rejected structurally and cannot overwrite a file outside verifier storage.
EXTERNAL_SENTINEL="$TEST_ROOT/external-sentinel.o"
printf '%s\n' preserved >"$EXTERNAL_SENTINEL"
ABSOLUTE_NAME_ARCHIVE="$TEST_ROOT/absolute-member-name.a"
xcrun ar -rcs "$ABSOLUTE_NAME_ARCHIVE" "$ORDINARY" "$EXPECTED_LINK"
append_extended_member "$ABSOLUTE_NAME_ARCHIVE" "$EXTERNAL_SENTINEL" \
	"$EXPECTED_LINK"
expect_failure 'archive member name is not a plain filename' \
	verify_macos "$ABSOLUTE_NAME_ARCHIVE" "$EXPECTED_ROOT"
[ "$(cat "$EXTERNAL_SENTINEL")" = preserved ] ||
	fail "absolute archive member name overwrote an external file"

# Exercise the target-checking sink directly too. This would overwrite the
# sentinel if it ever reused the diagnostic member name as a temporary path.
python3 -B - "$VERIFIER" "$EXPECTED_LINK" "$EXTERNAL_SENTINEL" <<'PY'
from pathlib import Path
import runpy
import sys

namespace = runpy.run_path(sys.argv[1])
member = namespace["ArchiveMember"](
    name=sys.argv[3], data=Path(sys.argv[2]).read_bytes()
)
namespace["verify_link_time_target"](
    member, namespace["PLATFORMS"]["macos"], "xcrun"
)
if Path(sys.argv[3]).read_text(encoding="utf-8") != "preserved\n":
    raise SystemExit("target check wrote through its archive-controlled member name")
PY

printf '%s\n' 'Apple static archive verifier tests passed'
