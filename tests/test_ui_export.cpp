#include "cosmos/campaign.hpp"
#include "cosmos/export.hpp"
#include "cosmos/faults_json.hpp"
#include "cosmos/minimize.hpp"
#include "cosmos/minimize_json.hpp"
#include "cosmos/scenario.hpp"
#include "cosmos/trace.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace {

void must(bool ok) { assert(ok); }

cosmos::FaultConfig oom_plan(double rate) {
    cosmos::FaultConfig plan;
    plan.enable_class(cosmos::FaultClass::Memory);
    must(plan.activate_site(cosmos::SiteId::malloc));
    cosmos::FaultRule rule;
    rule.rate = rate;
    must(rule.outcomes.add(cosmos::FaultKind::OutOfMemory, 1.0));
    must(plan.set_rule(cosmos::SiteId::malloc, std::move(rule)));
    return plan;
}

// Deterministic across the two runs: one explicit malloc so the wrapped allocator is the only
// gate, and a check that is a pure function of the universe index. The fault fires either way;
// only the applications's response (the check) differs.
cosmos::ScenarioReport run_universe(cosmos::Simulator& sim, uint64_t index) {
    auto scenario = cosmos::Scenario::on(sim, oom_plan(1.0));
    must(scenario.has_value());
    scenario->run([] {
        void* block = std::malloc(16);
        std::free(block);
    });
    scenario->quiesce();
    sim.advance_time(cosmos::Duration{1'000'000});
    scenario->check(
        "odd-index-fails", [index] { return index % 2 == 0; }, "index=" + std::to_string(index));
    scenario->note_covered("odd-index-seen", index % 2 == 1);
    return scenario->report();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    must(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool is_balanced(std::string_view text) {
    size_t braces = 0;
    size_t brackets = 0;
    for (const char c : text) {
        if (c == '{') ++braces;
        if (c == '}') --braces;
        if (c == '[') ++brackets;
        if (c == ']') --brackets;
    }
    return braces == 0 && brackets == 0;
}

std::vector<std::string> lines_of(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (const char c : text) {
        if (c == '\n') {
            if (!current.empty()) lines.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

const cosmos::Finding* find_finding(const cosmos::CampaignReport& report, const std::string& id) {
    for (const cosmos::Finding& finding : report.findings) {
        if (finding.id == id) return &finding;
    }
    return nullptr;
}

cosmos::ArtifactOptions artifact_options(const std::filesystem::path& root,
                                         cosmos::CampaignConfig campaign) {
    cosmos::ArtifactOptions options;
    options.root = root;
    options.campaign = std::move(campaign);
    options.build.id = "build-test-1";
    options.build.commit = "0123456789abcdef";
    options.build.binary_hash = "sha256:test";
    options.build.mode = "SIM";
    options.build.compiler = "test";
    options.build.host = "localhost";
    options.repro = "test-program --seed {seed}";
    return options;
}

std::filesystem::path test_root(std::string_view name) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        (std::string(name) + "-" + std::to_string(static_cast<long>(::getpid())));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return root;
}

size_t count_files(const std::filesystem::path& directory) {
    size_t count = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error)) ++count;
    }
    must(!error);
    return count;
}

// Exports a campaign with a sink. Used by the re-export and parallel-ordering tests, which care
// about the directory state rather than the individual assertions in the round-trip test.
cosmos::CampaignReport write_artifact_campaign(const std::filesystem::path& root, uint64_t trials,
                                               const std::string& id, unsigned parallel = 1) {
    cosmos::CampaignConfig config;
    config.trials = trials;
    config.base_seed = 42;
    config.parallel = parallel;
    config.program = "test_ui_export";
    config.id = id;
    config.name = "Artifact test";
    config.trace = true;
    config.trace_capacity = 4096;

    auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, config));
    must(writer.has_value());
    config.on_universe = writer->sink();
    const cosmos::CampaignReport report = cosmos::Campaign::run(config, run_universe);
    const auto finished = writer->finish(report);
    if (!finished) std::cerr << "artifact finish failed: " << finished.error() << std::endl;
    must(finished.has_value());
    return report;
}

void test_config_fingerprint_is_stable_and_sensitive() {
    const cosmos::FaultConfig first = oom_plan(1.0);
    const cosmos::FaultConfig second = oom_plan(1.0);
    const cosmos::FaultConfig different = oom_plan(0.5);

    must(cosmos::config_fingerprint(first) == cosmos::config_fingerprint(second));
    must(cosmos::config_fingerprint(first) != cosmos::config_fingerprint(different));
    must(cosmos::config_fingerprint(first).starts_with("0x"));
    must(cosmos::config_fingerprint(first).size() == 18);

    std::ostringstream json;
    cosmos::print_config_json(json, first);
    const std::string text = json.str();
    must(text.find("\"mode\": \"Safety\"") != std::string::npos);
    must(text.find("\"malloc\"") != std::string::npos);
    must(text.find("\"OutOfMemory\"") != std::string::npos);
    must(is_balanced(text));

    std::cout << "[PASS] test_config_fingerprint_is_stable_and_sensitive" << std::endl;
}

void test_minimize_json_is_versioned_and_explicit() {
    cosmos::TraceEntry first{};
    first.seq = 5;
    first.eligible_index = 3;
    first.at = cosmos::Time{200'000};
    first.site = cosmos::SiteId::malloc;
    first.outcome = cosmos::FaultKind::OutOfMemory;
    first.drew = true;
    first.status = cosmos::DecisionStatus::Fired;

    cosmos::TraceEntry second = first;
    second.seq = 7;
    second.eligible_index = 4;
    second.at = cosmos::Time{900'000};
    second.site = cosmos::SiteId::clock_gettime;
    second.amount = cosmos::Duration{50'000};
    second.outcome = cosmos::FaultKind::ClockStep;

    cosmos::TraceEntry third = first;
    third.seq = 9;
    third.eligible_index = 5;
    third.at = cosmos::Time{1'200'000};

    cosmos::MinimizeReport report;
    report.seed = 0x5CA1AB1E;
    report.finding_id = "double-oom";
    report.program = "test_minimize";
    report.candidates = {first, second, third};
    report.kept = {second, third};
    report.passes = 2;
    report.replays = 6;
    report.crashed_trials = 1;

    std::ostringstream json;
    cosmos::print_minimize_json(json, report);
    const std::string text = json.str();
    must(text.find("\"schema_version\": 1") != std::string::npos);
    must(text.find("\"seed\": \"1554098974\"") != std::string::npos);
    must(text.find("\"seed_hex\": \"0x000000005ca1ab1e\"") != std::string::npos);
    must(text.find("\"finding_id\": \"double-oom\"") != std::string::npos);
    must(text.find("\"only_faults\": [7, 9]") != std::string::npos);
    must(text.find("\"repro\": \"test_minimize --seed 1554098974 --only-faults=7,9\"") !=
         std::string::npos);
    must(text.find("\"fault_class\": \"Clock\"") != std::string::npos);
    must(text.find("\"amount_ns\": 50000") != std::string::npos);
    must(text.find("\"passes\": 2") != std::string::npos);
    must(text.find("\"replays\": 6") != std::string::npos);
    must(text.find("\"crashed_trials\": 1") != std::string::npos);
    must(is_balanced(text));

    report.refused = true;
    report.reason = "the trace is truncated";
    report.kept.clear();
    std::ostringstream refused_json;
    cosmos::print_minimize_json(refused_json, report);
    const std::string refused = refused_json.str();
    must(refused.find("\"refused\": true") != std::string::npos);
    must(refused.find("\"reason\": \"the trace is truncated\"") != std::string::npos);
    must(refused.find("\"repro\": null") != std::string::npos);
    must(refused.find("\"kept\": []") != std::string::npos);
    must(refused.find("\"seq\": 5") != std::string::npos);
    must(refused.find("\"crashed_trials\": 1") != std::string::npos);

    // An empty kept set is a completed report, not a refused one: the exact command is still a
    // replay (with every fault withheld), so it must not become null.
    report.refused = false;
    report.reason.clear();
    std::ostringstream empty_json;
    cosmos::print_minimize_json(empty_json, report);
    const std::string empty = empty_json.str();
    must(empty.find("\"repro\": \"test_minimize --seed 1554098974 --only-faults=\"") !=
         std::string::npos);
    must(empty.find("\"repro\": null") == std::string::npos);

    // Free-form finding ids and program names are JSON text, not shell-safe tokens or raw bytes.
    report.finding_id = "odd-\"id\n";
    report.program = "test \"min\"";
    std::ostringstream escaped_json;
    cosmos::print_minimize_json(escaped_json, report);
    const std::string escaped = escaped_json.str();
    must(escaped.find("\"finding_id\": \"odd-\\\"id\\n\"") != std::string::npos);
    must(escaped.find("\"repro\": \"test \\\"min\\\" --seed 1554098974 "
                      "--only-faults=\"") != std::string::npos);
    must(is_balanced(escaped));

    std::cout << "[PASS] test_minimize_json_is_versioned_and_explicit" << std::endl;
}

void test_campaign_json_adds_the_v1_fields() {
    cosmos::CampaignConfig config;
    config.trials = 6;
    config.base_seed = 42;
    config.parallel = 1;
    config.program = "test_ui_export";
    config.id = "json-only";
    config.name = "JSON only";
    config.trace = true;
    config.trace_capacity = 4096;
    const cosmos::CampaignReport report = cosmos::Campaign::run(config, run_universe);

    must(report.id == "json-only");
    must(report.name == "JSON only");
    must(report.trace);
    must(report.trace_capacity == 4096);

    const cosmos::Finding* finding = find_finding(report, "odd-index-fails");
    must(finding != nullptr);
    must(finding->at.ns == 1'000'000);
    must(finding->seeds.size() == 3);
    must(finding->seeds[0] == cosmos::universe_seed(config.base_seed, 1));
    must(finding->seeds[1] == cosmos::universe_seed(config.base_seed, 3));
    must(finding->seeds[2] == cosmos::universe_seed(config.base_seed, 5));

    std::ostringstream json;
    cosmos::print_campaign_json(json, report);
    const std::string text = json.str();
    must(text.find("\"schema_version\": 1") != std::string::npos);
    must(text.find("\"campaign_id\": \"json-only\"") != std::string::npos);
    must(text.find("\"at_ns\": 1000000") != std::string::npos);
    must(text.find("\"seed_dec\": ") != std::string::npos);
    must(text.find("\"seeds\": [") != std::string::npos);
    must(is_balanced(text));

    std::cout << "[PASS] test_campaign_json_adds_the_v1_fields" << std::endl;
}

void test_artifact_writer_round_trips_one_campaign() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("cosmos-ui-export-" + std::to_string(static_cast<long>(::getpid())));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    cosmos::CampaignConfig config;
    config.trials = 6;
    config.base_seed = 42;
    config.parallel = 1;
    config.program = "test_ui_export";
    config.id = "artifact-test";
    config.name = "Artifact test";
    config.trace = true;
    config.trace_capacity = 4096;

    auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, config));
    must(writer.has_value());
    config.on_universe = writer->sink();
    const cosmos::CampaignReport report = cosmos::Campaign::run(config, run_universe);
    if (auto finished = writer->finish(report); !finished) {
        std::cerr << "artifact finish failed: " << finished.error() << std::endl;
        must(false);
    }

    const std::string manifest = read_file(root / cosmos::kManifestFile);
    must(manifest.find("\"schema_version\": 1") != std::string::npos);
    must(manifest.find("\"campaign_id\": \"artifact-test\"") != std::string::npos);
    must(manifest.find("\"complete\": true") != std::string::npos);
    must(manifest.find("\"id\": \"build-test-1\"") != std::string::npos);
    must(manifest.find("\"trace\": true") != std::string::npos);
    must(manifest.find("\"repro\": \"test-program --seed {seed}\"") != std::string::npos);
    must(is_balanced(manifest));

    const std::string campaign = read_file(root / cosmos::kCampaignFile);
    must(campaign.find("\"runs\": 6") != std::string::npos);
    must(campaign.find("\"schema_version\": 1") != std::string::npos);
    must(is_balanced(campaign));

    const std::vector<std::string> rows = lines_of(read_file(root / cosmos::kUniverseIndexFile));
    must(rows.size() == 6);
    for (size_t index = 0; index < rows.size(); ++index) {
        must(is_balanced(rows[index]));
        must(rows[index].find("\"index\": " + std::to_string(index)) != std::string::npos);
        const std::string expected_status = index % 2 == 0 ? "passed" : "failed";
        must(rows[index].find("\"status\": \"" + expected_status + "\"") != std::string::npos);
        must(std::filesystem::exists(root / cosmos::kUniverseDir /
                                     (std::to_string(index) + ".json")));
    }

    const std::string failing = read_file(root / cosmos::kUniverseDir / "1.json");
    must(failing.find("\"status\": \"failed\"") != std::string::npos);
    must(failing.find("\"seed\": \"") != std::string::npos);
    must(failing.find("\"checks\": [") != std::string::npos);
    must(failing.find("\"ledger\": [") != std::string::npos);
    must(failing.find("\"config\": {") != std::string::npos);
    must(failing.find("\"present\": true") != std::string::npos);
    must(failing.find("\"path\": \"traces/1.bin\"") != std::string::npos);
    must(is_balanced(failing));

    const std::string passing = read_file(root / cosmos::kUniverseDir / "0.json");
    must(passing.find("\"status\": \"passed\"") != std::string::npos);
    must(passing.find("\"present\": true") != std::string::npos);

    const std::string trace_bytes = read_file(root / cosmos::kTraceDir / "1.bin");
    const auto decoded = cosmos::decode_trace(trace_bytes);
    must(decoded.has_value());
    must(decoded->universe_seed() == cosmos::universe_seed(config.base_seed, 1));
    must(decoded->size() > 0);
    must(failing.find(cosmos::json::hex(cosmos::trace_hash(*decoded))) != std::string::npos);

    // Config identities agree across the aggregate and the per-universe artifacts.
    const cosmos::Finding* finding = find_finding(report, "odd-index-fails");
    must(finding != nullptr);
    must(finding->seeds.size() == 3);
    must(failing.find(cosmos::config_fingerprint(oom_plan(1.0))) != std::string::npos);

    // No temp files survived an atomic write.
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        must(entry.path().extension() != ".tmp");
    }

    std::filesystem::remove_all(root, ignored);
    std::cout << "[PASS] test_artifact_writer_round_trips_one_campaign" << std::endl;
}

void test_artifact_writer_exports_a_minimization_report() {
    const std::filesystem::path root = test_root("cosmos-ui-minimization");

    cosmos::CampaignConfig config;
    config.trials = 4;
    config.base_seed = 42;
    config.parallel = 1;
    config.program = "test_ui_export";
    config.id = "minimization-test";
    config.name = "Minimization test";
    config.trace = true;
    config.trace_capacity = 4096;

    auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, config));
    must(writer.has_value());
    config.on_universe = writer->sink();
    const cosmos::CampaignReport report = cosmos::Campaign::run(config, run_universe);
    const cosmos::Finding* finding = find_finding(report, "odd-index-fails");
    must(finding != nullptr);
    must(!finding->seeds.empty());

    cosmos::MinimizeReport minimization;
    minimization.seed = finding->seeds.front();
    minimization.finding_id = finding->id;
    minimization.program = config.program;
    cosmos::TraceEntry entry{};
    entry.seq = 3;
    entry.eligible_index = 1;
    entry.site = cosmos::SiteId::malloc;
    entry.outcome = cosmos::FaultKind::OutOfMemory;
    entry.drew = true;
    entry.status = cosmos::DecisionStatus::Fired;
    minimization.candidates = {entry};
    minimization.kept = {entry};
    minimization.passes = 1;
    minimization.replays = 2;

    std::ostringstream minimization_json;
    cosmos::print_minimize_json(minimization_json, minimization);
    must(writer->write_minimization(minimization.seed, minimization_json.str()).has_value());
    must(writer->finish(report).has_value());

    const std::filesystem::path report_path =
        root / cosmos::minimization_relative_path(minimization.seed);
    must(std::filesystem::exists(report_path));
    const std::string text = read_file(report_path);
    must(text.find("\"schema_version\": 1") != std::string::npos);
    must(text.find("\"finding_id\": \"odd-index-fails\"") != std::string::npos);
    must(text.find("\"only_faults\": [3]") != std::string::npos);
    must(is_balanced(text));
    must(read_file(root / cosmos::kManifestFile).find("\"minimization_dir\": \"minimizations\"") !=
         std::string::npos);

    // A report may only name a seed this campaign actually wrote, a second report for the same
    // seed must not replace the first, and a finished writer accepts no more documents.
    must(!writer->write_minimization(minimization.seed, minimization_json.str()).has_value());
    must(writer->error().empty());

    const std::filesystem::path rejected_root = test_root("cosmos-ui-minimization-reject");
    auto rejected = cosmos::CampaignArtifactWriter::create(artifact_options(rejected_root, config));
    must(rejected.has_value());
    must(!rejected->write_minimization(1234, "{}").has_value());
    must(rejected->error().find("was not written by this campaign") != std::string::npos);

    const std::filesystem::path duplicate_root = test_root("cosmos-ui-minimization-duplicate");
    auto duplicate =
        cosmos::CampaignArtifactWriter::create(artifact_options(duplicate_root, config));
    must(duplicate.has_value());
    cosmos::detail::UniverseOutcome outcome;
    outcome.index = 0;
    outcome.seed = 42;
    outcome.report.seed = 42;
    outcome.report.checks.push_back(cosmos::CheckResult{"passes", "", true, cosmos::Time{}});
    duplicate->sink()(outcome);
    must(duplicate->write_minimization(42, "{\"schema_version\": 1, \"first\": true}").has_value());
    must(!duplicate->write_minimization(42, "{\"schema_version\": 1, \"second\": true}")
              .has_value());
    must(duplicate->error().find("already written") != std::string::npos);
    const std::string duplicate_text =
        read_file(duplicate_root / cosmos::minimization_relative_path(42));
    must(duplicate_text.find("\"first\"") != std::string::npos);
    must(duplicate_text.find("\"second\"") == std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::remove_all(rejected_root, ignored);
    std::filesystem::remove_all(duplicate_root, ignored);
    std::cout << "[PASS] test_artifact_writer_exports_a_minimization_report" << std::endl;
}

void test_verify_without_trace_bytes_still_exports_a_hash() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() /
        ("cosmos-ui-verify-" + std::to_string(static_cast<long>(::getpid())));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);

    cosmos::CampaignConfig config;
    config.trials = 2;
    config.base_seed = 7;
    config.parallel = 1;
    config.verify = true;
    config.trace_capacity = 4096;
    config.id = "verify-test";

    auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, config));
    must(writer.has_value());
    config.on_universe = writer->sink();
    const cosmos::CampaignReport report = cosmos::Campaign::run(config, run_universe);
    must(!report.refused);
    must(report.trace);
    must(report.findings.size() == 1);
    must(report.findings[0].id == "odd-index-fails");
    must(writer->finish(report).has_value());

    const std::string universe = read_file(root / cosmos::kUniverseDir / "0.json");
    must(universe.find("\"present\": false") != std::string::npos);
    must(universe.find("\"traced\": true") != std::string::npos);
    must(universe.find("\"hash\": \"0x0000000000000000\"") == std::string::npos);
    must(!std::filesystem::exists(root / cosmos::kTraceDir / "0.bin"));

    std::filesystem::remove_all(root, ignored);
    std::cout << "[PASS] test_verify_without_trace_bytes_still_exports_a_hash" << std::endl;
}

void test_events_and_state_artifacts_round_trip() {
    for (const bool explicit_events : {true, false}) {
        const std::string name = explicit_events ? "cosmos-ui-events" : "cosmos-ui-state-only";
        const std::filesystem::path root = test_root(name);
        cosmos::CampaignConfig config;
        config.trials = 1;
        config.base_seed = 11;
        config.parallel = 1;
        config.id = name;
        config.events = explicit_events;
        config.event_capacity = explicit_events ? 64 : 0;
        config.state = true;

        auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, config));
        must(writer.has_value());
        config.on_universe = writer->sink();
        const cosmos::CampaignReport report = cosmos::Campaign::run(config, run_universe);
        must(!report.refused);
        must(report.events);
        must(report.event_capacity == (explicit_events ? 64 : 4096));
        must(report.state);
        must(writer->finish(report).has_value());

        const std::filesystem::path event_path = root / cosmos::kEventDir / "0.bin";
        must(std::filesystem::exists(event_path));
        const std::string bytes = read_file(event_path);
        must(bytes.size() >= cosmos::kEventStreamHeaderBytes);
        const auto decoded = cosmos::decode_events(bytes);
        must(decoded.has_value());
        must(!decoded->entries.empty());

        const std::string manifest = read_file(root / cosmos::kManifestFile);
        // The manifest keeps the requested launch switch; the report/campaign.json carries the
        // derived intent (state implies capture), and finish() validates the derivation.
        must(manifest.find(explicit_events ? "\"events\": true" : "\"events\": false") !=
             std::string::npos);
        must(manifest.find("\"state\": true") != std::string::npos);
        const std::string universe = read_file(root / cosmos::kUniverseDir / "0.json");
        must(universe.find("\"events\": {\"present\": true") != std::string::npos);
        must(universe.find("\"state\": {\"schema_version\": 1") != std::string::npos);
        const std::string row = read_file(root / cosmos::kUniverseIndexFile);
        must(row.find("\"state_present\": true") != std::string::npos);

        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::cout << "[PASS] test_events_and_state_artifacts_round_trip" << std::endl;
}

void test_reexport_removes_stale_universe_and_trace_files() {
    const std::filesystem::path root = test_root("cosmos-ui-reexport");
    write_artifact_campaign(root, 6, "reexport");
    must(count_files(root / cosmos::kUniverseDir) == 6);
    must(count_files(root / cosmos::kTraceDir) == 6);
    // A stale report must not survive a smaller re-export.
    std::ofstream(root / cosmos::kMinimizationDir / "stale.json") << "{}";
    must(count_files(root / cosmos::kMinimizationDir) == 1);

    write_artifact_campaign(root, 2, "reexport");
    must(count_files(root / cosmos::kUniverseDir) == 2);
    must(count_files(root / cosmos::kTraceDir) == 2);
    must(count_files(root / cosmos::kMinimizationDir) == 0);
    must(!std::filesystem::exists(root / cosmos::kUniverseDir / "2.json"));
    must(!std::filesystem::exists(root / cosmos::kTraceDir / "2.bin"));
    must(lines_of(read_file(root / cosmos::kUniverseIndexFile)).size() == 2);
    must(read_file(root / cosmos::kManifestFile).find("\"complete\": true") != std::string::npos);

    // A different campaign id must not silently clobber the existing artifact root.
    cosmos::CampaignConfig other;
    other.trials = 1;
    other.program = "test_ui_export";
    other.id = "other";
    other.name = "Other";
    auto refused = cosmos::CampaignArtifactWriter::create(artifact_options(root, other));
    must(!refused.has_value());
    must(refused.error().find("already contains") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "[PASS] test_reexport_removes_stale_universe_and_trace_files" << std::endl;
}

void test_manifest_must_match_the_report_it_describes() {
    const std::filesystem::path root = test_root("cosmos-ui-manifest-mismatch");

    cosmos::CampaignConfig declared;
    declared.trials = 6;
    declared.base_seed = 42;
    declared.parallel = 1;
    declared.program = "test_ui_export";
    declared.id = "mismatch";
    declared.name = "Mismatch";
    declared.trace = true;
    declared.trace_capacity = 4096;

    auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, declared));
    must(writer.has_value());

    cosmos::CampaignConfig live = declared;
    live.trials = 4;
    live.on_universe = writer->sink();
    const cosmos::CampaignReport report = cosmos::Campaign::run(live, run_universe);
    const auto finished = writer->finish(report);
    must(!finished.has_value());
    must(finished.error().find("trials") != std::string::npos);

    const std::string manifest = read_file(root / cosmos::kManifestFile);
    must(manifest.find("\"complete\": false") != std::string::npos);
    must(manifest.find("does not match report runs") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "[PASS] test_manifest_must_match_the_report_it_describes" << std::endl;
}

void test_refused_campaign_is_not_a_complete_empty_artifact() {
    const std::filesystem::path root = test_root("cosmos-ui-refused");

    cosmos::CampaignConfig config;
    config.trials = 2;
    config.program = "test_ui_export";
    config.id = "refused";
    config.name = "Refused";
    config.trace = true;
    config.trace_capacity = 0;

    auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, config));
    must(writer.has_value());
    config.on_universe = writer->sink();
    const cosmos::CampaignReport report =
        cosmos::Campaign::run(config, [](cosmos::Simulator&, uint64_t) -> cosmos::ScenarioReport {
            must(false && "a refused campaign must not run a universe");
            return {};
        });
    must(report.refused);

    const auto finished = writer->finish(report);
    must(!finished.has_value());
    must(finished.error().find("refused") != std::string::npos);

    const std::string manifest = read_file(root / cosmos::kManifestFile);
    must(manifest.find("\"complete\": false") != std::string::npos);
    must(manifest.find("\"error\": \"campaign refused") != std::string::npos);
    must(read_file(root / cosmos::kCampaignFile).find("\"refused\": true") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "[PASS] test_refused_campaign_is_not_a_complete_empty_artifact" << std::endl;
}

void test_writer_rejects_unsafe_campaign_ids() {
    const std::filesystem::path root = test_root("cosmos-ui-bad-id");
    cosmos::CampaignConfig config;
    config.trials = 1;
    config.program = "test_ui_export";
    config.id = "../escape";
    config.name = "Bad";
    const auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, config));
    must(!writer.has_value());
    must(writer.error().find("campaign id") != std::string::npos);
    must(!std::filesystem::exists(root));
    std::cout << "[PASS] test_writer_rejects_unsafe_campaign_ids" << std::endl;
}

void test_writer_rejects_out_of_order_universes() {
    const std::filesystem::path root = test_root("cosmos-ui-order");
    cosmos::CampaignConfig config;
    config.trials = 2;
    config.program = "test_ui_export";
    config.id = "order";
    config.name = "Order";
    auto writer = cosmos::CampaignArtifactWriter::create(artifact_options(root, config));
    must(writer.has_value());
    const auto sink = writer->sink();

    cosmos::detail::UniverseOutcome first;
    first.index = 0;
    first.seed = 10;
    first.report.seed = 10;
    first.report.checks.push_back(cosmos::CheckResult{"passes", "", true, cosmos::Time{}});
    sink(first);
    must(writer->error().empty());

    cosmos::detail::UniverseOutcome second;
    second.index = 2;
    second.seed = 12;
    second.report.seed = 12;
    second.report.checks.push_back(cosmos::CheckResult{"passes", "", true, cosmos::Time{}});
    sink(second);
    must(writer->error().find("out of order or with a gap") != std::string::npos);

    cosmos::CampaignReport report;
    report.runs = 2;
    report.base_seed = config.base_seed;
    report.node_count = config.node_count;
    report.parallel = config.parallel;
    report.program = config.program;
    report.id = config.id;
    report.name = config.name;
    report.trace = false;
    report.trace_capacity = config.trace_capacity;
    must(!writer->finish(report).has_value());
    must(read_file(root / cosmos::kManifestFile).find("\"complete\": false") != std::string::npos);
    must(read_file(root / cosmos::kManifestFile).find("out of order") != std::string::npos);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "[PASS] test_writer_rejects_out_of_order_universes" << std::endl;
}

void test_parallel_export_preserves_universe_index_order() {
    const std::filesystem::path root = test_root("cosmos-ui-parallel");
    const cosmos::CampaignReport report = write_artifact_campaign(root, 8, "parallel", 4);
    must(report.crashed_runs == 0);
    const std::vector<std::string> rows = lines_of(read_file(root / cosmos::kUniverseIndexFile));
    must(rows.size() == 8);
    for (size_t index = 0; index < rows.size(); ++index) {
        must(rows[index].find("\"index\": " + std::to_string(index)) != std::string::npos);
        const std::string expected = index % 2 == 0 ? "passed" : "failed";
        must(rows[index].find("\"status\": \"" + expected + "\"") != std::string::npos);
        must(rows[index].find("\"present\": true") != std::string::npos);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::cout << "[PASS] test_parallel_export_preserves_universe_index_order" << std::endl;
}

void test_universe_and_campaign_json_escape_free_form_strings() {
    cosmos::UniverseSummary summary;
    summary.index = 7;
    summary.seed = 123;
    summary.status = "failed";
    summary.ledger_text = "ledger \"quoted\"\n";
    cosmos::CheckResult check;
    check.id = "a\"b\\c\n\t";
    check.id.push_back('\x01');
    check.detail = "line1\r\nline2";
    check.passed = false;
    check.at = cosmos::Time{1};
    summary.checks.push_back(check);
    cosmos::CheckResult unicode;
    unicode.id = "unicode";
    unicode.detail = "caf\xc3\xa9 bad \x80\xff";
    unicode.passed = true;
    summary.checks.push_back(unicode);

    std::ostringstream universe;
    cosmos::print_universe_json(universe, summary);
    const std::string universe_text = universe.str();
    must(universe_text.find("a\\\"b\\\\c\\n\\t\\u0001") != std::string::npos);
    must(universe_text.find("line1\\r\\nline2") != std::string::npos);
    must(universe_text.find("ledger \\\"quoted\\\"\\n") != std::string::npos);
    must(universe_text.find("caf\xc3\xa9") != std::string::npos);
    must(universe_text.find("bad \\ufffd") != std::string::npos);
    must(universe_text.find(std::string("bad ") + "\x80") == std::string::npos);
    must(universe_text.find("\"traced\": false") != std::string::npos);
    must(universe_text.find("\"hash\": null") != std::string::npos);
    must(is_balanced(universe_text));

    cosmos::CampaignReport report;
    cosmos::Finding finding;
    finding.id = "q\"id";
    finding.detail = "d\\e\n\x80";
    report.findings.push_back(finding);
    std::ostringstream campaign;
    cosmos::print_campaign_json(campaign, report);
    must(campaign.str().find("q\\\"id") != std::string::npos);
    must(campaign.str().find("d\\\\e\\n\\ufffd") != std::string::npos);
    must(is_balanced(campaign.str()));

    std::cout << "[PASS] test_universe_and_campaign_json_escape_free_form_strings" << std::endl;
}

struct GroupingNumpunct : std::numpunct<char> {
  protected:
    char do_thousands_sep() const override { return '\''; }
    std::string do_grouping() const override { return "\3"; }
};

struct GlobalLocaleGuard {
    explicit GlobalLocaleGuard(const std::locale& value) : previous(std::locale::global(value)) {}
    ~GlobalLocaleGuard() { std::locale::global(previous); }
    GlobalLocaleGuard(const GlobalLocaleGuard&) = delete;
    GlobalLocaleGuard& operator=(const GlobalLocaleGuard&) = delete;

    std::locale previous;
};

void test_json_printers_ignore_the_global_grouping_locale() {
    const std::locale grouped(std::locale::classic(), new GroupingNumpunct);
    const GlobalLocaleGuard global(grouped);

    cosmos::UniverseSummary summary;
    summary.index = 1000;
    summary.seed = 12345678901234567890ULL;
    summary.status = "passed";
    std::ostringstream row;
    row.imbue(grouped);
    cosmos::print_universe_row_json(row, summary);
    must(row.str().find("\"index\": 1000") != std::string::npos);
    must(row.str().find("1'000") == std::string::npos);

    cosmos::CampaignReport report;
    report.runs = 1000;
    report.base_seed = 12345678901234567890ULL;
    std::ostringstream campaign;
    campaign.imbue(grouped);
    cosmos::print_campaign_json(campaign, report);
    must(campaign.str().find("\"runs\": 1000") != std::string::npos);
    must(campaign.str().find("1'000") == std::string::npos);

    // The repro command owns a fresh stream; it must not inherit the process-wide grouping locale.
    cosmos::MinimizeReport minimization;
    minimization.seed = 12345678901234567890ULL;
    minimization.finding_id = "x";
    std::ostringstream minimize_json;
    cosmos::print_minimize_json(minimize_json, minimization);
    must(minimize_json.str().find("--seed 12345678901234567890") != std::string::npos);
    must(minimize_json.str().find("1'234'567") == std::string::npos);

    std::cout << "[PASS] test_json_printers_ignore_the_global_grouping_locale" << std::endl;
}

void test_atomic_write_does_not_follow_preexisting_temp_symlink() {
    const std::filesystem::path root = test_root("cosmos-ui-symlink");
    write_artifact_campaign(root, 2, "symlink");

    const std::filesystem::path victim =
        std::filesystem::temp_directory_path() /
        ("cosmos-ui-victim-" + std::to_string(static_cast<long>(::getpid())));
    {
        std::ofstream out(victim, std::ios::binary | std::ios::trunc);
        out << "victim";
    }
    std::error_code error;
    std::filesystem::create_symlink(victim, root / "campaign.json.tmp", error);
    must(!error);

    write_artifact_campaign(root, 2, "symlink");
    must(read_file(victim) == "victim");

    std::filesystem::remove_all(root, error);
    std::filesystem::remove(victim, error);
    std::cout << "[PASS] test_atomic_write_does_not_follow_preexisting_temp_symlink" << std::endl;
}

void test_a_zero_capacity_trace_is_refused_before_any_universe() {
    cosmos::CampaignConfig config;
    config.trials = 2;
    config.trace = true;
    config.trace_capacity = 0;
    const cosmos::CampaignReport report =
        cosmos::Campaign::run(config, [](cosmos::Simulator&, uint64_t) -> cosmos::ScenarioReport {
            must(false && "a refused campaign must not run a universe");
            return {};
        });
    must(report.refused);
    must(report.trace);
    must(report.findings.size() == 1);
    must(report.findings[0].detail.find("trace_capacity") != std::string::npos);

    std::cout << "[PASS] test_a_zero_capacity_trace_is_refused_before_any_universe" << std::endl;
}

} // namespace

int main() {
    test_config_fingerprint_is_stable_and_sensitive();
    test_minimize_json_is_versioned_and_explicit();
    test_campaign_json_adds_the_v1_fields();
    test_artifact_writer_round_trips_one_campaign();
    test_artifact_writer_exports_a_minimization_report();
    test_verify_without_trace_bytes_still_exports_a_hash();
    test_events_and_state_artifacts_round_trip();
    test_reexport_removes_stale_universe_and_trace_files();
    test_manifest_must_match_the_report_it_describes();
    test_refused_campaign_is_not_a_complete_empty_artifact();
    test_writer_rejects_unsafe_campaign_ids();
    test_writer_rejects_out_of_order_universes();
    test_parallel_export_preserves_universe_index_order();
    test_universe_and_campaign_json_escape_free_form_strings();
    test_json_printers_ignore_the_global_grouping_locale();
    test_atomic_write_does_not_follow_preexisting_temp_symlink();
    test_a_zero_capacity_trace_is_refused_before_any_universe();
    std::cout << "all ui export tests passed" << std::endl;
    return 0;
}
