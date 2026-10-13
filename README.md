# memalloc

A thread-caching `malloc` for Linux, written in C++20. Drop it into any program with `LD_PRELOAD`, the same way as tcmalloc or jemalloc.

- A **4.3 ns malloc+free pair** on the fast path, faster than tcmalloc (4.6), jemalloc (5.2) and glibc (5.4).
- **Linear scaling** for threads that allocate independently: 2.1 billion malloc/free calls per second on 4 cores, 3× tcmalloc and 1.35× jemalloc.
- **Faster than tcmalloc on mixed sizes** across 4 threads (994 against 916 million calls/s), and 7–46× faster than all three when an allocation needs a fresh slab (36 ns against 266–1670 ns).
- **No locks and no atomic operations** when a thread frees its own memory. Freeing memory another thread allocated takes one compare-and-swap.
- **O(1) size-class lookup** using bit operations, with no tables to search, for both small and large blocks.
- Benchmarked against glibc, tcmalloc and jemalloc with Google Benchmark, using pinned cores, a locked CPU clock and real-time scheduling.

## Design

```
                       malloc(size)
                            |
             size <= 512 KiB?  -- no -->  large pool
                            | yes             672 size classes, 32 per power of two
                            v                 one freelist + mutex per class
              per-thread partial slab list    mmap on a miss, 16-byte header
              for this size class (57)
                            |
          64 MiB arena (64 MiB-aligned, mapped by the thread)
          +---------+---------+---------+-----+---------+
          | slab 0  | slab 1  | slab 2  | ... | slab 63 |   1 MiB each
          | headers | blocks  | blocks  |     | blocks  |
          +---------+---------+---------+-----+---------+
```

**Small blocks (up to 512 KiB)** come from 1 MiB slabs. Each slab holds blocks of one size class. Slabs are carved out of 64 MiB arenas that each thread maps for itself. Slab 0 of every arena holds the 64 slab headers, so finding the header for any pointer is just address arithmetic: mask the address to the 64 MiB boundary to get the arena, then shift to get the slab index.

- **Size classes:** 8 and 16 bytes, then every 16 bytes up to 128, then four classes per power of two up to 512 KiB, which is 57 classes in all. A request's class and its index are both computed from the request size with `countl_zero` and shifts.
- **malloc:** each thread keeps, per size class, a list of slabs that still have room. A new slab hands out blocks by bumping a pointer, and its pages are only touched when the program writes to them. Freed blocks go onto a per-slab LIFO freelist, so the most recently freed, still cache-hot block is reused first.
- **free from the owning thread:** pushes the block onto the slab's freelist. This uses no atomics and no locks.
- **free from another thread:** pushes the block onto the slab's lock-free remote freelist with one compare-and-swap. When that remote freelist goes from empty to non-empty, the freeing thread also announces the slab to its owner through a per-owner, per-class lock-free list. When the owner runs out of space in a size class, it takes all announced slabs with a single exchange and reuses their blocks.
- **Is a pointer ours?** One bit per 64 MiB of address space (32 768 words cover 128 TiB) answers "is this pointer inside a memalloc arena?" with one load.

**Large blocks (above 512 KiB)** are rounded up to one of 672 size classes, 32 per power of two, so rounding wastes at most about 3%. The class index is computed in O(1) from the most significant bit and the next five bits. Each class has a freelist guarded by its own mutex, held only for the pointer swap, never during `mmap`. A miss maps exactly the class size, so every block on a list fits any request for that class.

All allocator code lives in headers (`slab_alloc.h`, `large_alloc.h`) and compiles into one translation unit (`malloc.cpp`), so the whole fast path is inlined into `malloc` and `free`.

## Results

One benchmark binary, four allocators. glibc's ptmalloc is the process default; tcmalloc, jemalloc and memalloc are loaded with `LD_PRELOAD`.

- **Machine:** Intel Core Ultra 9 285H, one P-core locked at 2900 MHz with turbo off, `SCHED_FIFO` scheduling, 13 Oct 2026.
- **Software:** glibc 2.43, gperftools tcmalloc 2.18.1 and jemalloc 5.3.0, as packaged by Ubuntu. memalloc is built with the same compiler flags Ubuntu used for those packages (`-O2`, LTO, stack protector, control-flow protection; no `-O3`, no `-march=native`).

Every benchmark writes into each block right after `malloc`, as a constructor would, so page faults on fresh memory count no matter which side takes them. Full tables with p99, p99.9 and max are produced in `experiment/results/report.md`.

### Single-thread latency (median, ns, lower is better)

| Case | glibc | tcmalloc | jemalloc | memalloc |
| --- | ---: | ---: | ---: | ---: |
| malloc+free of one 24 B block, repeated | 5.4 | 4.6 | 5.2 | **4.3** |
| free of a 64 B block | 11.4 | 9.8 | 9.0 | **3.8** |
| first malloc of a new size (14 sizes, 96 B–500 KiB) | 110 | 516 | 409 | **73** |
| 256 KiB malloc that needs fresh memory | 1402 | 1670 | 266 | **36** |
| free of a 256 KiB block | 49 | 182 | 195 | **30** |
| malloc+free of a 1 MiB block, repeated | 14.1 | 47.7 | 294 | **12.7** |
| free of a fresh 1 MiB block | 2650 | 62 | 220 | **36** |

Tail latency is low as well. The first malloc of a new size has a p99 of 370 ns, against 5.2–6.4 µs for the other three.

### Multithreaded throughput (million calls/s, higher is better)

| Case | glibc | tcmalloc | jemalloc | memalloc |
| --- | ---: | ---: | ---: | ---: |
| 24 B malloc+free, 1 thread | 385 | 483 | 384 | **569** |
| 24 B malloc+free, 4 threads | 1543 | 687 | 1559 | **2097** |
| mixed sizes 16 B–4 KiB, 4 threads | 400 | 916 | 644 | **994** |

### C++ containers (ns per iteration, lower is better)

| Case | glibc | tcmalloc | jemalloc | memalloc |
| --- | ---: | ---: | ---: | ---: |
| push 64 strings of 40 chars, pop them | 2275 | **363** | 433 | 428 |
| `vector::reserve(16 MiB)`, write one byte | 48.5 | 61.3 | 4116 | **42.4** |
| build a 4 KiB `std::string` by appending | 623 | **503** | 560 | 564 |

## Performance findings

Most of the work on the fast path came from reading disassembly, not from changing the algorithm.

- **One translation unit is 2.7× faster on the hot path.** With the slab code in its own `.cpp` file, GCC did not inline it into `malloc` and `free` inside the shared library, and a malloc+free pair took 17.6 ns. Moving the code into headers included by `malloc.cpp` brought it to 6.5 ns.
- **A `thread_local` with an initializer costs 2 ns on every `free`.** The thread id was a `thread_local` set by a lambda, so every read went through a compiler-generated "TLS init" call, with seven registers saved and restored around it. Making it a `constinit` value that is assigned on the thread's first slow-path allocation removed the call and brought the pair from 6.5 to 4.3 ns. A thread that has not allocated yet can't own a slab, so its frees correctly take the cross-thread path.
- **glibc's mutex is a benchmark trap.** In a process that has never started a second thread, glibc's `std::mutex` skips its atomic instructions. An uncontended lock looked 2× cheaper than a spinlock in single-thread benchmarks, but became 1.7× more expensive (41 ns against 24.6 ns per large malloc+free) once any thread had been created.
- **Benchmarks must touch memory.** An allocator that returns untouched pages looks extremely fast in a loop that never writes to its blocks. Writing each block after `malloc` moved memalloc's 64-byte bump allocation from 3.5 ns to 26 ns, in line with the others. It also moved the cost of a "slow" 1.4 µs free (a page fault on the first write) into the program, where it belongs.

## Build and test

Requires Linux, CMake 3.20+ and a C++20 compiler.

```bash
cmake -S . -B build && cmake --build build
cmake -S experiment -B experiment/build && cmake --build experiment/build

cmake --build experiment/build --target test    # runs the tests under LD_PRELOAD
```

To use it with any program:

```bash
LD_PRELOAD=$PWD/build/libmemalloc.so your_program
```

The tests in `experiment/test_alloc.cpp` cover:
- freelist order, `calloc` clearing and overflow, and `realloc` copying;
- slab boundaries and large-block reuse;
- C++ containers;
- a multithreaded stress test. Eight threads allocate random sizes from 1 byte to 3.5 MiB, and randomly free, `realloc`, or hand blocks to another thread to free. Each block carries a unique pattern that is checked before it is freed, so overlapping blocks, a block handed out twice, or lost writes all fail an assert.

## Benchmarks

```bash
sudo /bin/sh experiment/compare.sh
```

The script runs `experiment/build/memalloc_bench` once per allocator, finding tcmalloc and jemalloc through `ldconfig`. You can override their paths with `TCMALLOC_LIB` and `JEMALLOC_LIB`. It writes the logs, JSON results and `experiment/results/report.md`.

`sudo` is only needed for consistent conditions. The script pins the benchmark to the highest-capacity core, which is a P-core on hybrid Intel CPUs, locks that core's clock to its base frequency with turbo off, and runs with `SCHED_FIFO`. Override the frequency with `FREQ_KHZ`. The clock lock lasts until reboot.

Each benchmark has a one-line description in a `// bench-desc:` comment in `bench.cpp`. `compare.py` reads these into the report, so adding a benchmark needs no change to the script.

## Scope

memalloc focuses on the allocation fast paths. It implements `malloc`, `free`, `calloc` and `realloc`.

- **Alignment:** blocks are 16-byte aligned, and requests of 8 bytes or less get 8-byte alignment, as in tcmalloc and jemalloc. The aligned-allocation functions (`posix_memalign`, `aligned_alloc`, `memalign`) are not implemented, so preload memalloc only into programs that don't use them.
- **Threads:** each thread gets a permanent id, and a process may create up to 256 threads.
- **Memory retention:** freed slabs and large blocks stay mapped for reuse instead of going back to the OS. This trades memory footprint for latency, which is why `vector::reserve(16 MiB)` takes 42 ns here and 4.1 µs in jemalloc, which gives the memory back.

## Next steps

Each of these comes from a benchmark where tcmalloc or jemalloc still lead, and each has a known fix:

- **Batch cross-thread frees.** A free from another thread currently costs one compare-and-swap on a cache line the owner also uses. Collecting such frees in a small thread-local buffer and handing them over in batches, as jemalloc does, would remove most of that traffic.
- **A per-thread cache for large blocks.** Today every thread shares one list and mutex per large size class. A cache of one or two blocks per thread in front of the shared lists would let threads that reuse large buffers skip the lock entirely.
- **Replace `operator new` and `operator delete`.** C++ code currently goes through libstdc++ to reach `malloc`. Overriding the operators directly, including sized `delete`, which tells the allocator the size class, would cut a call and a lookup from every C++ allocation.
- **Return idle memory and recycle thread ids.** `madvise` idle slabs back to the OS on a timer, reclaim a thread's slabs and id when it exits, and add `pthread_atfork` handlers for the large-pool mutexes.

## Layout

| File | Contents |
| --- | --- |
| `malloc.cpp` | `malloc`, `free`, `calloc`, `realloc`, and the free path |
| `slab_alloc.h` | arenas, slabs, size classes, thread-local lists, cross-thread frees |
| `large_alloc.h` | large size classes and pooled `mmap` blocks |
| `experiment/test_alloc.cpp` | tests, including the multithreaded stress test |
| `experiment/bench.cpp` | Google Benchmark cases |
| `experiment/compare.py` | runs all allocators and writes the report |

## License

MIT. See [LICENSE](LICENSE).
