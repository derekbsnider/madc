#!/bin/bash
# build_jobs.sh — THE job count for a parallel build (make -j, cmake -j) on
# this host: the cores, cut down to what the host's MEMORY can carry.
#
#   make -C src -j"$(bash scripts/build_jobs.sh)" release
#
# The build container runs inside the owner's WSL2 VM (16 GB): `-j20` /
# `-j$(nproc)` sized a build by cores alone, and two -O2 builds at once
# exhausted the VM and took the owner's whole WSL down (2026-10-11). The
# container now carries a 12 GiB cgroup cap, no swap — read below.
# A job is budgeted MADC_JOB_MEM_MB (default 1536: the -O2 compiles peak at
# parser.cpp 1.6 GB, cir_builder.cpp 0.9 GB, lexer.cpp 0.7 GB — measured
# 2026-10-11) out of MemAvailable less a MADC_JOB_RESERVE_MB reserve (default
# 2048) for everything else, and a
# cgroup memory.max, when one is set, caps that. Never below 1, never above
# the cores.
#
# MemAvailable is read when the build STARTS: run builds one at a time — a
# second build started beside a running one sees the memory the first has
# not claimed yet.
set -u
per_job=${MADC_JOB_MEM_MB:-1536}
reserve=${MADC_JOB_RESERVE_MB:-2048}
cores=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

avail_mb=""
if [ -r /proc/meminfo ]; then
	avail_mb=$(awk '/^MemAvailable:/ { print int($2 / 1024) }' /proc/meminfo)
fi
if [ -r /sys/fs/cgroup/memory.max ]; then
	cg=$(cat /sys/fs/cgroup/memory.max)
	if [ "$cg" != max ] && [ -n "$cg" ]; then
		cg_mb=$(( cg / 1048576 ))
		if [ -z "$avail_mb" ] || [ "$cg_mb" -lt "$avail_mb" ]; then
			avail_mb=$cg_mb
		fi
	fi
fi
# No memory reading (macOS, a locked-down sandbox): the cores, as before.
if [ -z "$avail_mb" ]; then
	echo "$cores"
	exit 0
fi
jobs=$(( (avail_mb - reserve) / per_job ))
[ "$jobs" -lt 1 ] && jobs=1
[ "$jobs" -gt "$cores" ] && jobs=$cores
echo "$jobs"
