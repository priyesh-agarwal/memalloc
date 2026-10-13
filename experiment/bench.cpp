#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sched.h>
#include <string>
#include <sys/resource.h>
#include <unordered_map>
#include <vector>

// Run under LD_PRELOAD with the allocator being measured.
//
// Single-thread latency cases report p50/p99/p99.9/max as counters ("<row>.p50_ns").
// The one-shot cases (first slab, new slab, fresh mmap) cannot repeat inside a process,
// so they use Iterations(1) and time each call themselves.
// Multithread cases report items_per_second (malloc + free calls, all threads).
//
// memalloc never reuses thread ids and supports 256 threads per process. Every
// multithread run spawns threads-1 new threads, so iterations are fixed (no
// estimation runs) to keep the total far below that.

extern "C" {
void* malloc(size_t);
void free(void*);
}

using Clock = std::chrono::steady_clock;

static uint64_t ticks() {
    unsigned lo, hi;
    asm volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi) :: "memory");
    return (uint64_t)hi << 32 | lo;
}

static double ns_per_tick;

static int read_int_file(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (std::fscanf(f, "%d", &v) != 1) v = -1;
    std::fclose(f);
    return v;
}

// Highest-capacity cores, highest index first, so thread 0 stays off cpu0.
static std::vector<int> g_cores;

static void find_cores() {
    int best_cap = -1;
    for (int cpu = 0; cpu < 256; ++cpu) {
        char path[128];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        best_cap = std::max(best_cap, read_int_file(path));
    }
    for (int cpu = 255; cpu >= 0; --cpu) {
        char path[128];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        int cap = read_int_file(path);
        if (cap >= 0 && cap == best_cap) g_cores.push_back(cpu);
    }
    if (g_cores.empty()) g_cores.push_back(0);
}

static void pin_thread(int index) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(g_cores[static_cast<size_t>(index) % g_cores.size()], &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        std::perror("sched_setaffinity");
}

static void raise_priority() {
    // The session caps realtime CPU time at 200ms (ulimit -R). Past that the
    // kernel SIGKILLs a SCHED_FIFO process. sudo can raise the ceiling.
    rlimit unlimited{};
    unlimited.rlim_cur = RLIM_INFINITY;
    unlimited.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_RTTIME, &unlimited);

    // Threads created later inherit the policy.
    sched_param param{};
    param.sched_priority = 10;
    if (sched_setscheduler(0, SCHED_FIFO, &param) == 0)
        std::printf("sched FIFO\n");
    else if (setpriority(PRIO_PROCESS, 0, -20) == 0)
        std::printf("sched other, nice -20\n");
    else
        std::printf("sched unchanged (need cap_sys_nice or root for FIFO)\n");
}

static void warm_core() {
    volatile uint64_t sink = 0;
    auto t0 = Clock::now();
    while (Clock::now() - t0 < std::chrono::milliseconds(100)) sink += ticks();
}

static void print_cpu(int cpu) {
    char path[128];
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
    FILE* f = std::fopen(path, "r");
    char gov[64] = "unknown";
    if (f) {
        if (std::fscanf(f, "%63s", gov) != 1) gov[0] = '\0';
        std::fclose(f);
    }
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    int cur = read_int_file(path);
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_min_freq", cpu);
    int mn = read_int_file(path);
    std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", cpu);
    int mx = read_int_file(path);
    int turbo = read_int_file("/sys/devices/system/cpu/intel_pstate/no_turbo");

    std::printf("cpu %d  governor %s  freq %d MHz (min %d max %d)  no_turbo %d\n",
                cpu, gov, cur / 1000, mn / 1000, mx / 1000, turbo);
}

static void calibrate() {
    warm_core();
    auto t0 = Clock::now();
    uint64_t a = ticks();
    while (Clock::now() - t0 < std::chrono::milliseconds(100)) {}
    uint64_t b = ticks();
    double ns = std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    ns_per_tick = ns / static_cast<double>(b - a);
}

// Latency samples in rdtsc ticks. batch > 1 stores one averaged sample per batch.
class Samples {
public:
    Samples(size_t n_ops, int batch) : batch_(batch) { s_.reserve(n_ops / static_cast<size_t>(batch) + 2); }

    void start() {
        count_ = 0;
        t0_ = ticks();
    }

    void step() {
        if (++count_ < batch_) return;
        flush();
    }

    void add(uint64_t dt) { s_.push_back(dt); }

    // Counters "<row>.p50_ns" etc. An empty row gives "p50_ns".
    void report(benchmark::State& state, const std::string& row) {
        if (count_) flush();
        if (s_.empty()) return;
        std::sort(s_.begin(), s_.end());
        const std::string p = row.empty() ? "" : row + ".";
        state.counters[p + "p50_ns"] = at(0.50);
        state.counters[p + "p99_ns"] = at(0.99);
        state.counters[p + "p99.9_ns"] = at(0.999);
        state.counters[p + "max_ns"] = static_cast<double>(s_.back()) * ns_per_tick;
        state.counters[p + "samples"] = static_cast<double>(s_.size());
    }

private:
    void flush() {
        uint64_t t1 = ticks();
        uint64_t n = static_cast<uint64_t>(count_);
        s_.push_back((t1 - t0_ + n / 2) / n);
        t0_ = t1;
        count_ = 0;
    }

    double at(double p) const {
        int n = static_cast<int>(s_.size());
        int i = static_cast<int>(std::ceil(p * n)) - 1;
        i = std::clamp(i, 0, n - 1);
        return static_cast<double>(s_[static_cast<size_t>(i)]) * ns_per_tick;
    }

    std::vector<uint64_t> s_;
    uint64_t t0_ = 0;
    int batch_;
    int count_ = 0;
};

// Call construct() right after every malloc, as real code would run a constructor.
// Keep it outside the timed region when a case times single malloc calls.
//
// Every BENCHMARK(...)->Name("x") needs a description on the lines directly above it:
//   // bench-desc: what one iteration does
//   // bench-desc-<row>: per-row text, where <row> is the Samples::report() row ("malloc", "free")
// compare.py reads these comments from this file, so the binary never sees them. It
// appends args and thread counts itself, and picks the report table from the JSON:
// Samples counters go to the latency tables, threaded runs with SetItemsProcessed go to
// the throughput table, and everything else goes to the time-per-iteration table.

// Real code writes a block right after malloc (a constructor runs). Write the first
// 64 bytes, or the whole block if smaller: enough to fault in the first page and warm
// the first line, without turning large cases into a memset benchmark.
static void construct(void* p, size_t n) {
    auto* w = static_cast<uint64_t*>(p);
    const size_t words = std::min<size_t>(n, 64) / sizeof(uint64_t);
    for (size_t i = 0; i < words; ++i) w[i] = i;
}

// ---------------------------------------------------------------------------
// Single thread

static void BM_HotPair24(benchmark::State& state) {
    pin_thread(0);
    for (int i = 0; i < 10'000; ++i) free(malloc(24));
    Samples s(static_cast<size_t>(state.max_iterations), 64);
    s.start();
    for (auto _ : state) {
        void* p = malloc(24);
        construct(p, 24);
        benchmark::DoNotOptimize(p);
        free(p);
        s.step();
    }
    s.report(state, "");
    state.SetItemsProcessed(state.iterations() * 2);
}
// bench-desc: Allocate one 24-byte block, write it, free it, over and over. The best-case fast path: the same block is reused every time.
BENCHMARK(BM_HotPair24)->Name("hot_pair_24B")->Iterations(2'000'000);

static void BM_Bump64(benchmark::State& state) {
    pin_thread(0);
    constexpr int N = 50'000;
    static void* ptrs[N];
    void* warm = malloc(64);
    Samples sm(N, 64);
    Samples sf(N, 64);
    for (auto _ : state) {
        sm.start();
        for (int i = 0; i < N; ++i) {
            ptrs[i] = malloc(64);
            construct(ptrs[i], 64);
            sm.step();
        }
        sf.start();
        for (int i = 0; i < N; ++i) {
            free(ptrs[i]);
            sf.step();
        }
    }
    sm.report(state, "malloc");
    sf.report(state, "free");
    free(warm);
}
// bench-desc-malloc: Allocate 50,000 blocks of 64 bytes without freeing any, writing each one as a constructor would. The time includes that write, so page faults on fresh memory count no matter which side takes them.
// bench-desc-free: Free those 50,000 blocks in allocation order.
BENCHMARK(BM_Bump64)->Name("bump_64B")->Iterations(1);

static void BM_FirstSlab(benchmark::State& state) {
    pin_thread(0);
    free(malloc(8));
    constexpr size_t sizes[] = {
        96, 200, 500, 1000, 2000, 5000, 9000,
        20000, 40000, 80000, 100000, 200000, 400000, 500000,
    };
    constexpr int N = sizeof(sizes) / sizeof(sizes[0]);
    void* ptrs[N];
    Samples s(N, 1);
    for (auto _ : state) {
        for (int i = 0; i < N; ++i) {
            uint64_t t0 = ticks();
            ptrs[i] = malloc(sizes[i]);
            s.add(ticks() - t0);
            construct(ptrs[i], sizes[i]);
        }
    }
    s.report(state, "");
    for (int i = 0; i < N; ++i) free(ptrs[i]);
}
// bench-desc: The very first allocation at each of 14 sizes from 96 B to 500 KiB. Shows the one-time setup cost the allocator pays the first time it sees a size. Only the malloc call is timed.
BENCHMARK(BM_FirstSlab)->Name("first_slab_14_sizes")->Iterations(1);

static void BM_NewSlab256K(benchmark::State& state) {
    pin_thread(0);
    constexpr size_t SZ = 256 * 1024;
    constexpr int N = 16;
    void* block[N][4];
    Samples sm(N, 1);
    Samples sf(N, 1);
    for (auto _ : state) {
        for (int n = 0; n < N; ++n) {
            uint64_t t0 = ticks();
            block[n][0] = malloc(SZ);
            sm.add(ticks() - t0);
            construct(block[n][0], SZ);
            for (int i = 1; i < 4; ++i) {
                block[n][i] = malloc(SZ);
                construct(block[n][i], SZ);
            }
        }
        for (int n = 0; n < N; ++n) {
            uint64_t t0 = ticks();
            free(block[n][0]);
            sf.add(ticks() - t0);
        }
    }
    sm.report(state, "malloc");
    sf.report(state, "free");
    for (int n = 0; n < N; ++n)
        for (int i = 1; i < 4; ++i) free(block[n][i]);
}
// bench-desc-malloc: Allocate 64 blocks of 256 KiB and time every fourth malloc call, so the allocator keeps having to fetch memory it has never handed out before. Each block is written after its malloc, outside the timing.
// bench-desc-free: Free one block from each group of four while the other three stay allocated: returning memory to a region that is still mostly in use.
BENCHMARK(BM_NewSlab256K)->Name("new_slab_256KB")->Iterations(1);

static void BM_LargeFresh(benchmark::State& state) {
    pin_thread(0);
    const size_t bytes = static_cast<size_t>(state.range(0));
    constexpr int N = 50;
    void* ptrs[N];
    Samples sm(N, 1);
    Samples sf(N, 1);
    for (auto _ : state) {
        for (int i = 0; i < N; ++i) {
            uint64_t t0 = ticks();
            ptrs[i] = malloc(bytes);
            sm.add(ticks() - t0);
            construct(ptrs[i], bytes);
        }
        for (int i = 0; i < N; ++i) {
            uint64_t t0 = ticks();
            free(ptrs[i]);
            sf.add(ticks() - t0);
        }
    }
    sm.report(state, "malloc");
    sf.report(state, "free");
}
// bench-desc-malloc: Allocate 50 large blocks of the size shown with nothing to reuse, so each one is fresh memory, usually straight from the OS. Only the malloc call is timed; the start of each block is written afterwards.
// bench-desc-free: Free those 50 large blocks. The allocator may cache them for later or give them back to the OS.
BENCHMARK(BM_LargeFresh)->Name("large_fresh")->Arg(1 << 20)->Arg(2 << 20)->Iterations(1);

static void BM_LargeReuse(benchmark::State& state) {
    pin_thread(0);
    const size_t bytes = static_cast<size_t>(state.range(0));
    free(malloc(bytes));
    Samples s(static_cast<size_t>(state.max_iterations), 64);
    s.start();
    for (auto _ : state) {
        void* p = malloc(bytes);
        construct(p, bytes);
        benchmark::DoNotOptimize(p);
        free(p);
        s.step();
    }
    s.report(state, "");
    state.SetItemsProcessed(state.iterations() * 2);
}
// bench-desc: Allocate one large block of the size shown, write its first 64 bytes, free it, over and over. Measures how cheaply the allocator reuses a large block it just got back.
BENCHMARK(BM_LargeReuse)->Name("large_reuse")->Arg(1 << 20)->Arg(3 << 20)->Iterations(200'000);

// ---------------------------------------------------------------------------
// Multithread. Each thread is pinned to its own top-capacity core.

constexpr int kMaxThreads = 4;

// Each thread: malloc+free one 24-byte block. Measures thread-local scaling.
static void BM_MtHotPair(benchmark::State& state) {
    pin_thread(state.thread_index());
    for (int i = 0; i < 10'000; ++i) free(malloc(24));
    for (auto _ : state) {
        void* p = malloc(24);
        construct(p, 24);
        benchmark::DoNotOptimize(p);
        free(p);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
// bench-desc: hot_pair_24B on several threads at once, each with its own blocks. Threads share nothing, so an ideal allocator scales linearly.
BENCHMARK(BM_MtHotPair)->Name("mt_hot_pair_24B")
    ->ThreadRange(1, kMaxThreads)->Iterations(2'000'000)->UseRealTime();

static void BM_MtBatchMixed(benchmark::State& state) {
    pin_thread(state.thread_index());
    constexpr int K = 256;
    size_t sizes[K];
    uint32_t x = 12345u + static_cast<uint32_t>(state.thread_index());
    for (int i = 0; i < K; ++i) {
        x = x * 1664525u + 1013904223u;
        int shift = static_cast<int>((x >> 24) % 9);  // 16 B << 0..8
        sizes[i] = (size_t{16} << shift) + ((x >> 8) & 15);
    }
    void* ptrs[K];
    for (auto _ : state) {
        for (int i = 0; i < K; ++i) {
            ptrs[i] = malloc(sizes[i]);
            construct(ptrs[i], sizes[i]);
        }
        benchmark::DoNotOptimize(ptrs);
        for (int i = 0; i < K; ++i) free(ptrs[i]);
    }
    state.SetItemsProcessed(state.iterations() * K * 2);
}
// bench-desc: Each thread allocates 256 blocks of random sizes between 16 B and about 4 KiB, then frees them all. Closer to a real program than a single size.
BENCHMARK(BM_MtBatchMixed)->Name("mt_batch_mixed")
    ->ThreadRange(1, kMaxThreads)->Iterations(20'000)->UseRealTime();

struct alignas(64) Ring {
    static constexpr uint64_t kCap = 1024;
    alignas(64) std::atomic<uint64_t> head{0};
    alignas(64) std::atomic<uint64_t> tail{0};
    alignas(64) void* slot[kCap];
};
static Ring g_rings[kMaxThreads / 2];

static void spin_wait(int& spins) {
    __builtin_ia32_pause();
    // SCHED_FIFO never preempts a spinning thread; yield so a peer on a shared core can run.
    if (++spins >= 1024) {
        spins = 0;
        sched_yield();
    }
}

static void BM_MtRemoteFree(benchmark::State& state) {
    pin_thread(state.thread_index());
    Ring& ring = g_rings[state.thread_index() / 2];
    const bool producer = (state.thread_index() & 1) == 0;
    if (producer) {
        for (auto _ : state) {
            void* p = malloc(64);
            construct(p, 64);
            uint64_t t = ring.tail.load(std::memory_order_relaxed);
            int spins = 0;
            while (t - ring.head.load(std::memory_order_acquire) == Ring::kCap) spin_wait(spins);
            ring.slot[t & (Ring::kCap - 1)] = p;
            ring.tail.store(t + 1, std::memory_order_release);
        }
    } else {
        for (auto _ : state) {
            uint64_t h = ring.head.load(std::memory_order_relaxed);
            int spins = 0;
            while (ring.tail.load(std::memory_order_acquire) == h) spin_wait(spins);
            void* p = ring.slot[h & (Ring::kCap - 1)];
            ring.head.store(h + 1, std::memory_order_release);
            free(p);
        }
    }
    state.SetItemsProcessed(state.iterations());
}
// bench-desc: Threads work in pairs: one allocates 64-byte blocks and passes them through a queue, the other frees them. Measures freeing memory that another thread allocated, as in producer/consumer code.
BENCHMARK(BM_MtRemoteFree)->Name("mt_remote_free_64B")
    ->Threads(2)->Threads(4)->Iterations(1'000'000)->UseRealTime();

static void BM_MtLarge(benchmark::State& state) {
    pin_thread(state.thread_index());
    free(malloc(1 << 20));
    for (auto _ : state) {
        void* p = malloc(1 << 20);
        construct(p, 1 << 20);
        benchmark::DoNotOptimize(p);
        free(p);
    }
    state.SetItemsProcessed(state.iterations() * 2);
}
// bench-desc: Every thread allocates and frees 1 MiB blocks in a loop. Shows whether threads making large allocations end up waiting on each other.
BENCHMARK(BM_MtLarge)->Name("mt_large_1MB")
    ->ThreadRange(1, kMaxThreads)->Iterations(100'000)->UseRealTime();

// ---------------------------------------------------------------------------
// Container workloads. Reported as time per iteration; the name says what one iteration is.

static void BM_AppVectorGrow(benchmark::State& state) {
    pin_thread(0);
    for (auto _ : state) {
        std::vector<int> v;
        for (int i = 0; i < 1000; ++i) v.push_back(i);
        benchmark::DoNotOptimize(v.data());
    }
}
// bench-desc: Build a std::vector<int> with 1,000 push_back calls, starting empty, so it reallocates about 11 times as its capacity doubles.
BENCHMARK(BM_AppVectorGrow)->Name("app_vector_grow_1000_int")->Iterations(50'000);

static void BM_AppVectorReserve(benchmark::State& state) {
    pin_thread(0);
    for (auto _ : state) {
        std::vector<char> v;
        v.reserve(16 << 20);
        v.push_back(1);
        benchmark::DoNotOptimize(v.data());
    }
}
// bench-desc: Create a vector, reserve 16 MiB, write one byte, destroy it. The cost of a big allocation that is barely used.
BENCHMARK(BM_AppVectorReserve)->Name("app_vector_reserve_16MB")->Iterations(20'000);

static void BM_AppPushPopString(benchmark::State& state) {
    pin_thread(0);
    std::vector<std::string> v;
    v.reserve(64);
    for (auto _ : state) {
        for (int i = 0; i < 64; ++i) v.emplace_back(40, 'x');
        for (int i = 0; i < 64; ++i) v.pop_back();
    }
}
// bench-desc: Push 64 strings of 40 characters into a vector, then pop them all. Each string is too long for the small-string buffer, so each one allocates.
BENCHMARK(BM_AppPushPopString)->Name("app_push_pop_64_strings")->Iterations(50'000);

static void map_churn(benchmark::State& state) {
    std::unordered_map<uint64_t, uint64_t> m;
    m.reserve(16384);
    uint64_t next = 0;
    for (; next < 10'000; ++next) m.emplace(next, next);
    for (auto _ : state) {
        m.emplace(next, next);
        m.erase(next - 10'000);
        ++next;
    }
}

static void BM_AppMapChurn(benchmark::State& state) {
    pin_thread(0);
    map_churn(state);
}
// bench-desc: One insert and one erase on a std::unordered_map of about 10,000 entries. Each insert allocates a node and each erase frees one.
BENCHMARK(BM_AppMapChurn)->Name("app_unordered_map_insert_erase")->Iterations(2'000'000);

static void BM_AppStringAppend(benchmark::State& state) {
    pin_thread(0);
    for (auto _ : state) {
        std::string s;
        for (int i = 0; i < 512; ++i) s += "abcdefgh";
        benchmark::DoNotOptimize(s.data());
    }
}
// bench-desc: Build a 4 KiB std::string by appending 8 bytes 512 times, letting it reallocate as it grows.
BENCHMARK(BM_AppStringAppend)->Name("app_string_append_4KB")->Iterations(100'000);

static void BM_MtAppMapChurn(benchmark::State& state) {
    pin_thread(state.thread_index());
    map_churn(state);
    state.SetItemsProcessed(state.iterations() * 2);
}
// bench-desc: app_unordered_map_insert_erase on several threads at once, each with its own map.
BENCHMARK(BM_MtAppMapChurn)->Name("mt_app_unordered_map")
    ->ThreadRange(1, kMaxThreads)->Iterations(1'000'000)->UseRealTime();

int main(int argc, char** argv) {
    find_cores();
    pin_thread(0);
    raise_priority();
    calibrate();
    print_cpu(g_cores[0]);
    std::printf("tsc %.3f ns/tick\n", ns_per_tick);
    std::fflush(stdout);

    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
}
