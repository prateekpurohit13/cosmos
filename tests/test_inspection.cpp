// U3.2–U3.5 contract: engine producers, state enumeration, breakpoint pause/resume, command log
// and application telemetry. The canonical trace is deliberately not exercised here; it has its
// own suite and must stay unchanged (D5).

#include "cosmos/cosmos.hpp"
#include "cosmos/inspection.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

void must(bool condition) {
    if (!condition) {
        std::cerr << "requirement failed" << std::endl;
        std::abort();
    }
}

struct YieldContext {
    cosmos::Simulator* simulator = nullptr;
    int yields = 0;
};

void* yield_fiber(void* raw) {
    auto* context = static_cast<YieldContext*>(raw);
    for (int index = 0; index < 5; ++index) {
        ++context->yields;
        context->simulator->scheduler().yield();
    }
    return nullptr;
}

const cosmos::TaskInfo* find_task(const cosmos::InspectionState& state, uint64_t id) {
    for (const cosmos::TaskInfo& task : state.tasks) {
        if (task.id == id) return &task;
    }
    return nullptr;
}

bool stream_contains(const cosmos::EventSink& sink, cosmos::EventKind kind) {
    for (size_t index = 0; index < sink.size(); ++index) {
        if (sink[index].kind == kind) return true;
    }
    return false;
}

void test_scheduler_and_network_producers() {
    cosmos::Simulator simulator(1234);
    simulator.enable_event_capture(512);
    must(simulator.event_capture_attached());

    int sockets[2] = {-1, -1};
    must(simulator.net().pair(AF_INET, SOCK_STREAM, 0, sockets) == 0);
    char byte = 'a';
    must(simulator.net().send(sockets[0], &byte, 1, 0, cosmos::InjectedFault{}) == 1);
    char reply = 0;
    must(simulator.net().recv(sockets[1], &reply, 1, 0, cosmos::InjectedFault{}) == 1);
    must(reply == 'a');
    char dropped = 'd';
    must(simulator.net().send(sockets[0], &dropped, 1, 0,
                              cosmos::InjectedFault{cosmos::FaultKind::PacketDrop, {}}) == 1);

    YieldContext context{&simulator, 0};
    pthread_t thread{};
    must(simulator.spawn_node(0, &thread, yield_fiber, &context) == 0);
    simulator.scheduler().run_until_quiescence();
    must(context.yields == 5);

    const cosmos::EventSink& sink = simulator.event_sink();
    must(sink.size() > 0);
    must(sink.dropped() == 0);
    must(sink.rejected() == 0);
    must(stream_contains(sink, cosmos::EventKind::scheduler_spawn));
    must(stream_contains(sink, cosmos::EventKind::scheduler_run));
    must(stream_contains(sink, cosmos::EventKind::scheduler_yield));
    must(stream_contains(sink, cosmos::EventKind::scheduler_exit));
    must(stream_contains(sink, cosmos::EventKind::network_socket));
    must(stream_contains(sink, cosmos::EventKind::network_send));
    must(stream_contains(sink, cosmos::EventKind::network_recv));
    must(stream_contains(sink, cosmos::EventKind::network_enqueue));
    must(stream_contains(sink, cosmos::EventKind::network_drop));

    // Operator, node, application and random producers are part of the same contract. Exercise
    // them directly so a missing producer is a test failure, not a documentation claim.
    simulator.nodes().set_node_count(2);
    must(simulator.crash_node(1));
    must(simulator.reboot_node(1));
    simulator.scheduler().pause_node(1);
    simulator.scheduler().resume_node(1);
    cosmos::CommandLog commands;
    const cosmos::OperatorCommand command =
        commands.append(cosmos::Time{1}, cosmos::OperatorCommandKind::PauseNode, 1);
    cosmos::record_operator_command(simulator, command);
    simulator.record_application_event(cosmos::EventKind::app_always, "check-id", 0);
    cosmos::Simulator::set_current(&simulator);
    (void)std::rand();
    cosmos::Simulator::set_current(nullptr);
    must(stream_contains(sink, cosmos::EventKind::node_crash));
    must(stream_contains(sink, cosmos::EventKind::node_reboot));
    must(stream_contains(sink, cosmos::EventKind::node_pause));
    must(stream_contains(sink, cosmos::EventKind::node_resume));
    must(stream_contains(sink, cosmos::EventKind::operator_command));
    must(stream_contains(sink, cosmos::EventKind::app_always));
    must(stream_contains(sink, cosmos::EventKind::random_draw));

    // The encoded stream round-trips and preserves the sequence identity.
    const auto encoded = cosmos::encode_events(sink);
    must(encoded.has_value());
    const auto decoded = cosmos::decode_events(*encoded);
    must(decoded.has_value());
    must(decoded->entries.size() == sink.size());
    must(decoded->entries.front().seq == sink.first_seq());

    // State enumerators are deterministic and read-only.
    const cosmos::InspectionState state = cosmos::capture_state(simulator);
    must(state.seed == 1234);
    must(state.tasks.size() == 2); // main fiber + the spawned fiber
    const cosmos::TaskInfo* main_task = find_task(state, cosmos::kMainTask);
    must(main_task != nullptr);
    const cosmos::TaskInfo* spawned = find_task(state, 2);
    must(spawned != nullptr);
    must(spawned->state == cosmos::Task::State::Done);
    must(state.endpoints.size() == 2);
    must(state.nodes.size() == 2);
    must(state.nodes[0].live && state.nodes[1].live);
    must(state.application_labels.size() == 1);
    std::ostringstream state_json;
    cosmos::print_state_json(state_json, state);
    must(state_json.str().find("\"tasks\"") != std::string::npos);
    must(state_json.str().find("\"endpoints\"") != std::string::npos);

    std::cout << "[PASS] test_scheduler_and_network_producers" << std::endl;
}

void test_memory_and_clock_producers() {
    cosmos::Simulator simulator(5678);
    simulator.enable_event_capture(128);
    cosmos::Simulator::set_current(&simulator);

    void* block = std::malloc(32);
    must(block != nullptr);
    std::free(block);
    void* resized = std::malloc(8);
    must(resized != nullptr);
    resized = std::realloc(resized, 64);
    must(resized != nullptr);
    std::free(resized);

    struct timespec now{};
    must(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    cosmos::Simulator::set_current(nullptr);

    const cosmos::EventSink& sink = simulator.event_sink();
    must(stream_contains(sink, cosmos::EventKind::memory_alloc));
    must(stream_contains(sink, cosmos::EventKind::memory_free));
    must(stream_contains(sink, cosmos::EventKind::memory_realloc));
    must(stream_contains(sink, cosmos::EventKind::clock_gettime));

    const cosmos::InspectionState state = cosmos::capture_state(simulator);
    must(state.heap.active_allocations == 0);
    must(state.heap.total_allocation_count == 3);
    must(state.heap.active_bytes == 0);

    std::cout << "[PASS] test_memory_and_clock_producers" << std::endl;
}

void test_storage_producers_and_state() {
    cosmos::Simulator simulator(9012);
    simulator.enable_event_capture(128);
    char path[] = "/tmp/cosmos-inspection-XXXXXX";
    const int fd = ::mkstemp(path);
    must(fd >= 0);
    const char bytes[] = "durable";
    must(::write(fd, bytes, 4) == 4);
    simulator.storage().set_active_node(0);
    simulator.storage().track_open(fd, path, false);
    simulator.storage().record_write(fd, 0, "xx", 2, 0);
    simulator.storage().commit_fsync(fd);
    simulator.storage().reboot(0, true);

    const cosmos::InspectionState state = cosmos::capture_state(simulator);
    must(state.files.size() == 1);
    must(state.files[0].path == path);
    must(state.files[0].node == 0);

    const cosmos::EventSink& sink = simulator.event_sink();
    must(stream_contains(sink, cosmos::EventKind::storage_dirty));
    must(stream_contains(sink, cosmos::EventKind::storage_durable));
    must(stream_contains(sink, cosmos::EventKind::storage_rollback));

    ::close(fd);
    ::unlink(path);
    std::cout << "[PASS] test_storage_producers_and_state" << std::endl;
}

void test_breakpoint_pauses_and_resumes() {
    cosmos::Simulator simulator(3456);
    simulator.enable_event_capture(256);

    cosmos::BreakpointContext breakpoint;
    breakpoint.breakpoint.kind = cosmos::EventKind::scheduler_yield;
    breakpoint.breakpoint.occurrence = 2;
    simulator.event_sink().set_observer(&cosmos::breakpoint_observer, &breakpoint);

    YieldContext context{&simulator, 0};
    pthread_t thread{};
    must(simulator.spawn_node(0, &thread, yield_fiber, &context) == 0);

    simulator.scheduler().run_until_quiescence();
    must(breakpoint.hit);
    must(breakpoint.seen >= 2);
    must(context.yields == 2);
    const cosmos::InspectionState paused = cosmos::capture_state(simulator);
    must(paused.tasks.size() == 2);
    const cosmos::TaskInfo* paused_spawned = find_task(paused, 2);
    must(paused_spawned != nullptr);
    must(paused_spawned->state != cosmos::Task::State::Done);

    simulator.event_sink().clear_observer_hit();
    simulator.scheduler().run_until_quiescence();
    must(context.yields == 5);
    const cosmos::InspectionState finished = cosmos::capture_state(simulator);
    must(find_task(finished, 2) != nullptr);
    must(find_task(finished, 2)->state == cosmos::Task::State::Done);

    std::cout << "[PASS] test_breakpoint_pauses_and_resumes" << std::endl;
}

void test_telemetry_and_command_log() {
    cosmos::Simulator simulator(7890);
    simulator.enable_event_capture(128);

    auto& telemetry = simulator.telemetry();
    const uint64_t requests = telemetry.counter("kv.requests");
    const uint64_t depth = telemetry.gauge("kv.queue_depth");
    const uint64_t replicate = telemetry.span("kv.replicate");
    const uint64_t note = telemetry.annotation("kv.leader_elected");
    must(requests != 0 && depth != 0 && replicate != 0 && note != 0);
    must(telemetry.counter("kv.requests") == requests);
    must(telemetry.counter_add(requests, 3));
    must(telemetry.gauge_set(depth, 7));
    must(telemetry.annotation_set(note, 1));
    const uint64_t span = telemetry.span_begin(replicate);
    must(span != 0);
    simulator.advance_time(cosmos::Duration{250});
    must(telemetry.span_end(span));
    must(!telemetry.counter_add(depth, 1)); // wrong series kind is refused

    const cosmos::InspectionState state = cosmos::capture_state(simulator);
    must(state.telemetry.size() == 4);
    must(state.telemetry[0].total == 3);
    must(state.telemetry[1].total == 7);
    must(state.telemetry[2].observations == 1);
    must(state.telemetry[2].total == 250);

    cosmos::CommandLog log;
    const auto& crash = log.append(cosmos::Time{100}, cosmos::OperatorCommandKind::CrashNode, 0);
    must(cosmos::apply_operator_command(simulator, crash));
    must(!simulator.nodes().is_live(0));
    const auto& reboot = log.append(cosmos::Time{200}, cosmos::OperatorCommandKind::RebootNode, 0);
    must(cosmos::apply_operator_command(simulator, reboot));
    must(simulator.nodes().is_live(0));
    must(log.size() == 2);

    std::ostringstream commands;
    cosmos::print_commands_json(commands, 7890, log);
    must(commands.str().find("crash_node") != std::string::npos);
    must(commands.str().find("reboot_node") != std::string::npos);

    std::cout << "[PASS] test_telemetry_and_command_log" << std::endl;
}

void test_telemetry_recording_does_not_allocate() {
    cosmos::Simulator simulator(2468);
    simulator.enable_event_capture(128);
    cosmos::Simulator::set_current(&simulator);
    auto& telemetry = simulator.telemetry();
    const uint64_t counter = telemetry.counter("alloc.counter");
    const uint64_t gauge = telemetry.gauge("alloc.gauge");
    const uint64_t span = telemetry.span("alloc.span");
    const uint64_t annotation = telemetry.annotation("alloc.annotation");
    const cosmos::HeapStats before = simulator.heap().stats();
    must(telemetry.counter_add(counter, 1));
    must(telemetry.gauge_set(gauge, 2));
    const uint64_t open = telemetry.span_begin(span);
    must(open != 0);
    must(telemetry.span_end(open));
    must(telemetry.annotation_set(annotation, 3));
    const cosmos::HeapStats after = simulator.heap().stats();
    must(after.active_allocations == before.active_allocations);
    must(after.active_bytes == before.active_bytes);
    must(after.total_allocation_count == before.total_allocation_count);
    must(after.total_allocated_bytes == before.total_allocated_bytes);
    cosmos::Simulator::set_current(nullptr);

    std::cout << "[PASS] test_telemetry_recording_does_not_allocate" << std::endl;
}

void test_breakpoint_parsing() {
    must(cosmos::parse_event_kind("scheduler.run") == cosmos::EventKind::scheduler_run);
    must(!cosmos::parse_event_kind("scheduler.unknown").has_value());
    must(cosmos::parse_operator_command("crash_node") == cosmos::OperatorCommandKind::CrashNode);
    must(!cosmos::parse_operator_command("explode").has_value());

    cosmos::Event event;
    event.kind = cosmos::EventKind::network_drop;
    event.node = 2;
    event.payload.value = 9;
    cosmos::Breakpoint breakpoint;
    breakpoint.domain = cosmos::EventDomain::Network;
    breakpoint.node = 2;
    breakpoint.min_value = 8;
    must(breakpoint.matches(event));
    breakpoint.min_value = 10;
    must(!breakpoint.matches(event));

    std::cout << "[PASS] test_breakpoint_parsing" << std::endl;
}

} // namespace

int main() {
    test_scheduler_and_network_producers();
    test_memory_and_clock_producers();
    test_storage_producers_and_state();
    test_breakpoint_pauses_and_resumes();
    test_telemetry_and_command_log();
    test_telemetry_recording_does_not_allocate();
    test_breakpoint_parsing();
    std::cout << "All inspection tests passed successfully!" << std::endl;
    return 0;
}
