#pragma once

// The campaign runner (docs/design.md §13): many universes from one base seed, each with its own
// fault configuration, aggregated into a single findings report.
//
// A universe runs in a worker process, not in the caller's. A crashing universe must become a
// finding rather than take the campaign with it, and only a process boundary can contain a signal.
// The consequence is deliberate: writes to captured variables inside the body are not visible here,
// and its returned ScenarioReport is the only channel back.

#include "cosmos/assert.hpp"
#include "cosmos/coverage.hpp"
#include "cosmos/faults_json.hpp"
#include "cosmos/inspection.hpp"
#include "cosmos/ledger_print.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/simulator.hpp"

#include <algorithm>
#include <bitset>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cosmos {

namespace detail {
struct UniverseOutcome;
}

// Reserved ids: the harness's own findings, never an application's (scenario.hpp's convention).
inline constexpr const char* kCrashedUniverseId = "cosmos.campaign.crash";
inline constexpr const char* kThrewUniverseId = "cosmos.campaign.threw";
inline constexpr const char* kVerifyFailedId = "cosmos.campaign.verify";
inline constexpr const char* kForkFailedId = "cosmos.campaign.fork";

// True for the harness's own findings, which carry no universe and so no repro line.
inline bool is_harness_id(const std::string& id) { return id.rfind("cosmos.", 0) == 0; }

// Runs one universe. The campaign owns the Simulator; the callee drives a Scenario over it, so its
// report comes back as the universe's result.
using UniverseFn = std::function<ScenarioReport(Simulator&, uint64_t universe_index)>;
// Same body with the cluster size from CampaignConfig::node_count, so a fixture cannot disagree.
using NodeUniverseFn = std::function<ScenarioReport(Simulator&, uint64_t, uint32_t node_count)>;

// Seeded fuzzing is level 1; coverage guidance is level 2 (docs/state-exploration.md §4–§5). The
// seeded path keeps the existing forked slice runner. The guided path picks new seeds from a
// novelty corpus and runs one universe at a time so the parent can make the next choice.
enum class SeedGuidance : uint8_t { Seeded, Coverage };

constexpr const char* name_of(SeedGuidance guidance) {
    switch (guidance) {
    case SeedGuidance::Seeded:
        return "seeded";
    case SeedGuidance::Coverage:
        return "coverage";
    }
    return "seeded";
}

// One seed retained by the guided campaign because it added an edge the union did not have.
struct CorpusEntry {
    uint64_t seed = 0;
    uint64_t parent_seed = 0; // zero for the initial batch
    uint64_t index = 0;       // universe index that produced it
    uint64_t edges = 0;
    uint64_t new_edges = 0;
    uint64_t hash = 0;
    uint64_t generation = 0;
    bool finding = false;
    bool vacuous = false;
};

// Aggregate coverage facts. The corpus itself lives in CampaignReport::corpus. Per-universe
// bitmaps cross the worker channel (64 KiB each) so the parent can compute novelty; only the
// retained corpus is kept after selection.
struct CoverageStats {
    bool enabled = false;
    uint64_t runs_with_coverage = 0;
    uint64_t total_edges = 0;  // sum of per-run edge counts
    uint64_t unique_edges = 0; // edges in the corpus union
    uint64_t novel_runs = 0;   // runs that added at least one edge
    uint64_t corpus_size = 0;
    uint64_t corpus_dropped = 0; // novel runs not retained because the corpus was full
    std::string method{};
};

struct CampaignConfig {
    uint64_t trials = 1000;
    uint64_t base_seed = 0;
    unsigned parallel = std::thread::hardware_concurrency();
    uint32_t node_count = 1;
    // Level-2 exploration. Seeded runs the existing static slices. Coverage runs sequentially,
    // selecting each next seed from the retained novelty corpus. `parallel` is ignored by the
    // guided loop because the next seed depends on the previous result; the report records 1.
    SeedGuidance guidance = SeedGuidance::Seeded;
    // Collects a bitmap for every universe even without guidance. Guidance implies this.
    bool coverage = false;
    // Number of initial universe seeds before guided mutation begins. Zero means
    // min(trials, 8).
    uint64_t initial_trials = 0;
    // The corpus is bounded so a long campaign cannot grow the parent without limit.
    uint64_t max_corpus = 256;
    // Runs each universe twice and compares the full report and the canonical trace hash.
    // Implies tracing; a mismatch becomes a cosmos.campaign.verify finding.
    bool verify = false;
    // Trace buffer per run while verifying; overflow is reported, never hashed as if complete.
    uint64_t trace_capacity = kDefaultTraceCapacity;
    // Records the decision trace for every universe and keeps its canonical bytes for artifact
    // export. verify implies tracing but does not keep the bytes. Off by default because a trace
    // is ~48 MB per universe at the default capacity (docs/ui-design.md §7.3).
    bool trace = false;
    // Captures the versioned inspection event stream and, optionally, the final read-only state.
    // Both are additive artifacts (O18); disabling them leaves the canonical trace and every
    // existing artifact field unchanged. event_capacity is per universe and never grows.
    bool events = false;
    uint64_t event_capacity = 4096;
    bool state = false;
    // Named in the repro line so a finding can be re-run verbatim, e.g. "myapp_test".
    std::string program;
    // Artifact identity (docs/ui-design.md Appendix A). Optional for callers that only print a
    // report; the artifact writer requires an id.
    std::string id;
    std::string name;
    // Parent-side observer for every universe, called in ascending index order. The campaign still
    // owns the report; this is the artifact/UI seam and must never be called from a wrapper or a
    // running universe. The worker channel carries the trace bytes and realized config when asked.
    std::function<void(const detail::UniverseOutcome&)> on_universe;
};

// One violated assertion, deduplicated by id -- never by fault set, which is not globally minimal
// (plan §P4, "do not key findings dedup on the minimal fault set").
struct Finding {
    std::string id;
    uint64_t index = 0; // universe index, half of the (base_seed, index) repro identity
    uint64_t seed = 0;  // universe_seed(base_seed, index)
    std::string detail;
    Time at{};
    uint64_t universes_failed = 0;
    bool rare = false;  // reproduces in under a third of the runs
    std::string ledger; // rendered ledger of the first universe that failed this id
    // Every universe that failed this id, in ascending universe-index order. `seed` above is the
    // first one; this is what lets the UI list all repro seeds without re-scanning the campaign.
    std::vector<uint64_t> seeds;
};

// Why a site the configuration armed was never exercised (§11.4). Never a finding: a finding is a
// violated assertion, and vacuity is a fact about the configuration, not about the application.
enum class VacuityReason : uint8_t { NeverActivated, NeverReached, NeverFired };

// Stable tokens, so a dashboard can key on a reason without parsing prose.
constexpr const char* name_of(VacuityReason reason) {
    switch (reason) {
    case VacuityReason::NeverActivated:
        return "never_activated";
    case VacuityReason::NeverReached:
        return "never_reached";
    case VacuityReason::NeverFired:
        return "never_fired";
    }
    return "?";
}

// One site the campaign never exercised, with the counters that say which way it never happened.
struct VacuousSite {
    SiteId site = SiteId::malloc;
    uint64_t activated_runs = 0;
    uint64_t eligible_calls = 0;
    uint64_t injections = 0;
    VacuityReason reason = VacuityReason::NeverActivated;

    constexpr bool operator==(const VacuousSite&) const = default;
};

struct CampaignReport {
    uint64_t runs = 0;
    uint64_t failed_runs = 0;
    // Universes that produced no report at all: killed by a signal, or exited without one.
    uint64_t crashed_runs = 0;
    bool refused = false; // the harness declined to run; findings hold the reason
    std::vector<Finding> findings;
    std::vector<std::string> never_hit; // ids no universe reported as hit, kept apart from findings
    SiteCounterMap eligible_calls{};
    SiteCounterMap injections{};
    // Per site: universes whose configuration armed it. Without this, eligible_calls == 0 cannot
    // tell "nothing armed it" from "armed, and the workload never got there" (§11.4).
    SiteCounterMap activated_runs{};
    // Sites no universe exercised, ascending slot order, wrapper sites only: an event site is
    // invoked by the harness, so judging it here would invent vacuity (§11.4).
    std::vector<VacuousSite> vacuous;
    uint64_t out_of_scope_assertions = 0;

    // Echoed from the config so a report prints its own repro line without the caller's help.
    uint64_t base_seed = 0;
    uint32_t node_count = 0;
    unsigned parallel = 0;
    std::string program;
    bool verify = false;
    // Artifact identity and trace intent. `trace` is true when the run recorded decision traces
    // (verify implies it); `trace_capacity` is the buffer each universe was given.
    std::string id;
    std::string name;
    bool trace = false;
    uint64_t trace_capacity = 0;
    // Inspection intent echoed for artifact validation (O18). `events` is an observation capture,
    // not a second replay input (D5).
    bool events = false;
    uint64_t event_capacity = 0;
    bool state = false;
    // Exploration intent and results. A seeded campaign leaves guidance at Seeded, coverage
    // disabled, and an empty corpus.
    SeedGuidance guidance = SeedGuidance::Seeded;
    CoverageStats coverage{};
    std::vector<CorpusEntry> corpus{};
};

namespace detail {

// The worker channel carries the activation set in one u64 (serialize_outcome below).
static_assert(kSiteCount <= 64, "activation set must fit the worker channel's u64 field");

// One universe's result, in the form a parent process can fold after the universe is gone.
struct UniverseOutcome {
    uint64_t index = 0;
    uint64_t seed = 0;
    bool died = false;        // produced no report: a signal, or an exit without one
    int signal = 0;           // WTERMSIG when it died on a signal
    bool fork_failed = false; // parent had to run it in-process; not crash-contained
    std::string threw;        // nonempty when the universe's body threw instead of reporting
    uint64_t out_of_scope = 0;
    // Virtual time when the universe body returned. This is not a wall-clock duration, and the
    // UI must label it as virtual time (D10); wall_ns is the separate host measurement.
    Time ended_at{};
    uint64_t wall_ns = 0;
    bool traced = false;         // the injector recorded a trace for this universe
    uint64_t trace_hash = 0;     // canonical trace hash, valid only when traced
    uint64_t trace_dropped = 0;  // entries the trace buffer could not hold
    uint64_t trace_capacity = 0; // the buffer this universe was given
    // Version-1 inspection stream (O18). Presence is separate from an empty capture.
    bool events_attached = false;
    bool events_encoded = false;
    uint64_t event_capacity = 0;
    uint64_t event_dropped = 0;
    uint64_t event_rejected = 0;
    std::string event_bytes;
    std::string events_error; // nonempty when encode_events() refused the capture
    // Final read-only state document, empty unless capture_state was requested.
    std::string state_json;
    std::string verify_mismatch; // verify's first difference, empty on a deterministic run
    // Realized config and its identity. The config can differ per universe because
    // FaultConfig::sample() draws from the Swarm stream (D14).
    bool has_config = false;
    std::string config_json;
    std::string config_hash;
    // Structured ledger and its rendered form. The entries are the UI's filterable rows; the
    // rendered dump is kept because it is the existing human-readable explanation.
    std::vector<LedgerEntry> ledger_entries{};
    uint64_t ledger_dropped = 0;
    // Canonical fault-trace bytes, present only when the run was asked to keep them. They are the
    // same bytes trace_hash() covers, so an artifact can be verified without re-running.
    std::string trace_bytes;
    // Compiler coverage for this universe. Empty unless UniverseOptions::coverage is set.
    bool coverage_present = false;
    bool forced_choice_used = false; // branch snapshots only; campaign runs leave false
    CoverageSnapshot coverage{};
    // Snapshot branch children only. The campaign ignores it; snapshot.cpp owns the encoding.
    std::string nested_payload;
    ScenarioReport report{};
};

// What one universe run is asked for: verify traces every run and doubles it.
struct UniverseOptions {
    TraceOptions trace{};
    bool verify = false;
    // Keeps encode_trace() bytes on the outcome for artifact export. A campaign that only verifies
    // does not need a second copy of a potentially 48 MB trace.
    bool keep_trace_bytes = false;
    // Keeps the realized config JSON and structured ledger entries. Only an artifact sink needs
    // them; a report-only campaign should not pay to copy them through the worker channel.
    bool capture_metadata = false;
    // Versioned inspection capture (U3.2). The sink is constructed before the universe body runs.
    bool events = false;
    uint64_t event_capacity = 4096;
    bool state = false;
    // Copies the compiler coverage bitmap into the outcome after the body returns.
    bool coverage = false;
};

// Runs one universe in this process: the Simulator is per universe, so no state crosses between
// them.
inline UniverseOutcome run_one_universe(uint64_t index, uint64_t seed, const UniverseFn& build_fn,
                                        const UniverseOptions& options = {}) {
    UniverseOutcome outcome;
    outcome.index = index;
    outcome.seed = seed;
    const uint64_t out_of_scope_before = out_of_scope_assertions().count;
    {
        Simulator sim(seed);
        sim.set_default_trace_options(options.trace);
        if (options.events) sim.enable_event_capture(options.event_capacity);
        // Reset after the harness's own Simulator setup and before the body, so the bitmap
        // describes the application plus the wrapper calls it makes, not campaign bookkeeping.
        if (options.coverage) coverage_reset();
        const auto wall_start = std::chrono::steady_clock::now();
        try {
            outcome.report = build_fn(sim, index);
        } catch (const std::exception& error) {
            outcome.threw = error.what();
        } catch (...) {
            outcome.threw = "unknown exception";
        }
        outcome.wall_ns =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - wall_start)
                                      .count());
        // Capture coverage before ledger rendering, event encoding or state serialization. Those
        // are harness work compiled into the instrumented body and must not steer the corpus.
        if (options.coverage) {
            outcome.coverage_present = coverage_method() != "none";
            outcome.coverage = coverage_snapshot();
        }
        // Read while the universe still exists, and before the report is escaped or rendered: the
        // end time and the injector state die with the Simulator.
        outcome.ended_at = sim.now();
        if (const auto* injector = sim.injector_or_null()) {
            if (options.capture_metadata) {
                outcome.has_config = true;
                outcome.config_json = config_json(injector->config());
                outcome.config_hash = config_fingerprint(injector->config());
                for (const LedgerEntry& entry : injector->ledger()) {
                    outcome.ledger_entries.push_back(entry);
                }
                outcome.ledger_dropped = injector->ledger().dropped();
            }
        }
        // Rendered here because the injector dies with the Simulator, and a finding needs its
        // ledger.
        if (!outcome.report.no_check_failed) {
            if (const auto* injector = sim.injector_or_null()) {
                std::ostringstream dump;
                print_ledger(dump, *injector);
                outcome.report.ledger_dump = dump.str();
            }
        }
        // The inspection capture is read while the sink and the final state still exist. Encoding
        // allocates, so it happens after the workload, never on the record path.
        if (options.events) {
            sim.record_final_events();
            outcome.events_attached = sim.event_capture_attached();
            outcome.event_capacity = sim.event_sink().capacity();
            outcome.event_dropped = sim.event_sink().dropped();
            outcome.event_rejected = sim.event_sink().rejected();
            // A sink that was never attached must not write an empty header; presence is
            // separate from a complete zero-event capture.
            if (outcome.events_attached) {
                if (const auto encoded = encode_events(sim.event_sink()); encoded.has_value()) {
                    outcome.event_bytes = *encoded;
                    outcome.events_encoded = true;
                } else {
                    outcome.events_error = name_of(encoded.error());
                }
            }
        }
        if (options.state) {
            std::ostringstream state;
            print_state_json(state, capture_state(sim));
            outcome.state_json = state.str();
        }
        // Hashed here for the same reason: the trace dies with the Simulator.
        if (const auto* injector = sim.injector_or_null()) {
            const FaultTrace& trace = injector->trace();
            outcome.traced = trace.enabled();
            if (outcome.traced) {
                outcome.trace_capacity = trace.capacity();
                outcome.trace_dropped = trace.dropped();
                if (options.keep_trace_bytes) {
                    outcome.trace_bytes = encode_trace(trace);
                    outcome.trace_hash = detail::fnv1a_64(outcome.trace_bytes);
                } else {
                    outcome.trace_hash = trace_hash(trace);
                }
            }
        }
    }
    outcome.out_of_scope = out_of_scope_assertions().count - out_of_scope_before;
    return outcome;
}

// Names the first field that differs, so a determinism bug arrives with a diagnosis.
inline std::string first_report_difference(const ScenarioReport& first,
                                           const ScenarioReport& second) {
    if (first.seed != second.seed) return "seed";
    if (first.checks.size() != second.checks.size()) return "checks.size";
    for (size_t index = 0; index < first.checks.size(); ++index) {
        const CheckResult& a = first.checks[index];
        const CheckResult& b = second.checks[index];
        const std::string where = "checks[" + std::to_string(index) + "]";
        if (a.id != b.id) return where + ".id";
        if (a.detail != b.detail) return where + ".detail";
        if (a.passed != b.passed) return where + ".passed";
        if (a.at.ns != b.at.ns) return where + ".at";
    }
    if (first.coverage.size() != second.coverage.size()) return "coverage.size";
    for (size_t index = 0; index < first.coverage.size(); ++index) {
        const CoverageNote& a = first.coverage[index];
        const CoverageNote& b = second.coverage[index];
        const std::string where = "coverage[" + std::to_string(index) + "]";
        if (a.id != b.id) return where + ".id";
        if (a.hit != b.hit) return where + ".hit";
    }
    for (size_t slot = 0; slot < kSiteCount; ++slot) {
        const std::string site = name_of(site_at_slot(slot));
        if (first.eligible_calls[slot] != second.eligible_calls[slot]) {
            return "eligible_calls[" + site + "]";
        }
        if (first.injections[slot] != second.injections[slot]) return "injections[" + site + "]";
        if (first.activated_sites.test(slot) != second.activated_sites.test(slot)) {
            return "activated_sites[" + site + "]";
        }
    }
    if (first.active_allocations != second.active_allocations) return "active_allocations";
    if (first.no_check_failed != second.no_check_failed) return "no_check_failed";
    if (first.ledger_dump != second.ledger_dump) return "ledger_dump";
    return {};
}

inline std::string verify_difference(const UniverseOutcome& first, const UniverseOutcome& second) {
    if (first.threw != second.threw) {
        return "threw: first=\"" + first.threw + "\" second=\"" + second.threw + "\"";
    }
    // The same throw twice is deterministic; the kernel-level finding already reports it.
    if (!first.threw.empty()) return {};
    if (!first.traced || !second.traced) {
        return "tracing was not enabled; use Scenario::on(sim, plan) inside the campaign body";
    }
    const std::string difference = first_report_difference(first.report, second.report);
    if (!difference.empty()) return difference;
    if (first.trace_dropped != 0 || second.trace_dropped != 0) {
        return "trace overflowed (first=" + std::to_string(first.trace_dropped) +
               ", second=" + std::to_string(second.trace_dropped) + " entries dropped)";
    }
    if (first.trace_hash != second.trace_hash) return "trace hash";
    if (first.coverage_present != second.coverage_present) return "coverage presence";
    if (first.coverage_present) {
        if (first.coverage.hash != second.coverage.hash) return "coverage hash";
        if (first.coverage.edges != second.coverage.edges) return "coverage edge count";
    }
    return {};
}

// One universe, run twice from the same seed; the first run is the report, the second is evidence.
inline UniverseOutcome run_verified_universe(uint64_t index, uint64_t seed,
                                             const UniverseFn& build_fn,
                                             const UniverseOptions& options) {
    UniverseOutcome first = run_one_universe(index, seed, build_fn, options);
    UniverseOutcome second = run_one_universe(index, seed, build_fn, options);
    first.verify_mismatch = verify_difference(first, second);
    return first;
}

inline UniverseOutcome run_universe(uint64_t index, uint64_t seed, const UniverseFn& build_fn,
                                    const UniverseOptions& options) {
    return options.verify ? run_verified_universe(index, seed, build_fn, options)
                          : run_one_universe(index, seed, build_fn, options);
}

// The only place a finding is created or merged, so sequential and parallel runs cannot diverge.
class CampaignAccumulator {
  public:
    explicit CampaignAccumulator(bool collect_corpus = false, uint64_t corpus_limit = 256)
        : collect_corpus_(collect_corpus), corpus_limit_(std::max<uint64_t>(1, corpus_limit)) {
        coverage_union_.bits.assign(kCoverageBitmapBytes, 0);
    }

    // Outcomes must arrive in ascending index so the "first universe" evidence is the lowest one.
    void add(const UniverseOutcome& outcome) {
        ++report_.runs;
        if (outcome.died) {
            ++report_.crashed_runs;
            ++report_.failed_runs;
            Finding& finding = record_failure(kCrashedUniverseId, outcome);
            if (finding.universes_failed == 1) {
                finding.detail = outcome.signal != 0
                                     ? "killed by signal " + std::to_string(outcome.signal)
                                     : "exited without reporting";
            }
            return;
        }
        if (!outcome.threw.empty()) {
            ++report_.failed_runs;
            Finding& finding = record_failure(kThrewUniverseId, outcome);
            if (finding.universes_failed == 1) finding.detail = outcome.threw;
            return;
        }

        const ScenarioReport& universe = outcome.report;
        const bool verify_failed = !outcome.verify_mismatch.empty();
        if (!universe.no_check_failed || verify_failed) ++report_.failed_runs;
        if (verify_failed) {
            Finding& finding = record_failure(kVerifyFailedId, outcome);
            if (finding.universes_failed == 1) finding.detail = outcome.verify_mismatch;
        }
        for (const CheckResult& check : universe.checks) {
            if (check.passed) continue;
            Finding& finding = record_failure(check.id, outcome);
            if (finding.universes_failed == 1) {
                finding.detail = check.detail;
                finding.at = check.at;
                finding.ledger = universe.ledger_dump;
            }
        }
        // Coalesced by id, because a hit anywhere outweighs a miss everywhere.
        for (const CoverageNote& note : universe.coverage) {
            covered_[note.id] = covered_[note.id] || note.hit;
        }
        for (size_t slot = 0; slot < kSiteCount; ++slot) {
            report_.eligible_calls[slot] += universe.eligible_calls[slot];
            report_.injections[slot] += universe.injections[slot];
            report_.activated_runs[slot] += universe.activated_sites.test(slot) ? 1 : 0;
        }
        report_.out_of_scope_assertions += outcome.out_of_scope;
        if (outcome.coverage_present) observe_coverage(outcome);
    }

    // A complaint about the campaign itself, belonging to no single universe.
    void add_harness_finding(const char* id, std::string detail) {
        Finding& finding = find_or_add(id, UniverseOutcome{});
        finding.detail = std::move(detail);
        ++finding.universes_failed;
    }

    // std::map's ordering makes the findings and never_hit sequences a function of the universe set
    // alone, which is what lets a parallel run match a sequential one exactly.
    [[nodiscard]] CampaignReport finish() const {
        CampaignReport report = report_;
        report.findings.reserve(findings_.size());
        for (const auto& [id, finding] : findings_) {
            Finding copy = finding;
            copy.rare = copy.universes_failed * 3 < report.runs;
            report.findings.push_back(std::move(copy));
        }
        for (const auto& [id, hit] : covered_) {
            if (!hit) report.never_hit.push_back(id);
        }
        report.coverage.runs_with_coverage = coverage_runs_;
        report.coverage.total_edges = coverage_total_edges_;
        report.coverage.unique_edges = coverage_union_.edges;
        report.coverage.novel_runs = coverage_novel_runs_;
        report.coverage.corpus_size = corpus_.size();
        report.coverage.corpus_dropped = corpus_dropped_;
        if (coverage_runs_ > 0) {
            report.coverage.enabled = true;
            report.coverage.method = std::string(coverage_method());
        }
        if (collect_corpus_) report.corpus = corpus_;
        for (size_t slot = 0; slot < kWrapperSiteCount; ++slot) {
            VacuousSite site;
            site.site = site_at_slot(slot);
            site.activated_runs = report.activated_runs[slot];
            site.eligible_calls = report.eligible_calls[slot];
            site.injections = report.injections[slot];
            if (site.activated_runs == 0) {
                site.reason = VacuityReason::NeverActivated;
            } else if (site.eligible_calls == 0) {
                site.reason = VacuityReason::NeverReached;
            } else if (site.injections == 0) {
                site.reason = VacuityReason::NeverFired;
            } else {
                continue; // reached and fired: the site did work
            }
            report.vacuous.push_back(site);
        }
        return report;
    }

  private:
    // One place for the seed list, so a finding's first seed and its full list cannot disagree.
    Finding& record_failure(const std::string& id, const UniverseOutcome& outcome) {
        Finding& finding = find_or_add(id, outcome);
        // Crash, throw and verify findings are harness ids but still belong to one universe, so
        // they get a seed list too. add_harness_finding() bypasses this for ids with no universe.
        if (finding.seeds.empty() || finding.seeds.back() != outcome.seed) {
            finding.seeds.push_back(outcome.seed);
        }
        ++finding.universes_failed;
        return finding;
    }

    void observe_coverage(const UniverseOutcome& outcome) {
        ++coverage_runs_;
        coverage_total_edges_ += outcome.coverage.edges;
        const uint64_t novel = coverage_novelty(outcome.coverage, coverage_union_);
        coverage_merge(coverage_union_, outcome.coverage);
        if (novel > 0) ++coverage_novel_runs_;
        if (!collect_corpus_) return;
        if (corpus_.empty() || novel > 0) {
            if (corpus_.size() < corpus_limit_) {
                corpus_.push_back(CorpusEntry{.seed = outcome.seed,
                                              .parent_seed = 0,
                                              .index = outcome.index,
                                              .edges = outcome.coverage.edges,
                                              .new_edges = novel,
                                              .hash = outcome.coverage.hash,
                                              .generation = 0,
                                              .finding = !outcome.report.no_check_failed ||
                                                         !outcome.threw.empty() || outcome.died,
                                              .vacuous = outcome.report.vacuous()});
            } else {
                ++corpus_dropped_;
            }
        }
    }

    Finding& find_or_add(const std::string& id, const UniverseOutcome& outcome) {
        auto [position, inserted] = findings_.try_emplace(id);
        if (inserted) {
            position->second.id = id;
            position->second.index = outcome.index;
            position->second.seed = outcome.seed;
        }
        return position->second;
    }

    std::map<std::string, Finding> findings_;
    std::map<std::string, bool> covered_;
    CampaignReport report_{};
    bool collect_corpus_ = false;
    uint64_t corpus_limit_ = 256;
    std::vector<CorpusEntry> corpus_{};
    CoverageSnapshot coverage_union_{};
    uint64_t coverage_runs_ = 0;
    uint64_t coverage_total_edges_ = 0;
    uint64_t coverage_novel_runs_ = 0;
    uint64_t corpus_dropped_ = 0;
};

// ---- the worker channel ----------------------------------------------------
//
// Both ends are the same binary, so fields are written as raw host-order bytes and the payload is
// length-prefixed. An anonymous file rather than a pipe: a regular file cannot fill up, so a worker
// never blocks on a reader that is still waiting for it, and unlinking up front means nothing to
// clean up even when a worker dies mid-write.

template <typename T> void put_field(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(T));
}

inline void put_text(std::string& bytes, const std::string& text) {
    put_field(bytes, static_cast<uint64_t>(text.size()));
    bytes.append(text);
}

inline std::string serialize_outcome(const UniverseOutcome& outcome) {
    std::string bytes;
    put_field(bytes, outcome.index);
    put_field(bytes, outcome.seed);
    put_field(bytes, static_cast<uint8_t>(outcome.died ? 1 : 0));
    put_field(bytes, static_cast<int32_t>(outcome.signal));
    put_text(bytes, outcome.threw);
    put_field(bytes, outcome.out_of_scope);

    const ScenarioReport& report = outcome.report;
    put_field(bytes, report.seed);
    put_field(bytes, static_cast<uint8_t>(report.no_check_failed ? 1 : 0));
    put_field(bytes, static_cast<uint64_t>(report.checks.size()));
    for (const CheckResult& check : report.checks) {
        put_text(bytes, check.id);
        put_text(bytes, check.detail);
        put_field(bytes, static_cast<uint8_t>(check.passed ? 1 : 0));
        put_field(bytes, check.at.ns);
    }
    put_field(bytes, static_cast<uint64_t>(report.coverage.size()));
    for (const CoverageNote& note : report.coverage) {
        put_text(bytes, note.id);
        put_field(bytes, static_cast<uint8_t>(note.hit ? 1 : 0));
    }
    for (uint64_t count : report.eligible_calls)
        put_field(bytes, count);
    for (uint64_t count : report.injections)
        put_field(bytes, count);
    // kSiteCount == 18, so the activation set fits one field.
    put_field(bytes, static_cast<uint64_t>(report.activated_sites.to_ullong()));
    put_field(bytes, static_cast<uint64_t>(report.active_allocations));
    put_text(bytes, report.ledger_dump);
    put_field(bytes, static_cast<uint8_t>(outcome.traced ? 1 : 0));
    put_field(bytes, outcome.trace_hash);
    put_field(bytes, outcome.trace_dropped);
    put_field(bytes, outcome.trace_capacity);
    put_text(bytes, outcome.verify_mismatch);
    put_field(bytes, outcome.ended_at.ns);
    put_field(bytes, outcome.wall_ns);
    put_field(bytes, static_cast<uint8_t>(outcome.has_config ? 1 : 0));
    put_text(bytes, outcome.config_json);
    put_text(bytes, outcome.config_hash);
    put_field(bytes, static_cast<uint64_t>(outcome.ledger_entries.size()));
    for (const LedgerEntry& entry : outcome.ledger_entries) {
        put_field(bytes, entry.fire_index);
        put_field(bytes, entry.at.ns);
        put_field(bytes, static_cast<uint64_t>(entry.site));
        put_field(bytes, entry.eligible_index);
        put_field(bytes, static_cast<uint8_t>(entry.fault_class));
        put_field(bytes, static_cast<uint8_t>(entry.outcome));
        put_field(bytes, static_cast<uint8_t>(entry.drew ? 1 : 0));
        put_field(bytes, static_cast<uint8_t>(entry.status));
        put_field(bytes, static_cast<uint8_t>(entry.reason));
        put_field(bytes, static_cast<uint8_t>(entry.persistent ? 1 : 0));
    }
    put_field(bytes, outcome.ledger_dropped);
    put_text(bytes, outcome.trace_bytes);
    put_field(bytes, static_cast<uint8_t>(outcome.events_attached ? 1 : 0));
    put_field(bytes, static_cast<uint8_t>(outcome.events_encoded ? 1 : 0));
    put_field(bytes, outcome.event_capacity);
    put_field(bytes, outcome.event_dropped);
    put_field(bytes, outcome.event_rejected);
    put_text(bytes, outcome.event_bytes);
    put_text(bytes, outcome.events_error);
    put_text(bytes, outcome.state_json);
    put_field(bytes, static_cast<uint8_t>(outcome.coverage_present ? 1 : 0));
    put_field(bytes, outcome.coverage.edges);
    put_field(bytes, outcome.coverage.hash);
    put_text(bytes, std::string(outcome.coverage.bits.begin(), outcome.coverage.bits.end()));
    put_field(bytes, static_cast<uint8_t>(outcome.forced_choice_used ? 1 : 0));
    put_text(bytes, outcome.nested_payload);
    return bytes;
}

// Walks length-prefixed records read from a worker's scratch fd, one at a time. The length prefix
// is deliberately outside serialize_outcome(): the parent must be able to skip a record it cannot
// hold without first allocating the whole slice. A partial record stops the walk at the last good
// boundary, which is what recover_slice() re-runs.
class OutcomeReader {
  public:
    explicit OutcomeReader(int fd) : fd_(fd) {
        const off_t end = ::lseek(fd_, 0, SEEK_END);
        file_size_ = end > 0 ? static_cast<uint64_t>(end) : 0;
        (void)::lseek(fd_, 0, SEEK_SET);
    }

    bool next(UniverseOutcome& outcome) {
        uint64_t length = 0;
        if (!read_exact(&length, sizeof(length))) return false;
        file_offset_ += sizeof(length);
        if (file_offset_ > file_size_ || length > file_size_ - file_offset_ || length == 0) {
            return false;
        }
        if (length > std::numeric_limits<size_t>::max()) return false;
        std::string record(static_cast<size_t>(length), '\0');
        if (!read_exact(record.data(), record.size())) return false;
        file_offset_ += length;
        bytes_ = std::move(record);
        parse_offset_ = 0;
        return parse(outcome);
    }

  private:
    bool read_exact(void* data, size_t count) {
        size_t done = 0;
        while (done < count) {
            const ssize_t got = ::read(fd_, static_cast<char*>(data) + done, count - done);
            if (got < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (got == 0) return false;
            done += static_cast<size_t>(got);
        }
        return true;
    }

    bool parse(UniverseOutcome& outcome) {
        UniverseOutcome parsed;
        uint8_t flag = 0;
        int32_t signal = 0;
        uint64_t count = 0;

        if (!take(parsed.index) || !take(parsed.seed) || !take(flag) || !take(signal)) return false;
        parsed.died = flag != 0;
        parsed.signal = signal;
        if (!take_text(parsed.threw) || !take(parsed.out_of_scope)) return false;
        if (!take(parsed.report.seed) || !take(flag)) return false;
        parsed.report.no_check_failed = flag != 0;
        if (!take(count)) return false;
        for (uint64_t i = 0; i < count; ++i) {
            CheckResult check;
            if (!take_text(check.id) || !take_text(check.detail) || !take(flag)) return false;
            check.passed = flag != 0;
            if (!take(check.at.ns)) return false;
            parsed.report.checks.push_back(std::move(check));
        }
        if (!take(count)) return false;
        for (uint64_t i = 0; i < count; ++i) {
            CoverageNote note;
            if (!take_text(note.id) || !take(flag)) return false;
            note.hit = flag != 0;
            parsed.report.coverage.push_back(std::move(note));
        }
        for (uint64_t& value : parsed.report.eligible_calls) {
            if (!take(value)) return false;
        }
        for (uint64_t& value : parsed.report.injections) {
            if (!take(value)) return false;
        }
        uint64_t activation_mask = 0;
        if (!take(activation_mask)) return false;
        parsed.report.activated_sites = std::bitset<kSiteCount>(activation_mask);
        uint64_t active_allocations = 0;
        if (!take(active_allocations)) return false;
        if (active_allocations > std::numeric_limits<size_t>::max()) return false;
        parsed.report.active_allocations = static_cast<size_t>(active_allocations);
        if (!take_text(parsed.report.ledger_dump)) return false;
        uint8_t traced = 0;
        if (!take(traced) || !take(parsed.trace_hash) || !take(parsed.trace_dropped) ||
            !take(parsed.trace_capacity) || !take_text(parsed.verify_mismatch)) {
            return false;
        }
        parsed.traced = traced != 0;
        if (!take(parsed.ended_at.ns) || !take(parsed.wall_ns)) return false;
        uint8_t has_config = 0;
        if (!take(has_config) || !take_text(parsed.config_json) || !take_text(parsed.config_hash)) {
            return false;
        }
        parsed.has_config = has_config != 0;
        uint64_t ledger_count = 0;
        if (!take(ledger_count)) return false;
        // The channel is written by this binary, but a half-written record must not be read as a
        // gigantic vector request.
        if (ledger_count > kLedgerCapacity) return false;
        for (uint64_t index = 0; index < ledger_count; ++index) {
            LedgerEntry entry{};
            uint8_t fault_class = 0;
            uint8_t outcome = 0;
            uint8_t drew = 0;
            uint8_t status = 0;
            uint8_t reason = 0;
            uint8_t persistent = 0;
            if (!take(entry.fire_index) || !take(entry.at.ns) || !take(entry.site) ||
                !take(entry.eligible_index) || !take(fault_class) || !take(outcome) ||
                !take(drew) || !take(status) || !take(reason) || !take(persistent)) {
                return false;
            }
            if (fault_class >= static_cast<uint8_t>(FaultClass::_Count) ||
                outcome >= static_cast<uint8_t>(FaultKind::_Count) ||
                status > static_cast<uint8_t>(DecisionStatus::Skipped) ||
                reason >= static_cast<uint8_t>(SkipReason::_Count) ||
                !is_known_site(static_cast<SiteId>(entry.site))) {
                return false;
            }
            entry.fault_class = static_cast<FaultClass>(fault_class);
            entry.outcome = static_cast<FaultKind>(outcome);
            entry.drew = drew != 0;
            entry.status = static_cast<DecisionStatus>(status);
            entry.reason = static_cast<SkipReason>(reason);
            entry.persistent = persistent != 0;
            parsed.ledger_entries.push_back(entry);
        }
        if (!take(parsed.ledger_dropped) || !take_text(parsed.trace_bytes)) return false;
        uint8_t events_attached = 0;
        uint8_t events_encoded = 0;
        if (!take(events_attached) || !take(events_encoded) || !take(parsed.event_capacity) ||
            !take(parsed.event_dropped) || !take(parsed.event_rejected) ||
            !take_text(parsed.event_bytes) || !take_text(parsed.events_error) ||
            !take_text(parsed.state_json)) {
            return false;
        }
        parsed.events_attached = events_attached != 0;
        parsed.events_encoded = events_encoded != 0;
        if (parsed.events_encoded && !parsed.events_attached) return false;
        if (!parsed.events_encoded && !parsed.event_bytes.empty()) return false;

        uint8_t coverage_present = 0;
        if (!take(coverage_present) || !take(parsed.coverage.edges) ||
            !take(parsed.coverage.hash)) {
            return false;
        }
        parsed.coverage_present = coverage_present != 0;
        std::string coverage_bytes;
        if (!take_text(coverage_bytes)) return false;
        if (parsed.coverage_present) {
            if (coverage_bytes.size() != kCoverageBitmapBytes) return false;
            parsed.coverage.bits.assign(coverage_bytes.begin(), coverage_bytes.end());
        } else if (!coverage_bytes.empty()) {
            return false;
        }
        uint8_t forced_choice_used = 0;
        if (parse_offset_ < bytes_.size()) {
            if (!take(forced_choice_used)) return false;
        }
        parsed.forced_choice_used = forced_choice_used != 0;
        if (!take_text(parsed.nested_payload)) return false;

        outcome = std::move(parsed);
        return true;
    }

    template <typename T> bool take(T& value) {
        if (sizeof(T) > bytes_.size() - parse_offset_) return false;
        std::memcpy(&value, bytes_.data() + parse_offset_, sizeof(T));
        parse_offset_ += sizeof(T);
        return true;
    }

    bool take_text(std::string& text) {
        uint64_t length = 0;
        if (!take(length)) return false;
        if (length > bytes_.size() - parse_offset_) return false;
        text.assign(bytes_.data() + parse_offset_, static_cast<size_t>(length));
        parse_offset_ += static_cast<size_t>(length);
        return true;
    }

    int fd_ = -1;
    uint64_t file_size_ = 0;
    uint64_t file_offset_ = 0;
    std::string bytes_;
    size_t parse_offset_ = 0;
};

// Unlinked on creation: the worker's death leaves nothing behind, and no reader can be blocked.
inline int open_scratch_file() {
    const char* tmpdir = std::getenv("TMPDIR");
    std::string path = std::string(tmpdir != nullptr ? tmpdir : "/tmp") + "/cosmos-campaign-XXXXXX";
    const int fd = ::mkstemp(path.data());
    if (fd >= 0) ::unlink(path.c_str());
    return fd;
}

// Read after the worker is reaped; the child shares the file offset, so seeking first is required.
inline std::string read_scratch_file(int fd) {
    std::string bytes;
    if (::lseek(fd, 0, SEEK_SET) < 0) return bytes;
    char chunk[4096];
    for (;;) {
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got <= 0) break;
        bytes.append(chunk, static_cast<size_t>(got));
    }
    return bytes;
}

// Contiguous, so folding workers in order folds universes in order.
struct Slice {
    uint64_t begin = 0;
    uint64_t end = 0;
};

inline Slice slice_for(uint64_t trials, unsigned workers, unsigned worker) {
    return Slice{trials * worker / workers, trials * (worker + 1) / workers};
}

inline void run_slice_into(const Slice& slice, uint64_t base_seed, const UniverseFn& build_fn,
                           const UniverseOptions& options, CampaignAccumulator& accumulator,
                           const std::function<void(const UniverseOutcome&)>& sink = {}) {
    for (uint64_t index = slice.begin; index < slice.end; ++index) {
        const UniverseOutcome outcome =
            run_universe(index, universe_seed(base_seed, index), build_fn, options);
        accumulator.add(outcome);
        if (sink) sink(outcome);
    }
}

inline bool write_all(int fd, std::string_view bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        done += static_cast<size_t>(written);
    }
    return true;
}

// The child's whole job. Nothing here may touch the parent's memory and expect it to survive.
// Each record is length-prefixed so the parent can read one outcome at a time instead of the whole
// slice; a trace is large enough that the difference is the campaign's peak RSS.
inline bool run_slice_to_file(const Slice& slice, uint64_t base_seed, const UniverseFn& build_fn,
                              const UniverseOptions& options, int fd) {
    std::string bytes;
    for (uint64_t index = slice.begin; index < slice.end; ++index) {
        const std::string record = serialize_outcome(
            run_universe(index, universe_seed(base_seed, index), build_fn, options));
        put_field(bytes, static_cast<uint64_t>(record.size()));
        bytes += record;
        if (bytes.size() >= 1u << 20) {
            if (!write_all(fd, bytes)) return false;
            bytes.clear();
        }
    }
    return write_all(fd, bytes);
}

// One universe per worker for the tail of a slice whose worker died. A universe that dies again is
// attributed to its own seed, which is the whole point of paying for a second fork.
inline void recover_slice(uint64_t from, uint64_t to, uint64_t base_seed,
                          const UniverseFn& build_fn, const UniverseOptions& options,
                          CampaignAccumulator& accumulator,
                          const std::function<void(const UniverseOutcome&)>& sink = {}) {
    for (uint64_t index = from; index < to; ++index) {
        UniverseOutcome outcome;
        const int fd = open_scratch_file();
        const pid_t pid = fd < 0 ? -1 : ::fork();
        if (pid == 0) {
            try {
                if (!run_slice_to_file(Slice{index, index + 1}, base_seed, build_fn, options, fd)) {
                    ::_exit(1);
                }
            } catch (...) {
                ::_exit(1);
            }
            ::_exit(0);
        }
        if (pid < 0) {
            if (fd >= 0) ::close(fd);
            accumulator.add_harness_finding(
                kForkFailedId,
                "fork() failed during recovery; that universe ran in-process and is not "
                "crash-contained");
            outcome = run_universe(index, universe_seed(base_seed, index), build_fn, options);
            accumulator.add(outcome);
            if (sink) sink(outcome);
            continue;
        }
        int status = 0;
        ::waitpid(pid, &status, 0);
        OutcomeReader reader(fd);
        if (!reader.next(outcome)) {
            outcome.index = index;
            outcome.seed = universe_seed(base_seed, index);
            outcome.died = true;
            outcome.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        }
        ::close(fd);
        accumulator.add(outcome);
        if (sink) sink(outcome);
    }
}

// Runs one arbitrary seed in a forked worker. The seeded slice runner maps index -> seed; guided
// selection needs the seed as the input, because it is chosen by novelty rather than by position.
inline UniverseOutcome run_isolated(uint64_t index, uint64_t seed, const UniverseFn& build_fn,
                                    const UniverseOptions& options) {
    UniverseOutcome outcome;
    const int fd = open_scratch_file();
    const pid_t pid = fd < 0 ? -1 : ::fork();
    if (pid == 0) {
        try {
            const std::string record =
                serialize_outcome(run_universe(index, seed, build_fn, options));
            std::string bytes;
            put_field(bytes, static_cast<uint64_t>(record.size()));
            bytes += record;
            if (!write_all(fd, bytes)) ::_exit(1);
        } catch (...) {
            ::_exit(1);
        }
        ::_exit(0);
    }
    if (pid < 0) {
        if (fd >= 0) ::close(fd);
        outcome = run_universe(index, seed, build_fn, options);
        outcome.fork_failed = true;
        return outcome;
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    OutcomeReader reader(fd);
    if (!reader.next(outcome)) {
        outcome.index = index;
        outcome.seed = seed;
        outcome.died = true;
        outcome.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    }
    ::close(fd);
    return outcome;
}

} // namespace detail
inline CampaignReport refused_report(const CampaignConfig& config, const char* detail) {
    CampaignReport report;
    report.refused = true;
    report.base_seed = config.base_seed;
    report.node_count = config.node_count;
    report.parallel = 1; // guided selection is sequential by construction
    report.program = config.program;
    report.verify = config.verify;
    report.id = config.id;
    report.name = config.name;
    report.trace = config.verify || config.trace;
    report.trace_capacity = config.trace_capacity;
    report.events = config.events || config.state;
    report.event_capacity =
        report.events ? (config.event_capacity != 0 ? config.event_capacity : 4096) : 0;
    report.state = config.state;
    report.guidance = config.guidance;
    report.coverage.enabled = config.coverage || config.guidance == SeedGuidance::Coverage;
    if (report.coverage.enabled) report.coverage.method = std::string(coverage_method());
    Finding finding;
    finding.id = kVerifyFailedId;
    finding.detail = detail;
    report.findings.push_back(std::move(finding));
    return report;
}

// §11.4's sentence for one site: which way the campaign failed to exercise it.
inline std::string vacuity_detail(const VacuousSite& site) {
    switch (site.reason) {
    case VacuityReason::NeverActivated:
        return "never activated";
    case VacuityReason::NeverReached:
        return "activated in " + std::to_string(site.activated_runs) + " universes, never reached";
    case VacuityReason::NeverFired:
        return "activated in " + std::to_string(site.activated_runs) + " universes, reached " +
               std::to_string(site.eligible_calls) + " eligible calls, never fired";
    }
    return "?";
}

// §17.4's shape, plus the repro line §15 promises. Findings, never_hit and the sites nothing
// exercised print in a fixed order, all three as separate sections.
inline void print_campaign_report(std::ostream& out, const CampaignReport& report) {
    if (report.refused) {
        out << "REFUSED (" << report.runs << " universes run)\n";
    } else {
        out << "Campaign: " << report.runs << " universes, " << report.failed_runs << " failed, "
            << report.crashed_runs << " crashed";
        if (report.verify) out << "  (verify: double-run)";
        out << "\n";
    }
    for (const Finding& finding : report.findings) {
        out << "FINDING \"" << finding.id << "\"";
        if (!is_harness_id(finding.id)) {
            out << (finding.rare ? "  rare" : "") << "  universes = " << finding.universes_failed;
        }
        out << "\n";
        if (!finding.detail.empty()) out << "  detail  = \"" << finding.detail << "\"\n";
        if (finding.seed != 0) {
            out << "  t       = " << detail::format_ms(finding.at) << "\n";
            out << "  repro   = ";
            if (!report.program.empty()) out << report.program << " ";
            out << "--seed " << finding.seed << "   (base_seed " << report.base_seed << ", index "
                << finding.index << ")\n";
        }
        if (!finding.ledger.empty()) out << finding.ledger;
    }
    for (const std::string& id : report.never_hit) {
        out << "NEVER HIT: \"" << id << "\"\n";
    }
    for (const VacuousSite& site : report.vacuous) {
        out << "NOT EXERCISED: \"" << name_of(site.site) << "\"  (" << vacuity_detail(site)
            << ")\n";
    }
    if (report.out_of_scope_assertions > 0) {
        out << "OUT OF SCOPE: " << report.out_of_scope_assertions
            << " assertion(s) with no universe bound\n";
    }
}

namespace detail {

inline void json_escape(std::ostream& out, const std::string& text) { json::string(out, text); }

// A site's counts only mean something together: activated but never reached is §11.4's vacuity,
// reached but never fired is a rate that never got its chance.
inline void json_sites(std::ostream& out, const CampaignReport& report) {
    out << "  \"sites\": {";
    for (size_t slot = 0; slot < kSiteCount; ++slot) {
        if (slot != 0) out << ", ";
        out << '"' << name_of(site_at_slot(slot))
            << "\": {\"activated\": " << report.activated_runs[slot]
            << ", \"eligible\": " << report.eligible_calls[slot]
            << ", \"fired\": " << report.injections[slot] << '}';
    }
    out << "},\n";
}

} // namespace detail

// The dashboard's input (plan §P4): findings, never_hit kept in its own section, per-site counters
// and the repro seeds. Written to a stream, so the caller decides what to do with it.
inline void print_campaign_json(std::ostream& out, const CampaignReport& report) {
    const json::classic_locale_guard locale(out);
    out << "{\n";
    out << "  \"schema_version\": " << kArtifactSchemaVersion << ",\n";
    out << "  \"campaign_id\": ";
    detail::json_escape(out, report.id);
    out << ",\n  \"name\": ";
    detail::json_escape(out, report.name);
    out << ",\n";
    out << "  \"runs\": " << report.runs << ",\n";
    out << "  \"failed_runs\": " << report.failed_runs << ",\n";
    out << "  \"crashed_runs\": " << report.crashed_runs << ",\n";
    out << "  \"refused\": " << (report.refused ? "true" : "false") << ",\n";
    out << "  \"verify\": " << (report.verify ? "true" : "false") << ",\n";
    out << "  \"trace\": " << (report.trace ? "true" : "false") << ",\n";
    out << "  \"trace_capacity\": " << report.trace_capacity << ",\n";
    out << "  \"events\": " << (report.events ? "true" : "false") << ",\n";
    out << "  \"event_capacity\": " << report.event_capacity << ",\n";
    out << "  \"state\": " << (report.state ? "true" : "false") << ",\n";
    out << "  \"base_seed\": " << report.base_seed << ",\n";
    out << "  \"node_count\": " << report.node_count << ",\n";
    out << "  \"parallel\": " << report.parallel << ",\n";
    out << "  \"guidance\": \"" << name_of(report.guidance) << "\",\n";
    out << "  \"coverage\": {"
        << "\"enabled\": " << (report.coverage.enabled ? "true" : "false") << ", \"method\": ";
    detail::json_escape(out, report.coverage.method);
    out << ", \"runs_with_coverage\": " << report.coverage.runs_with_coverage
        << ", \"total_edges\": " << report.coverage.total_edges
        << ", \"unique_edges\": " << report.coverage.unique_edges
        << ", \"novel_runs\": " << report.coverage.novel_runs
        << ", \"corpus_size\": " << report.coverage.corpus_size
        << ", \"corpus_dropped\": " << report.coverage.corpus_dropped << "},\n";
    out << "  \"base_seed_dec\": ";
    detail::json_escape(out, json::decimal(report.base_seed));
    out << ",\n";
    out << "  \"findings\": [";
    for (size_t i = 0; i < report.findings.size(); ++i) {
        const Finding& finding = report.findings[i];
        out << (i == 0 ? "\n    " : ",\n    ");
        out << "{\"id\": ";
        detail::json_escape(out, finding.id);
        // `seed` stays a number for existing consumers; `seed_dec` is the precision-safe form the
        // UI must use, and `seeds` lists every universe that failed this id.
        out << ", \"index\": " << finding.index << ", \"seed\": " << finding.seed
            << ", \"seed_dec\": ";
        detail::json_escape(out, json::decimal(finding.seed));
        out << ", \"seed_hex\": ";
        detail::json_escape(out, json::hex(finding.seed));
        out << ", \"at_ns\": " << finding.at.ns
            << ", \"universes_failed\": " << finding.universes_failed
            << ", \"rare\": " << (finding.rare ? "true" : "false") << ", \"detail\": ";
        detail::json_escape(out, finding.detail);
        out << ", \"ledger\": ";
        detail::json_escape(out, finding.ledger);
        out << ", \"seeds\": [";
        for (size_t seed_index = 0; seed_index < finding.seeds.size(); ++seed_index) {
            if (seed_index != 0) out << ", ";
            detail::json_escape(out, json::decimal(finding.seeds[seed_index]));
        }
        out << "]}";
    }
    out << (report.findings.empty() ? "],\n" : "\n  ],\n");

    out << "  \"never_hit\": [";
    for (size_t i = 0; i < report.never_hit.size(); ++i) {
        out << (i == 0 ? "\n    " : ",\n    ");
        detail::json_escape(out, report.never_hit[i]);
    }
    out << (report.never_hit.empty() ? "],\n" : "\n  ],\n");

    // Its own group, never a flag on a finding: the Antithesis note under plan §P4 is explicit that
    // "properties that trivially failed by never being reached" are reported apart from failures.
    out << "  \"vacuous\": [";
    for (size_t i = 0; i < report.vacuous.size(); ++i) {
        const VacuousSite& site = report.vacuous[i];
        out << (i == 0 ? "\n    " : ",\n    ");
        out << "{\"site\": \"" << name_of(site.site) << "\", \"reason\": \"" << name_of(site.reason)
            << "\", \"activated_runs\": " << site.activated_runs
            << ", \"eligible\": " << site.eligible_calls << ", \"fired\": " << site.injections
            << '}';
    }
    out << (report.vacuous.empty() ? "],\n" : "\n  ],\n");

    detail::json_sites(out, report);
    out << "  \"out_of_scope_assertions\": " << report.out_of_scope_assertions << "\n";
    out << "}\n";
}

// Level-2 exploration (docs/state-exploration.md §5). The first `initial_trials` universes are
// seeded; after that each seed is derived from a corpus member and kept when its bitmap contains
// an edge the union did not. Every run still crosses a fork, so a crash remains a finding.
inline CampaignReport run_guided(const CampaignConfig& config, const UniverseFn& build_fn) {
    const detail::UniverseOptions options{
        .trace = {.enabled = config.verify || config.trace, .capacity = config.trace_capacity},
        .verify = config.verify,
        .keep_trace_bytes = config.trace && static_cast<bool>(config.on_universe),
        .capture_metadata = static_cast<bool>(config.on_universe),
        .events = config.events || config.state,
        .event_capacity = config.event_capacity != 0 ? config.event_capacity : 4096,
        .state = config.state,
        .coverage = true,
    };

    detail::CampaignAccumulator accumulator;
    const auto& sink = config.on_universe;
    std::vector<CorpusEntry> corpus;
    CoverageSnapshot union_bits;
    union_bits.bits.assign(kCoverageBitmapBytes, 0);
    const uint64_t max_corpus = std::max<uint64_t>(1, config.max_corpus);
    const uint64_t initial =
        std::min<uint64_t>(config.trials, config.initial_trials == 0 ? 8 : config.initial_trials);

    uint64_t runs_with_coverage = 0;
    uint64_t total_edges = 0;
    uint64_t novel_runs = 0;
    uint64_t corpus_dropped = 0;

    auto record = [&](uint64_t index, uint64_t seed, uint64_t parent_seed, uint64_t generation) {
        detail::UniverseOutcome outcome = detail::run_isolated(index, seed, build_fn, options);
        if (outcome.fork_failed) {
            accumulator.add_harness_finding(
                kForkFailedId,
                "fork() failed; that guided universe ran in-process and is not crash-contained");
        }
        accumulator.add(outcome);
        if (sink) sink(outcome);
        if (!outcome.coverage_present) return;
        ++runs_with_coverage;
        total_edges += outcome.coverage.edges;
        const uint64_t novel = coverage_novelty(outcome.coverage, union_bits);
        coverage_merge(union_bits, outcome.coverage);
        if (novel > 0) ++novel_runs;
        if (corpus.empty() || novel > 0) {
            if (corpus.size() < max_corpus) {
                corpus.push_back(CorpusEntry{.seed = seed,
                                             .parent_seed = parent_seed,
                                             .index = index,
                                             .edges = outcome.coverage.edges,
                                             .new_edges = novel,
                                             .hash = outcome.coverage.hash,
                                             .generation = generation,
                                             .finding = !outcome.report.no_check_failed ||
                                                        !outcome.threw.empty() || outcome.died,
                                             .vacuous = outcome.report.vacuous()});
            } else {
                ++corpus_dropped;
            }
        }
    };

    for (uint64_t index = 0; index < initial; ++index) {
        const uint64_t seed = universe_seed(config.base_seed, index);
        record(index, seed, 0, 0);
    }

    // A fixed mutation stream makes the chosen parents and children a function of the campaign
    // base seed, not of wall time or worker scheduling.
    Rng mutation(stream_seed(config.base_seed, StreamDomain::Swarm));
    for (uint64_t index = initial; index < config.trials; ++index) {
        uint64_t parent_seed = 0;
        uint64_t seed = universe_seed(config.base_seed, index);
        if (!corpus.empty()) {
            const CorpusEntry& parent =
                corpus[static_cast<size_t>(mutation.range(0, corpus.size() - 1))];
            parent_seed = parent.seed;
            seed = derive_seed(parent_seed ^ mutation.next(), index + 1);
        }
        record(index, seed, parent_seed, index + 1);
    }

    CampaignReport report = accumulator.finish();
    report.base_seed = config.base_seed;
    report.node_count = config.node_count;
    // Guided selection is sequential by construction: the next seed depends on the previous
    // novelty result. Echo the worker count the caller requested, but do not imply it ran.
    report.parallel = 1; // guided selection is sequential by construction
    report.program = config.program;
    report.verify = config.verify;
    report.id = config.id;
    report.name = config.name;
    report.trace = config.verify || config.trace;
    report.trace_capacity = config.trace_capacity;
    report.events = config.events || config.state;
    report.event_capacity =
        report.events ? (config.event_capacity != 0 ? config.event_capacity : 4096) : 0;
    report.state = config.state;
    report.guidance = SeedGuidance::Coverage;
    report.coverage.enabled = true;
    report.coverage.runs_with_coverage = runs_with_coverage;
    report.coverage.total_edges = total_edges;
    report.coverage.unique_edges = union_bits.edges;
    report.coverage.novel_runs = novel_runs;
    report.coverage.corpus_size = corpus.size();
    report.coverage.corpus_dropped = corpus_dropped;
    report.coverage.method = std::string(coverage_method());
    report.corpus = std::move(corpus);
    return report;
}

class Campaign {
  public:
    static CampaignReport run(const CampaignConfig& config, const NodeUniverseFn& build_fn) {
        const uint32_t nodes = config.node_count;
        return run(config, UniverseFn([build_fn, nodes](Simulator& sim, uint64_t index) {
                       return build_fn(sim, index, nodes);
                   }));
    }

    static CampaignReport run(const CampaignConfig& config, const UniverseFn& build_fn) {
        // A zero-capacity trace cannot record, so verify would have nothing to hash.
        if (config.verify && config.trace_capacity == 0) {
            return refused_report(config, "CampaignConfig::verify needs trace_capacity > 0");
        }
        if (config.trace && config.trace_capacity == 0) {
            return refused_report(config, "CampaignConfig::trace needs trace_capacity > 0");
        }
        if (config.events && config.event_capacity == 0) {
            return refused_report(config, "CampaignConfig::events needs event_capacity > 0");
        }
        if (config.guidance == SeedGuidance::Coverage) {
            return run_guided(config, build_fn);
        }
        const detail::UniverseOptions options{
            .trace = {.enabled = config.verify || config.trace, .capacity = config.trace_capacity},
            .verify = config.verify,
            .keep_trace_bytes = config.trace && static_cast<bool>(config.on_universe),
            .capture_metadata = static_cast<bool>(config.on_universe),
            .events = config.events || config.state,
            .event_capacity = config.event_capacity != 0 ? config.event_capacity : 4096,
            .state = config.state,
            .coverage = config.coverage,
        };
        const auto& sink = config.on_universe;

        const unsigned workers = std::max(1u, config.parallel);
        struct LaunchedWorker {
            detail::Slice slice;
            int fd = -1;
            pid_t pid = -1;
        };
        std::vector<LaunchedWorker> launched;
        for (unsigned worker = 0; worker < workers; ++worker) {
            const detail::Slice slice = detail::slice_for(config.trials, workers, worker);
            if (slice.begin == slice.end) continue;
            const int fd = detail::open_scratch_file();
            const pid_t pid = fd < 0 ? -1 : ::fork();
            // The child runs its slice and leaves; _exit skips inherited stdio and atexit handlers.
            if (pid == 0) {
                try {
                    if (!detail::run_slice_to_file(slice, config.base_seed, build_fn, options,
                                                   fd)) {
                        ::_exit(1);
                    }
                } catch (...) {
                    ::_exit(1);
                }
                ::_exit(0);
            }
            launched.push_back(LaunchedWorker{slice, fd, pid});
        }

        detail::CampaignAccumulator accumulator(config.coverage, config.max_corpus);
        for (const LaunchedWorker& worker : launched) {
            if (worker.pid > 0) {
                int status = 0;
                ::waitpid(worker.pid, &status, 0);
                detail::OutcomeReader reader(worker.fd);
                uint64_t next = worker.slice.begin;
                detail::UniverseOutcome outcome;
                while (reader.next(outcome)) {
                    accumulator.add(outcome);
                    if (sink) sink(outcome);
                    ++next;
                }
                // Shorter than the slice means the worker died or wrote short: recover the rest.
                if (next < worker.slice.end) {
                    detail::recover_slice(next, worker.slice.end, config.base_seed, build_fn,
                                          options, accumulator, sink);
                }
            } else {
                accumulator.add_harness_finding(
                    kForkFailedId,
                    "fork() failed; that slice ran in-process and is not crash-contained");
                detail::run_slice_into(worker.slice, config.base_seed, build_fn, options,
                                       accumulator, sink);
            }
            if (worker.fd >= 0) ::close(worker.fd);
        }

        CampaignReport report = accumulator.finish();
        report.base_seed = config.base_seed;
        report.node_count = config.node_count;
        report.parallel = config.parallel;
        report.program = config.program;
        report.verify = config.verify;
        report.id = config.id;
        report.name = config.name;
        report.trace = config.verify || config.trace;
        report.trace_capacity = config.trace_capacity;
        report.events = config.events || config.state;
        report.event_capacity =
            report.events ? (config.event_capacity != 0 ? config.event_capacity : 4096) : 0;
        report.state = config.state;
        report.guidance = config.guidance;
        report.coverage.enabled = config.coverage;
        if (config.coverage) report.coverage.method = std::string(coverage_method());
        return report;
    }
};

} // namespace cosmos
