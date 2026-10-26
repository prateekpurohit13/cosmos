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
#include "cosmos/scenario.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <time.h>
#include <utility>

namespace {

cosmos::FaultConfig demo_plan() {
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    plan.enable_class(cosmos::FaultClass::Clock);
    (void)plan.activate_site(cosmos::SiteId::malloc);
    (void)plan.activate_site(cosmos::SiteId::clock_gettime);

    cosmos::FaultRule memory;
    memory.rate = 0.35;
    (void)memory.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0);
    (void)plan.set_rule(cosmos::SiteId::malloc, std::move(memory));

    cosmos::FaultRule clock;
    clock.rate = 0.4;
    clock.amount = cosmos::Duration{50'000};
    (void)clock.outcomes.add(cosmos::FaultKind::ClockStep, 1.0);
    (void)plan.set_rule(cosmos::SiteId::clock_gettime, std::move(clock));
    return plan;
}

// Explicit wrapped calls are the only place the demo's faults can fire. The loop deliberately
// interleaves memory and clock decisions so the exported trace has more than one lane and more
// than one virtual timestamp. A fired OOM returns null; freeing null is harmless, so the demo
// application never has to recover from the fault to stay deterministic.
cosmos::ScenarioReport run_demo_universe(cosmos::Simulator& sim, uint64_t index) {
    auto scenario = cosmos::Scenario::on(sim, demo_plan());
    if (!scenario) {
        cosmos::ScenarioReport rejected;
        rejected.no_check_failed = false;
        return rejected;
    }
    scenario->run([&sim] {
        for (int step = 0; step < 12; ++step) {
            void* block = std::malloc(16);
            std::free(block);
            struct timespec now{};
            (void)clock_gettime(CLOCK_MONOTONIC, &now);
            sim.advance_time(cosmos::Duration{100'000});
        }
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
