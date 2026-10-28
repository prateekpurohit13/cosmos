// Scripted single-universe demo tool (docs/demo/DEMO.md).
//
// One universe, one named fault plan, one step artifact. This exists because the campaign harness
// samples its plans (`--mode swarm|mixed|chaos`) and the demonstration needs the opposite: a plan
// chosen by name, fired on a counted eligible call or at a virtual timestamp, with no draw at all.
// The ledger is therefore identical on every run, so the demo script can assert on it.
//
//   cosmos_demo --list
//   cosmos_demo --fault drop-forward --root /tmp/demo/04 --id step-04
//   cosmos_demo --fault none --seed 1 --root /tmp/demo/00 --id step-00
//
// Exit code is 0 whenever the universe ran; the verdict is the last line,
// `DEMO RESULT: PASS|FAIL acked-write-is-replicated`. The script compares it to the expectation.

#include "cosmos/campaign.hpp"
#include "cosmos/export.hpp"
#include "cosmos/ledger_print.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/trace.hpp"
#include "replicated_kv.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <pthread.h>
#include <sstream>
#include <string>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using namespace cosmos;

constexpr uint32_t kNodes = RKV_MAX_NODES;
constexpr const char* kPromise = "acked-write-is-replicated";
constexpr const char* kKey = "k";
constexpr const char* kValue = "v";

// The window every plan shares: warm up almost immediately (this fixture has no start-up cost
// worth protecting), and settle for ten milliseconds of virtual time, which is long enough for
// every repair a plan can schedule.
constexpr Time kWarmup = Time{100'000};
constexpr Time kQuiesce = Time{10'000'000};

struct Plan {
    FaultConfig cfg;
    std::string claim;
};

struct Options {
    std::string fault = "none";
    std::string mode = "chaos"; // chaos: victim/target/timing from the seed. scripted: fixed.
    std::string root;
    std::string id;
    uint64_t seed = 1;
    std::string build_id;
    std::string commit;
    std::string binary_hash;
    std::string host;
    bool dirty = false;
};

int g_client_rc = 99;

struct NodeArg {
    int node = 0;
};

void* node_thread(void* raw) {
    NodeArg* arg = static_cast<NodeArg*>(raw);
    (void)rkv_node_main(arg->node, kNodes, RKV_BASE_PORT);
    return nullptr;
}

// A point fault that fires on an exact eligible call and consumes no draw (§10.1).
void point_trigger(FaultConfig& cfg, FaultClass cls, SiteId site, FaultKind kind, uint64_t eligible,
                   Duration amount = Duration::zero()) {
    cfg.enable_class(cls);
    (void)cfg.activate_site(site);
    FaultRule rule;
    (void)rule.outcomes.add(kind, 1.0);
    rule.fire_on_eligible_call = eligible;
    rule.amount = amount;
    (void)cfg.set_rule(site, rule);
}

// A point fault that fires on every eligible call: used by the plan that refuses every connection.
void point_always(FaultConfig& cfg, FaultClass cls, SiteId site, FaultKind kind) {
    cfg.enable_class(cls);
    (void)cfg.activate_site(site);
    FaultRule rule;
    rule.rate = 1.0;
    (void)rule.outcomes.add(kind, 1.0);
    (void)cfg.set_rule(site, rule);
}

// Fires on every eligible call after `skip`, at most `budget` times, and consumes no draw.
void point_every_after(FaultConfig& cfg, FaultClass cls, SiteId site, FaultKind kind, uint64_t skip,
                       uint64_t budget) {
    cfg.enable_class(cls);
    (void)cfg.activate_site(site);
    FaultRule rule;
    rule.rate = 1.0;
    rule.skip_first = skip;
    rule.max_injections = budget;
    (void)rule.outcomes.add(kind, 1.0);
    (void)cfg.set_rule(site, rule);
}

FaultConfig window() {
    FaultConfig cfg;
    cfg.warmup_until = kWarmup;
    cfg.quiesce_after = kQuiesce;
    // The promise the fixture declares: at most one machine down, never fewer than two alive.
    cfg.max_crashed_nodes = 1;
    cfg.min_healthy_quorum = 2;
    return cfg;
}

void episode(FaultConfig& cfg, SiteId site, ScheduledEpisode scheduled) {
    cfg.enable_class(FaultClass::Process);
    (void)cfg.activate_site(site);
    cfg.scheduled_episodes.push_back(std::move(scheduled));
}

std::vector<std::pair<std::string, std::string>> plan_names() {
    return {
        {"none", "control: no fault at all, so the ack is expected to be replicated"},
        {"drop-forward", "one replication packet is silently lost; send() still reports success"},
        {"delay-forward", "one replication packet arrives late; the primary does not wait"},
        {"short-send", "one send transfers half a frame; send_all loops and completes it"},
        {"reset-forward", "the forward to backup 1 is reset; the primary drops that follower"},
        {"refuse-client", "every connection is refused; the client must refuse the write"},
        {"interrupt-sleep", "the client's settle sleep returns early; the write races start-up"},
        {"partition", "node 0 is split from node 1 for a window, then healed"},
        {"crash", "backup 2 is crashed and rebooted inside the replication window"},
        {"pause", "the primary is paused for a window, so the ack is late but not lost"},
        {"partition+crash", "the shipped regression plan: split from 1, kill 2, both healed"},
    };
}

std::optional<Plan> plan_for(const std::string& name, uint64_t seed, const std::string& mode) {
    const bool chaos = mode != "scripted";
    Rng swarm(stream_seed(seed, StreamDomain::Swarm));

    // Chaos draws use one fixed position per plan, so the realized plan is a function of
    // (plan name, seed) and nothing else: the same seed always names the same victim.
    auto at = [&] { return Time{static_cast<int64_t>(200'000 + swarm.range(0, 600'000))}; };
    auto repair = [&] { return Duration{static_cast<int64_t>(1'000'000 + swarm.range(0, 4'000'000))}; };
    auto backup = [&] { return static_cast<NodeId>(1 + swarm.range(0, 1)); };
    auto node = [&] { return static_cast<NodeId>(swarm.range(0, kNodes - 1)); };
    auto call = [&] { return uint64_t{1} + swarm.range(0, 2); };
    auto delay = [&] { return Duration{static_cast<int64_t>(1'000'000 + swarm.range(0, 4'000'000))}; };
    auto elapsed = [&] { return Duration{static_cast<int64_t>(100'000 + swarm.range(0, 700'000))}; };

    if (name == "none") {
        return Plan{window(), "no fault: the control run"};
    }
    if (name == "drop-forward") {
        auto cfg = window();
        const uint64_t hit = chaos ? call() : 2;
        point_trigger(cfg, FaultClass::Network, SiteId::send, FaultKind::PacketDrop, hit);
        return Plan{std::move(cfg), "drop a forward: send() still reports success"};
    }
    if (name == "delay-forward") {
        auto cfg = window();
        const uint64_t hit = chaos ? call() : 2;
        const Duration amount = chaos ? delay() : Duration{2'000'000};
        point_trigger(cfg, FaultClass::Network, SiteId::send, FaultKind::PacketDelay, hit, amount);
        return Plan{std::move(cfg), "delay a forward: the primary does not wait for it"};
    }
    if (name == "short-send") {
        auto cfg = window();
        const uint64_t hit = chaos ? call() : 2;
        point_trigger(cfg, FaultClass::Network, SiteId::send, FaultKind::ShortSend, hit);
        return Plan{std::move(cfg), "shorten a forward: send_all loops, so the frame lands"};
    }
    if (name == "reset-forward") {
        auto cfg = window();
        const uint64_t hit = chaos ? call() : 2;
        point_trigger(cfg, FaultClass::Network, SiteId::send, FaultKind::ConnReset, hit);
        return Plan{std::move(cfg), "reset a forward: that follower is dropped for the run"};
    }
    if (name == "drop-forwards") {
        auto cfg = window();
        // Eligible sends: 1 is the client's frame, 2 and 3 are the two forwards, so dropping the
        // two forwards leaves the client acked with no replica holding the write.
        point_every_after(cfg, FaultClass::Network, SiteId::send, FaultKind::PacketDrop, 1, 2);
        return Plan{std::move(cfg), "drop both forwards: acked, and no backup holds the write"};
    }
    if (name == "reset-forwards") {
        auto cfg = window();
        point_every_after(cfg, FaultClass::Network, SiteId::send, FaultKind::ConnReset, 1, 2);
        return Plan{std::move(cfg), "reset both forwards: the primary drops both followers"};
    }
    if (name == "refuse-client") {
        auto cfg = window();
        // The client retries fifty times with 200us pauses; the window has to outlast that, or the
        // last attempt is honoured after the gate closes and the write is acked anyway.
        cfg.quiesce_after = Time{30'000'000};
        point_always(cfg, FaultClass::Network, SiteId::connect, FaultKind::ConnRefused);
        return Plan{std::move(cfg), "refuse every connect: the honest answer is a refused write"};
    }
    if (name == "interrupt-sleep") {
        auto cfg = window();
        const Duration amount = chaos ? elapsed() : Duration{800'000};
        point_trigger(cfg, FaultClass::Clock, SiteId::nanosleep, FaultKind::SleepInterrupted, 1,
                      amount);
        return Plan{std::move(cfg), "wake the settle sleep early: the write races start-up"};
    }
    if (name == "partition") {
        auto cfg = window();
        const NodeId victim = chaos ? backup() : 1;
        const Time when = chaos ? at() : Time{500'000};
        const Duration until = chaos ? repair() : Duration{2'000'000};
        episode(cfg, SiteId::partition, ScheduledEpisode{when, Partition{{0u}, {victim}}, until});
        return Plan{std::move(cfg), "split the primary from one backup, then heal the split"};
    }
    if (name == "crash") {
        auto cfg = window();
        const NodeId victim = chaos ? backup() : 2;
        const Time when = chaos ? at() : Time{500'000};
        const Duration until = chaos ? repair() : Duration{3'000'000};
        episode(cfg, SiteId::crash_node, ScheduledEpisode{when, CrashNode{victim}, until});
        return Plan{std::move(cfg), "crash one backup inside the window, then reboot it"};
    }
    if (name == "pause") {
        auto cfg = window();
        const NodeId victim = chaos ? node() : 0;
        const Time when = chaos ? at() : Time{500'000};
        const Duration until = chaos ? repair() : Duration{2'000'000};
        episode(cfg, SiteId::pause_node, ScheduledEpisode{when, PauseNode{victim}, until});
        return Plan{std::move(cfg), "pause one node, then resume it: late, not lost"};
    }
    if (name == "partition+crash") {
        auto cfg = window();
        const NodeId victim = chaos ? backup() : 1;
        const NodeId crashed = 3 - victim;
        const Time when = chaos ? at() : Time{500'000};
        const Duration split_until = chaos ? repair() : Duration{2'000'000};
        const Duration crash_until = chaos ? repair() : Duration{3'000'000};
        episode(cfg, SiteId::partition, ScheduledEpisode{when, Partition{{0u}, {victim}}, split_until});
        episode(cfg, SiteId::crash_node, ScheduledEpisode{when, CrashNode{crashed}, crash_until});
        return Plan{std::move(cfg), "split from one backup and kill the other: the finding"};
    }
    return std::nullopt;
}

// One universe: the cluster as three fibres on the main thread, the client on the main fibre.
ScenarioReport run_universe(Simulator& sim, uint64_t index, uint32_t node_count,
                            const FaultConfig& plan) {
    (void)index;
    auto scenario = Scenario::on(sim, plan, node_count);
    if (!scenario) {
        ScenarioReport rejected;
        rejected.seed = sim.seed();
        rejected.no_check_failed = false;
        rejected.checks.push_back(
            CheckResult{kConfigId, "the demo plan was rejected", false, Time::zero()});
        return rejected;
    }

    rkv_reset();
    g_client_rc = 99;
    NodeArg args[kNodes];

    scenario->run([&] {
        pthread_t threads[kNodes] = {};
        for (int n = 1; n < static_cast<int>(kNodes); ++n) {
            args[n].node = n;
            (void)sim.spawn_node(static_cast<NodeId>(n), &threads[n], node_thread, &args[n]);
        }
        args[0].node = 0;
        (void)sim.spawn_node(0, &threads[0], node_thread, &args[0]);

        timespec settle{};
        settle.tv_nsec = 1'000'000L;
        nanosleep(&settle, nullptr); // t=1ms: scheduled episodes fire before this wake
        g_client_rc = rkv_client_put(RKV_BASE_PORT, kKey, kValue);

        // No manual heal: every episode booked its own repair at install time, so the drain below
        // advances to those heal stamps and the ledger shows the pair the design promises.
        nanosleep(&settle, nullptr);

        // Let virtual time reach the heals each episode scheduled, so the ledger shows the repair
        // the design promises rather than the quiesce fallback that force-heals the remainder.
        // This happens with the cluster still open: healing a partition after the shutdown has
        // closed every endpoint is a different, and less interesting, situation.
        timespec settle_long{};
        settle_long.tv_nsec = 6'000'000L;
        nanosleep(&settle_long, nullptr);
        rkv_shutdown();

        for (int n = 0; n < static_cast<int>(kNodes); ++n) {
            (void)pthread_join(threads[n], nullptr);
        }
    });

    scenario->quiesce();

    const bool acked = g_client_rc == 0;
    const int on1 = rkv_node_has(1, kKey, kValue);
    const int on2 = rkv_node_has(2, kKey, kValue);
    const bool replicated = on1 != 0 || on2 != 0;

    bool saw_wrapper = false;
    bool saw_partition = false;
    bool saw_crash = false;
    bool saw_pause = false;
    if (const auto* injector = sim.injector_or_null()) {
        for (const LedgerEntry& row : injector->ledger()) {
            if (static_cast<uint64_t>(row.site) < 1000) saw_wrapper = true;
            if (row.site == SiteId::partition) saw_partition = true;
            if (row.site == SiteId::crash_node) saw_crash = true;
            if (row.site == SiteId::pause_node) saw_pause = true;
        }
    }
    scenario->note_covered("wrapper-fault-fired", saw_wrapper);
    scenario->note_covered("partition-fired", saw_partition);
    scenario->note_covered("crash-node-fired", saw_crash);
    scenario->note_covered("pause-node-fired", saw_pause);
    scenario->note_covered("client-acked", acked);
    scenario->note_covered("backup-has-write", replicated);

    // The promise, with the state that decided it spelled out, so the demo can read it aloud.
    std::ostringstream detail;
    detail << "acked=" << (acked ? 1 : 0) << " backup1=" << on1 << " backup2=" << on2;
    scenario->check(kPromise, [acked, replicated] { return !acked || replicated; }, detail.str());
    scenario->check_no_leaks("no-active-allocations");
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
    std::cout << "usage: " << argv0 << " [options]\n"
              << "  --fault NAME         which plan to run (default none; --list shows them)\n"
              << "  --mode chaos|scripted  chaos derives victims and timings from the seed (default)\n"
              << "  --root DIR           artifact root for this step (empty: do not write)\n"
              << "  --id ID              campaign id for this step (default demo)\n"
              << "  --seed N             universe seed (default 1)\n"
              << "  --list               print the plans and what each claims\n"
              << "  --build-id/--commit/--binary-hash/--host/--dirty/--clean   provenance\n";
}

bool parse_args(int argc, char** argv, Options& opts, bool& show_help, bool& show_list) {
    show_help = false;
    show_list = false;
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
        } else if (arg == "--list") {
            show_list = true;
        } else if (arg == "--fault") {
            const char* v = value("--fault");
            if (v == nullptr) return false;
            opts.fault = v;
        } else if (arg == "--mode") {
            const char* v = value("--mode");
            if (v == nullptr) return false;
            opts.mode = v;
            if (opts.mode != "chaos" && opts.mode != "scripted") {
                std::cerr << "unknown --mode: " << opts.mode << "\n";
                return false;
            }
        } else if (arg == "--root") {
            const char* v = value("--root");
            if (v == nullptr) return false;
            opts.root = v;
        } else if (arg == "--id") {
            const char* v = value("--id");
            if (v == nullptr) return false;
            opts.id = v;
        } else if (arg == "--seed") {
            const char* v = value("--seed");
            if (v == nullptr || !parse_u64(v, opts.seed)) return false;
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

void print_plan(const Plan& plan) {
    const FaultConfig& cfg = plan.cfg;
    std::cout << "claim: " << plan.claim << "\n";
    std::cout << "window: warmup=" << detail::format_ms(cfg.warmup_until)
              << " quiesce_after=" << detail::format_ms(cfg.quiesce_after)
              << " limits(max_crashed=" << cfg.max_crashed_nodes
              << ", min_quorum=" << cfg.min_healthy_quorum << ")\n";
    std::cout << "classes:";
    for (size_t i = 0; i < kFaultClassCount; ++i) {
        const auto cls = static_cast<FaultClass>(i);
        if (!cfg.is_class_enabled(cls)) continue;
        std::cout << " " << name_of(cls);
        bool first = true;
        for (SiteId site : kAllSites) {
            if (class_of(site) != cls || !cfg.is_site_activated(site)) continue;
            std::cout << (first ? "[" : ",") << name_of(site);
            first = false;
        }
        if (!first) std::cout << "]";
    }
    std::cout << "\n";
    for (const ScheduledEpisode& scheduled : cfg.scheduled_episodes) {
        std::cout << "episode: at=" << detail::format_ms(scheduled.at) << " heal_in="
                  << detail::format_ms(Time{scheduled.until_heal.ns}) << " ";
        if (const auto* crash = std::get_if<CrashNode>(&scheduled.spec)) {
            std::cout << "crash node " << crash->id;
        } else if (const auto* pause = std::get_if<PauseNode>(&scheduled.spec)) {
            std::cout << "pause node " << pause->id;
        } else if (const auto* split = std::get_if<Partition>(&scheduled.spec)) {
            std::cout << "partition {";
            for (size_t i = 0; i < split->a.size(); ++i) std::cout << (i ? "," : "") << split->a[i];
            std::cout << "} | {";
            for (size_t i = 0; i < split->b.size(); ++i) std::cout << (i ? "," : "") << split->b[i];
            std::cout << "}";
        }
        std::cout << "\n";
    }
    for (SiteId site : kAllSites) {
        const FaultRule* rule = cfg.rule_for(site);
        if (rule == nullptr) continue;
        std::cout << "rule: " << name_of(site) << " rate=" << rule->rate
                  << " skip_first=" << rule->skip_first
                  << " max_injections=" << rule->max_injections;
        if (rule->fire_on_eligible_call.has_value()) {
            std::cout << " fire_on_eligible_call=" << *rule->fire_on_eligible_call;
        }
        if (rule->amount.ns != 0) std::cout << " amount=" << detail::format_ms(Time{rule->amount.ns});
        std::cout << " outcomes=";
        bool first = true;
        for (const SiteOutcome& outcome : rule->outcomes) {
            std::cout << (first ? "" : ",") << name_of(outcome.kind) << ":" << outcome.weight;
            first = false;
        }
        std::cout << "\n";
    }
}

void print_trace(const std::string& bytes) {
    auto decoded = decode_trace(bytes);
    if (!decoded) {
        std::cout << "trace: undecodable (source=" << name_of(decoded.error()) << ")\n";
        return;
    }
    const FaultTrace& trace = *decoded;
    std::cout << "trace: " << trace.size() << " decisions, hash=";
    std::cout << std::hex << trace_hash(trace) << std::dec << "\n";
    std::cout << "  seq  t          site           outcome          drew  status       eligible\n";
    for (const TraceEntry& entry : trace) {
        std::cout << "  " << entry.seq << "  " << std::left << std::setw(10)
                  << detail::format_ms(entry.at) << std::setw(15) << name_of(entry.site)
                  << std::setw(17) << name_of(entry.outcome) << std::setw(6)
                  << (entry.drew ? "yes" : "no") << std::setw(13) << name_of(entry.status)
                  << entry.eligible_index << "\n";
    }
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
    bool show_list = false;
    if (!parse_args(argc, argv, opts, show_help, show_list)) {
        usage(argv[0]);
        return 2;
    }
    if (show_help) {
        usage(argv[0]);
        return 0;
    }
    if (show_list) {
        for (const auto& [name, claim] : plan_names()) std::cout << name << "\t" << claim << "\n";
        return 0;
    }

    auto plan = plan_for(opts.fault, opts.seed, opts.mode);
    if (!plan) {
        std::cerr << "cosmos_demo: unknown --fault \"" << opts.fault << "\" (try --list)\n";
        return 2;
    }

    if (opts.host.empty()) {
        char hostname[256] = {};
        if (gethostname(hostname, sizeof(hostname) - 1) == 0) opts.host = hostname;
    }
    if (opts.host.empty()) opts.host = "unknown";
    if (opts.build_id.empty()) opts.build_id = opts.fault;
    if (opts.id.empty()) opts.id = "demo";

    const uint64_t universe = universe_seed(opts.seed, 0);
    std::cout << "universe: seed=" << opts.seed << " universe_seed=" << universe << " nodes=" << kNodes
              << " fault=" << opts.fault << " mode=" << opts.mode << "\n";
    print_plan(*plan);
    std::cout << "\n";

    CampaignConfig campaign;
    campaign.trials = 1; // one scripted universe per step
    campaign.base_seed = opts.seed;
    campaign.parallel = 1;
    campaign.node_count = kNodes;
    campaign.trace = true;
    campaign.trace_capacity = 512;
    campaign.events = true;
    campaign.event_capacity = 512;
    campaign.state = true;
    campaign.id = opts.id;
    campaign.name = "Cosmos demo: " + opts.fault;
    campaign.program = "cosmos_demo --fault " + opts.fault;

    std::string trace_bytes;
    std::string state_json;
    ScenarioReport saved;
    std::vector<LedgerEntry> saved_ledger;
    uint64_t saved_ledger_dropped = 0;

    std::optional<CampaignArtifactWriter> writer;
    if (!opts.root.empty()) {
        ArtifactOptions artifact;
        artifact.root = opts.root;
        artifact.campaign = campaign;
        artifact.build.id = opts.build_id;
        artifact.build.commit = opts.commit;
        artifact.build.dirty = opts.dirty;
        artifact.build.binary_hash = opts.binary_hash;
        artifact.build.mode = "SIM";
        artifact.build.compiler = compiler_name();
        artifact.build.host = opts.host;
        auto created = CampaignArtifactWriter::create(std::move(artifact));
        if (!created) {
            std::cerr << "cosmos_demo: " << created.error() << "\n";
            return 1;
        }
        writer.emplace(std::move(*created));
        campaign.on_universe = [&](const detail::UniverseOutcome& outcome) {
            trace_bytes = outcome.trace_bytes;
            state_json = outcome.state_json;
            saved = outcome.report;
            saved_ledger = outcome.ledger_entries;
            saved_ledger_dropped = outcome.ledger_dropped;
            writer->sink()(outcome);
        };
    }

    const CampaignReport report = Campaign::run(
        campaign, NodeUniverseFn([&plan](Simulator& sim, uint64_t index, uint32_t node_count) {
            return run_universe(sim, index, node_count, plan->cfg);
        }));

    // The report is aggregated by promise id; for one universe the finding set is the verdict.
    const bool found = !report.findings.empty();
    for (const Finding& finding : report.findings) {
        std::cout << "finding: \"" << finding.id << "\" universes=" << finding.universes_failed
                  << " seed=" << finding.seed << "\n";
        if (!finding.ledger.empty()) std::cout << finding.ledger;
    }
    if (!found) std::cout << "finding: none (no promise was broken)\n";

    if (!saved.checks.empty()) {
        std::cout << "\nchecks:\n";
        for (const CheckResult& check : saved.checks) {
            std::cout << "  " << (check.passed ? "PASS" : "FAIL") << "  " << check.id;
            if (!check.detail.empty()) std::cout << "  (" << check.detail << ")";
            std::cout << "  t=" << detail::format_ms(check.at) << "\n";
        }
    }
    if (!saved.coverage.empty()) {
        std::cout << "evidence:\n";
        for (const CoverageNote& note : saved.coverage) {
            std::cout << "  " << (note.hit ? "seen" : "not seen") << "  " << note.id << "\n";
        }
    }
    if (!saved_ledger.empty()) {
        std::cout << "ledger:\n";
        for (const LedgerEntry& row : saved_ledger) {
            std::cout << "  t=" << std::left << std::setw(10) << detail::format_ms(row.at)
                      << std::setw(8) << name_of(row.fault_class) << "site=" << std::setw(14)
                      << name_of(row.site) << std::setw(11) << name_of(row.status) << "drew="
                      << std::setw(4) << (row.drew ? "yes" : "no") << "eligible #"
                      << row.eligible_index << " -> " << name_of(row.outcome);
            if (row.reason != SkipReason::None) std::cout << " reason=" << name_of(row.reason);
            if (row.persistent) std::cout << " persistent";
            std::cout << "\n";
        }
        if (saved_ledger_dropped > 0) {
            std::cout << "  ... and " << saved_ledger_dropped << " more not recorded\n";
        }
    }

    if (!trace_bytes.empty()) {
        std::cout << "\n";
        print_trace(trace_bytes);
    }
    if (!state_json.empty()) {
        std::cout << "\nstate: " << state_json.size() << " bytes captured at quiesce\n";
    }

    if (writer) {
        auto finished = writer->finish(report);
        if (!finished) {
            std::cerr << "cosmos_demo: " << finished.error() << "\n";
            return 1;
        }
        std::cout << "\nartifact: " << opts.root << "\n";
    }

    std::cout << "\nDEMO RESULT: " << (found ? "FAIL" : "PASS") << " " << kPromise << "\n";
    return 0;
}
