#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <sys/mman.h>

// Single-threaded allocator.
// 256MB arenas, 64 slabs of 4MB. Slab objects sit in the 8KB before the arena.
// Size classes 8..1MB. Larger requests are easymalloc (one mmap, 16-byte header).
// A virgin slab takes one class and keeps it. free() never returns it to the virgin stack.
// malloc takes partially_filled's head. A full head is popped. A slab that was full
// and just gained a free block is appended at the tail.

namespace {

struct Block {
    Block* next;
};

class Slab {
    Slab* next{nullptr};
    Block* free_block{nullptr};
    
    // for slabs with less than 64-blocks, set bitmask to 111..10000..0 in advance
    uint64_t bitmask{0}; // bit(0) is free, bit(1) is occupied
    uint32_t lazy_next{0}; // block to use if free_block is nullptr
    uint32_t max_blocks; // slab is full if lazy_next == max_blocks, or bitmask is 111..1
    // when malloc(size) is called, size_class will be used to calculate block pointer
    // when free(ptr) is called, and bitmasks are used, you can calculate bit-number and update bitmask
    uint32_t size_class;
    uint32_t freelist_size{0};
    std::byte* base_ptr; // pointer to the first block

public:
    // 4MB slab
    static constexpr uint32_t SLAB_SIZE = 4 * 1024 * 1024;

    void setBase(std::byte* base) { base_ptr = base; }

    // Once, when the slab leaves the virgin stack. The class stays fixed.
    void setSizeClass(uint32_t sz_cls) {
        size_class = sz_cls;
        free_block = nullptr;
        max_blocks = SLAB_SIZE / sz_cls;
        if (max_blocks == 64) --max_blocks;
        bitmask = 0;
        if (max_blocks < 64) {
            // Low max_blocks bits are 0 (free). A shift of 64 is undefined.
            bitmask = ~0ULL << max_blocks;
        }
        lazy_next = 0;
        freelist_size = 0;
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

    // this is the head.
    void popHead(Slab*& head, Slab*& tail) {
        head = next;
        next = nullptr;
        if (!head) tail = nullptr;
    }

    // Never called on a full slab.
    Block* allocate() {
        if (max_blocks <= 64) {
            auto index = std::countr_zero(~bitmask);
            bitmask |= 1ULL << index;
            return reinterpret_cast<Block*>(base_ptr + index * size_class);
        }
        if (free_block) {
            --freelist_size;
            Block* ret = free_block;
            free_block = free_block->next;
            return ret;
        }
        return reinterpret_cast<Block*>(base_ptr + lazy_next++ * size_class);
    }

    bool isFull() const {
        return std::countr_zero(~bitmask) == 64
            || (free_block == nullptr && lazy_next == max_blocks);
    }

    uint32_t sizeClass() const { return size_class; }

    // True when the slab was full before this free.
    bool deallocate(Block* ptr) {
        if (max_blocks <= 64) {
            uint32_t index = static_cast<uint32_t>(
                (reinterpret_cast<std::byte*>(ptr) - base_ptr) / size_class);
            bool was_full = std::countr_zero(~bitmask) == 64;
            bitmask &= ~(1ULL << index);
            return was_full;
        }
        bool was_full = free_block == nullptr && lazy_next == max_blocks;
        ptr->next = free_block;
        free_block = ptr;
        ++freelist_size;
        return was_full;
    }
};

static_assert(sizeof(Slab) == 48);

// 64 * 48 = 3072, which fits in the 8KB in front of the slabs.
struct Arena {
    Slab slabs[64];
};

constexpr size_t kArenaMeta = 8192;
constexpr size_t kArenaData = size_t{1} << 28; // 256MB of slabs
constexpr size_t kDirtyHeader = 16;
constexpr size_t kMaxClass = 1024 * 1024;

static_assert(64 * sizeof(Slab) <= kArenaMeta);

// One bit per 256MB of the 47-bit user space. 2^19 bits = 64KB.
constexpr size_t kArenaSlots = size_t{1} << (47 - 28);
uint64_t inside_arena[kArenaSlots / 64]{};

bool inArena(uintptr_t slabs) {
    size_t bit = slabs >> 28;
    return (inside_arena[bit >> 6] >> (bit & 63)) & 1ull;
}

void markArena(uintptr_t slabs) {
    size_t bit = slabs >> 28;
    inside_arena[bit >> 6] |= 1ull << (bit & 63);
}

struct Partial {
    Slab* head;
    Slab* tail;
};

Partial partially_filled[64]{};

// Virgin slabs only. free() never pushes one back, so one mmap's 64 slabs fit.
Slab* empty_slabs[64];
int empty_count = 0;

// Exact class size -> index 0..60.
// 8..128 step by 16 in the >>4 domain. Above that, four prefixes per octave.
constexpr int getIndex(uint32_t size_cls) {
    if (size_cls <= 128) return size_cls >> 4;
    int k = 29 - std::countl_zero(size_cls);
    int top = size_cls >> k;
    return ((k - 5) << 2) + top + 4;
}

// Indexed by the request size, 0..64. Byte 64 starts the next cache line.
alignas(64) constexpr uint8_t kSmallClass[65] = {
    8,
    8, 8, 8, 8, 8, 8, 8, 8,
    16, 16, 16, 16, 16, 16, 16, 16,
    32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
    48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48,
    64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64,
};

// Request size -> class size. Larger than 1MB is easymalloc.
constexpr int getSizeClass(size_t size) {
    if (size > kMaxClass) return -1;
    if (size <= 64) return kSmallClass[size];
    int k = 29 - std::countl_zero(static_cast<uint32_t>(size));
    uint32_t q = 1u << k;
    return (size + q - 1) & ~(q - 1);
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
    return index == 61 && start == kMaxClass;
}

static_assert(classes_match());

// Page-aligned mmap. 16-byte header so the payload is 16-aligned.
// Stored length includes the header; free munmaps that many bytes.
void* easymalloc(size_t size) {
    if (size > SIZE_MAX - kDirtyHeader) return nullptr;
    size_t bytes = size + kDirtyHeader;
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    *static_cast<size_t*>(p) = bytes;
    return static_cast<std::byte*>(p) + kDirtyHeader;
}

// 512MB of slack plus the 8KB header is enough to land the slabs
// on a 256MB boundary with the header immediately in front.
Arena* getNewArena() {
    size_t map_len = (size_t{1} << 29) + kArenaMeta;
    void* mapped = mmap(nullptr, map_len, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) return nullptr;

    uintptr_t M = reinterpret_cast<uintptr_t>(mapped);
    uintptr_t B = (M + kArenaMeta + kArenaData - 1) & ~(kArenaData - 1);
    uintptr_t keep_lo = B - kArenaMeta;
    uintptr_t keep_hi = B + kArenaData;
    if (keep_lo > M) munmap(reinterpret_cast<void*>(M), keep_lo - M);
    if (keep_hi < M + map_len) munmap(reinterpret_cast<void*>(keep_hi), M + map_len - keep_hi);

    markArena(B);

    Arena* arena = reinterpret_cast<Arena*>(keep_lo);
    std::byte* data = reinterpret_cast<std::byte*>(B);
    for (int i = 0; i < 64; ++i) {
        arena->slabs[i].setBase(data + static_cast<size_t>(i) * Slab::SLAB_SIZE);
        empty_slabs[i] = &arena->slabs[i];
    }
    empty_count = 64;
    return arena;
}

Slab* slab_of(uintptr_t addr, uintptr_t slabs) {
    Arena* arena = reinterpret_cast<Arena*>(slabs - kArenaMeta);
    return &arena->slabs[(addr - slabs) >> 22];
}

void* do_malloc(size_t size) {
    int size_cls = getSizeClass(size);
    if (size_cls == -1) return easymalloc(size);

    Partial& list = partially_filled[getIndex(static_cast<uint32_t>(size_cls))];
    if (!list.head) [[unlikely]] {
        if (empty_count == 0 && !getNewArena()) return nullptr;
        Slab* fresh = empty_slabs[--empty_count];
        fresh->setSizeClass(static_cast<uint32_t>(size_cls));
        fresh->append(list.head, list.tail);
    }
    Block* ret = list.head->allocate();
    if (list.head->isFull()) list.head->popHead(list.head, list.tail);
    return ret;
}

void do_free(void* ptr) {
    if (!ptr) return;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t slabs = addr & ~(kArenaData - 1);
    if (!inArena(slabs)) {
        std::byte* target = reinterpret_cast<std::byte*>(ptr) - kDirtyHeader;
        size_t bytes = *reinterpret_cast<size_t*>(target);
        munmap(target, bytes);
        return;
    }

    Slab* slab = slab_of(addr, slabs);
    // A full slab is not on the list. One free block appends it at the tail
    // so the next malloc keeps using the old head.
    if (slab->deallocate(reinterpret_cast<Block*>(ptr))) {
        Partial& list = partially_filled[getIndex(slab->sizeClass())];
        slab->append(list.head, list.tail);
    }
}

void* do_calloc(size_t n, size_t size) {
    if (n != 0 && size > SIZE_MAX / n) return nullptr;
    size_t total = n * size;
    void* p = do_malloc(total);
    if (p && total) memset(p, 0, total);
    return p;
}

void* do_realloc(void* ptr, size_t size) {
    if (!ptr) return do_malloc(size);

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t slabs = addr & ~(kArenaData - 1);
    size_t old_usable;
    if (!inArena(slabs)) {
        size_t old_bytes = *reinterpret_cast<size_t*>(
            reinterpret_cast<std::byte*>(ptr) - kDirtyHeader);
        old_usable = old_bytes - kDirtyHeader;
    } else {
        old_usable = slab_of(addr, slabs)->sizeClass();
    }

    void* fresh = do_malloc(size);
    if (!fresh) return nullptr;
    if (size) memcpy(fresh, ptr, old_usable < size ? old_usable : size);
    do_free(ptr);
    return fresh;
}

} // namespace

extern "C" {

void* malloc(size_t size) { return do_malloc(size); }
void free(void* ptr) { do_free(ptr); }
void* calloc(size_t n, size_t size) { return do_calloc(n, size); }
void* realloc(void* ptr, size_t size) { return do_realloc(ptr, size); }

}
