#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
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

    // 1MB slabs hold 4 blocks. The 5th starts a new slab. Freeing a full slab
    // appends it at the tail, so malloc keeps bumping the new head.
    constexpr size_t MB = 1024 * 1024;
    unsigned char* p0 = static_cast<unsigned char*>(malloc(MB));
    unsigned char* p1 = static_cast<unsigned char*>(malloc(MB));
    unsigned char* p2 = static_cast<unsigned char*>(malloc(MB));
    unsigned char* p3 = static_cast<unsigned char*>(malloc(MB));
    assert(p1 == p0 + MB && p2 == p0 + 2 * MB && p3 == p0 + 3 * MB);
    fill(p0, 16, 1);
    unsigned char* q0 = static_cast<unsigned char*>(malloc(MB));
    assert(q0 != p0);
    free(p0);
    unsigned char* q1 = static_cast<unsigned char*>(malloc(MB));
    unsigned char* q2 = static_cast<unsigned char*>(malloc(MB));
    unsigned char* q3 = static_cast<unsigned char*>(malloc(MB));
    assert(q1 == q0 + MB && q2 == q0 + 2 * MB && q3 == q0 + 3 * MB);
    unsigned char* back = static_cast<unsigned char*>(malloc(MB));
    assert(back == p0);
    check(back, 16, 1);
    free(p1);
    free(p2);
    free(p3);
    free(q0);
    free(q1);
    free(q2);
    free(q3);
    free(back);

    // Bitmask class: 65536 bytes, 63 blocks per slab.
    constexpr size_t BM = 65536;
    std::vector<unsigned char*> blocks;
    blocks.push_back(static_cast<unsigned char*>(malloc(BM)));
    for (int i = 1; i < 63; ++i) {
        auto* p = static_cast<unsigned char*>(malloc(BM));
        assert(p == blocks[0] + i * BM);
        blocks.push_back(p);
    }
    auto* next_slab = static_cast<unsigned char*>(malloc(BM));
    assert(next_slab != blocks[0] + 63 * BM);
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

    std::cout << "ok\n";
}
