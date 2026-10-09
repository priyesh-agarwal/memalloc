#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <sched.h>
#include <sys/resource.h>

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

// Highest-capacity core, and among those the highest index, so this stays off cpu0.
static int pick_core() {
    int best = 0;
    int best_cap = -1;
    for (int cpu = 0; cpu < 256; ++cpu) {
        char path[128];
        std::snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
        int cap = read_int_file(path);
        if (cap < 0) continue;
        if (cap >= best_cap) {
            best_cap = cap;
            best = cpu;
        }
    }
    return best;
}

static void pin_and_raise(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) != 0)
        std::perror("sched_setaffinity");

    // The session caps realtime CPU time at 200ms (ulimit -R). Past that the
    // kernel SIGKILLs a SCHED_FIFO process. sudo can raise the ceiling.
    rlimit unlimited{};
    unlimited.rlim_cur = RLIM_INFINITY;
    unlimited.rlim_max = RLIM_INFINITY;
    setrlimit(RLIMIT_RTTIME, &unlimited);

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

static uint64_t samples[1 << 15];

// One sample per 64 finished operations. A short final batch is divided by its own length.
struct Lap {
    uint64_t t0;
    int begin;
    int n;
};

static void lap_start(Lap& lap) {
    lap.t0 = ticks();
    lap.begin = 0;
    lap.n = 0;
}

static void lap_at(Lap& lap, int i, int n_ops) {
    if ((i & 63) != 63 && i != n_ops - 1) return;
    uint64_t t1 = ticks();
    uint64_t count = static_cast<uint64_t>(i - lap.begin + 1);
    samples[lap.n++] = (t1 - lap.t0 + count / 2) / count;
    lap.t0 = t1;
    lap.begin = i + 1;
}

static int pct_index(int n, double p) {
    int i = static_cast<int>(std::ceil(p * n)) - 1;
    if (i < 0) return 0;
    if (i >= n) return n - 1;
    return i;
}

// unit 1 prints nanoseconds, 1000 prints microseconds.
static void report(const char* name, int n, double unit) {
    std::sort(samples, samples + n);
    auto val = [&](int i) { return samples[i] * ns_per_tick / unit; };
    std::printf("%-26s %7.1f %7.1f %7.1f %8.1f %s\n",
                name,
                val(pct_index(n, 0.50)),
                val(pct_index(n, 0.99)),
                val(pct_index(n, 0.999)),
                val(n - 1),
                unit == 1.0 ? "ns" : "us");
}

// malloc and free of one live block. The slab stays on partially_filled.
static void hot_pair() {
    constexpr int N = 2'000'000;
    for (int i = 0; i < 10'000; ++i) {
        void* p = malloc(24);
        free(p);
    }
    Lap lap;
    lap_start(lap);
    for (int i = 0; i < N; ++i) {
        void* p = malloc(24);
        free(p);
        lap_at(lap, i, N);
    }
    report("hot malloc+free 24B", lap.n, 1.0);
}

// First trip through a slab: bump allocate, no freelist.
static void bump_malloc() {
    constexpr int N = 50'000;
    static void* ptrs[N];
    void* warm = malloc(64);

    Lap lap;
    lap_start(lap);
    for (int i = 0; i < N; ++i) {
        ptrs[i] = malloc(64);
        lap_at(lap, i, N);
    }
    report("bump malloc 64B", lap.n, 1.0);
    for (int i = 0; i < N; ++i) static_cast<unsigned char*>(ptrs[i])[0] = 1;

    lap_start(lap);
    for (int i = 0; i < N; ++i) {
        free(ptrs[i]);
        lap_at(lap, i, N);
    }
    report("free 64B (not full)", lap.n, 1.0);
    free(warm);
}

// Each size is a class that has no slab yet. Arena already exists, so no mmap.
static void medium_new_slab() {
    void* warm = malloc(8);
    free(warm);

    constexpr size_t sizes[] = {
        96, 200, 500, 1000, 2000, 5000, 9000,
        20000, 40000, 80000, 100000, 200000, 400000, 700000,
    };
    constexpr int N = sizeof(sizes) / sizeof(sizes[0]);
    void* ptrs[N];

    for (int i = 0; i < N; ++i) {
        uint64_t t0 = ticks();
        ptrs[i] = malloc(sizes[i]);
        samples[i] = ticks() - t0;
    }
    report("medium first slab", N, 1.0);

    for (int i = 0; i < N; ++i) free(ptrs[i]);
}

// Four 1MB blocks fill a slab and pop it. The next malloc takes a virgin slab.
// Freeing one block of a full slab appends that slab at the tail.
static void full_slab() {
    constexpr size_t MB = 1024 * 1024;
    constexpr int N = 16;
    void* block[N][4];

    for (int n = 0; n < N; ++n) {
        uint64_t t0 = ticks();
        block[n][0] = malloc(MB);
        samples[n] = ticks() - t0;
        for (int i = 1; i < 4; ++i) block[n][i] = malloc(MB);
    }
    report("medium 1MB new slab", N, 1.0);

    for (int n = 0; n < N; ++n) {
        uint64_t t0 = ticks();
        free(block[n][0]);
        samples[n] = ticks() - t0;
    }
    report("free full 1MB slab", N, 1.0);

    for (int n = 0; n < N; ++n)
        for (int i = 1; i < 4; ++i) free(block[n][i]);
}

static void easy() {
    constexpr int N = 50;
    constexpr size_t bytes = 2 * 1024 * 1024;
    void* ptrs[N];
    for (int i = 0; i < N; ++i) {
        uint64_t t0 = ticks();
        ptrs[i] = malloc(bytes);
        samples[i] = ticks() - t0;
    }
    report("easymalloc 2MB", N, 1000.0);

    for (int i = 0; i < N; ++i) {
        uint64_t t0 = ticks();
        free(ptrs[i]);
        samples[i] = ticks() - t0;
    }
    report("free 2MB", N, 1000.0);
}

int main() {
    int cpu = pick_core();
    pin_and_raise(cpu);
    calibrate();
    print_cpu(cpu);
    std::printf("tsc %.3f ns/tick\n", ns_per_tick);
    std::printf("%-26s %7s %7s %7s %8s\n", "", "p50", "p99", "p99.9", "max");
    hot_pair();
    bump_malloc();
    medium_new_slab();
    full_slab();
    easy();
}
