// Real replicated-KV campaign harness.
//
// The distributed example is a plain POSIX program (examples/distributed/replicated_kv.c). This
// tool runs it as a campaign, with real build provenance, per-universe swarm-sampled fault plans,
// process/network episodes, canonical traces, the inspection event stream, final state and
// minimization reports.
//
// Modes:
//   regression  exact partition + crash plan from tests/test_distributed.cpp (known repro)
//   swarm       FaultConfig::sample only: wrapper-site rates, no process/network episodes
//   mixed       swarm rules + one partition + one crash in the replication window (default)
//   chaos       swarm rules + a randomized victim backup and crash target
//
// Build provenance is supplied by the caller (the Justfile does this automatically):
//   COSMOS_BUILD_COMMIT, COSMOS_BUILD_BINARY_HASH, COSMOS_BUILD_DIRTY, COSMOS_BUILD_HOST,
//   COSMOS_BUILD_ID
//
//   replicated_kv_campaign [options]
//   replicated_kv_campaign --root build/ui-artifacts/replicated-kv --trials 256 --verify

#include "cosmos/campaign.hpp"
#include "cosmos/export.hpp"
#include "cosmos/minimize.hpp"
#include "cosmos/minimize_json.hpp"
#include "cosmos/random.hpp"
#include "cosmos/scenario.hpp"
#include "replicated_kv.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <pthread.h>
#include <sstream>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using cosmos::Campaign;
using cosmos::CampaignConfig;
using cosmos::CheckResult;
using cosmos::CrashNode;
using cosmos::Duration;
using cosmos::FaultClass;
using cosmos::FaultConfig;
using cosmos::MinimizeConfig;
using cosmos::MinimizeReport;
using cosmos::NodeUniverseFn;
using cosmos::Partition;
using cosmos::Rng;
using cosmos::Scenario;
using cosmos::ScenarioReport;
using cosmos::Simulator;
using cosmos::SiteId;
using cosmos::StreamDomain;
using cosmos::Time;

constexpr uint32_t kNodes = RKV_MAX_NODES;
constexpr const char* kKey = "k";
constexpr const char* kValue = "v";
constexpr const char* kFinding = "acked-write-is-replicated";

int g_client_rc = 99;

struct Options {
    std::string root = "build/ui-artifacts/replicated-kv";
    std::string id = "replicated-kv";
    uint64_t trials = 256;
    uint64_t base_seed = 1;
    unsigned parallel = 0; // 0 = hardware concurrency
    bool verify = false;
    std::string mode = "mixed";
    uint64_t trace_capacity = 8192;
    uint64_t event_capacity = 4096;
    bool events = true;
    bool state = true;
    uint64_t minimize_per_finding = 1;

    std::string build_id;
    std::string commit;
    std::string binary_hash;
    std::string host;
    bool dirty = false;

    bool has_seed = false;
    uint64_t seed = 0;
};

struct NodeArg {
    int node = 0;
};

void* node_thread(void* raw) {
    NodeArg* arg = static_cast<NodeArg*>(raw);
    (void)rkv_node_main(arg->node, kNodes, RKV_BASE_PORT);
    return nullptr;
}

// The exact regression plan from tests/test_distributed.cpp.
FaultConfig regression_plan() {
    FaultConfig cfg;
    cfg.enable_class(FaultClass::Process);
    cfg.enable_class(FaultClass::Network);
    cfg.max_crashed_nodes = 1;
    cfg.min_healthy_quorum = 0;
    assert(cfg.activate_site(SiteId::crash_node));
    assert(cfg.activate_site(SiteId::partition));
    cfg.scheduled_episodes.push_back({Time{1'000'000}, Partition{{0}, {1}}, Duration{10'000'000}});
    cfg.scheduled_episodes.push_back({Time{1'000'000}, CrashNode{2}, Duration{5'000'000}});
    return cfg;
}

// Build the plan from the universe seed. The campaign uses the same function, so
// `--seed <finding seed>` reproduces the exact realized plan.
FaultConfig make_plan_for_seed(const Options& opts, uint64_t seed) {
    if (opts.mode == "regression") return regression_plan();

    Rng swarm(cosmos::stream_seed(seed, StreamDomain::Swarm));
    FaultConfig cfg = FaultConfig::sample(swarm);
    if (opts.mode == "swarm") {
        cfg.warmup_until = Time{100'000};
        cfg.quiesce_after = Time{10'000'000};
        return cfg;
    }

    // Process and network episodes are not wrapper calls, so arm them explicitly. The swarm
    // sampler still supplies the wrapper-site rules and activation set above.
    cfg.enable_class(FaultClass::Process);
    cfg.enable_class(FaultClass::Network);
    (void)cfg.activate_site(SiteId::crash_node);
    (void)cfg.activate_site(SiteId::partition);
    cfg.max_crashed_nodes = 1;
    cfg.min_healthy_quorum = 2; // 3 nodes, survive one crash without dropping below two live
    cfg.warmup_until = Time{100'000};
    cfg.quiesce_after = Time{10'000'000};

    // Put the fault inside the replication window (the workload writes at ~1ms) and vary it by
    // seed. This is a focused real campaign, not a hand-written fixed episode.
    const uint64_t at = 500'000 + swarm.range(0, 1'000'000);
    const uint64_t partition_until = 1'000'000 + swarm.range(0, 4'000'000);
    const uint64_t crash_until = 1'000'000 + swarm.range(0, 4'000'000);
    const int victim = opts.mode == "chaos" ? 1 + static_cast<int>(swarm.range(0, 1)) : 1;
    const int crashed = 3 - victim;

    cfg.scheduled_episodes.push_back({Time{static_cast<int64_t>(at)},
                                      Partition{{0u}, {static_cast<cosmos::NodeId>(victim)}},
                                      Duration{static_cast<int64_t>(partition_until)}});
    cfg.scheduled_episodes.push_back({Time{static_cast<int64_t>(at)},
                                      CrashNode{static_cast<cosmos::NodeId>(crashed)},
                                      Duration{static_cast<int64_t>(crash_until)}});
    return cfg;
}

FaultConfig make_plan(const Options& opts, uint64_t index) {
    return make_plan_for_seed(opts, cosmos::universe_seed(opts.base_seed, index));
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

    bool saw_partition = false;
    bool saw_crash = false;
    bool saw_wrapper = false;
    if (const auto* injector = sim.injector_or_null()) {
        for (const auto& row : injector->ledger()) {
            if (row.site == SiteId::partition) saw_partition = true;
            if (row.site == SiteId::crash_node) saw_crash = true;
            if (static_cast<uint64_t>(row.site) < 1000) saw_wrapper = true;
        }
    }

    scenario.note_covered("partition-fired", saw_partition);
    scenario.note_covered("crash-node-fired", saw_crash);
    scenario.note_covered("wrapper-fault-fired", saw_wrapper);
    scenario.note_covered("client-acked", acked);
    scenario.note_covered("backup-has-write", on_backup);
    scenario.note_covered("write-refused", !acked);

    scenario.check(kFinding, [acked, on_backup] { return !acked || on_backup; });
    scenario.check_no_leaks("no-active-allocations");
}

ScenarioReport run_universe(Simulator& sim, uint64_t index, uint32_t node_count,
                            const FaultConfig& plan) {
    (void)index;
    auto scenario = Scenario::on(sim, plan, node_count);
    if (!scenario) {
        ScenarioReport rejected;
        rejected.seed = sim.seed();
        rejected.no_check_failed = false;
        rejected.checks.push_back(
            CheckResult{cosmos::kConfigId, "fault plan was rejected", false, Time::zero()});
        return rejected;
    }
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

std::string env_string(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
}

bool env_bool(const char* name) {
    const std::string value = env_string(name);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool parse_u64(const char* text, uint64_t& out) {
    if (text == nullptr || *text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    out = static_cast<uint64_t>(value);
    return true;
}

void usage(const char* argv0) {
    std::cout
        << "usage: " << argv0 << " [options]\n"
        << "  --root DIR              artifact campaign directory (default "
           "build/ui-artifacts/replicated-kv)\n"
        << "  --id ID                 campaign id (default replicated-kv)\n"
        << "  --trials N              universes (default 256)\n"
        << "  --base-seed N           campaign base seed (default 1)\n"
        << "  --parallel N            worker processes, 0 = hardware concurrency (default 0)\n"
        << "  --verify                run each universe twice and compare report + trace hash\n"
        << "  --mode NAME             regression | swarm | mixed | chaos (default mixed)\n"
        << "  --trace-capacity N      canonical trace entries per universe (default 8192)\n"
        << "  --event-capacity N      inspection event entries per universe (default 4096)\n"
        << "  --no-events             do not capture the inspection event stream\n"
        << "  --no-state              do not capture the final state document\n"
        << "  --minimize-per-finding N  failing seeds minimized per finding, 0 = none (default 1)\n"
        << "  --seed N                run one seed directly (repro mode; no artifacts)\n"
        << "  --build-id ID           build identity (or COSMOS_BUILD_ID)\n"
        << "  --commit HASH           source commit (or COSMOS_BUILD_COMMIT)\n"
        << "  --binary-hash HASH      executable hash (or COSMOS_BUILD_BINARY_HASH)\n"
        << "  --host NAME             build host (or COSMOS_BUILD_HOST)\n"
        << "  --dirty                 mark the build dirty (or COSMOS_BUILD_DIRTY=1)\n"
        << "  --clean                 mark the build clean\n"
        << "  --help                  this text\n";
}

bool parse_args(int argc, char** argv, Options& opts, bool& show_help) {
    show_help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            show_help = true;
        } else if (arg == "--root") {
            const char* v = value("--root");
            if (v == nullptr) return false;
            opts.root = v;
        } else if (arg == "--id") {
            const char* v = value("--id");
            if (v == nullptr) return false;
            opts.id = v;
        } else if (arg == "--trials") {
            const char* v = value("--trials");
            if (v == nullptr || !parse_u64(v, opts.trials) || opts.trials == 0) return false;
        } else if (arg == "--base-seed") {
            const char* v = value("--base-seed");
            if (v == nullptr || !parse_u64(v, opts.base_seed)) return false;
        } else if (arg == "--parallel") {
            const char* v = value("--parallel");
            uint64_t n = 0;
            if (v == nullptr || !parse_u64(v, n) || n > UINT32_MAX) return false;
            opts.parallel = static_cast<unsigned>(n);
        } else if (arg == "--verify") {
            opts.verify = true;
        } else if (arg == "--mode") {
            const char* v = value("--mode");
            if (v == nullptr) return false;
            opts.mode = v;
            if (opts.mode != "regression" && opts.mode != "swarm" && opts.mode != "mixed" &&
                opts.mode != "chaos") {
                std::cerr << "unknown --mode: " << opts.mode << "\n";
                return false;
            }
        } else if (arg == "--trace-capacity") {
            const char* v = value("--trace-capacity");
            if (v == nullptr || !parse_u64(v, opts.trace_capacity) || opts.trace_capacity == 0)
                return false;
        } else if (arg == "--event-capacity") {
            const char* v = value("--event-capacity");
            if (v == nullptr || !parse_u64(v, opts.event_capacity) || opts.event_capacity == 0)
                return false;
        } else if (arg == "--no-events") {
            opts.events = false;
        } else if (arg == "--no-state") {
            opts.state = false;
        } else if (arg == "--minimize-per-finding") {
            const char* v = value("--minimize-per-finding");
            if (v == nullptr || !parse_u64(v, opts.minimize_per_finding)) return false;
        } else if (arg == "--seed") {
            const char* v = value("--seed");
            if (v == nullptr || !parse_u64(v, opts.seed)) return false;
            opts.has_seed = true;
        } else if (arg == "--build-id") {
            const char* v = value("--build-id");
            if (v == nullptr) return false;
            opts.build_id = v;
        } else if (arg == "--commit") {
            const char* v = value("--commit");
            if (v == nullptr) return false;
            opts.commit = v;
        } else if (arg == "--binary-hash") {
            const char* v = value("--binary-hash");
            if (v == nullptr) return false;
            opts.binary_hash = v;
        } else if (arg == "--host") {
            const char* v = value("--host");
            if (v == nullptr) return false;
            opts.host = v;
        } else if (arg == "--dirty") {
            opts.dirty = true;
        } else if (arg == "--clean") {
            opts.dirty = false;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

std::string short_commit(const std::string& commit) {
    return commit.size() <= 12 ? commit : commit.substr(0, 12);
}

} // namespace

int main(int argc, char** argv) {
    Options opts;
    opts.commit = env_string("COSMOS_BUILD_COMMIT");
    opts.binary_hash = env_string("COSMOS_BUILD_BINARY_HASH");
    opts.host = env_string("COSMOS_BUILD_HOST");
    opts.build_id = env_string("COSMOS_BUILD_ID");
    opts.dirty = env_bool("COSMOS_BUILD_DIRTY");

    bool show_help = false;
    if (!parse_args(argc, argv, opts, show_help)) {
        usage(argv[0]);
        return 2;
    }
    if (show_help) {
        usage(argv[0]);
        return 0;
    }

    if (opts.host.empty()) {
        char hostname[256] = {};
        if (gethostname(hostname, sizeof(hostname) - 1) == 0) opts.host = hostname;
    }
    if (opts.host.empty()) opts.host = "unknown";
    if (opts.build_id.empty()) {
        opts.build_id = opts.commit.empty() ? "replicated-kv-campaign"
                                            : "replicated-kv-" + short_commit(opts.commit) +
                                                  (opts.dirty ? "-dirty" : "");
    }

    CampaignConfig campaign;
    campaign.trials = opts.trials;
    campaign.base_seed = opts.base_seed;
    campaign.parallel =
        opts.parallel == 0 ? std::max(1u, std::thread::hardware_concurrency()) : opts.parallel;
    campaign.node_count = kNodes;
    campaign.verify = opts.verify;
    campaign.trace = true;
    campaign.trace_capacity = opts.trace_capacity;
    campaign.events = opts.events;
    campaign.event_capacity = opts.event_capacity;
    campaign.state = opts.state;
    campaign.program =
        (argc > 0 && argv[0] != nullptr ? std::string(argv[0]) : "replicated_kv_campaign") +
        " --mode " + opts.mode;
    campaign.id = opts.id;
    campaign.name = "Replicated KV campaign";

    // Repro mode: one seed, same plan derivation as the campaign, no artifact write.
    if (opts.has_seed) {
        Simulator sim(opts.seed);
        auto scenario = Scenario::on(sim, make_plan_for_seed(opts, opts.seed), kNodes);
        if (!scenario) {
            std::cerr << "replicated_kv_campaign: fault plan was rejected for seed " << opts.seed
                      << "\n";
            return 1;
        }
        run_cluster(*scenario);
        const ScenarioReport report = scenario->report();
        cosmos::print_report(std::cout, report);
        if (cosmos::failed(report)) {
            if (const auto* injector = sim.injector_or_null()) {
                cosmos::print_ledger(std::cout, *injector);
            }
            return 1;
        }
        return 0;
    }

    // Precompute the realized plan for every universe in the parent. The workers use the same
    // plan, and the parent keeps it for minimization after the campaign.
    std::vector<FaultConfig> plans;
    plans.reserve(static_cast<size_t>(opts.trials));
    for (uint64_t index = 0; index < opts.trials; ++index) {
        plans.push_back(make_plan(opts, index));
    }

    cosmos::ArtifactOptions artifact;
    artifact.root = opts.root;
    artifact.campaign = campaign;
    artifact.build.id = opts.build_id;
    artifact.build.commit = opts.commit;
    artifact.build.dirty = opts.dirty;
    artifact.build.binary_hash = opts.binary_hash;
    artifact.build.mode = "SIM";
    artifact.build.compiler = compiler_name();
    artifact.build.host = opts.host;
    artifact.repro = campaign.program + " --seed {seed}";

    auto writer = cosmos::CampaignArtifactWriter::create(std::move(artifact));
    if (!writer) {
        std::cerr << "replicated_kv_campaign: " << writer.error() << "\n";
        return 1;
    }

    std::map<uint64_t, uint64_t> index_by_seed;
    std::map<uint64_t, std::string> trace_by_seed;
    const auto writer_sink = writer->sink();
    campaign.on_universe = [&](const cosmos::detail::UniverseOutcome& outcome) {
        index_by_seed[outcome.seed] = outcome.index;
        if (!outcome.report.no_check_failed || outcome.died || !outcome.threw.empty()) {
            trace_by_seed[outcome.seed] = outcome.trace_bytes;
        }
        writer_sink(outcome);
    };

    std::cout << "campaign " << opts.id << ": " << opts.trials << " universes, base seed "
              << opts.base_seed << ", mode " << opts.mode << ", parallel " << campaign.parallel
              << ", verify " << (opts.verify ? "on" : "off") << "\n";
    std::cout << "build " << opts.build_id << " commit " << short_commit(opts.commit)
              << (opts.dirty ? " dirty" : " clean") << " binary "
              << (opts.binary_hash.empty() ? "unknown" : opts.binary_hash.substr(0, 12)) << "\n";

    const cosmos::CampaignReport report = Campaign::run(
        campaign, NodeUniverseFn([&plans](Simulator& sim, uint64_t index, uint32_t node_count) {
            if (index >= plans.size()) {
                ScenarioReport rejected;
                rejected.seed = sim.seed();
                rejected.no_check_failed = false;
                rejected.checks.push_back(CheckResult{cosmos::kConfigId,
                                                      "universe index has no precomputed plan",
                                                      false, Time::zero()});
                return rejected;
            }
            return run_universe(sim, index, node_count, plans[static_cast<size_t>(index)]);
        }));

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
            std::cerr << "replicated_kv_campaign: " << written.error() << "\n";
            return false;
        }
        minimized_findings[seed] = finding_id;
        ++minimized;
        return true;
    };

    for (const cosmos::Finding& finding : report.findings) {
        if (cosmos::is_harness_id(finding.id) || finding.id.empty()) continue;
        uint64_t minimized_here = 0;
        for (const uint64_t seed : finding.seeds) {
            if (minimized_here >= opts.minimize_per_finding) break;
            if (minimized_findings.find(seed) != minimized_findings.end()) continue;

            const auto index_it = index_by_seed.find(seed);
            if (index_it == index_by_seed.end() || index_it->second >= plans.size()) {
                if (!write_refused(seed, finding.id,
                                   "the universe index was not recorded for this seed")) {
                    return 1;
                }
                ++minimized_here;
                continue;
            }

            const auto trace_bytes = trace_by_seed.find(seed);
            if (trace_bytes == trace_by_seed.end() || trace_bytes->second.empty()) {
                if (!write_refused(seed, finding.id,
                                   "no canonical trace bytes were exported for this seed")) {
                    return 1;
                }
                ++minimized_here;
                continue;
            }
            const auto source = cosmos::decode_trace(trace_bytes->second);
            if (!source) {
                if (!write_refused(seed, finding.id, "the canonical trace could not be decoded")) {
                    return 1;
                }
                ++minimized_here;
                continue;
            }

            MinimizeConfig config;
            config.plan = plans[static_cast<size_t>(index_it->second)];
            config.finding_id = finding.id;
            config.node_count = kNodes;
            config.program = campaign.program;
            const MinimizeReport minimized_report = cosmos::minimize(
                *source, config, [](Scenario& scenario) { run_cluster(scenario); });
            std::ostringstream json;
            cosmos::print_minimize_json(json, minimized_report);
            if (auto written = writer->write_minimization(seed, json.str()); !written) {
                std::cerr << "replicated_kv_campaign: " << written.error() << "\n";
                return 1;
            }
            minimized_findings[seed] = finding.id;
            ++minimized;
            ++minimized_here;
        }
    }

    if (auto finished = writer->finish(report); !finished) {
        std::cerr << "replicated_kv_campaign: " << finished.error() << "\n";
        return 1;
    }

    std::cout << "wrote " << opts.root << " (" << report.runs << " universes, "
              << report.failed_runs << " failed, " << report.findings.size() << " findings, "
              << minimized << " minimization reports)\n";
    const std::filesystem::path ui_root = std::filesystem::path(opts.root).parent_path();
    std::cout << "serve: nix develop -c just ui \"" << (ui_root.empty() ? "." : ui_root.string())
              << "\"\n";
    cosmos::print_campaign_report(std::cout, report);
    return 0;
}
