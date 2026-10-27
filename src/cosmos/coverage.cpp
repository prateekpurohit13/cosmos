// Coverage state and sanitizer hooks. This translation unit is compiled without coverage
// instrumentation (see src/cosmos/CMakeLists.txt) so the hooks cannot recurse into themselves.

#include "cosmos/coverage.hpp"

#include <atomic>
#include <cstring>

namespace cosmos {
namespace {

constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

struct CoverageState {
    // Instrumented code can run on a host thread that is not the cooperative scheduler (a library
    // callback, for example). The map is therefore updated with relaxed atomics; ordering is
    // irrelevant because a universe reads it only after its own work has stopped.
    std::array<std::atomic<uint8_t>, kCoverageBitmapBytes> bits{};
    std::atomic<uint64_t> edges{0};
    std::atomic<bool> active{false};
};

// One process, one map. Campaign workers are forked processes; a branch child gets the parent's
// map at fork and resets it before running its own workload.
CoverageState g_coverage{};

// The anchor used by GCC's trace-pc form. Both the callback and every instrumented function live
// in the same non-PIE-independent image, so subtracting this removes the PIE load bias.
extern "C" char cosmos_coverage_anchor;

} // namespace

void detail::coverage_mark(uint64_t edge) {
    const uint64_t index = edge % kCoverageBitmapBits;
    const uint8_t mask = static_cast<uint8_t>(1u << (index & 7u));
    std::atomic<uint8_t>& byte = g_coverage.bits[static_cast<size_t>(index >> 3u)];
    uint8_t value = byte.load(std::memory_order_relaxed);
    while ((value & mask) == 0) {
        if (byte.compare_exchange_weak(value, static_cast<uint8_t>(value | mask),
                                       std::memory_order_relaxed, std::memory_order_relaxed)) {
            g_coverage.edges.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
}

void coverage_reset() {
    for (auto& byte : g_coverage.bits)
        byte.store(0, std::memory_order_relaxed);
    g_coverage.edges.store(0, std::memory_order_relaxed);
    g_coverage.active.store(true, std::memory_order_relaxed);
}

CoverageSnapshot coverage_snapshot() {
    CoverageSnapshot snapshot;
    snapshot.bits.reserve(g_coverage.bits.size());
    for (const auto& byte : g_coverage.bits) {
        snapshot.bits.push_back(byte.load(std::memory_order_relaxed));
    }
    snapshot.edges = g_coverage.edges.load(std::memory_order_relaxed);
    snapshot.hash = coverage_hash();
    g_coverage.active.store(false, std::memory_order_relaxed);
    return snapshot;
}

uint64_t coverage_edges() { return g_coverage.edges.load(std::memory_order_relaxed); }

bool coverage_active() { return g_coverage.active.load(std::memory_order_relaxed); }

uint64_t coverage_hash() {
    uint64_t hash = kFnvOffset;
    for (const auto& byte : g_coverage.bits) {
        hash ^= byte.load(std::memory_order_relaxed);
        hash *= kFnvPrime;
    }
    return hash;
}

uint64_t coverage_merge(CoverageSnapshot& into, const CoverageSnapshot& snapshot) {
    if (into.bits.size() != kCoverageBitmapBytes || snapshot.bits.size() != kCoverageBitmapBytes) {
        return 0;
    }
    uint64_t added = 0;
    for (size_t index = 0; index < kCoverageBitmapBytes; ++index) {
        const uint8_t before = into.bits[index];
        const uint8_t merged = static_cast<uint8_t>(before | snapshot.bits[index]);
        if (merged == before) continue;
        const uint8_t delta = static_cast<uint8_t>(merged & ~before);
        added += static_cast<uint64_t>(__builtin_popcount(static_cast<unsigned>(delta)));
        into.bits[index] = merged;
    }
    into.edges += added;
    return added;
}

uint64_t coverage_novelty(const CoverageSnapshot& snapshot, const CoverageSnapshot& union_bits) {
    if (snapshot.bits.size() != kCoverageBitmapBytes ||
        union_bits.bits.size() != kCoverageBitmapBytes) {
        return 0;
    }
    uint64_t novel = 0;
    for (size_t index = 0; index < kCoverageBitmapBytes; ++index) {
        const uint8_t delta = static_cast<uint8_t>(snapshot.bits[index] & ~union_bits.bits[index]);
        novel += static_cast<uint64_t>(__builtin_popcount(static_cast<unsigned>(delta)));
    }
    return novel;
}

} // namespace cosmos

extern "C" {

// Defined in libcosmos and used to normalize trace-pc return addresses. It must have external
// linkage so the callback can take its address.
char cosmos_coverage_anchor = 0;

#if defined(COSMOS_COVERAGE_GUARD)
// Clang's trace-pc-guard ABI. Guard ids are assigned once at process startup and remain stable for
// the life of the process, including across fork.
void __sanitizer_cov_trace_pc_guard_init(uint32_t* start, uint32_t* stop) {
    static uint32_t next_guard = 1;
    if (start == nullptr || stop == nullptr || start == stop) return;
    // A module can be initialized more than once in unusual link configurations; do not reset ids.
    for (uint32_t* guard = start; guard != stop; ++guard) {
        if (*guard == 0) *guard = next_guard++;
    }
}

void __sanitizer_cov_trace_pc_guard(uint32_t* guard) {
    if (guard == nullptr || *guard == 0) return;
    cosmos::detail::coverage_mark(static_cast<uint64_t>(*guard));
}
#elif defined(COSMOS_COVERAGE_PC)
// GCC's trace-pc ABI. Return addresses are normalized against the anchor and mixed so adjacent
// blocks do not collide in the fixed bitmap.
void __sanitizer_cov_trace_pc() {
    const auto pc = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const auto anchor = reinterpret_cast<uintptr_t>(&cosmos_coverage_anchor);
    const uint64_t offset = static_cast<uint64_t>(pc - anchor);
    uint64_t mixed = offset ^ (offset >> 17U) ^ (offset >> 31U);
    mixed *= 0x9e3779b97f4a7c15ULL;
    cosmos::detail::coverage_mark(mixed >> 7U);
}
#endif

} // extern "C"
