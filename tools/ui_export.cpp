// Writes a v1 UI artifact directory from a tiny deterministic campaign.
//
// This is the Phase-0/1/2 proof that one command can produce a directory the local UI can load
// (docs/ui-design.md U0.3, G0). It is deliberately a demo application, not the UI itself: the
// campaign body makes explicit memory and clock calls and a check that fails on odd universe
// indices, so the artifact has a finding, a multi-entry canonical trace, a passing universe and
// a vacuous site without needing a separate application fixture.
//
//   cosmos_ui_export [output-dir] [campaign-id] [trials]
//   cosmos_ui_export ui/fixtures/demo demo 4

#include "cosmos/campaign.hpp"
#include "cosmos/export.hpp"
#include "cosmos/minimize.hpp"
#include "cosmos/minimize_json.hpp"
#include "demo_scenario.hpp"

#include "cosmos/scenario.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <pthread.h>
#include <sstream>
#include <string>
#include <time.h>
#include <utility>
#include <vector>

namespace {

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
    const std::string root = argc > 1 ? argv[1] : "ui/fixtures/demo";
    const std::string campaign_id = argc > 2 ? argv[2] : "demo";
    const uint64_t trials = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 4;

    cosmos::CampaignConfig campaign;
    campaign.trials = trials;
    campaign.base_seed = 1;
    campaign.parallel = 1;
    campaign.program = "cosmos_ui_export";
    campaign.id = campaign_id;
    campaign.name = "Cosmos demo campaign";
    campaign.trace = true;
    campaign.trace_capacity = 4096;
    campaign.events = true;
    campaign.event_capacity = 2048;
    campaign.state = true;

    cosmos::ArtifactOptions options;
    options.root = root;
    options.campaign = campaign;
    options.build.id = "build-demo";
    options.build.commit = "0000000000000000000000000000000000000000";
    options.build.mode = "SIM";
    options.build.compiler = compiler_name();
    options.build.host = "demo";

    auto writer = cosmos::CampaignArtifactWriter::create(std::move(options));
    if (!writer) {
        std::cerr << "cosmos_ui_export: " << writer.error() << "\n";
        return 1;
    }
    // The writer receives every outcome in index order. Keep only the small identity map for all
    // universes and the canonical bytes for universes that failed a check: a passing trace is not
    // a minimization candidate, so retaining it would multiply memory for no reason.
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

    const cosmos::CampaignReport report =
        cosmos::Campaign::run(campaign, cosmos::demo::run_demo_universe);

    // Minimization is report-only (O5): the exporter runs it after the campaign over the recorded
    // traces and writes one document per failing seed. The UI never executes it.
    //
    // The artifact layout holds one report per seed. Findings are sorted by assertion id, so if a
    // seed fails several assertions the first finding wins deterministically; the writer rejects
    // a second document for the same seed. Per-finding reports are a future addition (G19).
    std::map<uint64_t, std::string> minimized_findings;
    uint64_t minimized = 0;
    uint64_t skipped = 0;
    auto write_refused = [&](uint64_t seed, const std::string& finding_id, std::string reason) {
        cosmos::MinimizeReport refused;
        refused.seed = seed;
        refused.finding_id = finding_id;
        refused.program = campaign.program;
        refused.refused = true;
        refused.reason = std::move(reason);
        std::ostringstream json;
        cosmos::print_minimize_json(json, refused);
        if (auto written = writer->write_minimization(seed, json.str()); !written) {
            std::cerr << "cosmos_ui_export: " << written.error() << "\n";
            return false;
        }
        minimized_findings[seed] = finding_id;
        ++minimized;
        return true;
    };

    for (const cosmos::Finding& finding : report.findings) {
        if (cosmos::is_harness_id(finding.id)) continue;
        // A report's finding identity is load-bearing; the artifact validator rejects an empty
        // id rather than accepting a report the UI cannot attribute to an assertion.
        if (finding.id.empty()) {
            std::cerr << "cosmos_ui_export: skipping minimization for an empty finding id\n";
            continue;
        }
        for (const uint64_t seed : finding.seeds) {
            if (minimized_findings.find(seed) != minimized_findings.end()) {
                ++skipped;
                continue;
            }
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
            const auto index = index_by_seed.find(seed);
            if (index == index_by_seed.end()) {
                if (!write_refused(seed, finding.id,
                                   "the universe index was not recorded for this seed")) {
                    return 1;
                }
                continue;
            }

            // Scenario::on installs this plan directly; the demo does not sample a swarm config,
            // so the template is the realized config for these universes.
            cosmos::MinimizeConfig minimize_config;
            minimize_config.plan = cosmos::demo::demo_plan();
            minimize_config.finding_id = finding.id;
            minimize_config.node_count = campaign.node_count;
            minimize_config.program = campaign.program;
            const uint64_t universe_index = index->second;
            const auto body = [universe_index](cosmos::Scenario& scenario) {
                cosmos::demo::run_demo_body(scenario, universe_index);
            };
            const cosmos::MinimizeReport minimized_report =
                cosmos::minimize(*source, minimize_config, body);
            std::ostringstream json;
            cosmos::print_minimize_json(json, minimized_report);
            if (auto written = writer->write_minimization(seed, json.str()); !written) {
                std::cerr << "cosmos_ui_export: " << written.error() << "\n";
                return 1;
            }
            minimized_findings[seed] = finding.id;
            ++minimized;
        }
    }

    if (auto finished = writer->finish(report); !finished) {
        std::cerr << "cosmos_ui_export: " << finished.error() << "\n";
        return 1;
    }

    std::cout << "wrote " << root << " (" << report.runs << " universes, " << report.failed_runs
              << " failed, " << report.findings.size() << " findings, " << minimized
              << " minimization reports)\n";
    if (skipped > 0) {
        std::cout << "skipped " << skipped
                  << " finding seed(s) that already had a minimization report "
                     "(one report per seed)\n";
    }
    cosmos::print_campaign_report(std::cout, report);
    return 0;
}
