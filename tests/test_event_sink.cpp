#include "cosmos/event_sink.hpp"
#include "cosmos/memory.hpp"
#include "cosmos/simulator.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

using cosmos::Event;
using cosmos::EventDomain;
using cosmos::EventKind;
using cosmos::EventPayload;
using cosmos::EventSink;
using cosmos::EventStream;
using cosmos::EventStreamError;

namespace {

constexpr uint64_t kSeed = 0x0123456789ABCDEFULL;
constexpr size_t kHeaderDropped = 10;
constexpr size_t kHeaderRejected = 18;
constexpr size_t kHeaderCount = 26;

void must(bool ok) { assert(ok); }

std::string encoded(const EventSink& sink) {
    auto result = cosmos::encode_events(sink);
    must(result.has_value());
    return *result;
}

std::string encoded(const EventStream& stream) {
    auto result = cosmos::encode_events(stream);
    must(result.has_value());
    return *result;
}

Event sample_event(EventKind kind, uint64_t value) {
    Event event;
    event.kind = kind;
    event.at = cosmos::Time{static_cast<int64_t>(value) * 1'000};
    event.payload.value = value;
    return event;
}

void put_u16(std::string& bytes, size_t offset, uint16_t value) {
    for (int shift = 0; shift < 16; shift += 8) {
        bytes[offset++] = static_cast<char>((value >> shift) & 0xFF);
    }
}

void put_u64(std::string& bytes, size_t offset, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes[offset++] = static_cast<char>((value >> shift) & 0xFF);
    }
}

void test_kind_table_is_complete_and_named() {
    // Pins the frozen U3.1 taxonomy: appending a kind is a deliberate contract change, not a
    // drive-by edit that silently rewrites the wire enum.
    static_assert(cosmos::kEventKindCount == 70);
    static_assert(cosmos::kEventDomainCount == 8);

    for (size_t slot = 0; slot < cosmos::kEventKindCount; ++slot) {
        const cosmos::EventKindInfo& info = cosmos::kEventKinds[slot];
        assert(static_cast<size_t>(info.kind) == slot + 1);
        assert(cosmos::is_known_event_kind(info.kind));
        assert(cosmos::domain_of(info.kind) == info.domain);
        assert(std::string_view(cosmos::name_of(info.kind)) == info.token);
        assert(std::string_view(cosmos::name_of(info.kind)) != "?");
        assert(std::string_view(cosmos::name_of(info.domain)) != "?");

        const std::string_view token = info.token;
        const std::string_view domain = cosmos::name_of(info.domain);
        assert(token.starts_with(domain));
        assert(token.size() > domain.size());
        assert(token[domain.size()] == '.');
    }

    assert(!cosmos::is_known_event_kind(EventKind::none));
    assert(!cosmos::is_known_event_kind(EventKind::_Count));
    assert(cosmos::domain_of(EventKind::none) == EventDomain::_Count);
    assert(std::string_view(cosmos::name_of(EventKind::none)) == "?");
    assert(std::string_view(cosmos::name_of(EventDomain::_Count)) == "?");
    assert(std::string_view(cosmos::name_of(EventKind::scheduler_spawn)) == "scheduler.spawn");
    assert(std::string_view(cosmos::name_of(EventKind::app_annotation)) ==
           "application.annotation");
    assert(std::string_view(cosmos::name_of(EventKind::operator_breakpoint_hit)) ==
           "operator.breakpoint_hit");

    std::cout << "[PASS] test_kind_table_is_complete_and_named" << std::endl;
}

void test_sink_stamps_universe_and_sequence() {
    EventSink sink(4);
    assert(!sink.seeded());
    must(sink.set_universe_seed(kSeed));
    assert(sink.seeded());
    assert(sink.universe_seed() == kSeed);
    assert(sink.capacity() == 4);
    assert(sink.enabled());
    assert(sink.size() == 0);
    assert(sink.next_seq() == 0);
    assert(sink.first_seq() == 0);
    assert(sink.dropped() == 0);
    assert(sink.rejected() == 0);
    assert(!sink.gap().has_value());

    sink.record(sample_event(EventKind::scheduler_spawn, 10));
    sink.record(sample_event(EventKind::network_send, 20));

    assert(sink.size() == 2);
    assert(sink.next_seq() == 2);
    assert(sink.first_seq() == 0);
    assert(sink.dropped() == 0);
    assert(sink.rejected() == 0);
    assert(!sink.gap().has_value());
    assert(sink[0].universe == kSeed);
    assert(sink[0].seq == 0);
    assert(sink[0].kind == EventKind::scheduler_spawn);
    assert(sink[0].payload.value == 10);
    assert(sink[1].seq == 1);
    assert(sink[1].universe == kSeed);

    // The seed is fixed once the capture has a sequence: changing it would relabel history.
    assert(!sink.set_universe_seed(0));
    assert(sink.universe_seed() == kSeed);

    std::cout << "[PASS] test_sink_stamps_universe_and_sequence" << std::endl;
}

void test_rejected_calls_do_not_poison_the_capture() {
    EventSink unseeded(4);
    assert(!unseeded.try_record(sample_event(EventKind::memory_alloc, 1)));
    assert(unseeded.size() == 0);
    assert(unseeded.next_seq() == 0);
    assert(unseeded.rejected() == 1);

    must(unseeded.set_universe_seed(kSeed));
    unseeded.record(sample_event(EventKind::memory_alloc, 2));
    assert(unseeded.size() == 1);
    assert(unseeded.next_seq() == 1);
    assert(unseeded.rejected() == 1);
    assert(unseeded[0].seq == 0);
    assert(unseeded[0].universe == kSeed);

    // A default-constructed Event has kind == none, which is not recordable. It must not enter the
    // ring and later make the whole artifact undecodable.
    EventSink sink(4, kSeed);
    assert(sink.seeded());
    assert(!sink.try_record(Event{}));
    assert(sink.size() == 0);
    assert(sink.next_seq() == 0);
    assert(sink.rejected() == 1);
    sink.record(sample_event(EventKind::app_counter, 3));
    assert(sink.size() == 1);
    assert(sink.rejected() == 1);

    const auto decoded = cosmos::decode_events(encoded(sink));
    must(decoded.has_value());
    assert(decoded->rejected == 1);
    assert(decoded->entries.size() == 1);

    // A hand-built stream with an unknown kind is refused rather than emitted.
    EventStream invalid;
    invalid.universe = kSeed;
    invalid.entries.push_back(sample_event(EventKind::none, 4));
    invalid.entries[0].universe = kSeed;
    const auto refused = cosmos::encode_events(invalid);
    must(!refused.has_value());
    assert(refused.error() == EventStreamError::BadValue);

    EventStream zero_gap;
    zero_gap.universe = kSeed;
    zero_gap.dropped = 5;
    const auto gap_refused = cosmos::encode_events(zero_gap);
    must(!gap_refused.has_value());
    assert(gap_refused.error() == EventStreamError::BadValue);

    EventStream contradictory_wall;
    contradictory_wall.universe = kSeed;
    Event wall_event = sample_event(EventKind::clock_gettime, 5);
    wall_event.universe = kSeed;
    wall_event.seq = 0;
    wall_event.wall_ns = 7;
    contradictory_wall.entries.push_back(wall_event);
    const auto wall_refused = cosmos::encode_events(contradictory_wall);
    must(!wall_refused.has_value());
    assert(wall_refused.error() == EventStreamError::BadValue);

    EventSink unseeded_enabled(4);
    assert(!unseeded_enabled.try_record(sample_event(EventKind::memory_alloc, 6)));
    assert(unseeded_enabled.rejected() == 1);
    const auto unseeded_refused = cosmos::encode_events(unseeded_enabled);
    must(!unseeded_refused.has_value());
    assert(unseeded_refused.error() == EventStreamError::BadValue);
    must(unseeded_enabled.set_universe_seed(kSeed));
    assert(cosmos::encode_events(unseeded_enabled).has_value());

    EventStream inconsistent_gap;
    inconsistent_gap.universe = kSeed;
    inconsistent_gap.dropped = 5;
    Event gap_event = sample_event(EventKind::app_gauge, 9);
    gap_event.universe = kSeed;
    gap_event.seq = 7;
    inconsistent_gap.entries.push_back(gap_event);
    const auto gap = inconsistent_gap.gap();
    must(gap.has_value());
    assert(gap->count == 7);
    assert(gap->first_seq == 7);

    std::cout << "[PASS] test_rejected_calls_do_not_poison_the_capture" << std::endl;
}

void test_encode_rejects_inconsistent_streams() {
    {
        EventStream overflow;
        overflow.universe = kSeed;
        overflow.dropped = std::numeric_limits<uint64_t>::max();
        Event event = sample_event(EventKind::scheduler_run, 0);
        event.universe = kSeed;
        event.seq = 0;
        overflow.entries.push_back(event);
        const auto result = cosmos::encode_events(overflow);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        EventStream wrong_universe;
        wrong_universe.universe = kSeed;
        Event event = sample_event(EventKind::scheduler_run, 0);
        event.universe = kSeed + 1;
        event.seq = 0;
        wrong_universe.entries.push_back(event);
        const auto result = cosmos::encode_events(wrong_universe);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        EventStream wrong_seq;
        wrong_seq.universe = kSeed;
        Event event = sample_event(EventKind::scheduler_run, 0);
        event.universe = kSeed;
        event.seq = 9;
        wrong_seq.entries.push_back(event);
        const auto result = cosmos::encode_events(wrong_seq);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }

    std::cout << "[PASS] test_encode_rejects_inconsistent_streams" << std::endl;
}

void test_move_leaves_the_source_disabled() {
    EventSink source(4, kSeed);
    source.record(sample_event(EventKind::scheduler_spawn, 7));
    assert(source.enabled());
    assert(source.size() == 1);

    EventSink moved(std::move(source));
    assert(!source.enabled());
    assert(!source.seeded());
    assert(source.size() == 0);
    assert(source.next_seq() == 0);
    assert(source.rejected() == 0);
    source.record(sample_event(EventKind::scheduler_run, 8));
    assert(source.size() == 0);

    assert(moved.enabled());
    assert(moved.seeded());
    assert(moved.size() == 1);
    assert(moved[0].payload.value == 7);

    EventSink assigned(2, kSeed);
    assigned = std::move(moved);
    assert(!moved.enabled());
    assert(moved.size() == 0);
    assert(assigned.size() == 1);
    assert(assigned[0].payload.value == 7);

    std::cout << "[PASS] test_move_leaves_the_source_disabled" << std::endl;
}

void test_disabled_sink_discards_without_a_sequence() {
    EventSink sink;
    assert(!sink.enabled());
    sink.record(sample_event(EventKind::memory_alloc, 1));
    sink.record(sample_event(EventKind::memory_free, 2));
    assert(sink.size() == 0);
    assert(sink.next_seq() == 0);
    assert(sink.first_seq() == 0);
    assert(sink.dropped() == 0);
    assert(sink.rejected() == 0);
    assert(!sink.gap().has_value());

    std::cout << "[PASS] test_disabled_sink_discards_without_a_sequence" << std::endl;
}

void test_ring_keeps_newest_and_exposes_the_gap() {
    EventSink sink(3, kSeed);
    for (uint64_t value = 0; value < 7; ++value) {
        sink.record(sample_event(EventKind::app_counter, value));
    }

    assert(sink.size() == 3);
    assert(sink.next_seq() == 7);
    assert(sink.first_seq() == 4);
    assert(sink.dropped() == 4);
    assert(sink.rejected() == 0);
    assert(sink[0].seq == 4);
    assert(sink[0].payload.value == 4);
    assert(sink[1].payload.value == 5);
    assert(sink[2].payload.value == 6);

    const auto gap = sink.gap();
    must(gap.has_value());
    assert(gap->universe == kSeed);
    assert(gap->count == 4);
    assert(gap->first_seq == 4);
    assert(gap->count == gap->first_seq);

    std::cout << "[PASS] test_ring_keeps_newest_and_exposes_the_gap" << std::endl;
}

EventSink populated_sink(size_t capacity = 8) {
    EventSink sink(capacity, kSeed);
    Event first;
    first.kind = EventKind::network_send;
    first.at = cosmos::Time{-12'345};
    first.wall_ns = 9'876'543'210;
    first.has_wall = true;
    first.node = 2;
    first.task = 0x1111;
    first.correlation_id = 0x2222;
    first.payload =
        EventPayload{.subject = 7, .related = 8, .value = 512, .extra = 64, .amount_ns = -99};
    sink.record(first);

    Event second;
    second.kind = EventKind::storage_write;
    second.at = cosmos::Time{0};
    second.has_wall = false;
    // A stale wall value must be normalized away when has_wall is false.
    second.wall_ns = 12345;
    second.node = cosmos::kNoNode;
    second.task = cosmos::kNoTask;
    second.correlation_id = cosmos::kNoCorrelation;
    second.payload = EventPayload{.subject = 3, .value = 4096, .extra = 7, .amount_ns = 17};
    sink.record(second);
    return sink;
}

void test_encode_decode_round_trips_every_field() {
    EventSink sink = populated_sink();
    const std::string bytes = encoded(sink);
    assert(bytes.size() == cosmos::kEventStreamHeaderBytes + 2 * cosmos::kEventEntryBytes);

    const auto decoded = cosmos::decode_events(bytes);
    must(decoded.has_value());
    const EventStream& stream = *decoded;
    assert(stream.universe == kSeed);
    assert(stream.dropped == 0);
    assert(stream.rejected == 0);
    assert(stream.entries.size() == 2);
    assert(stream.first_seq() == 0);
    assert(stream.next_seq() == 2);
    assert(!stream.gap().has_value());

    assert(stream.entries[0].universe == kSeed);
    assert(stream.entries[0].seq == 0);
    assert(stream.entries[0].at == cosmos::Time{-12'345});
    assert(stream.entries[0].wall_ns == 9'876'543'210);
    assert(stream.entries[0].has_wall);
    assert(stream.entries[0].kind == EventKind::network_send);
    assert(stream.entries[0].node == 2);
    assert(stream.entries[0].task == 0x1111);
    assert(stream.entries[0].correlation_id == 0x2222);
    assert(stream.entries[0].payload ==
           (EventPayload{.subject = 7, .related = 8, .value = 512, .extra = 64, .amount_ns = -99}));

    assert(stream.entries[1].universe == kSeed);
    assert(stream.entries[1].seq == 1);
    assert(stream.entries[1].at == cosmos::Time{0});
    assert(!stream.entries[1].has_wall);
    assert(stream.entries[1].wall_ns == 0);
    assert(stream.entries[1].kind == EventKind::storage_write);
    assert(stream.entries[1].node == cosmos::kNoNode);
    assert(stream.entries[1].task == cosmos::kNoTask);
    assert(stream.entries[1].correlation_id == cosmos::kNoCorrelation);
    assert(stream.entries[1].payload.amount_ns == 17);
    assert(stream.entries[1].wall_ns == 0);

    // Re-encoding a decoded stream is byte-identical: the wire form is canonical.
    assert(encoded(stream) == bytes);

    std::cout << "[PASS] test_encode_decode_round_trips_every_field" << std::endl;
}

void test_encoded_stream_preserves_the_gap() {
    EventSink sink(2, kSeed);
    for (uint64_t value = 0; value < 5; ++value) {
        sink.record(sample_event(EventKind::clock_gettime, value));
    }

    const std::string bytes = encoded(sink);
    const auto decoded = cosmos::decode_events(bytes);
    must(decoded.has_value());
    const EventStream& stream = *decoded;
    assert(stream.universe == kSeed);
    assert(stream.dropped == 3);
    assert(stream.entries.size() == 2);
    assert(stream.entries[0].seq == 3);
    assert(stream.entries[1].seq == 4);
    assert(stream.first_seq() == 3);
    assert(stream.next_seq() == 5);

    const auto gap = stream.gap();
    must(gap.has_value());
    assert(gap->universe == kSeed);
    assert(gap->count == 3);
    assert(gap->first_seq == 3);

    std::cout << "[PASS] test_encoded_stream_preserves_the_gap" << std::endl;
}

void test_empty_stream_round_trips() {
    EventSink sink(4, kSeed);

    const std::string bytes = encoded(sink);
    assert(bytes.size() == cosmos::kEventStreamHeaderBytes);
    const auto decoded = cosmos::decode_events(bytes);
    must(decoded.has_value());
    assert(decoded->universe == kSeed);
    assert(decoded->dropped == 0);
    assert(decoded->rejected == 0);
    assert(decoded->entries.empty());
    assert(decoded->first_seq() == 0);
    assert(decoded->next_seq() == 0);

    std::cout << "[PASS] test_empty_stream_round_trips" << std::endl;
}

void test_encoding_is_deterministic_and_rotation_independent() {
    EventSink left = populated_sink();
    EventSink right = populated_sink();
    assert(encoded(left) == encoded(right));

    left.record(sample_event(EventKind::app_annotation, 1));
    right.record(sample_event(EventKind::app_annotation, 1));
    assert(encoded(left) == encoded(right));

    // A wrapped ring must encode its retained window in logical order, independent of head_.
    EventSink wrapped(4, kSeed);
    for (uint64_t value = 0; value < 6; ++value) {
        wrapped.record(sample_event(EventKind::scheduler_run, value));
    }

    EventStream expected;
    expected.universe = kSeed;
    expected.dropped = 2;
    for (uint64_t value = 2; value < 6; ++value) {
        Event event = sample_event(EventKind::scheduler_run, value);
        event.universe = kSeed;
        event.seq = value;
        expected.entries.push_back(event);
    }

    assert(encoded(wrapped) == encoded(expected));

    std::cout << "[PASS] test_encoding_is_deterministic_and_rotation_independent" << std::endl;
}

void test_decode_rejects_malformed_streams() {
    const std::string good = encoded(populated_sink());
    const size_t first_entry = cosmos::kEventStreamHeaderBytes;
    const size_t second_entry = first_entry + cosmos::kEventEntryBytes;

    {
        const auto result = cosmos::decode_events("");
        must(!result.has_value());
        assert(result.error() == EventStreamError::Truncated);
    }
    {
        std::string bytes = good;
        bytes[0] = 2;
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadSource);
    }
    {
        std::string bytes = good;
        bytes[1] = 2;
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadVersion);
    }
    {
        const std::string bytes = good.substr(0, good.size() - 1);
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::Truncated);
    }
    {
        std::string bytes = good;
        bytes.push_back('\0');
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::TrailingBytes);
    }
    {
        // Header declares two entries but there is only one.
        std::string bytes = good;
        put_u64(bytes, kHeaderCount, 3);
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::Truncated);
    }
    {
        // The first retained sequence must equal dropped.
        std::string bytes = good;
        put_u64(bytes, first_entry, 9);
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        std::string bytes = good;
        bytes[first_entry + 16] = 2;
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        // has_wall == 0 with a non-zero wall value is contradictory.
        std::string bytes = good;
        put_u64(bytes, second_entry + 17, 1);
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        std::string bytes = good;
        put_u16(bytes, first_entry + 25, 0);
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        std::string bytes = good;
        put_u16(bytes, first_entry + 25, static_cast<uint16_t>(EventKind::_Count));
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        // The second retained event must follow the first.
        std::string bytes = good;
        put_u64(bytes, second_entry, 9);
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        // dropped + count must not overflow uint64_t.
        std::string bytes = good;
        put_u64(bytes, kHeaderDropped, std::numeric_limits<uint64_t>::max());
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        // Zero entries with a non-zero gap cannot come from a v1 ring.
        std::string bytes(cosmos::kEventStreamHeaderBytes, '\0');
        bytes[0] = static_cast<char>(cosmos::kEventStreamSource);
        bytes[1] = static_cast<char>(cosmos::kEventStreamVersion);
        put_u64(bytes, 2, kSeed);
        put_u64(bytes, kHeaderDropped, 1);
        put_u64(bytes, kHeaderCount, 0);
        const auto result = cosmos::decode_events(bytes);
        must(!result.has_value());
        assert(result.error() == EventStreamError::BadValue);
    }
    {
        // A rejected call leaves a complete zero-entry capture; rejected is not a gap.
        std::string bytes(cosmos::kEventStreamHeaderBytes, '\0');
        bytes[0] = static_cast<char>(cosmos::kEventStreamSource);
        bytes[1] = static_cast<char>(cosmos::kEventStreamVersion);
        put_u64(bytes, 2, kSeed);
        put_u64(bytes, kHeaderRejected, 3);
        const auto result = cosmos::decode_events(bytes);
        must(result.has_value());
        assert(result->rejected == 3);
        assert(result->dropped == 0);
        assert(result->entries.empty());
    }

    std::cout << "[PASS] test_decode_rejects_malformed_streams" << std::endl;
}

// The ring buffer is allocated before the universe is current, so recording is not an application
// allocation. The static-libstdc++ twin is the link where operator new is interposed and this is
// measurable. Cumulative counters catch a transient allocation that active_count() would miss.
void test_recording_does_not_allocate_with_a_universe_current() {
    constexpr uint64_t kCalls = 4096;
    EventSink sink(1024, kSeed);

    cosmos::Simulator sim(kSeed);
    cosmos::Simulator::set_current(&sim);
    const cosmos::HeapStats before = sim.heap().stats();
    for (uint64_t call = 0; call < kCalls; ++call) {
        sink.record(sample_event(EventKind::scheduler_run, call));
    }
    const cosmos::HeapStats after = sim.heap().stats();
    assert(after.active_allocations == 0);
    assert(after.total_allocation_count == before.total_allocation_count);
    assert(after.total_allocated_bytes == before.total_allocated_bytes);

    // A control allocation proves the oracle is measuring the same heap the sink must avoid.
    void* control = std::malloc(16);
    must(control != nullptr);
    assert(sim.heap().stats().total_allocation_count == before.total_allocation_count + 1);
    std::free(control);
    assert(sim.heap().stats().active_allocations == 0);
    cosmos::Simulator::set_current(nullptr);

    assert(sink.size() == 1024);
    assert(sink.next_seq() == kCalls);
    assert(sink.dropped() == kCalls - 1024);
    assert(sink.first_seq() == kCalls - 1024);

    std::cout << "[PASS] test_recording_does_not_allocate_with_a_universe_current" << std::endl;
}

} // namespace

int main() {
    test_kind_table_is_complete_and_named();
    test_sink_stamps_universe_and_sequence();
    test_rejected_calls_do_not_poison_the_capture();
    test_encode_rejects_inconsistent_streams();
    test_move_leaves_the_source_disabled();
    test_disabled_sink_discards_without_a_sequence();
    test_ring_keeps_newest_and_exposes_the_gap();
    test_encode_decode_round_trips_every_field();
    test_encoded_stream_preserves_the_gap();
    test_empty_stream_round_trips();
    test_encoding_is_deterministic_and_rotation_independent();
    test_decode_rejects_malformed_streams();
    test_recording_does_not_allocate_with_a_universe_current();
    std::cout << "All event sink tests passed successfully!" << std::endl;
    return 0;
}
