// Level-2/3 exploration artifact exporter.
//
// It runs two things in one campaign directory:
//   1. a coverage-guided campaign whose body has seed-dependent control flow, so the corpus has
//      more than a trivial initial entry;
//   2. one fork-COW snapshot of a two-fiber universe, branched at the scheduler's first choice.
//
// The branch results are observations. They do not change the canonical trace, and the branch
// report is an additive `exploration.json`, not a second replay input.

#include "cosmos/campaign.hpp"
#include "cosmos/exploration.hpp"
#include "cosmos/export.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/snapshot.hpp"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <pthread.h>
#include <sstream>
#include <string>
#include <vector>

namespace {

using cosmos::BranchContext;
using cosmos::BranchResult;
using cosmos::CampaignConfig;
using cosmos::CampaignReport;
using cosmos::ExplorationBranch;
using cosmos::ExplorationReport;
using cosmos::FaultConfig;
using cosmos::Scenario;
using cosmos::ScenarioReport;
using cosmos::Simulator;
using cosmos::Snapshot;
using cosmos::SnapshotInfo;

std::string g_order;

__attribute__((noinline)) void path_alpha() {
    asm volatile("" ::: "memory");
    volatile int value = 0;
    for (int i = 0; i < 3; ++i)
        value = value + i;
    (void)value;
}

__attribute__((noinline)) void path_beta() {
    asm volatile("" ::: "memory");
    volatile int value = 11;
    if (value > 3) value = value * 2;
    (void)value;
}

__attribute__((noinline)) void path_gamma() {
    asm volatile("" ::: "memory");
    volatile int value = 7;
    while (value > 0)
        value = value - 1;
    (void)value;
}

ScenarioReport run_guided_universe(Simulator& sim, uint64_t) {
    auto scenario = Scenario::on(sim, FaultConfig{});
    assert(scenario.has_value());
    scenario->run([&sim] {
        switch (sim.seed() % 3U) {
        case 0:
            path_alpha();
            break;
        case 1:
            path_beta();
            break;
        default:
            path_gamma();
            break;
        }
    });
    scenario->quiesce();
    scenario->check("guided-universe-always", [] { return true; });
    return scenario->report();
}

void* append_a(void*) {
    path_alpha();
    g_order.push_back('A');
    return nullptr;
}

void* append_b(void*) {
    path_beta();
    g_order.push_back('B');
    return nullptr;
}

ExplorationBranch to_exploration_branch(const cosmos::NestedBranch& result) {
    ExplorationBranch branch;
    branch.id = "S" + std::to_string(result.snapshot.id) + "-B" +
                std::to_string(result.context.branch_index);
    branch.snapshot = result.snapshot;
    branch.context = result.context;
    branch.died = result.died;
    branch.signal = result.signal;
    branch.forced_choice_applied = result.forced_choice_applied;
    branch.threw = result.threw;
    branch.error = result.error;
    branch.report = result.report;
    branch.ended_at = result.ended_at;
    branch.wall_ns = 0;
    branch.traced = result.traced;
    branch.trace_hash = result.trace_hash;
    branch.trace_capacity = result.trace_capacity;
    branch.trace_dropped = result.trace_dropped;
    branch.coverage_present = result.coverage_present;
    branch.coverage_method = result.coverage_method;
    branch.coverage_edges = result.coverage_edges;
    branch.coverage_hash = result.coverage_hash;
    return branch;
}

ExplorationBranch to_exploration_branch(const BranchResult& result) {
    ExplorationBranch branch;
    branch.id = "S" + std::to_string(result.snapshot.id) + "-B" +
                std::to_string(result.context.branch_index);
    branch.snapshot = result.snapshot;
    branch.context = result.context;
    branch.died = result.died;
    branch.signal = result.signal;
    branch.forced_choice_applied = result.forced_choice_applied;
    branch.threw = result.threw;
    branch.error = result.error;
    branch.report = result.report;
    branch.ended_at = result.ended_at;
    // Wall time is metadata, but an exported demo artifact should be reproducible; the branch
    // virtual end and event order are the meaningful axes.
    branch.wall_ns = 0;
    branch.traced = result.traced;
    branch.trace_hash = result.trace_hash;
    branch.trace_capacity = result.trace_capacity;
    branch.trace_dropped = result.trace_dropped;
    branch.coverage_present = result.coverage_present;
    branch.coverage_method = result.coverage_method;
    branch.coverage_edges = result.coverage.edges;
    branch.coverage_hash = result.coverage.hash;
    return branch;
}

const char* compiler_name() {
#if defined(__clang__)
    return "clang " __clang_version__;
#elif defined(__GNUC__)
    return "gcc " __VERSION__;
#else
    return "unknown";
#endif
}

} // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : "build/ui-artifacts/exploration";
    const uint64_t trials = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 24;

    CampaignConfig config;
    config.trials = trials;
    config.base_seed = 4242;
    // Guided selection is sequential: the next seed depends on the previous novelty result.
    config.parallel = 1;
    config.guidance = cosmos::SeedGuidance::Coverage;
    config.coverage = true;
    config.initial_trials = 2;
    config.max_corpus = 32;
    config.program = "exploration_campaign";
    config.id = "exploration";
    config.name = "Coverage and snapshot exploration";

    cosmos::ArtifactOptions artifact;
    artifact.root = root;
    artifact.campaign = config;
    artifact.build.id = "build-exploration";
    artifact.build.commit = "0000000000000000000000000000000000000000";
    artifact.build.mode = "SIM";
    artifact.build.compiler = compiler_name();
    artifact.build.host = "fixture";
    artifact.repro = config.program + " --seed {seed}";

    auto writer = cosmos::CampaignArtifactWriter::create(std::move(artifact));
    if (!writer) {
        std::cerr << "exploration_campaign: " << writer.error() << "\n";
        return 1;
    }
    config.on_universe = writer->sink();
    const CampaignReport campaign = cosmos::Campaign::run(config, run_guided_universe);

    ExplorationReport exploration;
    exploration.campaign = campaign;

    // Capture a two-fiber universe after the main workload has spawned both tasks but before any
    // scheduler choice. Each branch forces one ready index, so the same snapshot produces two
    // different orders without replaying the workload.
    {
        auto scenario = Scenario::create(0x5eed5eed, FaultConfig{});
        if (!scenario) {
            std::cerr << "exploration_campaign: the snapshot scenario was refused\n";
            return 1;
        }
        Scenario& universe = *scenario;
        g_order.clear();
        bool spawned = true;
        universe.run([&universe, &spawned] {
            pthread_t a = 0;
            pthread_t b = 0;
            if (universe.simulator().spawn_node(0, &a, append_a, nullptr) != 0) {
                spawned = false;
                return;
            }
            if (universe.simulator().spawn_node(0, &b, append_b, nullptr) != 0) {
                spawned = false;
            }
        });
        if (!spawned) {
            std::cerr << "exploration_campaign: could not spawn snapshot fibers\n";
            return 1;
        }

        auto snapshot =
            Snapshot::capture(universe, [](Scenario& branch, const BranchContext& context) {
                if (context.branch_index == 0) {
                    // A nested capture demonstrates that a branch can contain its own checkpoint;
                    // the artifact flattens the resulting tree with parent ids.
                    auto nested = Snapshot::capture(branch);
                    if (nested) {
                        BranchContext nested_context;
                        nested_context.branch_index = 0;
                        (void)nested->explore(nested_context);
                    }
                }
                branch.drain();
                branch.quiesce();
                branch.check("scheduler-order-ab", [] { return g_order == "AB"; });
            });
        if (!snapshot) {
            std::cerr << "exploration_campaign: " << snapshot.error() << "\n";
            return 1;
        }
        exploration.snapshots.push_back(snapshot->info());

        for (uint64_t branch_index = 0; branch_index < 2; ++branch_index) {
            BranchContext context;
            context.branch_index = branch_index;
            context.has_forced_choice = true;
            context.forced_choice = branch_index;
            const BranchResult result = snapshot->explore(context);
            exploration.branches.push_back(to_exploration_branch(result));
            for (const cosmos::SnapshotInfo& nested : result.nested_snapshots) {
                exploration.snapshots.push_back(nested);
            }
            for (const cosmos::NestedBranch& nested : result.nested_branches) {
                exploration.branches.push_back(to_exploration_branch(nested));
            }
        }
    }

    if (!exploration.snapshots.empty() && exploration.snapshots.front().ready_choices < 2) {
        std::cerr << "exploration_campaign: snapshot has fewer than two ready choices\n";
        return 1;
    }

    std::ostringstream exploration_json;
    cosmos::print_exploration_json(exploration_json, exploration);
    if (auto written = writer->write_exploration(exploration_json.str()); !written) {
        std::cerr << "exploration_campaign: " << written.error() << "\n";
        return 1;
    }
    if (auto finished = writer->finish(campaign); !finished) {
        std::cerr << "exploration_campaign: " << finished.error() << "\n";
        return 1;
    }

    std::cout << "wrote " << root << " (" << campaign.runs << " universes, "
              << campaign.corpus.size() << " corpus seeds, " << exploration.branches.size()
              << " branches, " << exploration.snapshots.size() << " snapshot)\n";
    cosmos::print_campaign_report(std::cout, campaign);
    return 0;
}
