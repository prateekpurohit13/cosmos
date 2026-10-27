#pragma once

// Compiler-assisted control-flow coverage for level-2 exploration (docs/state-exploration.md §5).
//
// The instrumentation is deliberately small and allocation-free: sanitizer callbacks run on every
// instrumented edge, including calls made while a universe is current. The callback only sets a
// bit in a fixed bitmap and increments a counter. A universe resets the map before its workload
// starts and copies it after the workload ends; the copy is the only allocation.
//
// GCC does not implement -fsanitize-coverage=trace-pc-guard, so the build uses trace-pc there and
// normalizes the return address against a fixed anchor in this translation unit. Clang uses the
// guard form and its stable guard ids.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace cosmos {

// 512K edges. The map is a fixed 64 KiB per process; it is sized for the whole test binary, not
// for a single universe, because sanitizer guard ids are assigned once at process startup.
inline constexpr size_t kCoverageBitmapBytes = 1 << 16;
inline constexpr size_t kCoverageBitmapBits = kCoverageBitmapBytes * 8;

// A detached copy of the map. `bits` is a byte-per-bit bitmap, matching the callback's writes.
struct CoverageSnapshot {
    std::vector<uint8_t> bits{};
    uint64_t edges = 0;
    uint64_t hash = 0;

    bool empty() const { return edges == 0; }
};

// Clears the map before a universe's workload starts. This must not run while a universe is in a
// wrapper: clearing zeros 64 KiB, which is acceptable outside the hot path and never during a
// fault decision.
void coverage_reset();

// Copies the current map. The bitmap is fixed, so this allocates once per requested capture.
CoverageSnapshot coverage_snapshot();

// Number of distinct instrumented edges hit since the last reset.
uint64_t coverage_edges();

// True between coverage_reset() and coverage_snapshot(). An end-of-run snapshot ends the active
// session, so a later unrelated capture (for example a snapshot's metadata) does not claim the
// previous campaign's bitmap as its own.
bool coverage_active();

// FNV-1a 64 over the bitmap. The hash is stable for equal hit sets, independent of hit order.
uint64_t coverage_hash();

// Set union support for guided selection. `into` is the accumulated corpus bitmap.
uint64_t coverage_merge(CoverageSnapshot& into, const CoverageSnapshot& snapshot);

// New bits in `snapshot` that are not present in `union_bits`. Both maps have the same fixed size.
uint64_t coverage_novelty(const CoverageSnapshot& snapshot, const CoverageSnapshot& union_bits);

// Stable token used by artifacts and the UI.
inline constexpr const char* kCoverageMethodGuard = "trace-pc-guard";
inline constexpr const char* kCoverageMethodPc = "trace-pc";

constexpr std::string_view coverage_method() {
#if defined(COSMOS_COVERAGE_GUARD)
    return kCoverageMethodGuard;
#elif defined(COSMOS_COVERAGE_PC)
    return kCoverageMethodPc;
#else
    return "none";
#endif
}

namespace detail {

// Called from the sanitizer hook. Kept in the header so the callback can be a single inlined
// branch; the state lives in coverage.cpp.
void coverage_mark(uint64_t edge);

} // namespace detail

} // namespace cosmos
