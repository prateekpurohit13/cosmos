// Writes a v1 UI artifact directory from the replicated-KV distributed example.
//
// The example itself is a plain POSIX program (examples/distributed/replicated_kv.c); this tool is
// the campaign harness that runs it once per seed and exports the result for the read-only UI. It
// uses the same partition-plus-crash plan as tests/test_distributed.cpp, so the finding it writes
// is the same acked-write-lost bug the test suite proves.
//
//   replicated_kv_ui_export [output-dir] [campaign-id] [trials]
//   replicated_kv_ui_export ui/fixtures/replicated-kv replicated-kv 4

#include "cosmos/campaign.hpp"
#include "cosmos/export.hpp"
#include "cosmos/minimize.hpp"
#include "cosmos/minimize_json.hpp"
#include "cosmos/scenario.hpp"
#include "replicated_kv.h"

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <pthread.h>
#include <sstream>
#include <string>
#include <time.h>
#include <utility>

namespace {

using cosmos::Campaign;
using cosmos::CampaignConfig;
using cosmos::CrashNode;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::MinimizeConfig;
using cosmos::MinimizeReport;
using cosmos::NodeUniverseFn;
using cosmos::Partition;
using cosmos::Scenario;
using cosmos::ScenarioReport;
using cosmos::Simulator;
using cosmos::SiteId;
using cosmos::Time;

constexpr uint32_t kNodes = RKV_MAX_NODES;
constexpr const char* kKey = "k";
constexpr const char* kValue = "v";
constexpr const char* kFinding = "acked-write-is-replicated";

int g_client_rc = 99;

struct NodeArg {
    int node = 0;
};

void* node_thread(void* raw) {
    NodeArg* arg = static_cast<NodeArg*>(raw);
    (void)rkv_node_main(arg->node, kNodes, RKV_BASE_PORT);
    return nullptr;
}

// The §17 plan: partition the primary from one backup and kill the other at the same instant.
FaultConfig distributed_plan() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Process);
    cfg.enable_class(FaultClass::Network);
    cfg.max_crashed_nodes = 1;
    cfg.min_healthy_quorum = 0;
    assert(cfg.activate_site(SiteId::crash_node));
    assert(cfg.activate_site(SiteId::partition));
    cfg.scheduled_episodes.push_back(
        {Time{1'000'000}, Partition{{0}, {1}}, cosmos::Duration{10'000'000}});
    cfg.scheduled_episodes.push_back({Time{1'000'000}, CrashNode{2}, cosmos::Duration{5'000'000}});
    return cfg;
}

// Drives one universe: start the cluster, write during the fault window, heal, stop, settle, check.
void run_cluster(Scenario& scenario) {
    rkv_reset();
    g_client_rc = 99;
    Simulator& sim = scenario.simulator();
    NodeArg args[kNodes];

    scenario.run([&] {
        pthread_t threads[kNodes] = {};
        for (int n = 1; n < static_cast<int>(kNodes); ++n) {
            args[n].node = n;
            assert(sim.spawn_node(static_cast<cosmos::NodeId>(n), &threads[n], node_thread,
                                  &args[n]) == 0);
        }
        args[0].node = 0;
        assert(sim.spawn_node(0, &threads[0], node_thread, &args[0]) == 0);

        timespec settle{};
        settle.tv_nsec = 1'000'000L;
        nanosleep(&settle, nullptr); // t=1ms: scheduled episodes fire before this wake
        g_client_rc = rkv_client_put(RKV_BASE_PORT, kKey, kValue);

        auto* injector = sim.injector_or_null();
        if (injector != nullptr) injector->heal_all();
        nanosleep(&settle, nullptr); // let the healed follower settle
        rkv_shutdown();              // wakes every blocked fiber deterministically

        for (int n = 0; n < static_cast<int>(kNodes); ++n) {
            assert(pthread_join(threads[n], nullptr) == 0);
        }
    });

    scenario.quiesce();

    const bool acked = g_client_rc == 0;
    const bool on_backup = rkv_node_has(1, kKey, kValue) != 0 || rkv_node_has(2, kKey, kValue) != 0;
    scenario.check(kFinding, [acked, on_backup] { return !acked || on_backup; });
}

ScenarioReport run_universe(Simulator& sim, uint64_t index, uint32_t node_count) {
    (void)index;
    auto scenario = Scenario::on(sim, distributed_plan(), node_count);
    assert(scenario.has_value());
    run_cluster(*scenario);
    return scenario->report();
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
    const std::string root = argc > 1 ? argv[1] : "ui/fixtures/replicated-kv";
    const std::string campaign_id = argc > 2 ? argv[2] : "replicated-kv";
    const uint64_t trials = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 4;

    CampaignConfig campaign;
    campaign.trials = trials;
    campaign.base_seed = 1;
    campaign.parallel = 1;
    campaign.node_count = kNodes;
    campaign.program = "replicated_kv";
    campaign.id = campaign_id;
    campaign.name = "Replicated KV campaign";
    campaign.trace = true;
    campaign.trace_capacity = 4096;
    campaign.events = true;
    campaign.event_capacity = 2048;
    campaign.state = true;

    cosmos::ArtifactOptions options;
    options.root = root;
    options.campaign = campaign;
    options.build.id = "build-rkv";
    options.build.commit = "0000000000000000000000000000000000000000";
    options.build.mode = "SIM";
    options.build.compiler = compiler_name();
    options.build.host = "replicated-kv";

    auto writer = cosmos::CampaignArtifactWriter::create(std::move(options));
    if (!writer) {
        std::cerr << "replicated_kv_ui_export: " << writer.error() << "\n";
        return 1;
    }

    // The writer receives every outcome in index order. Keep the trace bytes for failing universes
    // so minimization can replay them after the campaign; a passing trace is not a candidate.
    std::map<uint64_t, std::string> trace_by_seed;
    const auto writer_sink = writer->sink();
    campaign.on_universe = [&](const cosmos::detail::UniverseOutcome& outcome) {
        if (!outcome.report.no_check_failed || outcome.died || !outcome.threw.empty()) {
            trace_by_seed[outcome.seed] = outcome.trace_bytes;
        }
        writer_sink(outcome);
    };

    const cosmos::CampaignReport report = Campaign::run(campaign, NodeUniverseFn(run_universe));

    // Minimization is report-only (O5): run it after the campaign over the recorded traces and
    // write one document per failing seed. A refused report is an artifact too, not an omission.
    std::map<uint64_t, std::string> minimized_findings;
    uint64_t minimized = 0;
    auto write_refused = [&](uint64_t seed, const std::string& finding_id, std::string reason) {
        MinimizeReport refused;
        refused.seed = seed;
        refused.finding_id = finding_id;
        refused.program = campaign.program;
        refused.refused = true;
        refused.reason = std::move(reason);
        std::ostringstream json;
        cosmos::print_minimize_json(json, refused);
        if (auto written = writer->write_minimization(seed, json.str()); !written) {
            std::cerr << "replicated_kv_ui_export: " << written.error() << "\n";
            return false;
        }
        minimized_findings[seed] = finding_id;
        ++minimized;
        return true;
    };

    for (const cosmos::Finding& finding : report.findings) {
        if (cosmos::is_harness_id(finding.id) || finding.id.empty()) continue;
        for (const uint64_t seed : finding.seeds) {
            if (minimized_findings.find(seed) != minimized_findings.end()) continue;
            const auto trace_bytes = trace_by_seed.find(seed);
            if (trace_bytes == trace_by_seed.end() || trace_bytes->second.empty()) {
                if (!write_refused(seed, finding.id,
                                   "no canonical trace bytes were exported for this seed")) {
                    return 1;
                }
                continue;
            }
            const auto source = cosmos::decode_trace(trace_bytes->second);
            if (!source) {
                if (!write_refused(seed, finding.id, "the canonical trace could not be decoded")) {
                    return 1;
                }
                continue;
            }

            MinimizeConfig config;
            config.plan = distributed_plan();
            config.finding_id = finding.id;
            config.node_count = kNodes;
            config.program = campaign.program;
            const MinimizeReport minimized_report = cosmos::minimize(
                *source, config, [](Scenario& scenario) { run_cluster(scenario); });
            std::ostringstream json;
            cosmos::print_minimize_json(json, minimized_report);
            if (auto written = writer->write_minimization(seed, json.str()); !written) {
                std::cerr << "replicated_kv_ui_export: " << written.error() << "\n";
                return 1;
            }
            minimized_findings[seed] = finding.id;
            ++minimized;
        }
    }

    if (auto finished = writer->finish(report); !finished) {
        std::cerr << "replicated_kv_ui_export: " << finished.error() << "\n";
        return 1;
    }

    std::cout << "wrote " << root << " (" << report.runs << " universes, " << report.failed_runs
              << " failed, " << report.findings.size() << " findings, " << minimized
              << " minimization reports)\n";
    cosmos::print_campaign_report(std::cout, report);
    return 0;
}
