#include "cosmos/exploration.hpp"
#include "cosmos/export.hpp"
#include "cosmos/snapshot.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

bool is_balanced(const std::string& text) {
    int depth = 0;
    bool quoted = false;
    bool escaped = false;
    for (char ch : text) {
        if (quoted) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                quoted = false;
            }
            continue;
        }
        if (ch == '"') {
            quoted = true;
        } else if (ch == '{' || ch == '[') {
            ++depth;
        } else if (ch == '}' || ch == ']') {
            --depth;
            if (depth < 0) return false;
        }
    }
    return depth == 0 && !quoted;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    assert(in.good());
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::filesystem::path test_root(const char* name) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (std::string("cosmos-exploration-") + name + "-" +
                                                  std::to_string(static_cast<long>(::getpid())));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    return root;
}

void test_exploration_json_carries_coverage_snapshots_and_branches() {
    cosmos::CampaignReport campaign;
    campaign.id = "explore-json";
    campaign.name = "Explore JSON";
    campaign.guidance = cosmos::SeedGuidance::Coverage;
    campaign.coverage.enabled = true;
    campaign.coverage.method = "trace-pc";
    campaign.coverage.runs_with_coverage = 3;
    campaign.coverage.total_edges = 30;
    campaign.coverage.unique_edges = 12;
    campaign.coverage.novel_runs = 2;
    campaign.coverage.corpus_size = 1;
    campaign.corpus.push_back(cosmos::CorpusEntry{
        .seed = 18446744073709551615ULL,
        .parent_seed = 0,
        .index = 0,
        .edges = 12,
        .new_edges = 12,
        .hash = 0x1234,
        .generation = 0,
        .finding = false,
    });

    cosmos::ExplorationReport exploration;
    exploration.campaign = campaign;
    cosmos::SnapshotInfo snapshot;
    snapshot.id = 7;
    snapshot.seed = 42;
    snapshot.at = cosmos::Time{1'000'000};
    snapshot.ready_choices = 2;
    snapshot.parent_branch_index = 3;
    snapshot.method = "fork-cow";
    exploration.snapshots.push_back(snapshot);

    cosmos::ExplorationBranch branch;
    branch.id = "S7-B1";
    branch.snapshot = snapshot;
    branch.context.branch_index = 1;
    branch.context.has_forced_choice = true;
    branch.context.forced_choice = 1;
    branch.report.seed = 42;
    branch.report.no_check_failed = false;
    branch.report.checks.push_back(cosmos::CheckResult{"order", "BA", false, cosmos::Time::zero()});
    branch.coverage_present = true;
    branch.coverage_edges = 9;
    branch.coverage_hash = 0xabcd;
    exploration.branches.push_back(branch);

    std::ostringstream json;
    cosmos::print_exploration_json(json, exploration);
    const std::string text = json.str();
    assert(is_balanced(text));
    assert(text.find("\"schema_version\": 1") != std::string::npos);
    assert(text.find("\"campaign_id\": \"explore-json\"") != std::string::npos);
    assert(text.find("\"guidance\": \"coverage\"") != std::string::npos);
    assert(text.find("\"seed\": \"18446744073709551615\"") != std::string::npos);
    assert(text.find("\"id\": \"S7-B1\"") != std::string::npos);
    assert(text.find("\"parent_branch_index\": 3") != std::string::npos);
    assert(text.find("\"status\": \"failed\"") != std::string::npos);
    assert(text.find("\"coverage\": {\"present\": true, \"method\": \"") != std::string::npos);
    assert(text.find("\"edges\": 9") != std::string::npos);
    std::cout << "[PASS] test_exploration_json_carries_coverage_snapshots_and_branches"
              << std::endl;
}

void test_writer_round_trips_exploration_and_clears_it_on_re_export() {
    const std::filesystem::path root = test_root("writer");
    cosmos::CampaignConfig config;
    config.trials = 0;
    config.base_seed = 1;
    config.parallel = 1;
    config.id = "explore-writer";
    config.name = "Explore writer";
    config.program = "test_exploration";

    cosmos::ArtifactOptions options;
    options.root = root;
    options.campaign = config;
    options.build.id = "test-build";
    options.build.compiler = "test";
    options.build.host = "test";

    auto writer = cosmos::CampaignArtifactWriter::create(options);
    assert(writer.has_value());
    const std::string document = "{\"schema_version\": 1, \"campaign_id\": \"explore-writer\"}\n";
    assert(writer->write_exploration(document).has_value());
    cosmos::CampaignReport report;
    report.id = config.id;
    report.name = config.name;
    report.program = config.program;
    report.parallel = config.parallel;
    report.base_seed = config.base_seed;
    report.node_count = config.node_count;
    report.trace_capacity = config.trace_capacity;
    assert(writer->finish(report).has_value());
    assert(read_file(root / cosmos::kExplorationFile) == document);
    assert(read_file(root / cosmos::kManifestFile)
               .find("\"exploration_file\": \"exploration.json\"") != std::string::npos);

    // A re-export with the same campaign id must not leave the previous exploration document next
    // to a fresh manifest that does not describe it.
    auto second = cosmos::CampaignArtifactWriter::create(options);
    assert(second.has_value());
    assert(!std::filesystem::exists(root / cosmos::kExplorationFile));
    cosmos::CampaignReport empty;
    empty.id = config.id;
    empty.name = config.name;
    empty.program = config.program;
    empty.parallel = config.parallel;
    empty.base_seed = config.base_seed;
    empty.node_count = config.node_count;
    empty.trace_capacity = config.trace_capacity;
    assert(second->finish(empty).has_value());
    assert(!std::filesystem::exists(root / cosmos::kExplorationFile));
    assert(read_file(root / cosmos::kManifestFile).find("\"exploration_file\": null") !=
           std::string::npos);

    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::cout << "[PASS] test_writer_round_trips_exploration_and_clears_it_on_re_export"
              << std::endl;
}

} // namespace

int main() {
    test_exploration_json_carries_coverage_snapshots_and_branches();
    test_writer_round_trips_exploration_and_clears_it_on_re_export();
    std::cout << "All exploration tests passed successfully!" << std::endl;
    return 0;
}
