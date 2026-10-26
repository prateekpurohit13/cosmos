#pragma once

// Read-only inspection state, breakpoints and the operator command log (docs/ui-design.md
// U3.2–U3.4).
//
// Everything here is observation or an explicitly recorded intervention:
//   * capture_state() allocates and is called after a run (or while paused), never from a wrapper.
//   * Breakpoint matching runs inside EventSink::record() and must stay allocation-free.
//   * A command is an operator intervention. It is not applied silently: the interactive process
//     schedules it at a virtual time and records an operator.command event, so a repro is
//     seed + config + command log (D6).
//
// The inspection stream never feeds replay, minimization or trace_hash (D5).

#include "cosmos/event_sink.hpp"
#include "cosmos/json.hpp"
#include "cosmos/simulator.hpp"
#include "cosmos/telemetry.hpp"

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cosmos {

// ---- state enumeration ----------------------------------------------------

struct RngInfo {
    const char* stream = "?";
    uint64_t seed = 0;
    uint64_t draws = 0;
};

struct InspectionState {
    uint64_t seed = 0;
    Time virtual_time{};
    int64_t realtime_ns = 0;
    bool deadlocked = false;
    size_t ready_tasks = 0;
    size_t parked_tasks = 0;
    HeapStats heap{};
    std::vector<TaskInfo> tasks{};
    std::vector<MutexInfo> mutexes{};
    std::vector<CondInfo> conds{};
    std::vector<TimerInfo> timers{};
    std::vector<DeadlineInfo> deadlines{};
    std::vector<EndpointInfo> endpoints{};
    std::vector<PartitionInfo> partitions{};
    std::vector<FileInfo> files{};
    std::vector<NodeInfo> nodes{};
    std::vector<RngInfo> rng{};
    std::vector<TelemetrySeries> telemetry{};
    std::vector<std::string> application_labels{};
};

inline InspectionState capture_state(const Simulator& simulator) {
    InspectionState state;
    state.seed = simulator.seed();
    state.virtual_time = simulator.now();
    state.realtime_ns = simulator.clock().realtime_ns();
    state.deadlocked = simulator.scheduler().deadlocked();
    state.ready_tasks = simulator.scheduler().ready_count();
    state.parked_tasks = simulator.scheduler().parked_count();
    state.heap = simulator.heap().stats();
    state.tasks = simulator.scheduler().task_infos();
    state.mutexes = simulator.scheduler().mutex_infos();
    state.conds = simulator.scheduler().cond_infos();
    state.timers = simulator.scheduler().timer_infos();
    state.deadlines = simulator.scheduler().deadline_infos();
    state.endpoints = simulator.net().endpoint_infos();
    state.partitions = simulator.net().partition_infos();
    state.files = simulator.storage().file_infos();
    state.nodes = simulator.nodes().node_infos();
    state.rng = {
        RngInfo{"schedule", stream_seed(state.seed, StreamDomain::Schedule),
                simulator.scheduler().schedule_rng().draws()},
        RngInfo{"fault", stream_seed(state.seed, StreamDomain::Fault),
                simulator.injector_or_null() != nullptr ? simulator.injector_or_null()->draws()
                                                        : 0},
        RngInfo{"workload", stream_seed(state.seed, StreamDomain::Workload),
                simulator.workload_rng().draws()},
        RngInfo{"user", stream_seed(state.seed, StreamDomain::User), simulator.user_rng().draws()},
        RngInfo{"swarm", stream_seed(state.seed, StreamDomain::Swarm),
                simulator.swarm_rng().draws()},
    };
    for (const TelemetrySeries& series : simulator.telemetry().series()) {
        state.telemetry.push_back(series);
    }
    for (const RawString& label : simulator.application_labels()) {
        state.application_labels.emplace_back(label.begin(), label.end());
    }
    return state;
}

inline const char* name_of(Task::State state) {
    switch (state) {
    case Task::State::Ready:
        return "ready";
    case Task::State::Running:
        return "running";
    case Task::State::BlockedMutex:
        return "blocked_mutex";
    case Task::State::BlockedCond:
        return "blocked_cond";
    case Task::State::BlockedJoin:
        return "blocked_join";
    case Task::State::Sleeping:
        return "sleeping";
    case Task::State::Done:
        return "done";
    }
    return "?";
}

// ---- state JSON -----------------------------------------------------------

namespace inspection_detail {

inline void print_nodes(std::ostream& out, const std::vector<NodeId>& nodes) {
    out << '[';
    for (size_t index = 0; index < nodes.size(); ++index) {
        if (index != 0) out << ", ";
        out << nodes[index];
    }
    out << ']';
}

} // namespace inspection_detail

// A complete state document. It is embedded in universes/<index>.json as `state` and is also what
// the interactive inspector prints at a breakpoint, so both surfaces agree byte for byte.
inline void print_state_json(std::ostream& out, const InspectionState& state) {
    using namespace inspection_detail;
    const json::classic_locale_guard locale(out);
    out << "{\"schema_version\": " << kArtifactSchemaVersion << ", \"seed\": ";
    json::string(out, json::decimal(state.seed));
    out << ", \"virtual_time_ns\": " << state.virtual_time.ns << ", \"realtime_ns\": ";
    json::string(out, json::decimal(static_cast<uint64_t>(state.realtime_ns)));
    out << ", \"deadlocked\": ";
    json::boolean(out, state.deadlocked);
    out << ", \"ready_tasks\": " << state.ready_tasks
        << ", \"parked_tasks\": " << state.parked_tasks;
    out << ", \"heap\": {\"active_allocations\": " << state.heap.active_allocations
        << ", \"active_bytes\": " << state.heap.active_bytes
        << ", \"total_allocated_bytes\": " << state.heap.total_allocated_bytes
        << ", \"total_allocation_count\": " << state.heap.total_allocation_count << '}';

    out << ", \"tasks\": [";
    for (size_t index = 0; index < state.tasks.size(); ++index) {
        if (index != 0) out << ", ";
        const TaskInfo& task = state.tasks[index];
        out << "{\"id\": " << task.id << ", \"node\": " << task.node << ", \"state\": ";
        json::string(out, name_of(task.state));
        out << ", \"detached\": ";
        json::boolean(out, task.detached);
        out << ", \"stack_bytes\": " << task.stack_size << ", \"joiners\": " << task.joiners << '}';
    }
    out << ']';

    out << ", \"mutexes\": [";
    for (size_t index = 0; index < state.mutexes.size(); ++index) {
        if (index != 0) out << ", ";
        const MutexInfo& mutex = state.mutexes[index];
        out << "{\"id\": " << mutex.id << ", \"locked\": ";
        json::boolean(out, mutex.locked);
        out << ", \"owner\": " << mutex.owner << ", \"waiters\": " << mutex.waiters << '}';
    }
    out << ']';

    out << ", \"conds\": [";
    for (size_t index = 0; index < state.conds.size(); ++index) {
        if (index != 0) out << ", ";
        out << "{\"id\": " << state.conds[index].id
            << ", \"waiters\": " << state.conds[index].waiters << '}';
    }
    out << ']';

    out << ", \"timers\": [";
    for (size_t index = 0; index < state.timers.size(); ++index) {
        if (index != 0) out << ", ";
        const TimerInfo& timer = state.timers[index];
        out << "{\"seq\": " << timer.seq << ", \"task\": " << timer.task
            << ", \"wakeup_ns\": " << timer.wakeup.ns << '}';
    }
    out << ']';

    out << ", \"deadlines\": [";
    for (size_t index = 0; index < state.deadlines.size(); ++index) {
        if (index != 0) out << ", ";
        const DeadlineInfo& deadline = state.deadlines[index];
        out << "{\"id\": " << deadline.id << ", \"at_ns\": " << deadline.at.ns << ", \"armed\": ";
        json::boolean(out, deadline.armed);
        out << '}';
    }
    out << ']';

    out << ", \"endpoints\": [";
    for (size_t index = 0; index < state.endpoints.size(); ++index) {
        if (index != 0) out << ", ";
        const EndpointInfo& ep = state.endpoints[index];
        out << "{\"fd\": " << ep.fd << ", \"node\": " << ep.node << ", \"listener\": ";
        json::boolean(out, ep.listener);
        out << ", \"connected\": ";
        json::boolean(out, ep.connected);
        out << ", \"awaiting_accept\": ";
        json::boolean(out, ep.awaiting_accept);
        out << ", \"nonblocking\": ";
        json::boolean(out, ep.nonblocking);
        out << ", \"peer_closed\": ";
        json::boolean(out, ep.peer_closed);
        out << ", \"port\": " << ep.port << ", \"peer_port\": " << ep.peer_port
            << ", \"peer_fd\": " << ep.peer_fd << ", \"rx_bytes\": " << ep.rx_bytes
            << ", \"rx_packets\": " << ep.rx_packets
            << ", \"pending_accepts\": " << ep.pending_accepts << '}';
    }
    out << ']';

    out << ", \"partitions\": [";
    for (size_t index = 0; index < state.partitions.size(); ++index) {
        if (index != 0) out << ", ";
        out << "{\"a\": ";
        print_nodes(out, state.partitions[index].a);
        out << ", \"b\": ";
        print_nodes(out, state.partitions[index].b);
        out << '}';
    }
    out << ']';

    out << ", \"files\": [";
    for (size_t index = 0; index < state.files.size(); ++index) {
        if (index != 0) out << ", ";
        const FileInfo& file = state.files[index];
        out << "{\"id\": " << file.id << ", \"path\": ";
        json::string(out, file.path);
        out << ", \"node\": " << file.node << ", \"durable_size\": " << file.durable_size
            << ", \"unsynced_records\": " << file.unsynced_records
            << ", \"unsynced_bytes\": " << file.unsynced_bytes
            << ", \"last_sync_offset\": " << file.last_sync_offset
            << ", \"last_sync_length\": " << file.last_sync_length << ", \"degraded\": ";
        json::boolean(out, file.degraded);
        out << ", \"open_fds\": " << file.open_fds << '}';
    }
    out << ']';

    out << ", \"nodes\": [";
    for (size_t index = 0; index < state.nodes.size(); ++index) {
        if (index != 0) out << ", ";
        out << "{\"id\": " << state.nodes[index].id << ", \"live\": ";
        json::boolean(out, state.nodes[index].live);
        out << '}';
    }
    out << ']';

    out << ", \"rng\": [";
    for (size_t index = 0; index < state.rng.size(); ++index) {
        if (index != 0) out << ", ";
        out << "{\"stream\": ";
        json::string(out, state.rng[index].stream);
        out << ", \"seed\": ";
        json::string(out, json::decimal(state.rng[index].seed));
        out << ", \"draws\": " << state.rng[index].draws << '}';
    }
    out << ']';

    out << ", \"telemetry\": [";
    for (size_t index = 0; index < state.telemetry.size(); ++index) {
        if (index != 0) out << ", ";
        const TelemetrySeries& series = state.telemetry[index];
        out << "{\"id\": " << series.id << ", \"name\": ";
        json::string(out, std::string_view(series.name.data(), series.name.size()));
        out << ", \"kind\": ";
        json::string(out, name_of(series.kind));
        out << ", \"total\": " << series.total << ", \"observations\": " << series.observations
            << '}';
    }
    out << "]";

    out << ", \"application\": [";
    for (size_t index = 0; index < state.application_labels.size(); ++index) {
        if (index != 0) out << ", ";
        out << "{\"id\": " << index + 1 << ", \"name\": ";
        json::string(out, state.application_labels[index]);
        out << '}';
    }
    out << "]}";
}

// ---- breakpoints ----------------------------------------------------------

// A deterministic predicate over one recorded event. It is deliberately a closed set of scalar
// comparisons rather than an expression language: the inspector must be able to evaluate it on the
// record path without allocating, parsing or touching application memory.
struct Breakpoint {
    std::optional<EventDomain> domain{};
    std::optional<EventKind> kind{};
    std::optional<NodeId> node{};
    std::optional<uint64_t> min_value{};
    std::optional<int64_t> min_virtual_ns{};
    uint64_t occurrence = 1;

    bool matches(const Event& event) const {
        if (domain.has_value() && domain_of(event.kind) != *domain) return false;
        if (kind.has_value() && event.kind != *kind) return false;
        if (node.has_value() && event.node != *node) return false;
        if (min_value.has_value() && event.payload.value < *min_value) return false;
        if (min_virtual_ns.has_value() && event.at.ns < *min_virtual_ns) return false;
        return true;
    }
};

struct BreakpointContext {
    Breakpoint breakpoint{};
    uint64_t seen = 0;
    bool hit = false;
};

// EventSink observer. No allocation, no I/O, no simulation callback (U3.2 guardrail).
inline bool breakpoint_observer(void* raw, const Event& event) noexcept {
    auto* context = static_cast<BreakpointContext*>(raw);
    if (context == nullptr || context->hit || !context->breakpoint.matches(event)) return false;
    ++context->seen;
    if (context->seen < context->breakpoint.occurrence) return false;
    context->hit = true;
    return true;
}

// Resolves a token from docs/event-stream.md ("scheduler.run", "memory.alloc", ...). Returns null
// for a token that is not a declared kind, so a CLI can reject it instead of matching nothing.
inline std::optional<EventKind> parse_event_kind(std::string_view token) {
    for (const EventKindInfo& info : kEventKinds) {
        if (token == info.token) return info.kind;
    }
    return std::nullopt;
}

// ---- operator command log -------------------------------------------------

enum class OperatorCommandKind : uint8_t {
    CrashNode,
    RebootNode,
    PauseNode,
    ResumeNode,
    Partition,
    HealPartition,
    _Count,
};

constexpr const char* name_of(OperatorCommandKind kind) {
    switch (kind) {
    case OperatorCommandKind::CrashNode:
        return "crash_node";
    case OperatorCommandKind::RebootNode:
        return "reboot_node";
    case OperatorCommandKind::PauseNode:
        return "pause_node";
    case OperatorCommandKind::ResumeNode:
        return "resume_node";
    case OperatorCommandKind::Partition:
        return "partition";
    case OperatorCommandKind::HealPartition:
        return "heal_partition";
    case OperatorCommandKind::_Count:
        break;
    }
    return "?";
}

inline std::optional<OperatorCommandKind> parse_operator_command(std::string_view token) {
    for (uint8_t value = 0; value < static_cast<uint8_t>(OperatorCommandKind::_Count); ++value) {
        const auto kind = static_cast<OperatorCommandKind>(value);
        if (token == name_of(kind)) return kind;
    }
    return std::nullopt;
}

// A recorded intervention. `at` is virtual time; commands are applied at that exact stamp before
// any fiber woken at the same stamp. `id` is the command-log identity; the event correlation id is
// kOperatorCorrelationBase + id, keeping it separate from network and span chains.
struct OperatorCommand {
    uint64_t id = 0;
    Time at{};
    OperatorCommandKind kind = OperatorCommandKind::CrashNode;
    NodeId node = 0;
    std::vector<NodeId> a{};
    std::vector<NodeId> b{};
};

inline void record_operator_command(Simulator& simulator, const OperatorCommand& command) {
    EventSink* sink = simulator.event_sink_or_null();
    if (sink == nullptr || !sink->seeded()) return;
    Event event;
    event.at = simulator.now();
    event.kind = EventKind::operator_command;
    event.node = command.kind == OperatorCommandKind::Partition ||
                         command.kind == OperatorCommandKind::HealPartition
                     ? kNoNode
                     : command.node;
    event.task = kNoTask;
    event.correlation_id = kOperatorCorrelationBase + command.id;
    event.payload.subject = static_cast<uint64_t>(command.kind);
    event.payload.related = command.kind == OperatorCommandKind::Partition ||
                                    command.kind == OperatorCommandKind::HealPartition
                                ? static_cast<uint64_t>(command.a.size())
                                : command.node;
    event.payload.value = command.at.ns;
    sink->record(event);
}

// Applies a command to the live universe. Returns false when the target is not in the requested
// state; the caller records only on success so a refused intervention cannot look like it
// happened.
inline bool apply_operator_command(Simulator& simulator, const OperatorCommand& command) {
    switch (command.kind) {
    case OperatorCommandKind::CrashNode:
        return simulator.crash_node(command.node);
    case OperatorCommandKind::RebootNode:
        return simulator.reboot_node(command.node);
    case OperatorCommandKind::PauseNode:
        if (!simulator.nodes().is_known(command.node) ||
            simulator.scheduler().node_paused_at(command.node)) {
            return false;
        }
        simulator.scheduler().pause_node(command.node);
        return true;
    case OperatorCommandKind::ResumeNode:
        if (!simulator.nodes().is_known(command.node) ||
            !simulator.scheduler().node_paused_at(command.node)) {
            return false;
        }
        simulator.scheduler().resume_node(command.node);
        return true;
    case OperatorCommandKind::Partition:
        if (command.a.empty() || command.b.empty()) return false;
        simulator.net().partition(command.a, command.b);
        return true;
    case OperatorCommandKind::HealPartition:
        if (command.a.empty() || command.b.empty()) return false;
        return simulator.net().heal_partition(command.a, command.b);
    case OperatorCommandKind::_Count:
        break;
    }
    return false;
}

class CommandLog {
  public:
    // Appends in call order. The id is assigned by the log so a repro names commands densely.
    OperatorCommand append(Time at, OperatorCommandKind kind, NodeId node,
                           std::vector<NodeId> a = {}, std::vector<NodeId> b = {}) {
        OperatorCommand command;
        command.id = commands_.size() + 1;
        command.at = at;
        command.kind = kind;
        command.node = node;
        command.a = std::move(a);
        command.b = std::move(b);
        commands_.push_back(std::move(command));
        return commands_.back();
    }

    const std::vector<OperatorCommand>& commands() const { return commands_; }
    bool empty() const { return commands_.empty(); }
    size_t size() const { return commands_.size(); }

  private:
    std::vector<OperatorCommand> commands_{};
};

inline void print_commands_json(std::ostream& out, uint64_t seed, const CommandLog& log) {
    const json::classic_locale_guard locale(out);
    out << "{\"schema_version\": " << kArtifactSchemaVersion << ", \"seed\": ";
    json::string(out, json::decimal(seed));
    out << ", \"commands\": [";
    for (size_t index = 0; index < log.commands().size(); ++index) {
        if (index != 0) out << ", ";
        const OperatorCommand& command = log.commands()[index];
        out << "{\"id\": " << command.id << ", \"at_ns\": " << command.at.ns << ", \"kind\": ";
        json::string(out, name_of(command.kind));
        out << ", \"node\": " << command.node << ", \"a\": ";
        inspection_detail::print_nodes(out, command.a);
        out << ", \"b\": ";
        inspection_detail::print_nodes(out, command.b);
        out << '}';
    }
    out << "]}\n";
}

} // namespace cosmos
