#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run tools/arm64-tests/sve_native_oracle.cpp on an arm64 Linux SVE host over
# ssh, N times (two by default), each time from a fresh private directory.
# Each run copies only the oracle's source and the request, builds the oracle
# there with the host's C++ compiler, runs it, fetches the response, and
# removes the directory, so nothing stays on the host.
#
#   sve-native-oracle.sh [--prefix NAME] [--runs N] HOST REQUEST OUTPUT
#
# writes OUTPUT.1 ... OUTPUT.N, the responses of the runs, for
# sve-native-corpus.py --response, which requires them to give identical
# records. The directories are $HOME/NAME.XXXXXX on the host (NAME defaults
# to sve-native-oracle). Every ssh and transfer is time-bounded.

set -euo pipefail

prefix=sve-native-oracle
runs=2
while [[ $# -gt 0 ]]; do
	case "$1" in
	--prefix) prefix=$2; shift 2 ;;
	--runs) runs=$2; shift 2 ;;
	--) shift; break ;;
	-*) echo "unknown option $1" >&2; exit 64 ;;
	*) break ;;
	esac
done
if [[ $# -ne 3 || ! $prefix =~ ^[A-Za-z0-9._-]+$ || ! $runs =~ ^[1-9]$ ]]; then
	echo "usage: $0 [--prefix NAME] [--runs N] HOST REQUEST OUTPUT" >&2
	exit 64
fi
host=$1
request=$2
output=$3
source_file="$(cd "$(dirname "$0")" && pwd)/sve_native_oracle.cpp"
[[ -f $request && -f $source_file ]] || { echo "missing input" >&2; exit 66; }

ssh_options=(-o ConnectTimeout=15 -o BatchMode=yes)
remote() {
	timeout "$1" ssh "${ssh_options[@]}" "$host" "${@:2}" </dev/null
}

directory=
cleanup() {
	if [[ -n $directory ]]; then
		# Only a directory this script made.
		remote 60 "case '$directory' in \"\$HOME\"/$prefix.*) rm -rf -- '$directory' ;; esac" ||
			echo "could not remove $host:$directory" >&2
		directory=
	fi
}
trap cleanup EXIT

for ((run = 1; run <= runs; run++)); do
	directory=$(remote 60 "mktemp -d \"\$HOME/$prefix.XXXXXX\"")
	[[ $directory == /* ]] || { echo "bad remote directory" >&2; exit 70; }
	timeout 600 rsync -z -e "ssh ${ssh_options[*]}" \
		"$source_file" "$request" "$host:$directory/" </dev/null
	remote 1800 "set -e; cd '$directory';
		c++ -std=c++20 -O2 -Wall -Wextra -Werror \
			-o sve_native_oracle sve_native_oracle.cpp;
		timeout 1500 ./sve_native_oracle '$(basename "$request")' response"
	timeout 600 rsync -z -e "ssh ${ssh_options[*]}" \
		"$host:$directory/response" "$output.$run" </dev/null
	cleanup
	echo "run $run: $(shasum -a 256 "$output.$run" | cut -d' ' -f1)"
done
echo "$runs responses from $host"
