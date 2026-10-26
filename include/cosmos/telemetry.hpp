#pragma once

// Application telemetry for the inspection stream (docs/ui-design.md U3.5).
//
// The v1 event stream carries five numeric payload slots, so application names are not written
// into an event. Instead the application declares a series once and receives a small integer id;
// the declaration is exported next to the universe state, and every event references that id.
// The API deliberately accepts no payload pointer: network and storage bytes are redacted by
// construction, and an application cannot smuggle an unstructured body into the artifact.
//
// Recording a value is allocation-free after the series exists (a vector index plus an event
// assignment). Registration allocates, so declare series during setup, not from a hot loop.

#include "cosmos/event_sink.hpp"
#include "cosmos/time.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cosmos {

enum class TelemetryKind : uint8_t {
    Counter,
    Gauge,
    Span,
    Annotation,
    _Count,
};

constexpr const char* name_of(TelemetryKind kind) {
    switch (kind) {
    case TelemetryKind::Counter:
        return "counter";
    case TelemetryKind::Gauge:
        return "gauge";
    case TelemetryKind::Span:
        return "span";
    case TelemetryKind::Annotation:
        return "annotation";
    case TelemetryKind::_Count:
        break;
    }
    return "?";
}

struct TelemetrySeries {
    uint64_t id = 0;
    // Rule 7: a declaration may happen while a universe is current, so the name never comes from
    // the tracked heap.
    RawString name{};
    TelemetryKind kind = TelemetryKind::Counter;
    // Counter: accumulated value. Gauge: latest value. Span: summed completed duration in ns.
    // Annotation: latest value. `observations` is the number of recorded values.
    int64_t total = 0;
    uint64_t observations = 0;
};

class Telemetry {
  public:
    Telemetry() = default;

    void attach(EventSink* sink, const VirtualClock* clock) {
        sink_ = sink;
        clock_ = clock;
    }

    // Declares a series and returns its stable id. Idempotent by (kind, name): a second call
    // returns the existing id instead of creating a duplicate. An empty name is refused (returns
    // 0) rather than producing an unnamed series.
    uint64_t declare(TelemetryKind kind, std::string_view name) {
        if (name.empty()) return 0;
        for (const TelemetrySeries& series : series_) {
            if (series.kind == kind && series.name.size() == name.size() &&
                std::string_view(series.name.data(), series.name.size()) == name) {
                return series.id;
            }
        }
        const uint64_t id = series_.size() + 1;
        series_.push_back(TelemetrySeries{id, RawString(name.begin(), name.end()), kind, 0, 0});
        return id;
    }

    uint64_t counter(std::string_view name) { return declare(TelemetryKind::Counter, name); }
    uint64_t gauge(std::string_view name) { return declare(TelemetryKind::Gauge, name); }
    uint64_t span(std::string_view name) { return declare(TelemetryKind::Span, name); }
    uint64_t annotation(std::string_view name) { return declare(TelemetryKind::Annotation, name); }

    // Adds `delta` to a counter. Refuses an unknown or non-counter id without recording anything.
    bool counter_add(uint64_t id, uint64_t delta) {
        TelemetrySeries* series = find(id, TelemetryKind::Counter);
        if (series == nullptr) return false;
        series->total += static_cast<int64_t>(delta);
        ++series->observations;
        record(EventKind::app_counter, id, delta);
        return true;
    }

    bool gauge_set(uint64_t id, int64_t value) {
        TelemetrySeries* series = find(id, TelemetryKind::Gauge);
        if (series == nullptr) return false;
        series->total = value;
        ++series->observations;
        record_signed(EventKind::app_gauge, id, value);
        return true;
    }

    bool annotation_set(uint64_t id, int64_t value) {
        TelemetrySeries* series = find(id, TelemetryKind::Annotation);
        if (series == nullptr) return false;
        series->total = value;
        ++series->observations;
        record_signed(EventKind::app_annotation, id, value);
        return true;
    }

    // Starts a span. The returned id is the event correlation id, or 0 for an unknown series.
    // The start time is captured from the virtual clock so the end event carries a duration.
    uint64_t span_begin(uint64_t id) {
        TelemetrySeries* series = find(id, TelemetryKind::Span);
        if (series == nullptr) return 0;
        const uint64_t span_id = next_span_++;
        if (span_id == 0) return 0;
        if (open_span_count_ == open_spans_.size()) return 0; // bounded: no allocation here
        open_spans_[open_span_count_++] =
            OpenSpan{span_id, id, clock_ != nullptr ? clock_->now() : Time::zero()};
        record(EventKind::app_span, span_id, 0, id, kApplicationCorrelationBase + span_id);
        return span_id;
    }

    bool span_end(uint64_t span_id) {
        for (size_t index = 0; index < open_span_count_; ++index) {
            const OpenSpan open = open_spans_[index];
            if (open.span_id != span_id) continue;
            open_spans_[index] = open_spans_[open_span_count_ - 1];
            --open_span_count_;
            TelemetrySeries* series = find(open.series_id, TelemetryKind::Span);
            if (series == nullptr) return false;
            ++series->observations;
            const Time now = clock_ != nullptr ? clock_->now() : Time::zero();
            const int64_t duration = (now - open.started_at).ns;
            series->total += duration;
            record_signed(EventKind::app_span, span_id, duration, open.series_id, 1,
                          kApplicationCorrelationBase + span_id);
            return true;
        }
        return false;
    }

    size_t open_span_count() const { return open_span_count_; }
    const std::vector<TelemetrySeries, RawRealAllocator<TelemetrySeries>>& series() const {
        return series_;
    }

    // Finished spans and their durations are not retained individually; the stream carries each
    // span event and the state carries the per-series total and observation count.

  private:
    struct OpenSpan {
        uint64_t span_id = 0;
        uint64_t series_id = 0;
        Time started_at{};
    };

    TelemetrySeries* find(uint64_t id, TelemetryKind kind) {
        if (id == 0 || id > series_.size()) return nullptr;
        TelemetrySeries& series = series_[static_cast<size_t>(id - 1)];
        return series.id == id && series.kind == kind ? &series : nullptr;
    }

    void record(EventKind kind, uint64_t subject, uint64_t value, uint64_t related = 0,
                uint64_t correlation = kNoCorrelation) noexcept {
        if (sink_ == nullptr || !sink_->enabled() || !sink_->seeded()) return;
        Event event;
        event.at = clock_ != nullptr ? clock_->now() : Time::zero();
        event.kind = kind;
        event.node = kNoNode;
        event.correlation_id = correlation;
        event.payload.subject = subject;
        event.payload.related = related;
        event.payload.value = value;
        sink_->record(event);
    }

    void record_signed(EventKind kind, uint64_t subject, int64_t amount, uint64_t related = 0,
                       uint64_t value = 0, uint64_t correlation = kNoCorrelation) noexcept {
        if (sink_ == nullptr || !sink_->enabled() || !sink_->seeded()) return;
        Event event;
        event.at = clock_ != nullptr ? clock_->now() : Time::zero();
        event.kind = kind;
        event.node = kNoNode;
        event.correlation_id = correlation;
        event.payload.subject = subject;
        event.payload.related = related;
        event.payload.value = value;
        event.payload.amount_ns = amount;
        sink_->record(event);
    }

    static constexpr size_t kMaxOpenSpans = 256;

    EventSink* sink_ = nullptr;
    const VirtualClock* clock_ = nullptr;
    std::vector<TelemetrySeries, RawRealAllocator<TelemetrySeries>> series_{};
    std::array<OpenSpan, kMaxOpenSpans> open_spans_{};
    size_t open_span_count_ = 0;
    uint64_t next_span_ = kApplicationCorrelationBase;
};

} // namespace cosmos
