#pragma once

// JSON export for a MinimizeReport (docs/ui-design.md U2.4). The UI renders the report the
// minimizer produced; it does not re-run minimization, and it must not present a 1-minimal set as
// globally minimal (D8). Like every artifact document this is additive and schema-versioned.
//
// Layout: <campaign-root>/minimizations/<seed>.json. The seed, not the universe index, is the
// report identity, and it is a decimal string so JavaScript cannot round it.

#include "cosmos/json.hpp"
#include "cosmos/minimize.hpp"

#include <cstddef>
#include <cstdint>
#include <locale>
#include <ostream>
#include <sstream>
#include <string>

namespace cosmos {

namespace minimize_detail {

inline void print_entry_json(std::ostream& out, const TraceEntry& entry) {
    out << "{\"seq\": " << entry.seq << ", \"eligible_index\": " << entry.eligible_index
        << ", \"at_ns\": " << entry.at.ns << ", \"site\": ";
    json::string(out, name_of(entry.site));
    out << ", \"fault_class\": ";
    json::string(out, name_of(class_of(entry.site)));
    out << ", \"outcome\": ";
    json::string(out, name_of(entry.outcome));
    out << ", \"amount_ns\": " << entry.amount.ns << ", \"drew\": ";
    json::boolean(out, entry.drew);
    out << ", \"status\": ";
    json::string(out, name_of(entry.status));
    out << '}';
}

inline void print_entries_json(std::ostream& out, const std::vector<TraceEntry>& entries) {
    out << '[';
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) out << ", ";
        print_entry_json(out, entries[index]);
    }
    out << ']';
}

// The exact command the text report prints: program, seed and the kept decision seqs. The UI
// copies this rather than synthesizing flags the artifact never declared.
inline std::string repro_command(const MinimizeReport& report) {
    std::ostringstream out;
    // The caller's stream is guarded by print_minimize_json(), but this helper owns a new stream
    // and would otherwise inherit a process-wide grouping locale. A grouped seed is not the
    // exact command the report claims to carry.
    out.imbue(std::locale::classic());
    if (!report.program.empty()) out << report.program << ' ';
    out << "--seed " << report.seed << " --only-faults=";
    for (size_t index = 0; index < report.kept.size(); ++index) {
        if (index != 0) out << ',';
        out << report.kept[index].seq;
    }
    return out.str();
}

} // namespace minimize_detail

inline void print_minimize_json(std::ostream& out, const MinimizeReport& report) {
    using namespace minimize_detail;
    const json::classic_locale_guard locale(out);
    out << "{\n";
    out << "  \"schema_version\": " << kArtifactSchemaVersion << ",\n";
    out << "  \"seed\": ";
    json::string(out, json::decimal(report.seed));
    out << ",\n  \"seed_hex\": ";
    json::string(out, json::hex(report.seed));
    out << ",\n  \"finding_id\": ";
    json::string(out, report.finding_id);
    out << ",\n  \"program\": ";
    json::string(out, report.program);
    out << ",\n  \"refused\": ";
    json::boolean(out, report.refused);
    out << ",\n  \"reason\": ";
    json::string(out, report.reason);
    out << ",\n  \"candidates\": ";
    print_entries_json(out, report.candidates);
    out << ",\n  \"kept\": ";
    print_entries_json(out, report.kept);
    out << ",\n  \"only_faults\": [";
    for (size_t index = 0; index < report.kept.size(); ++index) {
        if (index != 0) out << ", ";
        out << report.kept[index].seq;
    }
    out << "],\n  \"repro\": ";
    // A refused report has no set to replay. Empty kept is still a set (the finding reproduces
    // with every fault withheld), so it keeps the exact, empty --only-faults command.
    if (report.refused) {
        out << "null";
    } else {
        json::string(out, repro_command(report));
    }
    out << ",\n  \"passes\": " << report.passes;
    out << ",\n  \"replays\": " << report.replays;
    out << ",\n  \"crashed_trials\": " << report.crashed_trials;
    out << "\n}\n";
}

} // namespace cosmos
