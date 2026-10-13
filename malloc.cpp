#include "large_alloc.h"
#include "slab_alloc.h"

#include <cstring>

namespace memalloc {

void* do_malloc(size_t size) {
    if (size > kLargeFloor) return large_malloc(size);
    return slab_malloc(size);
}

void do_free(void* ptr) {
    if (!ptr) return;

    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t slabs = addr & ~(kArenaData - 1);

    if (!inArena(slabs)) {
        large_free(ptr);
        return;
    }

    Slab* slab = slab_of(addr, slabs);
    Block* block = reinterpret_cast<Block*>(ptr);
    auto sc_index = getIndex(slab->sizeClass());

    if (slab->owner_id == TID) {
        // local free case
        // adding to tail, malloc will use head
        if (slab->deallocate(block)) {
            Partial& list = partially_filled[sc_index];
            slab->append(list.head, list.tail);
        }
    }
    else {
        // remote free case
        if (slab->remote_deallocate(block)) {
            std::atomic<Slab*>& target_list_head = remote_lists[slab->owner_id][sc_index];

            Slab* old_slab_head = target_list_head.load(std::memory_order_relaxed);
            do {
                slab->remote_next.store(old_slab_head, std::memory_order_relaxed);

            } while (!target_list_head.compare_exchange_weak(
                        old_slab_head, slab,
                        std::memory_order_release,
                        std::memory_order_relaxed));
        }
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
        old_usable = large_payload_size(ptr);
    } else {
        old_usable = slab_payload_size(addr, slabs);
    }

    void* fresh = do_malloc(size);
    if (!fresh) return nullptr;
    if (size) memcpy(fresh, ptr, old_usable < size ? old_usable : size);
    do_free(ptr);
    return fresh;
}

} // namespace memalloc

extern "C" {

void* malloc(size_t size) { return memalloc::do_malloc(size); }
void free(void* ptr) { memalloc::do_free(ptr); }
void* calloc(size_t n, size_t size) { return memalloc::do_calloc(n, size); }
void* realloc(void* ptr, size_t size) { return memalloc::do_realloc(ptr, size); }

}
