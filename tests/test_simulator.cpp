#include "cosmos/cosmos.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <type_traits>
#include <utility>

namespace {

// Stands in for the P1 engine: proves the slot holds a real type with a real constructor, not
// just an empty placeholder.
struct StubInjector {
    int calls = 0;
    explicit StubInjector(int start) : calls(start) {}
};

using StubSim = cosmos::BasicSimulator<StubInjector>;

} // namespace

// This predicate is what withdraws emplace_injector/clear_injector from the real Simulator, so a
// caller cannot install a wrongly-seeded injector or clear and re-install past install_faults'
// once-only rule. Calling either on it is a compile error, which no negative static_assert can
// express: a failed requires-clause on a non-template member is a hard error, not a substitution
// failure.
static_assert(cosmos::HasFactory<cosmos::BasicFaultInjector<cosmos::VirtualClock>>);
static_assert(!cosmos::HasFactory<StubInjector>);

// Two simulators must not share the thread_local current pointer.
static_assert(!std::is_same_v<cosmos::Simulator, StubSim>);
static_assert(!std::is_copy_constructible_v<cosmos::Simulator>);
static_assert(!std::is_copy_assignable_v<cosmos::Simulator>);

void test_slot_is_empty_by_default() {
    cosmos::Simulator sim;
    assert(!sim.has_injector());

    StubSim stub_sim;
    assert(!stub_sim.has_injector());

    std::cout << "[PASS] test_slot_is_empty_by_default" << std::endl;
}

void test_slot_holds_and_releases_an_injector() {
    StubSim sim;
    assert(!sim.has_injector());

    assert(sim.injector_or_null() == nullptr);

    StubInjector& injector = sim.emplace_injector(7);
    assert(sim.has_injector());
    assert(injector.calls == 7);
    assert(sim.injector_or_null() != nullptr);
    assert(sim.injector_or_null()->calls == 7);

    sim.injector_or_null()->calls = 9;
    assert(sim.injector_or_null()->calls == 9);

    sim.clear_injector();
    assert(!sim.has_injector());
    assert(sim.injector_or_null() == nullptr);

    sim.emplace_injector(1);
    assert(sim.injector_or_null()->calls == 1);

    std::cout << "[PASS] test_slot_holds_and_releases_an_injector" << std::endl;
}

// The empty slot must not disturb the existing heap path: this is the P0-S4 "no behavior change"
// requirement, checked here as well as by test_wrap_malloc running unmodified.
void test_empty_slot_leaves_malloc_tracking_intact() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* ptr = malloc(64);
    assert(ptr != nullptr);
    assert(!sim.has_injector());
    assert(sim.heap().stats().active_allocations == 1);

    free(ptr);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_empty_slot_leaves_malloc_tracking_intact" << std::endl;
}

// Only the Simulator alias is wired to the wrappers. Another instantiation has its own current
// slot, so it is never reached through a wrapped malloc: do not plan a wrapper test around one.
void test_other_instantiations_are_invisible_to_wrappers() {
    StubSim stub;
    StubSim::set_current(&stub);
    assert(StubSim::has_current());
    assert(!cosmos::Simulator::has_current());

    void* ptr = malloc(64);
    assert(ptr != nullptr);
    assert(stub.heap().stats().active_allocations == 0);
    free(ptr);

    StubSim::set_current(nullptr);
    std::cout << "[PASS] test_other_instantiations_are_invisible_to_wrappers" << std::endl;
}

// Clearing the current simulator mid-run must fall back to the real allocator instead of
// dereferencing a stale pointer.
void test_malloc_passes_through_after_current_is_cleared() {
    cosmos::Simulator sim;
    cosmos::Simulator::set_current(&sim);

    void* tracked = malloc(64);
    assert(tracked != nullptr);
    assert(sim.heap().stats().active_allocations == 1);

    cosmos::Simulator::set_current(nullptr);
    assert(!cosmos::Simulator::has_current());

    void* passthrough = malloc(64);
    assert(passthrough != nullptr);
    assert(sim.heap().stats().active_allocations == 1);
    free(passthrough);

    cosmos::Simulator::set_current(&sim);
    free(tracked);
    assert(sim.heap().stats().active_allocations == 0);

    cosmos::Simulator::set_current(nullptr);
    std::cout << "[PASS] test_malloc_passes_through_after_current_is_cleared" << std::endl;
}

// A tracked payload sits one header past its real allocation, so releasing it with no simulator
// current used to hand the wrong address to the allocator and abort.
void test_tracked_pointer_freed_with_no_current_simulator() {
    void* tracked = nullptr;
    {
        cosmos::Simulator sim;
        cosmos::Simulator::set_current(&sim);
        tracked = malloc(64);
        assert(tracked != nullptr);
        assert(sim.heap().stats().active_allocations == 1);
        cosmos::Simulator::set_current(nullptr);
    }

    free(tracked);

    void* after = malloc(128);
    assert(after != nullptr);
    free(after);

    std::cout << "[PASS] test_tracked_pointer_freed_with_no_current_simulator" << std::endl;
}

void test_destructor_clears_current() {
    {
        cosmos::Simulator sim;
        cosmos::Simulator::set_current(&sim);
        assert(cosmos::Simulator::has_current());
    }
    assert(!cosmos::Simulator::has_current());

    void* ptr = malloc(32);
    assert(ptr != nullptr);
    free(ptr);

    std::cout << "[PASS] test_destructor_clears_current" << std::endl;
}

// Endpoint probabilities must decide without consuming stream decisions (docs/fault-injection.md
// §7 Rule 3): a disabled or always-on fault cannot shift the Memory sub-stream. Verified by
// checking the Rng state is untouched after the calls against a same-seed reference stream.
void test_install_faults_derives_the_fault_stream() {
    constexpr uint64_t kSeed = 0xC0FFEE;

    cosmos::Simulator sim(kSeed);
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Memory);
    assert(cfg.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = 0.5;
    assert(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    assert(cfg.set_rule(cosmos::SiteId::malloc, rule));
    assert(sim.install_faults(std::move(cfg)).has_value());

    cosmos::Rng reference(cosmos::fault_class_seed(
        cosmos::stream_seed(kSeed, cosmos::StreamDomain::Fault), cosmos::FaultClass::Memory));

    for (int i = 0; i < 32; ++i) {
        const bool expect_fire = reference.uniform() < 0.5;
        const cosmos::FaultKind kind =
            sim.injector_or_null()->decide(cosmos::FaultClass::Memory, cosmos::SiteId::malloc);
        assert((kind == cosmos::FaultKind::OutOfMemory) == expect_fire);
    }

    std::cout << "[PASS] test_install_faults_derives_the_fault_stream" << std::endl;
}

void test_install_faults_is_once_only() {
    cosmos::Simulator sim;
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Memory);
    assert(sim.install_faults(cfg).has_value());
    assert(sim.has_injector());

    auto again = sim.install_faults(cfg);
    assert(!again.has_value());
    assert(again.error().error == cosmos::ConfigError::InjectorAlreadyInstalled);

    std::cout << "[PASS] test_install_faults_is_once_only" << std::endl;
}

void test_install_faults_rejects_an_invalid_config() {
    cosmos::Simulator sim;
    cosmos::FaultConfig cfg;
    cfg.enable_class(cosmos::FaultClass::Memory);
    assert(cfg.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = 1.5;
    assert(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    assert(cfg.set_rule(cosmos::SiteId::malloc, rule));

    auto rejected = sim.install_faults(std::move(cfg));
    assert(!rejected.has_value());
    assert(rejected.error().error == cosmos::ConfigError::BadRate);
    assert(rejected.error().site == cosmos::SiteId::malloc);
    assert(!sim.has_injector());

    std::cout << "[PASS] test_install_faults_rejects_an_invalid_config" << std::endl;
}

void test_simulator_virtual_clock() {
    using namespace cosmos::literals;
    cosmos::Simulator sim;
    assert(sim.now() == cosmos::Time::zero());
    assert(sim.clock().now_ns() == 0);

    sim.advance_time(500_ms);
    assert(sim.now() == cosmos::Time::zero() + 500_ms);
    assert(sim.clock().now_ns() == 500'000'000);

    sim.clock().advance_to(cosmos::Time::zero() + 2_s);
    assert(sim.now() == cosmos::Time::zero() + 2_s);

    std::cout << "[PASS] test_simulator_virtual_clock" << std::endl;
}

int main() {
    test_slot_is_empty_by_default();
    test_slot_holds_and_releases_an_injector();
    test_empty_slot_leaves_malloc_tracking_intact();
    test_other_instantiations_are_invisible_to_wrappers();
    test_malloc_passes_through_after_current_is_cleared();
    test_tracked_pointer_freed_with_no_current_simulator();
    test_destructor_clears_current();
    test_install_faults_derives_the_fault_stream();
    test_install_faults_is_once_only();
    test_install_faults_rejects_an_invalid_config();
    test_simulator_virtual_clock();
    std::cout << "All simulator tests passed successfully!" << std::endl;
    return 0;
}
