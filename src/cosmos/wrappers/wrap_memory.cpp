#include "cosmos/cosmos.hpp"

#include "wrapper_fault.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {

// Releases a block whose owning heap died first. The registry entry is dropped only on success: a
// stale one would misroute a later passthrough allocation that reuses the address.
void free_orphaned_block(void* ptr) {
    auto* header = cosmos::header_for(ptr);
    if (header->magic == cosmos::COSMOS_CANARY_MAGIC) {
        header->magic = cosmos::COSMOS_FREED_MAGIC;
        __real_free(header);
        cosmos::detail::AllocRegistry::instance().unregister(ptr);
    }
}

// Never faulted: the block never belonged to the currently active universe.
void* reallocate_orphaned_block(void* ptr, size_t new_size, bool engine_call) {
    auto* header = cosmos::header_for(ptr);
    if (header->magic != cosmos::COSMOS_CANARY_MAGIC) {
        errno = ENOMEM;
        return nullptr;
    }

    const size_t old_size = header->requested_size;
    // Engine work must not pollute the sim heap (mirrors malloc/calloc passthrough):
    // a tracked orphan reallocated by the engine becomes an untracked host block.
    // engine_call is sampled before the wrapper's guard (which is held here), so it
    // must be passed in rather than re-reading in_wrapper_logic.
    void* new_ptr = (cosmos::Simulator::has_current() && !engine_call)
                        ? cosmos::Simulator::current()->heap().allocate(new_size)
                        : __real_malloc(new_size);
    if (!new_ptr) {
        errno = ENOMEM;
        return nullptr;
    }

    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    free_orphaned_block(ptr);
    return new_ptr;
}

bool decided_oom(cosmos::Simulator* sim, cosmos::SiteId site) {
    return cosmos::wrappers::decide_for(sim, cosmos::FaultClass::Memory, site) ==
           cosmos::FaultKind::OutOfMemory;
}

// One memory producer. The heap owns allocation ids; this only reports what the heap already
// decided, so recording can never change a free or realloc outcome.
void record_memory(cosmos::Simulator* sim, cosmos::EventKind kind, cosmos::EventPayload payload) {
    cosmos::EventSink* sink = sim->event_sink_or_null();
    if (sink == nullptr || !sink->seeded()) return;
    cosmos::Event event;
    event.at = sim->now();
    event.kind = kind;
    event.node = sim->current_node();
    event.task = sim->scheduler().current_task_id();
    event.payload = payload;
    sink->record(event);
}

void record_allocation(cosmos::Simulator* sim, void* ptr) {
    if (ptr == nullptr) return;
    const auto* header = cosmos::header_for(ptr);
    record_memory(sim, cosmos::EventKind::memory_alloc,
                  cosmos::EventPayload{
                      .subject = header->alloc_id,
                      .value = static_cast<uint64_t>(header->requested_size),
                      .extra = static_cast<uint64_t>(sim->heap().stats().active_allocations)});
}

void record_free(cosmos::Simulator* sim, uint64_t alloc_id, size_t size) {
    record_memory(sim, cosmos::EventKind::memory_free,
                  cosmos::EventPayload{
                      .subject = alloc_id,
                      .value = static_cast<uint64_t>(size),
                      .extra = static_cast<uint64_t>(sim->heap().stats().active_allocations)});
}

} // namespace

extern "C" {

void* __real_malloc(size_t size);
void __real_free(void* ptr);
void* __real_calloc(size_t nmemb, size_t size);
void* __real_realloc(void* ptr, size_t size);

void* __wrap_malloc(size_t size) {
    if (cosmos::wrappers::in_wrapper_logic || !cosmos::Simulator::has_current()) {
        return __real_malloc(size);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();
    // Decided before the heap is touched, so a fired OOM leaves TrackedHeap exactly as it was.
    if (cosmos::wrappers::memory_alloc_eligible(size) && decided_oom(sim, cosmos::SiteId::malloc)) {
        errno = ENOMEM;
        return nullptr;
    }

    void* ptr = sim->heap().allocate(size);
    record_allocation(sim, ptr);
    return ptr;
}

void __wrap_free(void* ptr) {
    if (!ptr) return;
    cosmos::wrappers::ReentrancyGuard guard;

    // Ownership-based, not context-based (Rule 7): an engine call that frees an application block
    // must still reach that block's heap. Handing a sim payload to __real_free would release the
    // address header_size past the block and corrupt the host allocator instead.
    auto ownership = cosmos::detail::AllocRegistry::instance().ownership_of(ptr);
    if (ownership.kind == cosmos::detail::OwnerKind::Owned) {
        const auto* header = cosmos::header_for(ptr);
        const uint64_t alloc_id = header->alloc_id;
        const size_t size = header->requested_size;
        if (ownership.owner->deallocate(ptr) && cosmos::Simulator::has_current()) {
            record_free(cosmos::Simulator::current(), alloc_id, size);
        }
        return;
    }
    if (ownership.kind == cosmos::detail::OwnerKind::Orphaned) {
        const auto* header = cosmos::header_for(ptr);
        const uint64_t alloc_id = header->alloc_id;
        const size_t size = header->requested_size;
        free_orphaned_block(ptr);
        if (cosmos::Simulator::has_current()) {
            record_free(cosmos::Simulator::current(), alloc_id, size);
        }
        return;
    }

    __real_free(ptr);
}

void* __wrap_calloc(size_t nmemb, size_t size) {
    if (cosmos::wrappers::in_wrapper_logic || !cosmos::Simulator::has_current()) {
        return __real_calloc(nmemb, size);
    }
    cosmos::wrappers::ReentrancyGuard guard;

    auto* sim = cosmos::Simulator::current();

    if (!cosmos::wrappers::memory_calloc_eligible(nmemb, size)) {
        errno = ENOMEM;
        return nullptr;
    }

    if (decided_oom(sim, cosmos::SiteId::calloc)) {
        errno = ENOMEM;
        return nullptr;
    }

    const size_t total_size = nmemb * size;
    void* ptr = sim->heap().allocate(total_size);
    if (ptr) {
        memset(ptr, 0, total_size);
        record_allocation(sim, ptr);
    }
    return ptr;
}

void* __wrap_realloc(void* ptr, size_t size) {
    // Rule 7: an engine call must not spend a Memory draw, but it must still route by ownership --
    // a tracked payload handed to __real_realloc would be reallocated from its header offset.
    const bool engine_call = cosmos::wrappers::in_wrapper_logic;
    cosmos::wrappers::ReentrancyGuard guard;

    auto ownership = cosmos::detail::AllocRegistry::instance().ownership_of(ptr);

    if (size == 0) {
        if (ownership.kind == cosmos::detail::OwnerKind::Owned) {
            const auto* header = cosmos::header_for(ptr);
            const uint64_t alloc_id = header->alloc_id;
            const size_t old_size = header->requested_size;
            if (ownership.owner->deallocate(ptr) && cosmos::Simulator::has_current()) {
                record_free(cosmos::Simulator::current(), alloc_id, old_size);
            }
        } else if (ownership.kind == cosmos::detail::OwnerKind::Orphaned) {
            const auto* header = cosmos::header_for(ptr);
            const uint64_t alloc_id = header->alloc_id;
            const size_t old_size = header->requested_size;
            free_orphaned_block(ptr);
            if (cosmos::Simulator::has_current()) {
                record_free(cosmos::Simulator::current(), alloc_id, old_size);
            }
        } else if (ptr) {
            __real_free(ptr);
        }
        return nullptr;
    }

    if (!ptr) {
        if (!cosmos::Simulator::has_current() || engine_call) {
            return __real_malloc(size);
        }
        auto* sim = cosmos::Simulator::current();
        if (cosmos::wrappers::memory_realloc_eligible(ptr, size, /*owned_by_sim=*/false) &&
            decided_oom(sim, cosmos::SiteId::realloc)) {
            errno = ENOMEM;
            return nullptr;
        }
        void* allocated = sim->heap().allocate(size);
        record_allocation(sim, allocated);
        return allocated;
    }

    if (ownership.kind == cosmos::detail::OwnerKind::Owned) {
        if (!engine_call && cosmos::Simulator::has_current()) {
            auto* sim = cosmos::Simulator::current();
            if (cosmos::wrappers::memory_realloc_eligible(ptr, size, sim->heap().owns(ptr)) &&
                decided_oom(sim, cosmos::SiteId::realloc)) {
                errno = ENOMEM;
                return nullptr;
            }
        }
        const auto* old_header = cosmos::header_for(ptr);
        const uint64_t old_id = old_header->alloc_id;
        const size_t old_size = old_header->requested_size;
        void* new_ptr = ownership.owner->reallocate(ptr, size);
        if (!new_ptr) {
            errno = ENOMEM;
            return nullptr;
        }
        if (cosmos::Simulator::has_current()) {
            const auto* new_header = cosmos::header_for(new_ptr);
            record_memory(cosmos::Simulator::current(), cosmos::EventKind::memory_realloc,
                          cosmos::EventPayload{.subject = new_header->alloc_id,
                                               .related = old_id,
                                               .value = static_cast<uint64_t>(size),
                                               .extra = static_cast<uint64_t>(old_size)});
        }
        return new_ptr;
    }

    if (ownership.kind == cosmos::detail::OwnerKind::Orphaned) {
        return reallocate_orphaned_block(ptr, size, engine_call);
    }

    return __real_realloc(ptr, size);
}

} // extern "C"
