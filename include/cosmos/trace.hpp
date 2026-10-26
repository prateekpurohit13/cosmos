#pragma once

#include "cosmos/faults.hpp"
#include "cosmos/time.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cosmos {

// One entry per post-gate decision, in the order the decisions happened (§11.2). Gate-blocked
// calls never reach the boundary and are counted instead, so the ledger stays fires-only.
struct TraceEntry {
    uint64_t seq;
    uint64_t eligible_index;
    Time at;
    SiteId site;
    Duration amount;
    FaultKind outcome;
    bool drew;
    DecisionStatus status = DecisionStatus::Fired;

    constexpr bool operator==(const TraceEntry&) const = default;
};

// 1M entries is ~48 MB, allocated once per universe, and only when a trace was asked for.
inline constexpr uint64_t kDefaultTraceCapacity = 1 << 20;

struct TraceOptions {
    bool enabled = false;
    uint64_t capacity = kDefaultTraceCapacity;
};

// Fixed capacity, never grows: decide() runs inside __wrap_malloc, so recording must not allocate
// (Rule 7). Overflow is counted rather than dropped silently, like FaultLedger's.
class FaultTrace {
  public:
    FaultTrace() = default;

    explicit FaultTrace(uint64_t capacity) : capacity_(capacity) {
        if (capacity_ > 0) entries_.reserve(static_cast<size_t>(capacity_));
    }

    bool enabled() const { return capacity_ > 0; }
    uint64_t capacity() const { return capacity_; }
    uint64_t dropped() const { return dropped_; }

    // Which universe wrote this, so a replay needs no seed argument. Zero until a Simulator names
    // it: a trace recorded straight from an injector belongs to no universe and replays as such.
    uint64_t universe_seed() const { return universe_seed_; }
    void set_universe_seed(uint64_t seed) { universe_seed_ = seed; }

    void record(const TraceEntry& entry) {
        if (capacity_ == 0) return;
        if (entries_.size() == capacity_) {
            ++dropped_;
            return;
        }
        entries_.push_back(entry);
    }

    size_t size() const { return entries_.size(); }
    const TraceEntry& operator[](size_t index) const { return entries_[index]; }
    const TraceEntry* begin() const { return entries_.data(); }
    const TraceEntry* end() const { return entries_.data() + entries_.size(); }

  private:
    std::vector<TraceEntry> entries_{};
    uint64_t capacity_ = 0;
    uint64_t dropped_ = 0;
    uint64_t universe_seed_ = 0;
};

// Which entries a replay honours (docs/fault-injection.md §11.2). One representation — the seqs to
// withhold, sorted — behind the three shapes a caller needs.
class TraceFilter {
  public:
    TraceFilter() = default;

    static TraceFilter honour_all() { return TraceFilter{}; }

    // Exactly these entries are withheld, whatever they are.
    static TraceFilter withhold(std::vector<uint64_t> seqs) {
        TraceFilter filter;
        filter.withheld_ = std::move(seqs);
        std::sort(filter.withheld_.begin(), filter.withheld_.end());
        filter.withheld_.erase(std::unique(filter.withheld_.begin(), filter.withheld_.end()),
                               filter.withheld_.end());
        return filter;
    }

    // Every fire and episode start except the kept ones: the `--only-faults=N` repro. Point
    // entries that injected nothing are never withheld — there is no fault in them to withhold.
    static TraceFilter only_faults(const FaultTrace& source, std::vector<uint64_t> kept_seqs) {
        std::sort(kept_seqs.begin(), kept_seqs.end());
        TraceFilter filter;
        for (const TraceEntry& entry : source) {
            if (entry.outcome == FaultKind::None && !is_event_site(entry.site)) continue;
            if (!std::binary_search(kept_seqs.begin(), kept_seqs.end(), entry.seq)) {
                filter.withheld_.push_back(entry.seq);
            }
        }
        return filter;
    }

    bool withholds(uint64_t seq) const {
        return std::binary_search(withheld_.begin(), withheld_.end(), seq);
    }

    size_t withheld() const { return withheld_.size(); }

  private:
    std::vector<uint64_t> withheld_{};
};

// ---- canonical encoding (§11.3) -------------------------------------------
//
// Fixed widths, declared entry order, little-endian, no padding: the bytes, not the struct.
// Header: source u8, count u64, universe_seed u64. Capacity and dropped are not canonical.
inline constexpr uint8_t kTraceSource = 1;
inline constexpr size_t kTraceHeaderBytes = 1 + 8 + 8;
inline constexpr size_t kTraceEntryBytes = 8 + 8 + 8 + 8 + 8 + 1 + 1 + 1;

enum class TraceError : uint8_t { Truncated, BadSource, BadValue, TrailingBytes };

constexpr const char* name_of(TraceError error) {
    switch (error) {
    case TraceError::Truncated:
        return "Truncated";
    case TraceError::BadSource:
        return "BadSource";
    case TraceError::BadValue:
        return "BadValue";
    case TraceError::TrailingBytes:
        return "TrailingBytes";
    }
    return "?";
}

// The encoding omits padding, so the struct can be wider than its bytes.
static_assert(kTraceEntryBytes == 43);
static_assert(sizeof(TraceEntry) == 48);
static_assert(sizeof(TraceEntry{}.seq) == 8);
static_assert(sizeof(TraceEntry{}.eligible_index) == 8);
static_assert(sizeof(TraceEntry{}.at) == 8);
static_assert(sizeof(TraceEntry{}.site) == 8);
static_assert(sizeof(TraceEntry{}.amount) == 8);
static_assert(sizeof(TraceEntry{}.outcome) == 1);
static_assert(sizeof(TraceEntry{}.drew) == 1);
static_assert(sizeof(TraceEntry{}.status) == 1);

namespace detail {

inline void append_u8(std::string& bytes, uint8_t value) {
    bytes.push_back(static_cast<char>(value));
}

inline void append_u64(std::string& bytes, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

inline void append_i64(std::string& bytes, int64_t value) {
    append_u64(bytes, static_cast<uint64_t>(value));
}

inline uint8_t read_u8(std::string_view bytes, size_t& offset) {
    return static_cast<uint8_t>(bytes[offset++]);
}

inline uint64_t read_u64(std::string_view bytes, size_t& offset) {
    uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<uint64_t>(static_cast<uint8_t>(bytes[offset++])) << shift;
    }
    return value;
}

inline int64_t read_i64(std::string_view bytes, size_t& offset) {
    return static_cast<int64_t>(read_u64(bytes, offset));
}

inline uint64_t fnv1a_64(std::string_view bytes) {
    uint64_t hash = 14695981039346656037ULL;
    for (const char raw : bytes) {
        hash ^= static_cast<uint8_t>(raw);
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace detail

inline std::string encode_trace(const FaultTrace& trace) {
    std::string bytes;
    bytes.reserve(kTraceHeaderBytes + kTraceEntryBytes * trace.size());
    detail::append_u8(bytes, kTraceSource);
    detail::append_u64(bytes, static_cast<uint64_t>(trace.size()));
    detail::append_u64(bytes, trace.universe_seed());
    for (const TraceEntry& entry : trace) {
        detail::append_u64(bytes, entry.seq);
        detail::append_u64(bytes, entry.eligible_index);
        detail::append_i64(bytes, entry.at.ns);
        detail::append_u64(bytes, static_cast<uint64_t>(entry.site));
        detail::append_i64(bytes, entry.amount.ns);
        detail::append_u8(bytes, static_cast<uint8_t>(entry.outcome));
        detail::append_u8(bytes, entry.drew ? 1 : 0);
        detail::append_u8(bytes, static_cast<uint8_t>(entry.status));
    }
    return bytes;
}

// A decoded trace is replayable: capacity is buffer size, not schedule, so count 0 gets one slot.
inline std::expected<FaultTrace, TraceError> decode_trace(std::string_view bytes) {
    if (bytes.size() < kTraceHeaderBytes) return std::unexpected(TraceError::Truncated);
    size_t offset = 0;
    if (detail::read_u8(bytes, offset) != kTraceSource) {
        return std::unexpected(TraceError::BadSource);
    }
    const uint64_t count = detail::read_u64(bytes, offset);
    const uint64_t seed = detail::read_u64(bytes, offset);
    const size_t remaining = bytes.size() - offset;
    if (count > remaining / kTraceEntryBytes) return std::unexpected(TraceError::Truncated);
    if (remaining != static_cast<size_t>(count) * kTraceEntryBytes) {
        return std::unexpected(TraceError::TrailingBytes);
    }

    FaultTrace trace(count == 0 ? 1 : count);
    trace.set_universe_seed(seed);
    for (uint64_t index = 0; index < count; ++index) {
        TraceEntry entry{};
        entry.seq = detail::read_u64(bytes, offset);
        entry.eligible_index = detail::read_u64(bytes, offset);
        entry.at = Time{detail::read_i64(bytes, offset)};
        const uint64_t site = detail::read_u64(bytes, offset);
        entry.amount = Duration{detail::read_i64(bytes, offset)};
        const uint8_t outcome = detail::read_u8(bytes, offset);
        const uint8_t drew = detail::read_u8(bytes, offset);
        const uint8_t status = detail::read_u8(bytes, offset);
        if (!is_known_site(static_cast<SiteId>(site))) return std::unexpected(TraceError::BadValue);
        if (outcome >= static_cast<uint8_t>(FaultKind::_Count)) {
            return std::unexpected(TraceError::BadValue);
        }
        if (drew > 1) return std::unexpected(TraceError::BadValue);
        if (status > static_cast<uint8_t>(DecisionStatus::Skipped)) {
            return std::unexpected(TraceError::BadValue);
        }
        entry.site = static_cast<SiteId>(site);
        entry.outcome = static_cast<FaultKind>(outcome);
        entry.drew = drew != 0;
        entry.status = static_cast<DecisionStatus>(status);
        trace.record(entry);
    }
    return trace;
}

inline uint64_t trace_hash(const FaultTrace& trace) {
    return detail::fnv1a_64(encode_trace(trace));
}

} // namespace cosmos
