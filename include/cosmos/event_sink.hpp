#pragma once

// Bounded, versioned event stream for inspection telemetry (docs/ui-design.md U3.1).
//
// This stream is deliberately separate from the canonical FaultTrace (D5): replay and trace_hash
// consume trace.hpp only. The event stream explains what a universe did across the scheduler,
// network, storage, memory, clock, randomness, application and operator domains. It never feeds
// replay, minimization or trace_hash, and recording an event never changes simulation semantics.
//
// U3.1 fixes the envelope, sequence, gap and wire contract. U3.2 is the first task that installs
// producers in the engine; this header does not instrument anything by itself.
//
// Recording must stay off the allocation path (Rule 7 and docs/ui-design.md §10.2). EventSink
// allocates its fixed buffer once in the constructor; record() only stamps and stores. The ring
// keeps the most recent events, drops the oldest when full, and exposes that loss as an explicit
// gap. It never grows and never pretends a truncated capture is complete.

#include "cosmos/faults.hpp"
#include "cosmos/time.hpp"

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cosmos {

// A flat kind list is grouped into lanes by domain. Both enumerations are append-only: a new kind
// takes the next value before `_Count`, and an existing value is never renumbered or reused.
enum class EventDomain : uint8_t {
    Scheduler,
    Network,
    Storage,
    Memory,
    Clock,
    Random,
    Application,
    Operator,
    _Count
};

inline constexpr size_t kEventDomainCount = static_cast<size_t>(EventDomain::_Count);

constexpr const char* name_of(EventDomain domain) {
    switch (domain) {
    case EventDomain::Scheduler:
        return "scheduler";
    case EventDomain::Network:
        return "network";
    case EventDomain::Storage:
        return "storage";
    case EventDomain::Memory:
        return "memory";
    case EventDomain::Clock:
        return "clock";
    case EventDomain::Random:
        return "random";
    case EventDomain::Application:
        return "application";
    case EventDomain::Operator:
        return "operator";
    case EventDomain::_Count:
        break;
    }
    return "?";
}

enum class EventKind : uint16_t {
    none = 0,

    scheduler_spawn = 1,
    scheduler_run,
    scheduler_block,
    scheduler_wake,
    scheduler_exit,
    scheduler_yield,
    scheduler_mutex_lock,
    scheduler_mutex_unlock,
    scheduler_mutex_contend,
    scheduler_cond_wait,
    scheduler_cond_signal,
    scheduler_cond_broadcast,
    scheduler_join,
    scheduler_detach,
    scheduler_timer_arm,
    scheduler_timer_fire,
    scheduler_timer_cancel,
    scheduler_deadlock,
    scheduler_choice,

    network_socket,
    network_bind,
    network_listen,
    network_connect,
    network_accept,
    network_close,
    network_send,
    network_recv,
    network_enqueue,
    network_deliver,
    network_drop,
    network_delay,
    network_reorder,
    network_corrupt,
    network_endpoint_state,
    network_partition,
    network_heal,

    storage_open,
    storage_read,
    storage_write,
    storage_fsync,
    storage_fdatasync,
    storage_pwrite,
    storage_writev,
    storage_ftruncate,
    storage_dirty,
    storage_durable,
    storage_torn,
    storage_rollback,
    storage_degraded,

    memory_alloc,
    memory_free,
    memory_realloc,
    memory_canary,
    memory_leak,

    clock_gettime,
    clock_gettimeofday,
    clock_sleep,
    clock_step,

    random_draw,

    app_always,
    app_sometimes,
    app_reachable,
    app_request,
    app_response,
    app_counter,
    app_gauge,
    app_span,
    app_annotation,

    operator_command,
    operator_breakpoint_hit,

    _Count
};

// Number of recordable kinds. `none` is the default-constructed value and is not recordable.
inline constexpr size_t kEventKindCount = static_cast<size_t>(EventKind::_Count) - 1;

struct EventKindInfo {
    EventKind kind;
    EventDomain domain;
    const char* token;
};

// The table is written out rather than derived so a kind that grew without a name fails a
// static_assert instead of becoming "?" forever. Tokens are the stable wire/diagnostic names.
inline constexpr std::array<EventKindInfo, kEventKindCount> kEventKinds{{
    {EventKind::scheduler_spawn, EventDomain::Scheduler, "scheduler.spawn"},
    {EventKind::scheduler_run, EventDomain::Scheduler, "scheduler.run"},
    {EventKind::scheduler_block, EventDomain::Scheduler, "scheduler.block"},
    {EventKind::scheduler_wake, EventDomain::Scheduler, "scheduler.wake"},
    {EventKind::scheduler_exit, EventDomain::Scheduler, "scheduler.exit"},
    {EventKind::scheduler_yield, EventDomain::Scheduler, "scheduler.yield"},
    {EventKind::scheduler_mutex_lock, EventDomain::Scheduler, "scheduler.mutex_lock"},
    {EventKind::scheduler_mutex_unlock, EventDomain::Scheduler, "scheduler.mutex_unlock"},
    {EventKind::scheduler_mutex_contend, EventDomain::Scheduler, "scheduler.mutex_contend"},
    {EventKind::scheduler_cond_wait, EventDomain::Scheduler, "scheduler.cond_wait"},
    {EventKind::scheduler_cond_signal, EventDomain::Scheduler, "scheduler.cond_signal"},
    {EventKind::scheduler_cond_broadcast, EventDomain::Scheduler, "scheduler.cond_broadcast"},
    {EventKind::scheduler_join, EventDomain::Scheduler, "scheduler.join"},
    {EventKind::scheduler_detach, EventDomain::Scheduler, "scheduler.detach"},
    {EventKind::scheduler_timer_arm, EventDomain::Scheduler, "scheduler.timer_arm"},
    {EventKind::scheduler_timer_fire, EventDomain::Scheduler, "scheduler.timer_fire"},
    {EventKind::scheduler_timer_cancel, EventDomain::Scheduler, "scheduler.timer_cancel"},
    {EventKind::scheduler_deadlock, EventDomain::Scheduler, "scheduler.deadlock"},
    {EventKind::scheduler_choice, EventDomain::Scheduler, "scheduler.choice"},

    {EventKind::network_socket, EventDomain::Network, "network.socket"},
    {EventKind::network_bind, EventDomain::Network, "network.bind"},
    {EventKind::network_listen, EventDomain::Network, "network.listen"},
    {EventKind::network_connect, EventDomain::Network, "network.connect"},
    {EventKind::network_accept, EventDomain::Network, "network.accept"},
    {EventKind::network_close, EventDomain::Network, "network.close"},
    {EventKind::network_send, EventDomain::Network, "network.send"},
    {EventKind::network_recv, EventDomain::Network, "network.recv"},
    {EventKind::network_enqueue, EventDomain::Network, "network.enqueue"},
    {EventKind::network_deliver, EventDomain::Network, "network.deliver"},
    {EventKind::network_drop, EventDomain::Network, "network.drop"},
    {EventKind::network_delay, EventDomain::Network, "network.delay"},
    {EventKind::network_reorder, EventDomain::Network, "network.reorder"},
    {EventKind::network_corrupt, EventDomain::Network, "network.corrupt"},
    {EventKind::network_endpoint_state, EventDomain::Network, "network.endpoint_state"},
    {EventKind::network_partition, EventDomain::Network, "network.partition"},
    {EventKind::network_heal, EventDomain::Network, "network.heal"},

    {EventKind::storage_open, EventDomain::Storage, "storage.open"},
    {EventKind::storage_read, EventDomain::Storage, "storage.read"},
    {EventKind::storage_write, EventDomain::Storage, "storage.write"},
    {EventKind::storage_fsync, EventDomain::Storage, "storage.fsync"},
    {EventKind::storage_fdatasync, EventDomain::Storage, "storage.fdatasync"},
    {EventKind::storage_pwrite, EventDomain::Storage, "storage.pwrite"},
    {EventKind::storage_writev, EventDomain::Storage, "storage.writev"},
    {EventKind::storage_ftruncate, EventDomain::Storage, "storage.ftruncate"},
    {EventKind::storage_dirty, EventDomain::Storage, "storage.dirty"},
    {EventKind::storage_durable, EventDomain::Storage, "storage.durable"},
    {EventKind::storage_torn, EventDomain::Storage, "storage.torn"},
    {EventKind::storage_rollback, EventDomain::Storage, "storage.rollback"},
    {EventKind::storage_degraded, EventDomain::Storage, "storage.degraded"},

    {EventKind::memory_alloc, EventDomain::Memory, "memory.alloc"},
    {EventKind::memory_free, EventDomain::Memory, "memory.free"},
    {EventKind::memory_realloc, EventDomain::Memory, "memory.realloc"},
    {EventKind::memory_canary, EventDomain::Memory, "memory.canary"},
    {EventKind::memory_leak, EventDomain::Memory, "memory.leak"},

    {EventKind::clock_gettime, EventDomain::Clock, "clock.gettime"},
    {EventKind::clock_gettimeofday, EventDomain::Clock, "clock.gettimeofday"},
    {EventKind::clock_sleep, EventDomain::Clock, "clock.sleep"},
    {EventKind::clock_step, EventDomain::Clock, "clock.step"},

    {EventKind::random_draw, EventDomain::Random, "random.draw"},

    {EventKind::app_always, EventDomain::Application, "application.always"},
    {EventKind::app_sometimes, EventDomain::Application, "application.sometimes"},
    {EventKind::app_reachable, EventDomain::Application, "application.reachable"},
    {EventKind::app_request, EventDomain::Application, "application.request"},
    {EventKind::app_response, EventDomain::Application, "application.response"},
    {EventKind::app_counter, EventDomain::Application, "application.counter"},
    {EventKind::app_gauge, EventDomain::Application, "application.gauge"},
    {EventKind::app_span, EventDomain::Application, "application.span"},
    {EventKind::app_annotation, EventDomain::Application, "application.annotation"},

    {EventKind::operator_command, EventDomain::Operator, "operator.command"},
    {EventKind::operator_breakpoint_hit, EventDomain::Operator, "operator.breakpoint_hit"},
}};

constexpr bool event_kinds_are_consistent() {
    for (size_t index = 0; index < kEventKindCount; ++index) {
        if (static_cast<size_t>(kEventKinds[index].kind) != index + 1) return false;
    }
    return true;
}
static_assert(event_kinds_are_consistent(),
              "EventKind values must be contiguous and in kEventKinds order");

constexpr bool event_tokens_are_unique() {
    for (size_t left = 0; left < kEventKindCount; ++left) {
        for (size_t right = left + 1; right < kEventKindCount; ++right) {
            if (std::string_view(kEventKinds[left].token) == kEventKinds[right].token) {
                return false;
            }
        }
    }
    return true;
}
static_assert(event_tokens_are_unique(), "EventKind tokens must be unique");

// A token is the lane name plus a dot plus the kind, so a filter built from name_of(domain) works.
constexpr bool event_tokens_have_domain_prefix() {
    for (size_t index = 0; index < kEventKindCount; ++index) {
        const std::string_view token = kEventKinds[index].token;
        const std::string_view prefix = name_of(kEventKinds[index].domain);
        if (token.size() <= prefix.size()) return false;
        if (token.substr(0, prefix.size()) != prefix) return false;
        if (token[prefix.size()] != '.') return false;
    }
    return true;
}
static_assert(event_tokens_have_domain_prefix(), "EventKind tokens must be <domain>.<kind>");

constexpr const EventKindInfo* event_kind_info(EventKind kind) {
    const size_t value = static_cast<size_t>(kind);
    if (value == 0 || value > kEventKindCount) return nullptr;
    return &kEventKinds[value - 1];
}

constexpr bool is_known_event_kind(EventKind kind) { return event_kind_info(kind) != nullptr; }

constexpr EventDomain domain_of(EventKind kind) {
    const EventKindInfo* info = event_kind_info(kind);
    return info == nullptr ? EventDomain::_Count : info->domain;
}

constexpr const char* name_of(EventKind kind) {
    const EventKindInfo* info = event_kind_info(kind);
    return info == nullptr ? "?" : info->token;
}

// One envelope for every domain. `subject` is the primary object (task, fd, allocation, packet,
// endpoint, span); `related` is the secondary object (peer, parent, waiter, target); `value` is a
// size, count, state or outcome code; `extra` is a kind-specific second scalar (a length when
// `value` is an offset, a transferred count when `value` is requested, and so on); `amount_ns` is
// a duration, deadline or offset. The exact slot meaning is fixed by the kind and documented in
// docs/event-stream.md. The payload stays numeric in v1 so record() cannot allocate; application
// strings arrive with U3.5.
struct EventPayload {
    uint64_t subject = 0;
    uint64_t related = 0;
    uint64_t value = 0;
    uint64_t extra = 0;
    int64_t amount_ns = 0;

    constexpr bool operator==(const EventPayload&) const = default;
};

// The envelope every UI timeline event carries (§3.4). `universe` is the seed, not the universe
// index, and `(universe, seq)` is the event identity. `at` is virtual time; wall time is optional
// host metadata and is never an ordering key. A node uses kNoNode for "absent"; task and
// correlation reserve 0, so producers must number real task/correlation ids from 1.
inline constexpr NodeId kNoNode = static_cast<NodeId>(-1);
inline constexpr uint64_t kNoTask = 0;
inline constexpr uint64_t kNoCorrelation = 0;
// v1 assigns sequences 0 .. kMaxEventSequence. UINT64_MAX itself is reserved so dropped + count
// and first_seq + index can never overflow.
inline constexpr uint64_t kMaxEventSequence = std::numeric_limits<uint64_t>::max() - 1;

// Layout is intentionally not pinned: the wire format below is explicit, so struct padding is
// irrelevant and must not become an accidental compatibility gate.
struct Event {
    uint64_t universe = 0;
    uint64_t seq = 0;
    Time at{};
    int64_t wall_ns = 0;
    uint64_t task = kNoTask;
    uint64_t correlation_id = kNoCorrelation;
    EventPayload payload{};
    EventKind kind = EventKind::none;
    NodeId node = kNoNode;
    bool has_wall = false;

    constexpr bool operator==(const Event&) const = default;
};

static_assert(std::is_trivially_copyable_v<Event>,
              "Event must stay trivially copyable so recording cannot allocate");

// The gap before the first retained event. A consumer MUST render this instead of presenting the
// retained window as the whole universe. v1 capture starts at sequence zero, so count and
// first_seq are equal; both are carried so a consumer never has to assume that. The v1 decoder
// enforces the equality, because a capture that starts mid-run needs a new version.
struct EventGap {
    uint64_t universe = 0;
    uint64_t count = 0;
    uint64_t first_seq = 0;

    constexpr bool operator==(const EventGap&) const = default;
};

// Fixed-capacity ring. Construct it before the universe starts, then set the seed once. The buffer
// is allocated in the constructor, outside the wrapped allocation path; record() is a stamped
// assignment into that buffer. One writer only: a universe belongs to one OS thread, and this
// class does not lock. Copying is disabled because it would allocate a second buffer; move it or
// copy the decoded EventStream instead.
class EventSink {
  public:
    explicit EventSink(size_t capacity = 0) : entries_(capacity), capacity_(capacity) {}
    EventSink(size_t capacity, uint64_t universe_seed)
        : entries_(capacity), capacity_(capacity), universe_seed_(universe_seed), seeded_(true) {}

    EventSink(const EventSink&) = delete;
    EventSink& operator=(const EventSink&) = delete;

    EventSink(EventSink&& other) noexcept { move_from(other); }
    EventSink& operator=(EventSink&& other) noexcept {
        if (this != &other) move_from(other);
        return *this;
    }

    bool enabled() const { return capacity_ != 0; }
    bool seeded() const { return seeded_; }
    size_t capacity() const { return capacity_; }
    uint64_t universe_seed() const { return universe_seed_; }

    // The seed is part of every event, so changing it after recording would make the capture
    // internally inconsistent. Returns false after the first record; the caller must set it first.
    [[nodiscard]] bool set_universe_seed(uint64_t seed) {
        if (next_seq_ != 0) return false;
        universe_seed_ = seed;
        seeded_ = true;
        return true;
    }

    // Records one event and assigns its sequence and universe. Disabled sinks discard silently
    // (off means off); an enabled full ring overwrites its oldest event and counts the loss. An
    // invalid kind, a missing seed or sequence exhaustion is a producer contract violation: it is
    // rejected and counted in rejected(), not assigned a sequence or written into the ring.
    // record() asserts that the call was accepted; use try_record() when a producer can recover.
    void record(Event event) noexcept {
        if (capacity_ == 0) return;
        const bool accepted = try_record(std::move(event));
        assert(accepted && "EventSink::record rejected; use try_record() to handle it");
        (void)accepted;
    }

    // Non-asserting form. A disabled sink returns true (there is nothing to do); an enabled sink
    // returns false when the call was rejected and counted in rejected().
    [[nodiscard]] bool try_record(Event event) noexcept {
        if (capacity_ == 0) return true;
        if (!seeded_ || !is_known_event_kind(event.kind) || next_seq_ > kMaxEventSequence) {
            ++rejected_;
            return false;
        }
        if (!event.has_wall) event.wall_ns = 0;
        event.universe = universe_seed_;
        event.seq = next_seq_++;
        entries_[write_] = event;
        if (++write_ == capacity_) write_ = 0;
        if (size_ < capacity_) {
            ++size_;
        } else {
            // The slot just written held the oldest event; the next slot is now the oldest.
            head_ = write_;
        }
        return true;
    }

    size_t size() const { return size_; }
    // The number of accepted events. While the sequence space is not exhausted, this is also the
    // sequence the next accepted event will receive.
    uint64_t next_seq() const { return next_seq_; }
    // Number of events evicted from the front, never the number of events in the buffer.
    uint64_t dropped() const { return next_seq_ - static_cast<uint64_t>(size_); }
    // Calls rejected before recording (unknown kind, unseeded sink, sequence exhaustion).
    uint64_t rejected() const { return rejected_; }
    // Sequence of the oldest retained event, or next_seq() when the buffer is empty.
    uint64_t first_seq() const {
        if (size_ == 0) return next_seq_;
        return entries_[head_].seq;
    }

    // Null until the ring has evicted something. The gap covers [0, first_seq).
    std::optional<EventGap> gap() const {
        if (dropped() == 0) return std::nullopt;
        return EventGap{universe_seed_, dropped(), first_seq()};
    }

    // Logical order: 0 is the oldest retained event, size() - 1 the newest. An out-of-range index
    // returns a default event in release rather than dividing by a zero capacity.
    const Event& operator[](size_t index) const {
        assert(index < size_);
        if (index >= size_) return empty_event();
        return entries_[(head_ + index) % capacity_];
    }

  private:
    static const Event& empty_event() {
        static const Event empty{};
        return empty;
    }

    // A moved-from ring must be disabled, not a live-looking buffer with an empty vector.
    void move_from(EventSink& other) noexcept {
        entries_ = std::move(other.entries_);
        capacity_ = other.capacity_;
        head_ = other.head_;
        write_ = other.write_;
        size_ = other.size_;
        next_seq_ = other.next_seq_;
        rejected_ = other.rejected_;
        universe_seed_ = other.universe_seed_;
        seeded_ = other.seeded_;

        other.entries_.clear();
        other.capacity_ = 0;
        other.head_ = 0;
        other.write_ = 0;
        other.size_ = 0;
        other.next_seq_ = 0;
        other.rejected_ = 0;
        other.universe_seed_ = 0;
        other.seeded_ = false;
    }

    std::vector<Event> entries_{};
    size_t capacity_ = 0;
    size_t head_ = 0;  // index of the oldest retained event
    size_t write_ = 0; // index the next event will overwrite
    size_t size_ = 0;
    uint64_t next_seq_ = 0;
    uint64_t rejected_ = 0;
    uint64_t universe_seed_ = 0;
    bool seeded_ = false;
};

// A decoded capture. `dropped` is the number of events the ring evicted before the first retained
// entry; `rejected` is the number of calls refused before they received a sequence. `entries` are
// contiguous sequence numbers in ascending order. An empty stream with dropped == 0 is a complete
// capture of zero events, but by itself it does not prove a sink was attached: an artifact writer
// must record presence separately, as it does for the canonical trace.
struct EventStream {
    uint64_t universe = 0;
    uint64_t dropped = 0;
    uint64_t rejected = 0;
    std::vector<Event> entries{};

    uint64_t first_seq() const { return entries.empty() ? dropped : entries.front().seq; }
    uint64_t next_seq() const { return dropped + static_cast<uint64_t>(entries.size()); }

    std::optional<EventGap> gap() const {
        // Derive both fields from the retained entries, so a hand-built inconsistent stream cannot
        // render a gap whose count and first sequence disagree.
        const uint64_t first = first_seq();
        if (first == 0) return std::nullopt;
        return EventGap{universe, first, first};
    }

    bool operator==(const EventStream&) const = default;
};

// Version 1 wire format: little-endian, fixed widths, no padding. The source byte distinguishes
// this stream from the canonical trace (source 1; source 2 is unallocated). The header carries the
// universe, the explicit drop/reject counts and the retained count, so a reader never has to infer
// a gap from a missing seq.
inline constexpr uint8_t kEventStreamSource = 3;
inline constexpr uint8_t kEventStreamVersion = 1;
inline constexpr size_t kEventStreamHeaderBytes = 1 + 1 + 8 + 8 + 8 + 8;
inline constexpr size_t kEventEntryBytes = 8 + 8 + 1 + 8 + 2 + 4 + 8 + 8 + 8 + 8 + 8 + 8 + 8;
static_assert(kEventStreamHeaderBytes == 34);
static_assert(kEventEntryBytes == 87);

enum class EventStreamError : uint8_t { Truncated, BadSource, BadVersion, BadValue, TrailingBytes };

constexpr const char* name_of(EventStreamError error) {
    switch (error) {
    case EventStreamError::Truncated:
        return "Truncated";
    case EventStreamError::BadSource:
        return "BadSource";
    case EventStreamError::BadVersion:
        return "BadVersion";
    case EventStreamError::BadValue:
        return "BadValue";
    case EventStreamError::TrailingBytes:
        return "TrailingBytes";
    }
    return "?";
}

// These helpers are deliberately local to this codec. trace.hpp's helpers are private to the
// canonical encoder, and sharing them would make a UI-contract change touch D5's frozen bytes.
namespace event_detail {

inline void append_u8(std::string& bytes, uint8_t value) {
    bytes.push_back(static_cast<char>(value));
}

inline void append_u16(std::string& bytes, uint16_t value) {
    for (int shift = 0; shift < 16; shift += 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

inline void append_u32(std::string& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
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

inline uint16_t read_u16(std::string_view bytes, size_t& offset) {
    uint16_t value = 0;
    for (int shift = 0; shift < 16; shift += 8) {
        value |= static_cast<uint16_t>(static_cast<uint8_t>(bytes[offset++])) << shift;
    }
    return value;
}

inline uint32_t read_u32(std::string_view bytes, size_t& offset) {
    uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset++])) << shift;
    }
    return value;
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

inline void append_header(std::string& bytes, uint64_t universe, uint64_t dropped,
                          uint64_t rejected, uint64_t count) {
    append_u8(bytes, kEventStreamSource);
    append_u8(bytes, kEventStreamVersion);
    append_u64(bytes, universe);
    append_u64(bytes, dropped);
    append_u64(bytes, rejected);
    append_u64(bytes, count);
}

inline void append_event(std::string& bytes, const Event& event) {
    append_u64(bytes, event.seq);
    append_i64(bytes, event.at.ns);
    append_u8(bytes, event.has_wall ? 1 : 0);
    append_i64(bytes, event.wall_ns);
    append_u16(bytes, static_cast<uint16_t>(event.kind));
    append_u32(bytes, static_cast<uint32_t>(event.node));
    append_u64(bytes, event.task);
    append_u64(bytes, event.correlation_id);
    append_u64(bytes, event.payload.subject);
    append_u64(bytes, event.payload.related);
    append_u64(bytes, event.payload.value);
    append_u64(bytes, event.payload.extra);
    append_i64(bytes, event.payload.amount_ns);
}

} // namespace event_detail

// Encodes the retained window. Capacity is capture policy, not content, so it is not written.
// Inconsistency is a structured BadValue rather than an assert, because release builds produce the
// artifacts and must not emit a stream that their own decoder rejects.
inline std::expected<std::string, EventStreamError> encode_events(const EventSink& sink) {
    if (sink.enabled() && !sink.seeded()) return std::unexpected(EventStreamError::BadValue);
    if (sink.size() == 0 && sink.dropped() != 0) {
        return std::unexpected(EventStreamError::BadValue);
    }
    std::string bytes;
    bytes.reserve(kEventStreamHeaderBytes + kEventEntryBytes * sink.size());
    event_detail::append_header(bytes, sink.universe_seed(), sink.dropped(), sink.rejected(),
                                static_cast<uint64_t>(sink.size()));
    for (size_t index = 0; index < sink.size(); ++index) {
        const Event& event = sink[index];
        if (event.universe != sink.universe_seed()) {
            return std::unexpected(EventStreamError::BadValue);
        }
        if (event.seq != sink.first_seq() + index) {
            return std::unexpected(EventStreamError::BadValue);
        }
        if (!is_known_event_kind(event.kind)) {
            return std::unexpected(EventStreamError::BadValue);
        }
        if (!event.has_wall && event.wall_ns != 0) {
            return std::unexpected(EventStreamError::BadValue);
        }
        event_detail::append_event(bytes, event);
    }
    return bytes;
}

inline std::expected<std::string, EventStreamError> encode_events(const EventStream& stream) {
    if (stream.entries.empty() && stream.dropped != 0) {
        return std::unexpected(EventStreamError::BadValue);
    }
    if (stream.dropped > std::numeric_limits<uint64_t>::max() - stream.entries.size()) {
        return std::unexpected(EventStreamError::BadValue);
    }
    std::string bytes;
    bytes.reserve(kEventStreamHeaderBytes + kEventEntryBytes * stream.entries.size());
    event_detail::append_header(bytes, stream.universe, stream.dropped, stream.rejected,
                                static_cast<uint64_t>(stream.entries.size()));
    for (size_t index = 0; index < stream.entries.size(); ++index) {
        const Event& event = stream.entries[index];
        if (event.universe != stream.universe) {
            return std::unexpected(EventStreamError::BadValue);
        }
        if (event.seq != stream.dropped + index) {
            return std::unexpected(EventStreamError::BadValue);
        }
        if (!is_known_event_kind(event.kind)) {
            return std::unexpected(EventStreamError::BadValue);
        }
        if (!event.has_wall && event.wall_ns != 0) {
            return std::unexpected(EventStreamError::BadValue);
        }
        event_detail::append_event(bytes, event);
    }
    return bytes;
}

// Rejects a stream rather than guessing at a gap, a missing version or a non-contiguous sequence.
inline std::expected<EventStream, EventStreamError> decode_events(std::string_view bytes) {
    if (bytes.size() < kEventStreamHeaderBytes) return std::unexpected(EventStreamError::Truncated);

    size_t offset = 0;
    if (event_detail::read_u8(bytes, offset) != kEventStreamSource) {
        return std::unexpected(EventStreamError::BadSource);
    }
    if (event_detail::read_u8(bytes, offset) != kEventStreamVersion) {
        return std::unexpected(EventStreamError::BadVersion);
    }
    const uint64_t universe = event_detail::read_u64(bytes, offset);
    const uint64_t dropped = event_detail::read_u64(bytes, offset);
    const uint64_t rejected = event_detail::read_u64(bytes, offset);
    const uint64_t count = event_detail::read_u64(bytes, offset);

    const size_t remaining = bytes.size() - offset;
    if (count > remaining / kEventEntryBytes) {
        return std::unexpected(EventStreamError::Truncated);
    }
    if (remaining != static_cast<size_t>(count) * kEventEntryBytes) {
        return std::unexpected(EventStreamError::TrailingBytes);
    }
    // A non-empty ring never evicts its last entry, so zero entries with a non-zero gap cannot be
    // a v1 capture. Reject it instead of presenting a stream that lost everything.
    if (count == 0 && dropped != 0) return std::unexpected(EventStreamError::BadValue);
    if (dropped > std::numeric_limits<uint64_t>::max() - count) {
        return std::unexpected(EventStreamError::BadValue);
    }

    EventStream stream;
    stream.universe = universe;
    stream.dropped = dropped;
    stream.rejected = rejected;
    stream.entries.reserve(static_cast<size_t>(count));
    for (uint64_t index = 0; index < count; ++index) {
        Event event{};
        event.universe = universe;
        event.seq = event_detail::read_u64(bytes, offset);
        event.at = Time{event_detail::read_i64(bytes, offset)};
        const uint8_t has_wall = event_detail::read_u8(bytes, offset);
        event.wall_ns = event_detail::read_i64(bytes, offset);
        event.kind = static_cast<EventKind>(event_detail::read_u16(bytes, offset));
        event.node = static_cast<NodeId>(event_detail::read_u32(bytes, offset));
        event.task = event_detail::read_u64(bytes, offset);
        event.correlation_id = event_detail::read_u64(bytes, offset);
        event.payload.subject = event_detail::read_u64(bytes, offset);
        event.payload.related = event_detail::read_u64(bytes, offset);
        event.payload.value = event_detail::read_u64(bytes, offset);
        event.payload.extra = event_detail::read_u64(bytes, offset);
        event.payload.amount_ns = event_detail::read_i64(bytes, offset);

        if (event.seq != dropped + index) return std::unexpected(EventStreamError::BadValue);
        if (has_wall > 1) return std::unexpected(EventStreamError::BadValue);
        if (has_wall == 0 && event.wall_ns != 0) {
            return std::unexpected(EventStreamError::BadValue);
        }
        if (!is_known_event_kind(event.kind)) {
            return std::unexpected(EventStreamError::BadValue);
        }
        event.has_wall = has_wall != 0;
        stream.entries.push_back(event);
    }
    return stream;
}

} // namespace cosmos
