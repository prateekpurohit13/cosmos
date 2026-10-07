#include "cosmos/assert.hpp"
#include "cosmos/scenario.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr uint64_t kSeed = 4242;

void must(bool ok) { assert(ok); }

cosmos::Scenario make(uint64_t seed = kSeed) {
    cosmos::FaultPlan plan;
    auto scenario = cosmos::Scenario::create(seed, std::move(plan));
    must(scenario.has_value());
    return std::move(*scenario);
}

const cosmos::CheckResult* find_check(const cosmos::ScenarioReport& report, const std::string& id) {
    for (const cosmos::CheckResult& result : report.checks) {
        if (result.id == id) return &result;
    }
    return nullptr;
}

const cosmos::CoverageNote* find_coverage(const cosmos::ScenarioReport& report,
                                          const std::string& id) {
    for (const cosmos::CoverageNote& note : report.coverage) {
        if (note.id == id) return &note;
    }
    return nullptr;
}

int count_coverage(const cosmos::ScenarioReport& report, const std::string& id) {
    int seen = 0;
    for (const cosmos::CoverageNote& note : report.coverage) {
        if (note.id == id) ++seen;
    }
    return seen;
}

// A passing invariant records nothing, so a workload whose only property held has still verified
// nothing: the vacuity rule outranks it.
void test_always_true_is_silent() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::always(true, "counter-monotonic"); });
    scenario.quiesce();

    const cosmos::ScenarioReport& report = scenario.report();
    must(find_check(report, "counter-monotonic") == nullptr);
    must(report.no_check_failed);
    must(report.vacuous());

    std::cout << "[PASS] test_always_true_is_silent" << std::endl;
}

void test_always_false_records_id_and_detail() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::always(false, "counter-monotonic", "counter went backwards"); });
    scenario.quiesce();

    const cosmos::ScenarioReport& report = scenario.report();
    const cosmos::CheckResult* result = find_check(report, "counter-monotonic");
    must(result != nullptr);
    must(!result->passed);
    must(result->detail == "counter went backwards");
    must(!report.no_check_failed);
    must(!report.passed());
    must(!report.vacuous());

    std::cout << "[PASS] test_always_false_records_id_and_detail" << std::endl;
}

// A bare COSMOS_CHECK names its own call site, and the detail survives to the report.
void test_cosmos_check_names_the_call_site() {
    cosmos::Scenario scenario = make();
    scenario.run([] { COSMOS_CHECK(1 + 1 == 3, "arithmetic"); });
    scenario.quiesce();

    const cosmos::CheckResult* result = find_check(scenario.report(), "arithmetic");
    must(result != nullptr);
    must(!result->passed);
    must(result->detail.find("test_assert.cpp:") != std::string::npos);

    std::cout << "[PASS] test_cosmos_check_names_the_call_site" << std::endl;
}

// A workload may call sometimes() in a loop; only the OR across calls is meaningful, so the id
// must appear once with the widest verdict.
void test_sometimes_coalesces_by_id() {
    cosmos::Scenario scenario = make();
    scenario.run([] {
        for (int i = 0; i < 5; ++i)
            cosmos::sometimes(i == 3, "retry-path");
    });
    scenario.quiesce();

    const cosmos::ScenarioReport& report = scenario.report();
    must(count_coverage(report, "retry-path") == 1);
    must(find_coverage(report, "retry-path")->hit);

    std::cout << "[PASS] test_sometimes_coalesces_by_id" << std::endl;
}

void test_sometimes_stays_visible_when_never_hit() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::sometimes(false, "disk-full-path"); });
    scenario.quiesce();

    const cosmos::CoverageNote* note = find_coverage(scenario.report(), "disk-full-path");
    must(note != nullptr);
    must(!note->hit);

    std::cout << "[PASS] test_sometimes_stays_visible_when_never_hit" << std::endl;
}

void test_reachable_marks_hit() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::reachable("warmup-complete"); });
    scenario.quiesce();

    const cosmos::CoverageNote* note = find_coverage(scenario.report(), "warmup-complete");
    must(note != nullptr);
    must(note->hit);

    std::cout << "[PASS] test_reachable_marks_hit" << std::endl;
}

// S0's sad path: the binding is per thread and restored on scope exit, so a second universe must
// neither see the first's violations nor inherit its coverage.
void test_assertions_do_not_leak_across_universes() {
    cosmos::Scenario first = make(kSeed);
    first.run([] {
        cosmos::always(false, "first-universe-only");
        cosmos::sometimes(false, "first-universe-path");
    });
    first.quiesce();

    cosmos::Scenario second = make(kSeed + 1);
    second.run([] { cosmos::always(true, "second-universe-only"); });
    second.quiesce();

    const cosmos::ScenarioReport& report = second.report();
    must(find_check(report, "first-universe-only") == nullptr);
    must(find_coverage(report, "first-universe-path") == nullptr);
    must(report.no_check_failed);
    must(report.coverage.empty());

    std::cout << "[PASS] test_assertions_do_not_leak_across_universes" << std::endl;
}

// check() runs the oracle under the same binding, so an always() inside an oracle is a violation
// rather than a crash.
void test_always_inside_oracle_is_recorded() {
    cosmos::Scenario scenario = make();
    scenario.run([] {});
    scenario.quiesce();
    scenario.check("replicas-agree", [] {
        cosmos::always(false, "replica-digest-match", "replica 2 differs");
        return true;
    });

    const cosmos::ScenarioReport& report = scenario.report();
    must(find_check(report, "replica-digest-match") != nullptr);
    must(!report.no_check_failed);

    std::cout << "[PASS] test_always_inside_oracle_is_recorded" << std::endl;
}

void test_violation_is_printed_by_report() {
    cosmos::Scenario scenario = make();
    scenario.run([] { cosmos::always(false, "counter-monotonic", "counter went backwards"); });
    scenario.quiesce();

    std::ostringstream out;
    cosmos::print_report(out, scenario.report());
    const std::string text = out.str();
    must(text.find("counter-monotonic") != std::string::npos);
    must(text.find("counter went backwards") != std::string::npos);

    std::cout << "[PASS] test_violation_is_printed_by_report" << std::endl;
}

} // namespace

int main() {
    test_always_true_is_silent();
    test_always_false_records_id_and_detail();
    test_cosmos_check_names_the_call_site();
    test_sometimes_coalesces_by_id();
    test_sometimes_stays_visible_when_never_hit();
    test_reachable_marks_hit();
    test_assertions_do_not_leak_across_universes();
    test_always_inside_oracle_is_recorded();
    test_violation_is_printed_by_report();
    std::cout << "All assertion tests passed successfully!" << std::endl;
    return 0;
}
