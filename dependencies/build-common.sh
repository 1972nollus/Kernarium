# SPDX-License-Identifier: GPL-2.0-or-later
#
# Helpers the dependency builders share. Sourcing this file only defines
# names. A caller defines fail() and ROOT_DIR, and sets PYTHON before it
# publishes.

# find_tool VARIABLE CANDIDATE...: set VARIABLE to the path of the first
# candidate on PATH. A tool found through a relative path is made absolute,
# because builders run their tools in other directories.
find_tool()
{
	variable=$1
	shift
	for candidate in "$@"; do
		tool_path=$(command -v "$candidate" 2>/dev/null || true)
		if [ -n "$tool_path" ]; then
			case $tool_path in
			/*) ;;
			*/*) tool_path="$PWD/$tool_path" ;;
			esac
			eval "$variable=\$tool_path"
			return
		fi
	done
	fail "required build tool is unavailable: $*"
}

# publish_verified STAGE DESTINATION VERIFY LABEL: verify the stage with the
# VERIFY function and rename it to DESTINATION, which must not exist. A
# destination that a concurrent build published first (publish-prefix.py
# status 1) is verified and reused, and the stage is left for the caller's
# cleanup; any other publisher failure fails. STAGE is cleared once the
# stage has become the destination.
publish_verified()
{
	publish_stage=$1
	publish_destination=$2
	publish_verify=$3
	publish_label=$4
	"$publish_verify" "$publish_stage"
	publish_status=0
	"$PYTHON" "$ROOT_DIR/dependencies/publish-prefix.py" \
		"$publish_stage" "$publish_destination" || publish_status=$?
	case $publish_status in
	0)
		STAGE=
		printf '%s\n' "published $publish_label: $publish_destination"
		;;
	1)
		"$publish_verify" "$publish_destination" ||
			fail "concurrently published $publish_label is invalid: $publish_destination"
		printf '%s\n' \
			"reusing concurrently published $publish_label: $publish_destination"
		;;
	*)
		fail "could not publish $publish_label: $publish_destination"
		;;
	esac
}
