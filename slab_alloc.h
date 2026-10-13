#pragma once

#include "large_alloc.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <sys/mman.h>

namespace memalloc {

constexpr size_t kMaxThreads = 256;

// kMaxThreads means "not assigned yet". A thread gets its id on its first slab_malloc
// slow path, before it can own a slab, so free() from a thread without an id never
// matches a slab owner and correctly takes the remote path.
inline thread_local constinit size_t TID = kMaxThreads;

inline size_t assign_tid() {
    static std::atomic<size_t> next_index{0};
    size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
    if (idx >= kMaxThreads) [[unlikely]] {
        return 0UL;
    }
    return idx;
}

struct Block {
    Block* next;
};

class Slab {
public:
    Slab* next{nullptr};
    Block* free_block{nullptr};

    uint32_t lazy_next{0};
    uint32_t max_blocks;
    uint32_t size_class;
    std::byte* base_ptr;
    size_t owner_id;

    alignas(64) std::atomic<Block*> remote_free_block{nullptr};
    std::atomic<Slab*> remote_next{nullptr};

    static constexpr uint32_t SLAB_SIZE = 1 * 1024 * 1024;

    void setBase(std::byte* base) {
        base_ptr = base;
        owner_id = TID;
    }

    void setSizeClass(uint32_t sz_cls) {
        size_class = sz_cls;
        free_block = nullptr;
        max_blocks = SLAB_SIZE / sz_cls;
        lazy_next = 0;
    }

    void append(Slab*& head, Slab*& tail) {
        next = nullptr;
        if (!tail) {
            head = tail = this;
            return;
        }
        tail->next = this;
        tail = this;
    }

    void popHead(Slab*& head, Slab*& tail) {
        head = next;
        next = nullptr;
        if (!head) tail = nullptr;
    }

    Block* allocate() {
        if (free_block != nullptr) {
            Block* ret = free_block;
            free_block = free_block->next;
            return ret;
        }
        return reinterpret_cast<Block*>(base_ptr + lazy_next++ * size_class);
    }

    void pullRemoteToLocal() {
        next = remote_next.load(std::memory_order_relaxed);
        free_block = remote_free_block.exchange(nullptr, std::memory_order_acq_rel);
    }

    bool isLocalFull() const { return free_block == nullptr && lazy_next == max_blocks; }

    uint32_t sizeClass() const { return size_class; }

    bool deallocate(Block* ptr) {
        bool was_full = isLocalFull();
        ptr->next = free_block;
        free_block = ptr;
        return was_full;
    }

    bool remote_deallocate(Block* ptr) {
        Block* old_head = remote_free_block.load(std::memory_order_relaxed);
        do {
            ptr->next = old_head;
        } while (!remote_free_block.compare_exchange_weak(old_head, ptr, std::memory_order_acq_rel,
                                                          std::memory_order_relaxed));
        return old_head == nullptr;
    }
};

static_assert(sizeof(Slab) == 128);

struct Partial {
    Slab* head;
    Slab* tail;
};

constexpr size_t kArenaShift = 26;
constexpr size_t kArenaData = size_t{1} << kArenaShift;

struct Arena {
    Slab slabs[64];
};

inline std::array<std::array<std::atomic<Slab*>, 64>, kMaxThreads> remote_lists{};
inline thread_local Partial partially_filled[64]{};

constexpr size_t kMaxClass = kLargeFloor;
constexpr int kUserSlabFirst = 1;
constexpr int kUserSlabCount = 63;

static_assert(64 * sizeof(Slab) <= Slab::SLAB_SIZE);

constexpr size_t kArenaSlots = size_t{1} << (41 - kArenaShift);
inline std::atomic<uint64_t> inside_arena[kArenaSlots]{};

inline thread_local Slab* empty_slabs[kUserSlabCount]{};
inline thread_local int empty_count = 0;

alignas(64) inline constexpr uint8_t kSmallClass[65] = {
    8,
    8, 8, 8, 8, 8, 8, 8, 8,
    16, 16, 16, 16, 16, 16, 16, 16,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
    48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
};

constexpr int getSizeClass(size_t size) {
    if (size <= 64) return kSmallClass[size];
    int k = 29 - std::countl_zero(static_cast<uint32_t>(size));
    uint32_t q = 1u << k;
    return (size + q - 1) & ~(q - 1);
}

constexpr int getIndex(uint32_t size_cls) {
    if (size_cls <= 128) return size_cls >> 4;
    int k = 29 - std::countl_zero(size_cls);
    int top = size_cls >> k;
    return ((k - 5) << 2) + top + 4;
}

inline bool inArena(uintptr_t slabs) {
    size_t bit = slabs >> kArenaShift;
    auto value = inside_arena[bit >> 6].load(std::memory_order_acquire);
    return (value >> (bit & 63)) & 1ull;
}

inline void markArena(uintptr_t slabs) {
    size_t bit = slabs >> kArenaShift;
    inside_arena[bit >> 6].fetch_or(1ull << (bit & 63), std::memory_order_release);
}

inline Arena* getNewArena() {
    size_t map_len = kArenaData << 1;
    void* mapped = mmap(nullptr, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return nullptr;

    uintptr_t M = reinterpret_cast<uintptr_t>(mapped);
    uintptr_t B = (M + kArenaData - 1) & ~(kArenaData - 1);
    uintptr_t keep_hi = B + kArenaData;
    if (B > M) munmap(reinterpret_cast<void*>(M), B - M);
    if (keep_hi < M + map_len) munmap(reinterpret_cast<void*>(keep_hi), M + map_len - keep_hi);

    markArena(B);

    Arena* arena = reinterpret_cast<Arena*>(B);
    for (int i = kUserSlabFirst; i < kUserSlabFirst + kUserSlabCount; ++i) {
        arena->slabs[i].setBase(reinterpret_cast<std::byte*>(B) + static_cast<size_t>(i) * Slab::SLAB_SIZE);
        empty_slabs[i - kUserSlabFirst] = &arena->slabs[i];
    }
    empty_count = kUserSlabCount;
    return arena;
}

inline Slab* slab_of(uintptr_t addr, uintptr_t arena_base) {
    Arena* arena = reinterpret_cast<Arena*>(arena_base);
    return &arena->slabs[(addr - arena_base) >> 20];
}

inline void* slab_malloc(size_t size) {
    int size_cls = getSizeClass(size);
    auto sc_index = getIndex(static_cast<uint32_t>(size_cls));
    Partial& list = partially_filled[sc_index];
    if (!list.head) [[unlikely]] {
        if (TID == kMaxThreads) [[unlikely]] TID = assign_tid();
        Slab* remote_slabs = remote_lists[TID][sc_index].exchange(nullptr, std::memory_order_acq_rel);
        if (!remote_slabs) [[unlikely]] {
            if (empty_count == 0 && !getNewArena()) return nullptr;
            Slab* fresh = empty_slabs[--empty_count];
            fresh->setSizeClass(static_cast<uint32_t>(size_cls));
            fresh->append(list.head, list.tail);
        } else {
            Slab* cur = remote_slabs;
            Slab* prev = remote_slabs;
            while (cur) {
                cur->pullRemoteToLocal();
                prev = cur;
                cur = cur->next;
            }
            list.head = remote_slabs;
            list.tail = prev;
        }
    }
    Block* ret = list.head->allocate();
    if (list.head->isLocalFull()) list.head->popHead(list.head, list.tail);
    return ret;
}

inline size_t slab_payload_size(uintptr_t addr, uintptr_t slabs) {
    return slab_of(addr, slabs)->sizeClass();
}

constexpr bool classes_match() {
    uint32_t prev = 0;
    uint32_t start = 8;
    int index = 0;
    auto check = [&](uint32_t size) {
        if (getSizeClass(size) != static_cast<int>(size)) return false;
        if (getIndex(size) != index) return false;
        if (getSizeClass(prev) != static_cast<int>(size)) return false;
        uint32_t mid = prev + (size - prev) / 2;
        if (getSizeClass(mid) != static_cast<int>(size)) return false;
        prev = size + 1;
        ++index;
        return true;
    };
    if (!check(start)) return false;
    start += 8;
    if (!check(start)) return false;
    while (start < kMaxClass) {
        uint32_t factor = 16;
        if (start >= 128) factor = std::bit_floor(start) >> 2;
        start += factor;
        if (!check(start)) return false;
    }
    return index == 57 && start == kMaxClass;
}

static_assert(classes_match());

} // namespace memalloc
