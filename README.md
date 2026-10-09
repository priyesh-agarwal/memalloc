# memalloc

A single-threaded `malloc` for Linux. Load it the same way as tcmalloc or jemalloc: the program is built against libc, and `LD_PRELOAD` supplies this library.

Requests up to 1 MB come from 4 MB slabs inside 256 MB arenas. Larger requests are a direct `mmap`. `free` of those drops the pages with `MADV_DONTNEED` and leaves the mapping in place.

## Build

CMake 3.20+, a C++20 compiler, and Linux.

```bash
cmake -S . -B build
cmake --build build

cmake -S experiment -B experiment/build
```

`libmemalloc.so` is compiled with the same codegen flags Ubuntu used for the installed tcmalloc and jemalloc packages: `-O2 -g`, frame pointers, stack protector, control-flow protection, LTO, `-Wl,-Bsymbolic-functions`, and relro. No `-O3` and no `-march=native`.

The bench binary is built as `RelWithDebInfo`. It is not linked to an allocator.

## Run the comparison

```bash
sudo /bin/sh experiment/compare.sh
```

`sudo` drops `LD_PRELOAD`, so the script sets it after it starts. That is what gets `SCHED_FIFO`. The script finds `libtcmalloc` and `libjemalloc` through `ldconfig`, which covers Debian multiarch and Fedora `/usr/lib64`. Point `TCMALLOC_LIB` or `JEMALLOC_LIB` at a `.so` if they live somewhere else.

The same run locks the clock. The bench pins the highest-capacity core, and among those the highest index, so it stays off cpu 0. On a hybrid Intel CPU that core is a P-core. The script sets every core of that capacity to `performance` and to `base_frequency`, which is the non-turbo rate the package can hold. On the machine behind the table below, those cores are capacity 1024 and `base_frequency` is 2900 MHz. Intel hosts also get `intel_pstate/no_turbo` set. Hosts with no `cpu_capacity` file get every core locked the same way. The lock lasts until reboot.

`FREQ_KHZ=2900000 sudo /bin/sh experiment/compare.sh` overrides the frequency. Use a rate the chip holds with turbo off, not the turbo maximum.

Each run should print `sched FIFO` and the locked frequency. Per-allocator logs and the combined report are written to `experiment/results/`. The report is `experiment/results/report.md`.

## Allocator comparison

One bench binary, four allocators. glibc ptmalloc is the process default. tcmalloc, jemalloc, and memalloc are loaded with `LD_PRELOAD`. The median is from batch-of-128 `rdtsc` samples, except the short rows, which time every call. CPU 5 locked at 2900 MHz, `no_turbo` 1, TSC 0.271 ns/tick. 9 Oct 2026. All four runs were `SCHED_FIFO`.

memalloc and tcmalloc both sit at **4.3 ns** for the hot 24-byte malloc/free pair, p50.

Rows with 14, 16, or 50 samples have p99 equal to the max, because that is what nearest-rank does at that length.

### Fast path, p50

Latency per operation (ns). The hot row is a `malloc(24)` / `free` pair. The bump row is `malloc(64)` of a fresh object. The free row frees those 64-byte objects after one byte is touched.

| Case | glibc | tcmalloc | jemalloc | memalloc |
| --- | ---: | ---: | ---: | ---: |
| hot malloc+free 24B | 5.4 | 4.3 | 5.2 | 4.3 |
| bump malloc 64B | 41.0 | 37.2 | 9.2 | 2.7 |
| free 64B (not full) | 10.6 | 9.5 | 9.0 | 3.3 |

### First use of a size, p50

Latency per malloc or free (ns). "First slab" is one malloc of each of 14 sizes from 96 B to 700 KB. "1MB new slab" times the malloc that opens the next group of four 1 MB blocks.

| Case | glibc | tcmalloc | jemalloc | memalloc |
| --- | ---: | ---: | ---: | ---: |
| medium first slab | 91.1 | 324.4 | 1014.5 | 44.5 |
| medium 1MB new slab | 1570.1 | 6147.0 | 2005.8 | 32.0 |
| free full 1MB slab | 2957.9 | 49.9 | 162.8 | 35.8 |

### 2 MB malloc and free, p50

Latency per call (µs). Fifty calls, each timed alone. tcmalloc and jemalloc recycle these mappings. memalloc maps a fresh range, and `free` drops the pages with `MADV_DONTNEED`.

| Case | glibc | tcmalloc | jemalloc | memalloc |
| --- | ---: | ---: | ---: | ---: |
| malloc 2MB | 2.0 | 0.1 | 0.5 | 2.5 |
| free 2MB | 2.8 | 0.1 | 0.2 | 1.0 |

### All four percentiles

| Case | glibc | tcmalloc | jemalloc | memalloc | Unit |
| --- | ---: | ---: | ---: | ---: | --- |
| hot pair p50 | 5.4 | 4.3 | 5.2 | 4.3 | ns |
| hot pair p99 | 5.7 | 5.2 | 13.0 | 8.4 | ns |
| hot pair p99.9 | 19.3 | 17.4 | 21.2 | 19.3 | ns |
| hot pair max | 106.6 | 131.3 | 116.4 | 63.2 | ns |
| bump malloc 64B p50 | 41.0 | 37.2 | 9.2 | 2.7 | ns |
| bump malloc 64B p99 | 252.6 | 105.0 | 40.7 | 17.4 | ns |
| free 64B p50 | 10.6 | 9.5 | 9.0 | 3.3 | ns |
| free 64B p99 | 14.6 | 37.2 | 13.3 | 5.7 | ns |
| medium first slab p50 | 91.1 | 324.4 | 1014.5 | 44.5 | ns |
| medium first slab p99 | 7426.8 | 3544.4 | 11068.9 | 345.6 | ns |
| 1MB new slab p50 | 1570.1 | 6147.0 | 2005.8 | 32.0 | ns |
| 1MB new slab p99 | 3171.1 | 12187.6 | 8396.9 | 71.1 | ns |
| free full 1MB p50 | 2957.9 | 49.9 | 162.8 | 35.8 | ns |
| free full 1MB p99 | 8842.8 | 251.7 | 732.4 | 73.8 | ns |
| malloc 2MB p50 | 2.0 | 0.1 | 0.5 | 2.5 | µs |
| malloc 2MB p99 | 12.4 | 11.7 | 5.2 | 11.8 | µs |
| free 2MB p50 | 2.8 | 0.1 | 0.2 | 1.0 | µs |
| free 2MB p99 | 6.4 | 0.2 | 15.0 | 9.0 | µs |

Source: one run of `experiment/build/memalloc_bench` per allocator. glibc, tcmalloc 2.18.1-1, and jemalloc 5.3.0-4 are the installed libraries, not rebuilt.
