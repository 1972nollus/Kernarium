#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Geekbench 6 as every benchmark harness runs it, from the payload an image
# carries and without uploading results. /etc/uml-geekbench.env may set
# UML_GEEKBENCH_WORKLOADS to comma-separated workload numbers, which runs only
# those single-core workloads. For an image that carries no payload,
# UML_GEEKBENCH_PAYLOAD may name a block device holding an EROFS image with a
# Geekbench 6 release at its root; the script mounts it read-only and runs its
# geekbench6 instead. The run prints UML_GEEKBENCH_READY, then
# UML_GEEKBENCH_TARGETS:<list> when targeted, and ends with
# UML_GEEKBENCH_END:<status>, exiting with Geekbench's status. A problem before
# the benchmark starts prints UML_GEEKBENCH_ERROR:<reason> instead.

benchmark=/opt/geekbench/release/geekbench6
parameters=/etc/uml-geekbench.env
payload_root=/opt/geekbench-payload

fail()
{
	echo "UML_GEEKBENCH_ERROR:$*"
	exit 1
}

workloads=
payload=
if [ -f "$parameters" ]; then
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
		''|'#'*) ;;
		UML_GEEKBENCH_WORKLOADS=*) workloads=${line#*=} ;;
		UML_GEEKBENCH_PAYLOAD=/dev/*) payload=${line#*=} ;;
		*) fail "unknown $parameters line: $line" ;;
		esac
	done < "$parameters"
	case "$workloads" in
	*[!0-9,]*|,*|*,|*,,*) fail "malformed workload list: $workloads" ;;
	esac
fi
if [ -n "$payload" ]; then
	mkdir -p "$payload_root" &&
		mount -t erofs -o ro "$payload" "$payload_root" ||
		fail "cannot mount the Geekbench payload $payload"
	benchmark=$payload_root/geekbench6
fi
[ -x "$benchmark" ] || fail "the image has no Geekbench payload"

export HOME=/root
echo "UML_GEEKBENCH_READY"
if [ -n "$workloads" ]; then
	echo "UML_GEEKBENCH_TARGETS:$workloads"
	"$benchmark" --no-upload --single-core --section 1 --workload "$workloads"
else
	"$benchmark" --no-upload
fi
status=$?
printf '\nUML_GEEKBENCH_END:%s\n' "$status"
exit "$status"
