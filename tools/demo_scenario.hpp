#pragma once

// The deterministic demo application shared by cosmos_ui_export and cosmos_inspect. It is not a
// library contract: it exists so the artifact exporter and the interactive inspector agree on the
// same universe and the same telemetry.

#include "cosmos/scenario.hpp"
#include "cosmos/simulator.hpp"

#include <cstdint>
#include <cstdlib>
#include <pthread.h>
#include <string>
#include <sys/socket.h>
#include <time.h>

namespace cosmos::demo {

cosmos::FaultConfig demo_plan() {
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    plan.enable_class(cosmos::FaultClass::Clock);
    (void)plan.activate_site(cosmos::SiteId::malloc);
    (void)plan.activate_site(cosmos::SiteId::clock_gettime);

    cosmos::FaultRule memory;
    memory.rate = 0.35;
    (void)memory.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0);
    (void)plan.set_rule(cosmos::SiteId::malloc, std::move(memory));

    cosmos::FaultRule clock;
    clock.rate = 0.4;
    clock.amount = cosmos::Duration{50'000};
    (void)clock.outcomes.add(cosmos::FaultKind::ClockStep, 1.0);
    (void)plan.set_rule(cosmos::SiteId::clock_gettime, std::move(clock));
    return plan;
}

// A fiber and a virtual socket pair give the state inspector real scheduler/network rows without
// any host thread or socket. It draws no fault, so the canonical trace is unchanged.
struct EchoContext {
    cosmos::Simulator* simulator = nullptr;
    int fd = -1;
};

void* echo_fiber(void* raw) {
    auto* context = static_cast<EchoContext*>(raw);
    char byte = 0;
    if (context->simulator->net().recv(context->fd, &byte, 1, 0, cosmos::InjectedFault{}) == 1) {
        byte = static_cast<char>(byte + 1);
        (void)context->simulator->net().send(context->fd, &byte, 1, 0, cosmos::InjectedFault{});
    }
    return nullptr;
}

struct DemoContext {
    bool injected_oom = false;
    int sockets[2] = {-1, -1};
    pthread_t echo_thread{};
    EchoContext echo{};
};

// Sets up the fiber and socket pair. The inspector calls this before its own drain so a breakpoint
// can stop between the two halves of the demo.
inline DemoContext prepare_demo(cosmos::Scenario& scenario) {
    DemoContext context;
    auto& simulator = scenario.simulator();
    (void)simulator.net().pair(AF_INET, SOCK_STREAM, 0, context.sockets);
    context.echo = EchoContext{&simulator, context.sockets[1]};
    (void)simulator.spawn_node(0, &context.echo_thread, echo_fiber, &context.echo);
    return context;
}

// Explicit wrapped calls are the only place the demo's faults can fire. The loop deliberately
// interleaves memory and clock decisions so the exported trace has more than one lane and more
// than one virtual timestamp. A fired OOM returns null; freeing null is harmless, so the demo
// application never has to recover from the fault to stay deterministic.
inline void run_demo_workload(cosmos::Scenario& scenario, uint64_t index, DemoContext& context) {
    auto& telemetry = scenario.simulator().telemetry();
    const uint64_t steps = telemetry.counter("demo.steps");
    const uint64_t oom_gauge = telemetry.gauge("demo.injected_oom");
    const uint64_t step_span = telemetry.span("demo.step");
    auto& simulator = scenario.simulator();
    scenario.run([&context, &scenario, &telemetry, steps, oom_gauge, step_span, &simulator, index] {
        (void)index;
        char byte = 7;
        (void)simulator.net().send(context.sockets[0], &byte, 1, 0, cosmos::InjectedFault{});
        char reply = 0;
        (void)simulator.net().recv(context.sockets[0], &reply, 1, 0, cosmos::InjectedFault{});
        void* joined = nullptr;
        (void)simulator.scheduler().join(context.echo_thread, &joined);
        for (int step = 0; step < 12; ++step) {
            const uint64_t span = telemetry.span_begin(step_span);
            void* block = std::malloc(16);
            if (block == nullptr) context.injected_oom = true;
            std::free(block);
            struct timespec now{};
            (void)clock_gettime(CLOCK_MONOTONIC, &now);
            // Sleep rather than advance_time directly: the scheduler then has a virtual-time
            // boundary where an operator command deadline can fire.
            const struct timespec step_delay{0, 100'000};
            (void)simulator.scheduler().sleep_ns(&step_delay, nullptr);
            telemetry.counter_add(steps, 1);
            telemetry.gauge_set(oom_gauge, context.injected_oom ? 1 : 0);
            telemetry.span_end(span);
        }
    });
}

// The check is a pure function of the universe index *and* whether an injected OOM was observed,
// which gives the minimizer something real to keep: for a failing odd index the finding needs at
// least one OOM fire and does not need the clock steps. (The campaign's odd universes all see an
// OOM, so this is the same failing set the earlier index-only check described.)
inline void finish_demo(cosmos::Scenario& scenario, uint64_t index, bool injected_oom) {
    scenario.quiesce();
    // A non-zero virtual end makes the artifact's time labels reviewable.
    scenario.simulator().advance_time(cosmos::Duration{1'000'000});
    scenario.check(
        "odd-index-fails", [index, injected_oom] { return index % 2 == 0 || !injected_oom; },
        "universe " + std::to_string(index) + " was asked to fail");
    scenario.note_covered("odd-index-seen", index % 2 == 1);
}

inline void run_demo_body(cosmos::Scenario& scenario, uint64_t index) {
    DemoContext context = prepare_demo(scenario);
    run_demo_workload(scenario, index, context);
    finish_demo(scenario, index, context.injected_oom);
}

cosmos::ScenarioReport run_demo_universe(cosmos::Simulator& sim, uint64_t index) {
    auto scenario = cosmos::Scenario::on(sim, demo_plan());
    if (!scenario) {
        cosmos::ScenarioReport rejected;
        rejected.no_check_failed = false;
        return rejected;
    }
    run_demo_body(*scenario, index);
    return scenario->report();
}

} // namespace cosmos::demo
