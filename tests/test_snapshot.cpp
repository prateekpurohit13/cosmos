#include "cosmos/scenario.hpp"
#include "cosmos/snapshot.hpp"
#include "cosmos/trace.hpp"

#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using cosmos::BranchContext;
using cosmos::BranchResult;
using cosmos::FaultConfig;
using cosmos::Scenario;
using cosmos::SimSubstrate;
using cosmos::Snapshot;
using cosmos::SnapshotInfo;

std::string g_order;
Scenario* g_capture_scenario = nullptr;
bool g_capture_rejected = false;

void* capture_from_fiber(void*) {
    auto captured = Snapshot::capture(*g_capture_scenario);
    g_capture_rejected = !captured.has_value();
    return nullptr;
}

__attribute__((noinline)) void branch_a() {
    asm volatile("" ::: "memory");
    volatile int value = 1;
    (void)value;
}

__attribute__((noinline)) void branch_b() {
    asm volatile("" ::: "memory");
    volatile int value = 2;
    (void)value;
}

void* append_a(void*) {
    branch_a();
    g_order.push_back('A');
    return nullptr;
}

void* append_b(void*) {
    branch_b();
    g_order.push_back('B');
    return nullptr;
}

std::string temp_path(const char* tag) {
    return "/tmp/cosmos-snapshot-" + std::string(tag) + "-" +
           std::to_string(static_cast<long>(::getpid()));
}

void test_snapshot_info_and_branch_choices() {
    g_order.clear();
    auto scenario = Scenario::create(17, FaultConfig{});
    assert(scenario.has_value());
    Scenario& universe = *scenario;
    universe.run([&universe] {
        pthread_t a = 0;
        pthread_t b = 0;
        assert(universe.simulator().spawn_node(0, &a, append_a, nullptr) == 0);
        assert(universe.simulator().spawn_node(0, &b, append_b, nullptr) == 0);
    });

    auto info = Snapshot::capture(*scenario, [](Scenario& branch, const BranchContext&) {
        branch.drain();
        branch.quiesce();
        branch.check("order-is-ab", [] { return g_order == "AB"; });
    });
    assert(info.has_value());
    const SnapshotInfo& snapshot_info = info->info();
    assert(snapshot_info.tasks >= 3); // main + two fibers
    assert(snapshot_info.ready_choices >= 2);
    assert(snapshot_info.seed == 17);
    assert(snapshot_info.method == "fork-cow");

    BranchContext first;
    first.branch_index = 0;
    first.has_forced_choice = true;
    first.forced_choice = 0;
    const BranchResult passed = info->explore(first);
    assert(!passed.died);
    assert(passed.error.empty());
    assert(passed.report.passed());
    assert(passed.forced_choice_applied);
    assert(passed.coverage_present);
    assert(passed.coverage.edges > 0);

    BranchContext second;
    second.branch_index = 1;
    second.has_forced_choice = true;
    second.forced_choice = 1;
    const BranchResult failed = info->explore(second);
    assert(!failed.died);
    assert(failed.error.empty());
    assert(!failed.report.passed());
    assert(failed.forced_choice_applied);
    bool saw_check = false;
    for (const auto& check : failed.report.checks) {
        if (check.id == "order-is-ab") {
            saw_check = true;
            assert(!check.passed);
        }
    }
    assert(saw_check);

    BranchContext invalid;
    invalid.branch_index = 2;
    invalid.has_forced_choice = true;
    invalid.forced_choice = snapshot_info.ready_choices;
    const BranchResult rejected = info->explore(invalid);
    assert(rejected.error.find("outside") != std::string::npos);

    BranchContext seeded;
    seeded.branch_index = 2;
    seeded.has_schedule_seed = true;
    seeded.schedule_seed = 0; // zero is a real seed, not "unset"
    const BranchResult seeded_result = info->explore(seeded);
    assert(seeded_result.error.empty());
    assert(!seeded_result.forced_choice_applied);

    const BranchResult duplicate = info->explore(seeded);
    assert(duplicate.error.find("already") != std::string::npos);

    std::cout << "[PASS] test_snapshot_info_and_branch_choices" << std::endl;
}

void test_quiesced_snapshot_records_fault_quiet() {
    auto scenario = Scenario::create(37, FaultConfig{});
    assert(scenario.has_value());
    scenario->run([] { branch_a(); });
    scenario->quiesce();
    scenario->check("parent-check", [] { return true; });
    auto snapshot = Snapshot::capture(*scenario);
    assert(snapshot.has_value());
    assert(snapshot->info().faults_quiet);
    BranchContext context;
    context.branch_index = 0;
    const BranchResult branch = snapshot->explore(context);
    assert(branch.error.empty());
    // The parent's check is not branch evidence. A branch that records no check is vacuous.
    assert(branch.report.checks.empty());
    assert(branch.report.vacuous());
    std::cout << "[PASS] test_quiesced_snapshot_records_fault_quiet" << std::endl;
}

void test_capture_from_a_fiber_is_rejected() {
    auto scenario = Scenario::create(43, FaultConfig{});
    assert(scenario.has_value());
    g_capture_scenario = &*scenario;
    g_capture_rejected = false;
    Scenario& universe = *scenario;
    universe.run([&universe] {
        pthread_t fiber = 0;
        assert(universe.simulator().spawn_node(0, &fiber, capture_from_fiber, nullptr) == 0);
        universe.drain();
        assert(universe.simulator().scheduler().join(fiber, nullptr) == 0);
    });
    assert(g_capture_rejected);
    std::cout << "[PASS] test_capture_from_a_fiber_is_rejected" << std::endl;
}

void test_branch_crash_is_contained() {
    g_order.clear();
    auto scenario = Scenario::create(19, FaultConfig{});
    assert(scenario.has_value());
    Scenario& universe = *scenario;
    universe.run([&universe] {
        pthread_t a = 0;
        pthread_t b = 0;
        assert(universe.simulator().spawn_node(0, &a, append_a, nullptr) == 0);
        assert(universe.simulator().spawn_node(0, &b, append_b, nullptr) == 0);
    });

    auto snapshot = Snapshot::capture(*scenario, [](Scenario& branch, const BranchContext& ctx) {
        branch.drain();
        if (ctx.has_forced_choice && ctx.forced_choice == 0) std::abort();
    });
    assert(snapshot.has_value());

    BranchContext crash;
    crash.branch_index = 0;
    crash.has_forced_choice = true;
    crash.forced_choice = 0;
    const BranchResult died = snapshot->explore(crash);
    assert(died.died);
    assert(died.signal == SIGABRT);

    BranchContext alive;
    alive.branch_index = 1;
    alive.has_forced_choice = true;
    alive.forced_choice = 1;
    const BranchResult survived = snapshot->explore(alive);
    assert(!survived.died);
    assert(survived.error.empty());
    std::cout << "[PASS] test_branch_crash_is_contained" << std::endl;
}

void test_storage_is_restored_between_branches() {
    const std::string path = temp_path("storage");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "base";
    }
    const int fd = ::open(path.c_str(), O_RDWR);
    assert(fd >= 0);

    g_order.clear();
    auto scenario = Scenario::create(23, FaultConfig{});
    assert(scenario.has_value());
    scenario->simulator().storage().track_open(fd, path, false);
    scenario->run([] { branch_a(); });

    const std::string late_path = path + ".late";
    {
        auto snapshot = Snapshot::capture(*scenario, [path, fd, late_path](Scenario& branch,
                                                                           const BranchContext&) {
            char before[8] = {};
            const bool redirects = branch.simulator().storage().has_redirects();
            const std::string resolved = branch.simulator().storage().redirect_path(path);
            // A newly opened path is redirected...
            const int branch_fd = ::open(path.c_str(), O_RDWR);
            assert(branch_fd >= 0);
            assert(::read(branch_fd, before, 4) == 4);
            ::close(branch_fd);
            // ...and the captured descriptor is replaced with one on the private copy.
            assert(::read(fd, before, 4) == 4);
            branch.quiesce();
            branch.check(
                "branch-sees-captured-file", [&before] { return std::string(before, 4) == "base"; },
                std::string("before=") + std::string(before, 4));
            branch.check(
                "branch-has-storage-redirects",
                [redirects, resolved, path] { return redirects && resolved != path; },
                "resolved=" + resolved);
            assert(::write(fd, "branch", 6) == 6);
            // A path first seen in the branch is private too: it must not appear in the
            // parent, and unlinking it must not remove the captured source.
            const std::string new_path = path + ".new";
            const int new_fd = ::open(new_path.c_str(), O_CREAT | O_RDWR, 0600);
            assert(new_fd >= 0);
            assert(::write(new_fd, "new", 3) == 3);
            ::close(new_fd);
            assert(::unlink(new_path.c_str()) == 0);
            // The parent created this after capture, so the captured view does not contain it.
            assert(::open(late_path.c_str(), O_RDONLY) < 0);
            const int late_fd = ::open(late_path.c_str(), O_CREAT | O_RDWR, 0600);
            assert(late_fd >= 0);
            assert(::write(late_fd, "branch-late", 11) == 11);
            ::close(late_fd);
        });
        assert(snapshot.has_value());

        // The parent is allowed to continue after capture. Its own post-capture write must not be
        // rewound by a branch, and the branch must still see the captured bytes.
        {
            std::ofstream parent_out(path, std::ios::binary | std::ios::trunc);
            parent_out << "parent";
        }
        {
            std::ofstream late_out(late_path, std::ios::binary | std::ios::trunc);
            late_out << "late";
        }

        BranchContext first;
        first.branch_index = 0;
        // This scenario has no scheduler tasks, so there is no ready choice to force.
        const BranchResult result = snapshot->explore(first);
        assert(!result.died);
        assert(result.error.empty());
        assert(result.report.passed());

        BranchContext second;
        second.branch_index = 1;
        const BranchResult second_result = snapshot->explore(second);
        assert(!second_result.died);
        assert(second_result.error.empty());
        assert(second_result.report.passed());

        std::ifstream after(path, std::ios::binary);
        std::string content;
        after >> content;
        assert(content == "parent");
        assert(!std::filesystem::exists(path + ".new"));
        std::ifstream late_after(late_path, std::ios::binary);
        std::string late_content;
        late_after >> late_content;
        assert(late_content == "late");
    }
    std::ifstream final(path, std::ios::binary);
    std::string final_content;
    final >> final_content;
    assert(final_content == "parent");
    ::close(fd);
    ::unlink(path.c_str());
    ::unlink(late_path.c_str());
    std::cout << "[PASS] test_storage_is_restored_between_branches" << std::endl;
}

void test_dead_checkpoint_is_a_branch_error_not_a_signal() {
    auto scenario = Scenario::create(31, FaultConfig{});
    assert(scenario.has_value());
    scenario->run([] { branch_a(); });

    {
        auto snapshot = Snapshot::capture(*scenario, [](Scenario&, const BranchContext&) {
            // Kill the checkpoint (the branch child's parent) before it can acknowledge. The
            // parent harness must observe a failed branch, not die on SIGPIPE from its next write.
            (void)::kill(::getppid(), SIGKILL);
        });
        assert(snapshot.has_value());
        BranchContext context;
        context.branch_index = 0;
        const BranchResult result = snapshot->explore(context);
        assert(!result.error.empty());
        // Leaving the scope runs ~Snapshot; a send to the dead checkpoint must return EPIPE
        // without terminating this process.
    }
    std::cout << "[PASS] test_dead_checkpoint_is_a_branch_error_not_a_signal" << std::endl;
}

void test_capture_does_not_pollute_the_tracked_heap() {
    g_order.clear();
    auto scenario = Scenario::create(41, FaultConfig{});
    assert(scenario.has_value());
    scenario->run([] { branch_a(); });
    cosmos::Simulator& sim = scenario->simulator();
    cosmos::Simulator::set_current(&sim);
    const size_t before = sim.heap().active_count();
    {
        auto snapshot = Snapshot::capture(*scenario);
        assert(snapshot.has_value());
        const size_t after = sim.heap().active_count();
        cosmos::Simulator::set_current(nullptr);
        assert(after == before);
    }
    cosmos::Simulator::set_current(&sim);
    BranchContext seeded;
    seeded.branch_index = 0;
    seeded.has_schedule_seed = true;
    seeded.schedule_seed = 0;
    // A branch with a schedule seed is reproducible across two captures of the same universe.
    auto first = Snapshot::capture(*scenario);
    auto second = Snapshot::capture(*scenario);
    cosmos::Simulator::set_current(nullptr);
    assert(first.has_value());
    assert(second.has_value());
    const BranchResult a = first->explore(seeded);
    const BranchResult b = second->explore(seeded);
    assert(a.error.empty());
    assert(b.error.empty());
    assert(a.report.checks.size() == b.report.checks.size());
    assert(a.coverage.edges == b.coverage.edges);
    assert(a.coverage.hash == b.coverage.hash);
    std::cout << "[PASS] test_capture_does_not_pollute_the_tracked_heap" << std::endl;
}

void test_branch_trace_is_a_suffix() {
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    assert(plan.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = 1.0;
    assert(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    assert(plan.set_rule(cosmos::SiteId::malloc, std::move(rule)));

    auto scenario = Scenario::create(53, std::move(plan), 1, cosmos::TraceOptions{true, 64});
    assert(scenario.has_value());
    scenario->run([] {
        void* block = std::malloc(8);
        std::free(block);
    });
    scenario->quiesce();
    auto snapshot = Snapshot::capture(*scenario);
    assert(snapshot.has_value());
    assert(snapshot->info().trace_entries >= 1);

    BranchContext context;
    context.branch_index = 0;
    const BranchResult branch = snapshot->explore(context);
    assert(branch.error.empty());
    assert(branch.traced);
    const auto decoded = cosmos::decode_trace(branch.trace_bytes);
    assert(decoded.has_value());
    // No decision happened after the snapshot; the branch identity is the empty suffix.
    assert(decoded->size() == 0);
    std::cout << "[PASS] test_branch_trace_is_a_suffix" << std::endl;
}

void test_untracked_regular_fd_is_isolated() {
    const std::string path = temp_path("untracked");
    const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);
    assert(::write(fd, "base", 4) == 4);
    auto scenario = Scenario::create(59, FaultConfig{});
    assert(scenario.has_value());
    scenario->run([] { branch_a(); });
    auto snapshot = Snapshot::capture(*scenario, [fd](Scenario& branch, const BranchContext&) {
        branch.quiesce();
        assert(::write(fd, "branch", 6) == 6);
    });
    assert(snapshot.has_value());
    BranchContext context;
    context.branch_index = 0;
    const BranchResult result = snapshot->explore(context);
    assert(result.error.empty());
    std::ifstream after(path, std::ios::binary);
    std::string content;
    after >> content;
    assert(content == "base");
    ::close(fd);
    ::unlink(path.c_str());
    std::cout << "[PASS] test_untracked_regular_fd_is_isolated" << std::endl;
}

void test_nested_capture_is_recorded_and_ids_are_distinct() {
    g_order.clear();
    auto scenario = Scenario::create(61, FaultConfig{});
    assert(scenario.has_value());
    Scenario& universe = *scenario;
    universe.run([&universe] {
        pthread_t a = 0;
        pthread_t b = 0;
        assert(universe.simulator().spawn_node(0, &a, append_a, nullptr) == 0);
        assert(universe.simulator().spawn_node(0, &b, append_b, nullptr) == 0);
    });

    auto snapshot = Snapshot::capture(*scenario, [](Scenario& branch, const BranchContext&) {
        auto nested = Snapshot::capture(branch);
        assert(nested.has_value());
        BranchContext context;
        context.branch_index = 0;
        context.has_forced_choice = true;
        context.forced_choice = 0;
        const BranchResult result = nested->explore(context);
        assert(result.error.empty());
    });
    assert(snapshot.has_value());
    const uint64_t outer_id = snapshot->info().id;

    BranchContext first;
    first.branch_index = 0;
    const BranchResult first_result = snapshot->explore(first);
    assert(first_result.error.empty());
    assert(first_result.nested_snapshots.size() == 1);
    assert(first_result.nested_branches.size() == 1);
    const SnapshotInfo& nested_info = first_result.nested_snapshots.front();
    assert(nested_info.id != outer_id);
    assert(nested_info.parent_id == outer_id);
    assert(nested_info.parent_branch_index == 0);
    assert(first_result.nested_branches.front().snapshot.id == nested_info.id);

    BranchContext second;
    second.branch_index = 1;
    const BranchResult second_result = snapshot->explore(second);
    assert(second_result.error.empty());
    assert(second_result.nested_snapshots.size() == 1);
    assert(second_result.nested_snapshots.front().id != nested_info.id);
    assert(second_result.nested_snapshots.front().parent_branch_index == 1);
    std::cout << "[PASS] test_nested_capture_is_recorded_and_ids_are_distinct" << std::endl;
}

void test_substrate_seam_returns_a_branch() {
    g_order.clear();
    auto scenario = Scenario::create(29, FaultConfig{});
    assert(scenario.has_value());
    Scenario& universe = *scenario;
    universe.run([&universe] {
        pthread_t a = 0;
        pthread_t b = 0;
        assert(universe.simulator().spawn_node(0, &a, append_a, nullptr) == 0);
        assert(universe.simulator().spawn_node(0, &b, append_b, nullptr) == 0);
    });

    SimSubstrate substrate;
    auto snapshot = substrate.save_snapshot(*scenario, [](Scenario& branch, const BranchContext&) {
        branch.drain();
        branch.quiesce();
        branch.check("order-is-ab", [] { return g_order == "AB"; });
    });
    assert(snapshot.has_value());
    BranchContext context;
    context.branch_index = 0;
    context.has_forced_choice = true;
    context.forced_choice = 1;
    const BranchResult result = substrate.restore_snapshot(**snapshot, context);
    assert(!result.died);
    assert(!result.report.passed());
    std::cout << "[PASS] test_substrate_seam_returns_a_branch" << std::endl;
}

} // namespace

int main() {
    test_snapshot_info_and_branch_choices();
    test_quiesced_snapshot_records_fault_quiet();
    test_capture_from_a_fiber_is_rejected();
    test_branch_crash_is_contained();
    test_storage_is_restored_between_branches();
    test_dead_checkpoint_is_a_branch_error_not_a_signal();
    test_capture_does_not_pollute_the_tracked_heap();
    test_branch_trace_is_a_suffix();
    test_untracked_regular_fd_is_isolated();
    test_nested_capture_is_recorded_and_ids_are_distinct();
    test_substrate_seam_returns_a_branch();
    std::cout << "All snapshot tests passed successfully!" << std::endl;
    return 0;
}
