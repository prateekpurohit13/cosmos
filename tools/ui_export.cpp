// Writes a v1 UI artifact directory from a tiny deterministic campaign.
//
// This is the Phase-0/1 proof that one command can produce a directory the local UI can load
// (docs/ui-design.md U0.3, G0). It is deliberately a demo application, not the UI itself: the
// campaign body has one explicit malloc and a check that fails on odd universe indices, so the
// artifact has a finding, a trace, a passing universe and a vacuous site without needing a
// separate application fixture.
//
//   cosmos_ui_export [output-dir] [campaign-id] [trials]
//   cosmos_ui_export ui/fixtures/demo demo 4

#include "cosmos/campaign.hpp"
#include "cosmos/export.hpp"
#include "cosmos/scenario.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

cosmos::FaultConfig demo_plan() {
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    (void)plan.activate_site(cosmos::SiteId::malloc);
    cosmos::FaultRule rule;
    rule.rate = 1.0;
    (void)rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0);
    (void)plan.set_rule(cosmos::SiteId::malloc, std::move(rule));
    return plan;
}

// One explicit allocation is the only wrapped call the fault can fire on. Under the shared
// libstdc++ link the harness's std::strings and vectors do not reach __wrap_malloc, so the
// universe stays deterministic and cheap.
cosmos::ScenarioReport run_demo_universe(cosmos::Simulator& sim, uint64_t index) {
    auto scenario = cosmos::Scenario::on(sim, demo_plan());
    if (!scenario) {
        cosmos::ScenarioReport rejected;
        rejected.no_check_failed = false;
        return rejected;
    }
    scenario->run([] {
        void* block = std::malloc(16);
        std::free(block);
    });
    scenario->quiesce();
    // A non-zero virtual end makes the artifact's time labels reviewable.
    sim.advance_time(cosmos::Duration{1'000'000});
    scenario->check(
        "odd-index-fails", [index] { return index % 2 == 0; },
        "universe " + std::to_string(index) + " was asked to fail");
    scenario->note_covered("odd-index-seen", index % 2 == 1);
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
    campaign.on_universe = writer->sink();

    const cosmos::CampaignReport report = cosmos::Campaign::run(campaign, run_demo_universe);
    if (auto finished = writer->finish(report); !finished) {
        std::cerr << "cosmos_ui_export: " << finished.error() << "\n";
        return 1;
    }

    std::cout << "wrote " << root << " (" << report.runs << " universes, " << report.failed_runs
              << " failed, " << report.findings.size() << " findings)\n";
    cosmos::print_campaign_report(std::cout, report);
    return 0;
}
