#pragma once

// Level-2/3 exploration artifacts. Coverage and branch data are observations, not replay inputs:
// the canonical FaultTrace and its hash remain the only replay identity. The artifact is additive
// to the schema-v1 campaign layout and lives at `exploration.json`.

#include "cosmos/campaign.hpp"
#include "cosmos/export.hpp"
#include "cosmos/snapshot.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace cosmos {

struct ExplorationBranch {
    std::string id{};
    SnapshotInfo snapshot{};
    BranchContext context{};
    bool died = false;
    int signal = 0;
    bool forced_choice_applied = false;
    std::string threw{};
    std::string error{};
    ScenarioReport report{};
    Time ended_at{};
    uint64_t wall_ns = 0;
    bool traced = false;
    uint64_t trace_hash = 0;
    uint64_t trace_capacity = 0;
    uint64_t trace_dropped = 0;
    bool coverage_present = false;
    std::string coverage_method{};
    uint64_t coverage_edges = 0;
    uint64_t coverage_hash = 0;
};

struct ExplorationReport {
    CampaignReport campaign{};
    std::vector<SnapshotInfo> snapshots{};
    std::vector<ExplorationBranch> branches{};
};

inline const char* branch_status(const ExplorationBranch& branch) {
    if (branch.died) return "crashed";
    if (!branch.error.empty()) return "error";
    if (!branch.threw.empty()) return "threw";
    if (!branch.report.no_check_failed) return "failed";
    if (branch.report.vacuous()) return "vacuous";
    return "passed";
}

inline std::string branch_id(const ExplorationBranch& branch) {
    if (!branch.id.empty()) return branch.id;
    return "S" + std::to_string(branch.snapshot.id) + "-B" +
           std::to_string(branch.context.branch_index);
}

inline void print_snapshot_json(std::ostream& out, const SnapshotInfo& info) {
    out << "{\"id\": " << info.id << ", \"parent_id\": " << info.parent_id << ", \"seed\": ";
    json::string(out, json::decimal(info.seed));
    out << ", \"parent_branch_index\": ";
    if (info.parent_branch_index == kNoBranch) {
        out << "null";
    } else {
        out << info.parent_branch_index;
    }
    out << ", \"at_ns\": " << info.at.ns << ", \"tasks\": " << info.tasks
        << ", \"ready\": " << info.ready << ", \"blocked\": " << info.blocked
        << ", \"ready_choices\": " << info.ready_choices
        << ", \"heap_allocations\": " << info.heap_allocations
        << ", \"heap_bytes\": " << info.heap_bytes << ", \"endpoints\": " << info.endpoints
        << ", \"pending_packets\": " << info.pending_packets
        << ", \"storage_files\": " << info.storage_files
        << ", \"trace_decisions\": " << info.trace_decisions
        << ", \"trace_entries\": " << info.trace_entries << ", \"coverage_present\": ";
    json::boolean(out, info.coverage_present);
    out << ", \"coverage_edges\": " << info.coverage_edges << ", \"coverage_hash\": ";
    if (info.coverage_present) {
        json::string(out, json::hex(info.coverage_hash));
    } else {
        out << "null";
    }
    out << ", \"faults_quiet\": ";
    json::boolean(out, info.faults_quiet);
    out << ", \"method\": ";
    json::string(out, info.method);
    out << '}';
}

inline void print_coverage_corpus_json(std::ostream& out, const CampaignReport& campaign) {
    out << "{\"enabled\": ";
    json::boolean(out, campaign.coverage.enabled);
    out << ", \"guidance\": ";
    json::string(out, name_of(campaign.guidance));
    out << ", \"method\": ";
    json::string(out, campaign.coverage.method);
    out << ", \"runs_with_coverage\": " << campaign.coverage.runs_with_coverage
        << ", \"total_edges\": " << campaign.coverage.total_edges
        << ", \"unique_edges\": " << campaign.coverage.unique_edges
        << ", \"novel_runs\": " << campaign.coverage.novel_runs
        << ", \"corpus_size\": " << campaign.coverage.corpus_size
        << ", \"corpus_dropped\": " << campaign.coverage.corpus_dropped << ", \"corpus\": [";
    for (size_t index = 0; index < campaign.corpus.size(); ++index) {
        const CorpusEntry& entry = campaign.corpus[index];
        if (index != 0) out << ", ";
        out << "{\"seed\": ";
        json::string(out, json::decimal(entry.seed));
        out << ", \"parent_seed\": ";
        if (entry.parent_seed == 0) {
            out << "null";
        } else {
            json::string(out, json::decimal(entry.parent_seed));
        }
        out << ", \"index\": " << entry.index << ", \"edges\": " << entry.edges
            << ", \"new_edges\": " << entry.new_edges << ", \"hash\": ";
        json::string(out, json::hex(entry.hash));
        out << ", \"generation\": " << entry.generation << ", \"finding\": ";
        json::boolean(out, entry.finding);
        out << ", \"vacuous\": ";
        json::boolean(out, entry.vacuous);
        out << '}';
    }
    out << "]}";
}

inline void print_exploration_json(std::ostream& out, const ExplorationReport& report) {
    const json::classic_locale_guard locale(out);
    out << "{\n";
    out << "  \"schema_version\": " << kArtifactSchemaVersion << ",\n";
    out << "  \"campaign_id\": ";
    json::string(out, report.campaign.id);
    out << ",\n  \"guidance\": ";
    json::string(out, name_of(report.campaign.guidance));
    out << ",\n  \"coverage\": ";
    print_coverage_corpus_json(out, report.campaign);
    out << ",\n  \"snapshots\": [";
    for (size_t index = 0; index < report.snapshots.size(); ++index) {
        if (index != 0) out << ", ";
        print_snapshot_json(out, report.snapshots[index]);
    }
    out << "],\n  \"branches\": [";
    for (size_t index = 0; index < report.branches.size(); ++index) {
        const ExplorationBranch& branch = report.branches[index];
        if (index != 0) out << ", ";
        out << "{\"id\": ";
        json::string(out, branch_id(branch));
        out << ", \"snapshot_id\": " << branch.snapshot.id
            << ", \"parent_snapshot_id\": " << branch.snapshot.parent_id
            << ", \"branch_index\": " << branch.context.branch_index
            << ", \"forced_choice_applied\": ";
        json::boolean(out, branch.forced_choice_applied);
        out << ", \"faults_quiet\": ";
        json::boolean(out, branch.snapshot.faults_quiet);
        out << ", \"has_forced_choice\": ";
        json::boolean(out, branch.context.has_forced_choice);
        out << ", \"forced_choice\": ";
        if (branch.context.has_forced_choice) {
            out << branch.context.forced_choice;
        } else {
            out << "null";
        }
        out << ", \"has_schedule_seed\": ";
        json::boolean(out, branch.context.has_schedule_seed);
        out << ", \"schedule_seed\": ";
        json::string(out, json::decimal(branch.context.schedule_seed));
        out << ", \"seed\": ";
        json::string(out, json::decimal(branch.report.seed));
        out << ", \"at_ns\": " << branch.snapshot.at.ns << ", \"status\": ";
        json::string(out, branch_status(branch));
        const bool conclusive = !branch.died && branch.error.empty() && branch.threw.empty();
        out << ", \"no_check_failed\": ";
        if (conclusive) {
            json::boolean(out, branch.report.no_check_failed);
        } else {
            out << "null";
        }
        out << ", \"vacuous\": ";
        if (conclusive) {
            json::boolean(out, branch.report.vacuous());
        } else {
            out << "null";
        }
        out << ", \"virtual_end_ns\": ";
        if (conclusive) {
            out << branch.ended_at.ns;
        } else {
            out << "null";
        }
        out << ", \"wall_ns\": ";
        if (branch.wall_ns > 0) {
            out << branch.wall_ns;
        } else {
            out << "null";
        }
        out << ", \"signal\": " << branch.signal << ", \"threw\": ";
        json::string(out, branch.threw);
        out << ", \"error\": ";
        json::string(out, branch.error);
        out << ", \"checks\": ";
        export_detail::print_checks_json(out, branch.report.checks);
        out << ", \"coverage\": {\"present\": ";
        json::boolean(out, branch.coverage_present);
        out << ", \"method\": ";
        json::string(out, branch.coverage_method);
        out << ", \"edges\": " << branch.coverage_edges << ", \"hash\": ";
        if (branch.coverage_present) {
            json::string(out, json::hex(branch.coverage_hash));
        } else {
            out << "null";
        }
        out << "}, \"trace\": {\"traced\": ";
        json::boolean(out, branch.traced);
        out << ", \"hash\": ";
        if (branch.traced) {
            json::string(out, json::hex(branch.trace_hash));
        } else {
            out << "null";
        }
        out << ", \"capacity\": " << branch.trace_capacity
            << ", \"dropped\": " << branch.trace_dropped << "}}";
    }
    out << "]\n}\n";
}

} // namespace cosmos
