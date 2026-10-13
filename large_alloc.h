#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <mutex>
#include <sys/mman.h>

namespace memalloc {

constexpr size_t kLargeFloor = 512ULL * 1024; // slab path for size <= this
constexpr size_t kLargeMaxPayload = 1ULL << 40; // upper end of large size-class grid (compile-time)

struct Header {
    size_t size;
    Header* next;
};

static_assert(sizeof(Header) == 16);

constexpr int count_large_classes() {
    size_t start = kLargeFloor;
    int n = 0;
    while (start < kLargeMaxPayload) {
        ++n;
        start += std::bit_floor(start) >> 5;
    }
    return n;
}

constexpr int kNumLargeClasses = count_large_classes();
static_assert(kNumLargeClasses == 672);

constexpr size_t roundLargeClass(size_t size) {
    size_t q = size_t{1} << (58 - std::countl_zero(size));
    return (size + q - 1) & ~(q - 1);
}

constexpr int getLargeBlockIndex(size_t sc) {
    int msb = 63 - std::countl_zero(sc);
    int top = static_cast<int>(sc >> (msb - 5));
    return (msb - 19) * 32 + (top - 32);
}

constexpr bool large_classes_match() {
    size_t start = kLargeFloor;
    for (int idx = 0; idx < kNumLargeClasses; ++idx) {
        size_t next = start + (std::bit_floor(start) >> 5);
        if (roundLargeClass(start) != start) return false;
        if (getLargeBlockIndex(start) != idx) return false;
        if (roundLargeClass(start + 1) != next) return false;
        start = next;
    }
    return start == kLargeMaxPayload && getLargeBlockIndex(start) == kNumLargeClasses;
}

static_assert(large_classes_match());

struct LargeList {
    std::mutex mu;
    Header* head{nullptr};
};

inline std::array<LargeList, kNumLargeClasses + 1> large_blocks{};

inline Header* header_from_payload(void* ptr) {
    return reinterpret_cast<Header*>(static_cast<std::byte*>(ptr) - sizeof(Header));
}

inline void* payload_from_header(Header* h) {
    return reinterpret_cast<std::byte*>(h) + sizeof(Header);
}

inline void* large_mmap(size_t payload) {
    size_t bytes = payload + sizeof(Header);
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    auto* h = static_cast<Header*>(p);
    h->size = payload;
    h->next = nullptr;
    return payload_from_header(h);
}

inline void* large_malloc(size_t size) {
    const size_t sc = roundLargeClass(size);
    LargeList& list = large_blocks[getLargeBlockIndex(sc)];

    {
        std::lock_guard lock(list.mu);
        if (Header* h = list.head) {
            list.head = h->next;
            h->next = nullptr;
            return payload_from_header(h);
        }
    }

    return large_mmap(sc);
}

inline void large_free(void* ptr) {
    Header* header = header_from_payload(ptr);
    const int idx = getLargeBlockIndex(header->size);
    LargeList& list = large_blocks[idx];
    std::lock_guard lock(list.mu);
    header->next = list.head;
    list.head = header;
}

inline size_t large_payload_size(void* ptr) {
    return header_from_payload(ptr)->size;
}

} // namespace memalloc
