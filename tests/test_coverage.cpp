#include "cosmos/campaign.hpp"
#include "cosmos/coverage.hpp"
#include "cosmos/scenario.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

namespace {

using cosmos::CampaignConfig;
using cosmos::CampaignReport;
using cosmos::CoverageSnapshot;
using cosmos::SeedGuidance;
using cosmos::Simulator;

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

void test_bitmap_records_edges_and_hashes_differently() {
    cosmos::coverage_reset();
    assert(cosmos::coverage_edges() == 0);
    branch_a();
    const CoverageSnapshot first = cosmos::coverage_snapshot();
    assert(first.edges > 0);
    assert(first.bits.size() == cosmos::kCoverageBitmapBytes);

    cosmos::coverage_reset();
    branch_b();
    const CoverageSnapshot second = cosmos::coverage_snapshot();
    assert(second.edges > 0);
    // The two functions share the test harness's surrounding edges; the branch bodies differ, so
    // the complete hit sets cannot be identical.
    assert(first.hash != second.hash);
    assert(second.edges > 0);
    std::cout << "[PASS] test_bitmap_records_edges_and_hashes_differently" << std::endl;
}

void test_merge_and_novelty_are_set_operations() {
    cosmos::coverage_reset();
    branch_a();
    const CoverageSnapshot first = cosmos::coverage_snapshot();

    cosmos::coverage_reset();
    branch_a();
    branch_b();
    const CoverageSnapshot both = cosmos::coverage_snapshot();

    CoverageSnapshot union_bits = first;
    const uint64_t novel = cosmos::coverage_novelty(both, union_bits);
    assert(novel > 0);
    const uint64_t added = cosmos::coverage_merge(union_bits, both);
    assert(added == novel);
    assert(cosmos::coverage_novelty(both, union_bits) == 0);
    assert(union_bits.edges == both.edges);
    std::cout << "[PASS] test_merge_and_novelty_are_set_operations" << std::endl;
}

// A tiny seed-dependent body: even seeds take branch A, odd seeds take branch B. Guided selection
// must retain at least one seed for each path.
cosmos::ScenarioReport run_guided_body(Simulator& sim, uint64_t) {
    auto scenario = cosmos::Scenario::on(sim, cosmos::FaultConfig{});
    assert(scenario.has_value());
    scenario->run([&sim] {
        if ((sim.seed() & 1U) == 0) {
            branch_a();
        } else {
            branch_b();
        }
    });
    scenario->quiesce();
    scenario->check("always", [] { return true; });
    return scenario->report();
}

void test_seeded_coverage_is_aggregated() {
    CampaignConfig config;
    config.trials = 4;
    config.base_seed = 3;
    config.parallel = 1;
    config.coverage = true;
    config.program = "test_coverage";
    const CampaignReport report = cosmos::Campaign::run(config, run_guided_body);
    assert(!report.refused);
    assert(report.guidance == SeedGuidance::Seeded);
    assert(report.coverage.enabled);
    assert(report.coverage.runs_with_coverage == 4);
    assert(report.coverage.unique_edges > 0);
    assert(report.coverage.total_edges > 0);
    assert(!report.corpus.empty());
    std::cout << "[PASS] test_seeded_coverage_is_aggregated" << std::endl;
}

void test_guided_campaign_is_deterministic() {
    CampaignConfig config;
    config.trials = 8;
    config.base_seed = 9;
    config.parallel = 1;
    config.guidance = SeedGuidance::Coverage;
    config.coverage = true;
    config.initial_trials = 2;
    config.max_corpus = 4;

    const CampaignReport first = cosmos::Campaign::run(config, run_guided_body);
    const CampaignReport second = cosmos::Campaign::run(config, run_guided_body);
    assert(first.corpus.size() == second.corpus.size());
    for (size_t index = 0; index < first.corpus.size(); ++index) {
        assert(first.corpus[index].seed == second.corpus[index].seed);
        assert(first.corpus[index].parent_seed == second.corpus[index].parent_seed);
        assert(first.corpus[index].index == second.corpus[index].index);
        assert(first.corpus[index].new_edges == second.corpus[index].new_edges);
    }
    assert(first.coverage.unique_edges == second.coverage.unique_edges);
    std::cout << "[PASS] test_guided_campaign_is_deterministic" << std::endl;
}

void test_guided_campaign_keeps_a_novelty_corpus() {
    CampaignConfig config;
    config.trials = 12;
    config.base_seed = 1;
    config.parallel = 1;
    config.guidance = SeedGuidance::Coverage;
    config.coverage = true;
    config.initial_trials = 2;
    config.max_corpus = 8;
    config.program = "test_coverage";

    const CampaignReport report = cosmos::Campaign::run(config, run_guided_body);
    assert(!report.refused);
    assert(report.runs == 12);
    assert(report.guidance == SeedGuidance::Coverage);
    assert(report.coverage.enabled);
    assert(report.coverage.runs_with_coverage == 12);
    assert(report.coverage.unique_edges > 0);
    assert(!report.corpus.empty());
    assert(report.coverage.corpus_size == report.corpus.size());
    assert(report.corpus.size() <= config.max_corpus);

    bool saw_novel = false;
    bool saw_seed = false;
    for (const cosmos::CorpusEntry& entry : report.corpus) {
        saw_novel = saw_novel || entry.new_edges > 0;
        saw_seed = saw_seed || entry.seed != 0;
    }
    assert(saw_novel);
    assert(saw_seed);
    std::cout << "[PASS] test_guided_campaign_keeps_a_novelty_corpus" << std::endl;
}

} // namespace

int main() {
    test_bitmap_records_edges_and_hashes_differently();
    test_merge_and_novelty_are_set_operations();
    test_seeded_coverage_is_aggregated();
    test_guided_campaign_is_deterministic();
    test_guided_campaign_keeps_a_novelty_corpus();
    std::cout << "All coverage tests passed successfully!" << std::endl;
    return 0;
}
