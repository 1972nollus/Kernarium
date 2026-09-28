#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Geekbench 6 as every benchmark harness runs it, from the payload an image
# carries and without uploading results. /etc/uml-geekbench.env may set
# UML_GEEKBENCH_WORKLOADS to comma-separated workload numbers, which runs only
# those single-core workloads. The run prints UML_GEEKBENCH_READY, then
# UML_GEEKBENCH_TARGETS:<list> when targeted, and ends with
# UML_GEEKBENCH_END:<status>, exiting with Geekbench's status. A problem before
# the benchmark starts prints UML_GEEKBENCH_ERROR:<reason> instead.

benchmark=/opt/geekbench/release/geekbench6
parameters=/etc/uml-geekbench.env

fail()
{
	echo "UML_GEEKBENCH_ERROR:$*"
	exit 1
}

workloads=
if [ -f "$parameters" ]; then
	while IFS= read -r line || [ -n "$line" ]; do
		case "$line" in
		''|'#'*) ;;
		UML_GEEKBENCH_WORKLOADS=*) workloads=${line#*=} ;;
		*) fail "unknown $parameters line: $line" ;;
		esac
	done < "$parameters"
	case "$workloads" in
	*[!0-9,]*|,*|*,|*,,*) fail "malformed workload list: $workloads" ;;
	esac
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
