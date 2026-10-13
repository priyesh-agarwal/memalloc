#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
void* malloc(size_t);
void free(void*);
void* calloc(size_t, size_t);
void* realloc(void*, size_t);
}

static void fill(void* p, size_t n, unsigned char tag) {
    auto* b = static_cast<unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) b[i] = static_cast<unsigned char>(tag + i);
}

static void check(void* p, size_t n, unsigned char tag) {
    auto* b = static_cast<unsigned char*>(p);
    for (size_t i = 0; i < n; ++i) {
        assert(b[i] == static_cast<unsigned char>(tag + i));
    }
}

// ---------------------------------------------------------------------------
// Multithreaded stress. Every thread allocates random sizes (mostly small, some slab
// sizes up to 512KB, a few large blocks), and randomly frees its own blocks, reallocs
// them, or hands them to another thread to free. Each live block holds a pattern unique
// to it, checked before every free and realloc, so overlapping blocks, a block handed
// out twice, or a lost write show up as an assert.

namespace stress {

constexpr int kThreads = 8;
constexpr int kOpsPerThread = 300'000;
constexpr size_t kMaxLive = 4096;
constexpr size_t kEdge = 256;  // pattern covers the first and last kEdge bytes

struct Blk {
    unsigned char* p;
    size_t n;
    uint32_t id;
};

struct Inbox {
    std::mutex mu;
    std::vector<Blk> items;
};

Inbox inbox[kThreads];
std::atomic<int> finished{0};

uint32_t next_rand(uint32_t& x) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

size_t pick_size(uint32_t& x) {
    uint32_t r = next_rand(x) % 1000;
    uint32_t v = next_rand(x);
    if (r < 700) return 1 + v % 256;
    if (r < 950) return 1 + v % 16384;
    if (r < 995) return 1 + v % (512 * 1024);
    return 512 * 1024 + 1 + v % (3 * 1024 * 1024);
}

unsigned char pat(uint32_t id, size_t i) {
    return static_cast<unsigned char>((id * 2654435761u + static_cast<uint32_t>(i) * 40503u) >> 24);
}

template <typename F>
void for_covered(size_t n, F f) {
    if (n <= 2 * kEdge) {
        for (size_t i = 0; i < n; ++i) f(i);
        return;
    }
    for (size_t i = 0; i < kEdge; ++i) f(i);
    for (size_t i = n - kEdge; i < n; ++i) f(i);
}

void fill_blk(const Blk& b) {
    for_covered(b.n, [&](size_t i) { b.p[i] = pat(b.id, i); });
}

void check_blk(const Blk& b) {
    for_covered(b.n, [&](size_t i) { assert(b.p[i] == pat(b.id, i)); });
}

// 16-byte alignment, except requests under 16 bytes, which get 8 (as in tcmalloc and
// jemalloc: no type with 16-byte alignment fits in them).
void check_aligned(void* p, size_t n) {
    assert(p);
    assert(reinterpret_cast<uintptr_t>(p) % (n < 16 ? 8 : 16) == 0);
}

Blk make_blk(size_t n, uint32_t id) {
    auto* p = static_cast<unsigned char*>(malloc(n));
    check_aligned(p, n);
    Blk b{p, n, id};
    fill_blk(b);
    return b;
}

void drain(int self) {
    std::vector<Blk> got;
    {
        std::lock_guard lock(inbox[self].mu);
        got.swap(inbox[self].items);
    }
    for (const Blk& b : got) {
        check_blk(b);
        free(b.p);
    }
}

void worker(int self) {
    uint32_t x = 0x9e3779b9u ^ static_cast<uint32_t>(self * 7919 + 1);
    uint32_t id = static_cast<uint32_t>(self) << 24;
    std::vector<Blk> live;
    live.reserve(kMaxLive);

    for (int op = 0; op < kOpsPerThread; ++op) {
        uint32_t r = next_rand(x) % 100;
        if (live.empty() || (r < 45 && live.size() < kMaxLive)) {
            live.push_back(make_blk(pick_size(x), ++id));
        } else {
            size_t k = next_rand(x) % live.size();
            Blk b = live[k];
            check_blk(b);
            if (r < 75) {
                free(b.p);
                live[k] = live.back();
                live.pop_back();
            } else if (r < 90) {
                int to = (self + 1 + static_cast<int>(next_rand(x) % (kThreads - 1))) % kThreads;
                std::lock_guard lock(inbox[to].mu);
                inbox[to].items.push_back(b);
                live[k] = live.back();
                live.pop_back();
            } else {
                size_t n = pick_size(x);
                auto* p = static_cast<unsigned char*>(realloc(b.p, n));
                check_aligned(p, n);
                size_t kept = std::min({b.n, n, kEdge});
                for (size_t i = 0; i < kept; ++i) assert(p[i] == pat(b.id, i));
                live[k] = Blk{p, n, ++id};
                fill_blk(live[k]);
            }
        }
        if ((op & 63) == 0) drain(self);
    }

    finished.fetch_add(1);
    while (finished.load() < kThreads) std::this_thread::yield();
    drain(self);
    for (const Blk& b : live) {
        check_blk(b);
        free(b.p);
    }
}

void run() {
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& t : threads) t.join();
}

}  // namespace stress

int main() {
    assert(malloc(0) != nullptr);
    free(nullptr);

    // Freelist LIFO on the 8-byte class.
    void* a = malloc(8);
    void* b = malloc(8);
    void* c = malloc(8);
    free(a);
    free(b);
    free(c);
    void* c2 = malloc(8);
    void* b2 = malloc(8);
    void* a2 = malloc(8);
    assert(c2 == c && b2 == b && a2 == a);
    free(a2);
    free(b2);
    free(c2);

    // calloc must clear a reused block.
    void* z = calloc(32, 1);
    assert(z);
    memset(z, 0xff, 32);
    free(z);
    z = calloc(32, 1);
    assert(z);
    auto* bytes = static_cast<unsigned char*>(z);
    for (int i = 0; i < 32; ++i) assert(bytes[i] == 0);
    free(z);
    volatile size_t huge = SIZE_MAX;
    assert(calloc(huge, 2) == nullptr);

    // realloc always copies into a new block.
    void* r = malloc(40);
    fill(r, 40, 7);
    void* r2 = realloc(r, 48);
    assert(r2 && r2 != r);
    check(r2, 40, 7);
    void* r3 = realloc(r2, 80);
    assert(r3 && r3 != r2);
    check(r3, 40, 7);
    free(r3);
    void* from_null = realloc(nullptr, 24);
    assert(from_null);
    free(from_null);
    void* gone = malloc(16);
    void* gone2 = realloc(gone, 0);
    assert(gone2);
    free(gone2);

    // 1MB is above the slab limit (512KB), so it comes from the large pool. A freed
    // block goes back on its size-class list and the next malloc of that class reuses it.
    constexpr size_t MB = 1024 * 1024;
    unsigned char* p0 = static_cast<unsigned char*>(malloc(MB));
    unsigned char* p1 = static_cast<unsigned char*>(malloc(MB));
    assert(p1 != p0);
    // Payload past the first word survives free and reuse.
    constexpr size_t kLink = sizeof(void*);
    fill(p0 + kLink, 16, 1);
    free(p0);
    unsigned char* back = static_cast<unsigned char*>(malloc(MB));
    assert(back == p0);
    check(back + kLink, 16, 1);
    free(p1);
    free(back);

    // 65536-byte class: 16 blocks per 1MB slab.
    constexpr size_t BM = 65536;
    constexpr int kPerSlab = 16;
    std::vector<unsigned char*> blocks;
    blocks.push_back(static_cast<unsigned char*>(malloc(BM)));
    for (int i = 1; i < kPerSlab; ++i) {
        auto* p = static_cast<unsigned char*>(malloc(BM));
        assert(p == blocks[0] + i * BM);
        blocks.push_back(p);
    }
    auto* next_slab = static_cast<unsigned char*>(malloc(BM));
    assert(next_slab != blocks[0] + kPerSlab * BM);
    for (auto* p : blocks) free(p);
    free(next_slab);

    // Overlapping lifetimes of a medium class.
    constexpr int N = 100;
    void* many[N];
    for (int i = 0; i < N; ++i) {
        many[i] = malloc(1000);
        fill(many[i], 100, static_cast<unsigned char>(i));
    }
    for (int i = 0; i < N; i += 2) free(many[i]);
    for (int i = 1; i < N; i += 2) check(many[i], 100, static_cast<unsigned char>(i));
    for (int i = 1; i < N; i += 2) free(many[i]);

    constexpr size_t BIG = 2 * MB;
    unsigned char* big = static_cast<unsigned char*>(malloc(BIG));
    assert(big && reinterpret_cast<uintptr_t>(big) % 16 == 0);
    fill(big, 64, 9);
    unsigned char* grown = static_cast<unsigned char*>(realloc(big, 3 * MB));
    assert(grown && grown != big);
    check(grown, 64, 9);
    free(grown);

    std::vector<int> v;
    for (int i = 0; i < 100000; ++i) v.push_back(i);
    for (int i = 0; i < 100000; ++i) assert(v[i] == i);

    std::string s;
    for (int i = 0; i < 100000; ++i) s.push_back(static_cast<char>('a' + i % 26));
    for (int i = 0; i < 100000; ++i) assert(s[i] == static_cast<char>('a' + i % 26));

    std::vector<std::string> words;
    for (int i = 0; i < 1000; ++i) words.push_back(s.substr(0, 100 + i % 50));
    for (int i = 0; i < 1000; ++i) assert(words[i].size() == static_cast<size_t>(100 + i % 50));

    // malloc/free of one block, the hot path, repeated.
    for (int i = 0; i < 10000; ++i) {
        void* p = malloc(24);
        *static_cast<int*>(p) = i;
        assert(*static_cast<int*>(p) == i);
        free(p);
    }

    stress::run();

    std::cout << "ok\n";
}
