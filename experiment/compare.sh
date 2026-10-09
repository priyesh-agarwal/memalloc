#!/bin/sh
# Same bench binary, four allocators. glibc is the process default (ptmalloc).
# The others are loaded the way tcmalloc and jemalloc are loaded: LD_PRELOAD.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
bench=$root/experiment/build/memalloc_bench
out=$root/experiment/results
mkdir -p "$out"

if [ ! -x "$bench" ]; then
    echo "missing $bench" >&2
    echo "configure and build experiment/ first" >&2
    exit 1
fi
if [ ! -f "$root/build/libmemalloc.so" ]; then
    echo "missing $root/build/libmemalloc.so" >&2
    echo "configure and build the allocator first" >&2
    exit 1
fi

run() {
    name=$1
    preload=$2
    echo "=== $name ==="
    if [ -n "$preload" ]; then
        env -u LD_PRELOAD LD_PRELOAD="$preload" "$bench"
    else
        env -u LD_PRELOAD "$bench"
    fi
}

run glibc "" | tee "$out/glibc.txt"
run tcmalloc /usr/lib/x86_64-linux-gnu/libtcmalloc.so.4 | tee "$out/tcmalloc.txt"
run jemalloc /usr/lib/x86_64-linux-gnu/libjemalloc.so.2 | tee "$out/jemalloc.txt"
run memalloc "$root/build/libmemalloc.so" | tee "$out/memalloc.txt"
