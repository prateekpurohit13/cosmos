#pragma once

// Artifact export and the v1 UI contract (docs/ui-design.md Appendix A).
//
// The simulation binary is not the UI's server (D4). A campaign forks workers, the parent folds
// their outcomes into a CampaignReport, and a parent-side sink may additionally write the
// per-universe rows, configs, ledgers and canonical traces that a local UI loads. Nothing here
// changes the simulation semantics or the canonical FaultTrace encoding (D5).
//
// Layout (schema version 1):
//
//   <root>/manifest.json          campaign identity, build, config, completeness
//   <root>/campaign.json          aggregate CampaignReport JSON
//   <root>/universes.ndjson       one compact row per universe, ascending index
//   <root>/universes/<index>.json full UniverseSummary
//   <root>/traces/<index>.bin     canonical FaultTrace bytes, when captured
//   <root>/events/<index>.bin     versioned inspection stream bytes, when captured
//
// Every JSON object carries `schema_version`. Seeds and hashes that do not fit a JavaScript
// number are decimal/hex strings; a truncated seed is not a repro identity.

#include "cosmos/campaign.hpp"
#include "cosmos/json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cosmos {

inline constexpr const char* kManifestFile = "manifest.json";
inline constexpr const char* kCampaignFile = "campaign.json";
inline constexpr const char* kUniverseIndexFile = "universes.ndjson";
inline constexpr const char* kUniverseDir = "universes";
inline constexpr const char* kTraceDir = "traces";
inline constexpr const char* kEventDir = "events";
// Optional per-seed MinimizeReport documents; a campaign may have none.
inline constexpr const char* kMinimizationDir = "minimizations";

struct BuildInfo {
    // Content address of the build. The caller supplies it; an empty id falls back to the binary
    // hash, then the commit, so every manifest still names something.
    std::string id;
    std::string commit;
    bool dirty = false;
    std::string binary_hash;
    std::string mode = "SIM";
    std::string compiler;
    std::string host;
};

inline std::string build_identity(const BuildInfo& build) {
    if (!build.id.empty()) return build.id;
    if (!build.binary_hash.empty()) return build.binary_hash;
    if (!build.commit.empty()) return build.commit;
    return "unknown";
}

struct UniverseSummary {
    uint64_t index = 0;
    uint64_t seed = 0;
    // One of: passed, failed, vacuous, crashed, threw, verify_failed. Vacuity is deliberately its
    // own status rather than a failure (D7).
    std::string status;
    bool died = false;
    int signal = 0;
    std::string threw;
    bool no_check_failed = true;
    bool vacuous = false;
    bool verify_failed = false;
    std::string verify_mismatch;
    Time virtual_end{};
    uint64_t wall_ns = 0;
    std::vector<CheckResult> checks{};
    std::vector<CoverageNote> coverage{};
    size_t active_allocations = 0;
    uint64_t out_of_scope_assertions = 0;
    bool has_config = false;
    std::string config_hash;
    std::string config_json; // a complete JSON object, produced by print_config_json()
    std::vector<LedgerEntry> ledger{};
    uint64_t ledger_dropped = 0;
    std::string ledger_text; // the rendered dump, kept for the v1 detail view
    bool traced = false;
    bool trace_present = false;
    uint64_t trace_hash = 0;
    uint64_t trace_capacity = 0;
    uint64_t trace_dropped = 0;
    std::string trace_path; // relative to the campaign root, empty when absent
    bool events_attached = false;
    bool events_present = false;
    uint64_t event_capacity = 0;
    uint64_t event_dropped = 0;
    uint64_t event_rejected = 0;
    std::string events_path;  // relative to the campaign root, empty when absent
    std::string events_error; // nonempty when the capture was refused by the encoder
    std::string state_json;   // complete state object, empty when not captured
    SiteCounterMap eligible_calls{};
    SiteCounterMap injections{};
    SiteActivationSet activated_sites{};
};

inline std::string trace_relative_path(uint64_t index) {
    return std::string(kTraceDir) + "/" + std::to_string(index) + ".bin";
}

inline std::string event_relative_path(uint64_t index) {
    return std::string(kEventDir) + "/" + std::to_string(index) + ".bin";
}

inline std::string minimization_relative_path(uint64_t seed) {
    return std::string(kMinimizationDir) + "/" + std::to_string(seed) + ".json";
}

inline UniverseSummary summarize(const detail::UniverseOutcome& outcome) {
    UniverseSummary summary;
    summary.index = outcome.index;
    summary.seed = outcome.seed;
    summary.died = outcome.died;
    summary.signal = outcome.signal;
    summary.threw = outcome.threw;
    summary.verify_failed = !outcome.verify_mismatch.empty();
    summary.verify_mismatch = outcome.verify_mismatch;
    summary.virtual_end = outcome.ended_at;
    summary.wall_ns = outcome.wall_ns;
    summary.has_config = outcome.has_config;
    summary.config_hash = outcome.config_hash;
    summary.config_json = outcome.config_json;
    summary.ledger = outcome.ledger_entries;
    summary.ledger_dropped = outcome.ledger_dropped;
    summary.traced = outcome.traced;
    summary.trace_present = !outcome.trace_bytes.empty();
    summary.trace_hash = outcome.trace_hash;
    summary.trace_capacity = outcome.trace_capacity;
    summary.trace_dropped = outcome.trace_dropped;
    if (summary.trace_present) summary.trace_path = trace_relative_path(outcome.index);
    summary.events_attached = outcome.events_attached;
    summary.events_present = outcome.events_encoded;
    summary.event_capacity = outcome.event_capacity;
    summary.event_dropped = outcome.event_dropped;
    summary.event_rejected = outcome.event_rejected;
    if (summary.events_present) summary.events_path = event_relative_path(outcome.index);
    summary.events_error = outcome.events_error;
    summary.state_json = outcome.state_json;

    const ScenarioReport& report = outcome.report;
    summary.no_check_failed = report.no_check_failed;
    summary.checks = report.checks;
    summary.coverage = report.coverage;
    summary.active_allocations = report.active_allocations;
    summary.out_of_scope_assertions = outcome.out_of_scope;
    summary.eligible_calls = report.eligible_calls;
    summary.injections = report.injections;
    summary.activated_sites = report.activated_sites;
    summary.ledger_text = report.ledger_dump;
    summary.vacuous = !outcome.died && outcome.threw.empty() && report.vacuous();

    if (outcome.died) {
        summary.status = "crashed";
    } else if (!outcome.threw.empty()) {
        summary.status = "threw";
    } else if (summary.verify_failed) {
        summary.status = "verify_failed";
    } else if (!report.no_check_failed) {
        summary.status = "failed";
    } else if (summary.vacuous) {
        summary.status = "vacuous";
    } else {
        summary.status = "passed";
    }
    return summary;
}

namespace export_detail {

inline void print_checks_json(std::ostream& out, const std::vector<CheckResult>& checks) {
    out << '[';
    for (size_t index = 0; index < checks.size(); ++index) {
        if (index != 0) out << ", ";
        const CheckResult& check = checks[index];
        out << "{\"id\": ";
        json::string(out, check.id);
        out << ", \"detail\": ";
        json::string(out, check.detail);
        out << ", \"passed\": ";
        json::boolean(out, check.passed);
        out << ", \"at_ns\": " << check.at.ns << '}';
    }
    out << ']';
}

inline void print_coverage_json(std::ostream& out, const std::vector<CoverageNote>& notes) {
    out << '[';
    for (size_t index = 0; index < notes.size(); ++index) {
        if (index != 0) out << ", ";
        out << "{\"id\": ";
        json::string(out, notes[index].id);
        out << ", \"hit\": ";
        json::boolean(out, notes[index].hit);
        out << '}';
    }
    out << ']';
}

inline void print_ledger_json(std::ostream& out, const std::vector<LedgerEntry>& entries) {
    out << '[';
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) out << ", ";
        const LedgerEntry& entry = entries[index];
        out << "{\"fire_index\": " << entry.fire_index << ", \"at_ns\": " << entry.at.ns
            << ", \"site\": ";
        json::string(out, name_of(entry.site));
        out << ", \"eligible_index\": " << entry.eligible_index << ", \"fault_class\": ";
        json::string(out, name_of(entry.fault_class));
        out << ", \"outcome\": ";
        json::string(out, name_of(entry.outcome));
        out << ", \"drew\": ";
        json::boolean(out, entry.drew);
        out << ", \"status\": ";
        json::string(out, name_of(entry.status));
        out << ", \"reason\": ";
        json::string(out, name_of(entry.reason));
        out << ", \"persistent\": ";
        json::boolean(out, entry.persistent);
        out << '}';
    }
    out << ']';
}

// Universe sites are a bool, not a run count: this is one execution, not an aggregate.
inline void print_universe_sites_json(std::ostream& out, const UniverseSummary& summary) {
    out << '{';
    for (size_t slot = 0; slot < kSiteCount; ++slot) {
        if (slot != 0) out << ", ";
        json::string(out, name_of(site_at_slot(slot)));
        out << ": {\"activated\": ";
        json::boolean(out, summary.activated_sites.test(slot));
        out << ", \"eligible\": " << summary.eligible_calls[slot]
            << ", \"fired\": " << summary.injections[slot] << '}';
    }
    out << '}';
}

inline uint64_t total_injections(const UniverseSummary& summary) {
    uint64_t total = 0;
    for (uint64_t count : summary.injections)
        total += count;
    return total;
}

inline uint64_t total_eligible(const UniverseSummary& summary) {
    uint64_t total = 0;
    for (uint64_t count : summary.eligible_calls)
        total += count;
    return total;
}

inline size_t failed_checks(const UniverseSummary& summary) {
    size_t failed = 0;
    for (const CheckResult& check : summary.checks) {
        if (!check.passed) ++failed;
    }
    return failed;
}

inline void print_events_json(std::ostream& out, const UniverseSummary& summary) {
    out << "{\"present\": ";
    json::boolean(out, summary.events_present);
    out << ", \"attached\": ";
    json::boolean(out, summary.events_attached);
    out << ", \"path\": ";
    if (summary.events_path.empty()) {
        out << "null";
    } else {
        json::string(out, summary.events_path);
    }
    out << ", \"version\": " << static_cast<unsigned>(kEventStreamVersion)
        << ", \"capacity\": " << summary.event_capacity
        << ", \"dropped\": " << summary.event_dropped
        << ", \"rejected\": " << summary.event_rejected << ", \"error\": ";
    json::string(out, summary.events_error);
    out << '}';
}

inline void print_trace_json(std::ostream& out, const UniverseSummary& summary) {
    out << "{\"present\": ";
    json::boolean(out, summary.trace_present);
    out << ", \"traced\": ";
    json::boolean(out, summary.traced);
    out << ", \"path\": ";
    if (summary.trace_path.empty()) {
        out << "null";
    } else {
        json::string(out, summary.trace_path);
    }
    out << ", \"hash\": ";
    // A non-traced run has no hash identity; 0x0 is a well-formed hash to a naive consumer.
    if (summary.traced) {
        json::string(out, json::hex(summary.trace_hash));
    } else {
        out << "null";
    }
    out << ", \"capacity\": " << summary.trace_capacity
        << ", \"dropped\": " << summary.trace_dropped << '}';
}

} // namespace export_detail

// Full per-universe document. The config is embedded verbatim rather than re-rendered, so the
// artifact shows the injector's realized config even if this header is newer than the engine.
inline void print_universe_json(std::ostream& out, const UniverseSummary& summary) {
    using namespace export_detail;
    const json::classic_locale_guard locale(out);
    out << "{\n";
    out << "  \"schema_version\": " << kArtifactSchemaVersion << ",\n";
    out << "  \"index\": " << summary.index << ",\n";
    out << "  \"seed\": ";
    json::string(out, json::decimal(summary.seed));
    out << ",\n  \"seed_hex\": ";
    json::string(out, json::hex(summary.seed));
    out << ",\n  \"status\": ";
    json::string(out, summary.status);
    out << ",\n  \"died\": ";
    json::boolean(out, summary.died);
    out << ",\n  \"signal\": " << summary.signal << ",\n  \"threw\": ";
    json::string(out, summary.threw);
    out << ",\n  \"no_check_failed\": ";
    json::boolean(out, summary.no_check_failed);
    out << ",\n  \"vacuous\": ";
    json::boolean(out, summary.vacuous);
    out << ",\n  \"verify_failed\": ";
    json::boolean(out, summary.verify_failed);
    out << ",\n  \"verify_mismatch\": ";
    json::string(out, summary.verify_mismatch);
    out << ",\n  \"virtual_end_ns\": " << summary.virtual_end.ns;
    out << ",\n  \"wall_ns\": " << summary.wall_ns;
    out << ",\n  \"checks\": ";
    print_checks_json(out, summary.checks);
    out << ",\n  \"coverage\": ";
    print_coverage_json(out, summary.coverage);
    out << ",\n  \"active_allocations\": " << summary.active_allocations;
    out << ",\n  \"out_of_scope_assertions\": " << summary.out_of_scope_assertions;
    out << ",\n  \"config_hash\": ";
    json::string(out, summary.config_hash);
    out << ",\n  \"config\": ";
    if (summary.has_config) {
        out << summary.config_json;
    } else {
        out << "null";
    }
    out << ",\n  \"trace\": ";
    print_trace_json(out, summary);
    out << ",\n  \"events\": ";
    print_events_json(out, summary);
    out << ",\n  \"state\": ";
    if (summary.state_json.empty()) {
        out << "null";
    } else {
        out << summary.state_json;
    }
    out << ",\n  \"ledger\": ";
    print_ledger_json(out, summary.ledger);
    out << ",\n  \"ledger_dropped\": " << summary.ledger_dropped;
    out << ",\n  \"ledger_text\": ";
    json::string(out, summary.ledger_text);
    out << ",\n  \"sites\": ";
    print_universe_sites_json(out, summary);
    out << "\n}\n";
}

// Compact row for universes.ndjson. Deliberately excludes checks, ledger entries and the config:
// a 10,000-universe campaign must not require a 10,000-file read to render its table.
inline void print_universe_row_json(std::ostream& out, const UniverseSummary& summary) {
    using namespace export_detail;
    const json::classic_locale_guard locale(out);
    out << "{\"schema_version\": " << kArtifactSchemaVersion << ", \"index\": " << summary.index
        << ", \"seed\": ";
    json::string(out, json::decimal(summary.seed));
    out << ", \"status\": ";
    json::string(out, summary.status);
    out << ", \"no_check_failed\": ";
    json::boolean(out, summary.no_check_failed);
    out << ", \"vacuous\": ";
    json::boolean(out, summary.vacuous);
    out << ", \"verify_failed\": ";
    json::boolean(out, summary.verify_failed);
    out << ", \"checks\": {\"total\": " << summary.checks.size()
        << ", \"failed\": " << failed_checks(summary)
        << "}, \"eligible\": " << total_eligible(summary)
        << ", \"fired\": " << total_injections(summary)
        << ", \"active_allocations\": " << summary.active_allocations << ", \"config_hash\": ";
    json::string(out, summary.config_hash);
    out << ", \"virtual_end_ns\": " << summary.virtual_end.ns
        << ", \"wall_ns\": " << summary.wall_ns << ", \"trace\": ";
    print_trace_json(out, summary);
    out << ", \"events\": ";
    print_events_json(out, summary);
    out << ", \"state_present\": ";
    json::boolean(out, !summary.state_json.empty());
    out << '}';
}

inline void print_build_json(std::ostream& out, const BuildInfo& build) {
    out << "{\"id\": ";
    json::string(out, build_identity(build));
    out << ", \"commit\": ";
    json::string(out, build.commit);
    out << ", \"dirty\": ";
    json::boolean(out, build.dirty);
    out << ", \"binary_hash\": ";
    json::string(out, build.binary_hash);
    out << ", \"mode\": ";
    json::string(out, build.mode);
    out << ", \"compiler\": ";
    json::string(out, build.compiler);
    out << ", \"host\": ";
    json::string(out, build.host);
    out << '}';
}

inline void print_manifest_json(std::ostream& out, const CampaignConfig& config,
                                const BuildInfo& build, int64_t created_unix_ns,
                                int64_t completed_unix_ns, bool complete, std::string_view error) {
    const json::classic_locale_guard locale(out);
    out << "{\n";
    out << "  \"schema_version\": " << kArtifactSchemaVersion << ",\n";
    out << "  \"campaign_id\": ";
    json::string(out, config.id);
    out << ",\n  \"name\": ";
    json::string(out, config.name);
    out << ",\n  \"program\": ";
    json::string(out, config.program);
    out << ",\n  \"complete\": ";
    json::boolean(out, complete);
    out << ",\n  \"created_unix_ns\": " << created_unix_ns;
    out << ",\n  \"completed_unix_ns\": ";
    json::optional_number(out, completed_unix_ns > 0, static_cast<uint64_t>(completed_unix_ns));
    out << ",\n  \"error\": ";
    json::string(out, error);
    out << ",\n  \"build\": ";
    print_build_json(out, build);
    out << ",\n  \"config\": {\"trials\": " << config.trials << ", \"base_seed\": ";
    json::string(out, json::decimal(config.base_seed));
    out << ", \"parent_seed\": " << config.base_seed << ", \"parallel\": " << config.parallel
        << ", \"node_count\": " << config.node_count << ", \"verify\": ";
    json::boolean(out, config.verify);
    out << ", \"trace\": ";
    json::boolean(out, config.verify || config.trace);
    out << ", \"trace_capacity\": " << config.trace_capacity << ", \"events\": ";
    json::boolean(out, config.events);
    out << ", \"event_capacity\": " << (config.events ? config.event_capacity : 0)
        << ", \"state\": ";
    json::boolean(out, config.state);
    out << ", \"program\": ";
    json::string(out, config.program);
    out << "},\n";
    out << "  \"paths\": {\"campaign\": ";
    json::string(out, kCampaignFile);
    out << ", \"universes\": ";
    json::string(out, kUniverseIndexFile);
    out << ", \"universe_dir\": ";
    json::string(out, kUniverseDir);
    out << ", \"trace_dir\": ";
    json::string(out, kTraceDir);
    out << ", \"event_dir\": ";
    json::string(out, kEventDir);
    out << ", \"minimization_dir\": ";
    json::string(out, kMinimizationDir);
    out << "}\n}\n";
}

namespace export_detail {

inline int64_t unix_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Write through a uniquely-named sibling temp file and rename. A fixed `<target>.tmp` name would
// let a pre-existing symlink redirect the write outside the artifact root; mkstemp creates a new
// 0600 regular file with O_EXCL and cannot follow one. fsync covers a process crash (the directory
// entry itself is not synced, which is enough for this local artifact store).
inline std::expected<void, std::string> write_atomic(const std::filesystem::path& path,
                                                     std::string_view bytes) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return std::unexpected("create_directories(" + path.parent_path().string() +
                                   "): " + error.message());
        }
    }
    std::string pattern = path.string() + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    const int fd = ::mkstemp(name.data());
    if (fd < 0) {
        return std::unexpected("mkstemp(" + path.string() + "): " + std::strerror(errno));
    }
    const std::string temporary(name.data());
    auto remove_temporary = [&]() {
        ::close(fd);
        ::unlink(temporary.c_str());
    };
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t written = ::write(fd, bytes.data() + done, bytes.size() - done);
        if (written < 0) {
            if (errno == EINTR) continue;
            remove_temporary();
            return std::unexpected("write(" + temporary + "): " + std::strerror(errno));
        }
        if (written == 0) {
            remove_temporary();
            return std::unexpected("write(" + temporary + "): short write");
        }
        done += static_cast<size_t>(written);
    }
    if (::fsync(fd) != 0) {
        const std::string reason = std::strerror(errno);
        remove_temporary();
        return std::unexpected("fsync(" + temporary + "): " + reason);
    }
    if (::close(fd) != 0) {
        const std::string reason = std::strerror(errno);
        ::unlink(temporary.c_str());
        return std::unexpected("close(" + temporary + "): " + reason);
    }
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const std::string reason = std::strerror(errno);
        ::unlink(temporary.c_str());
        return std::unexpected("rename(" + temporary + " -> " + path.string() + "): " + reason);
    }
    return {};
}

// Campaign ids become a directory name, so `.`/`..` and separators are rejected rather than
// normalised: an id that escapes its root is a path traversal, not a naming preference.
inline bool is_safe_component(std::string_view text) {
    if (text.empty() || text == "." || text == "..") return false;
    for (const char raw : text) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        const bool alphanumeric = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                                  (byte >= '0' && byte <= '9');
        if (alphanumeric || raw == '-' || raw == '_' || raw == '.') continue;
        return false;
    }
    return true;
}

inline bool is_json_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// The writer owns its manifest format, and campaign ids are safe components with no escapes, so a
// small reader is enough to detect an accidental overwrite of a different campaign. A full JSON
// parser would be a new dependency for one guard.
inline std::optional<std::string> read_manifest_campaign_id(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    constexpr std::string_view kKey = "\"campaign_id\"";
    size_t pos = text.find(kKey);
    if (pos == std::string::npos) return std::nullopt;
    pos += kKey.size();
    while (pos < text.size() && is_json_space(text[pos]))
        ++pos;
    if (pos >= text.size() || text[pos] != ':') return std::nullopt;
    ++pos;
    while (pos < text.size() && is_json_space(text[pos]))
        ++pos;
    if (pos >= text.size() || text[pos] != '"') return std::nullopt;
    ++pos;
    std::string id;
    while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\') return std::nullopt;
        id.push_back(text[pos++]);
    }
    if (pos >= text.size()) return std::nullopt;
    return id;
}

} // namespace export_detail

struct ArtifactOptions {
    std::filesystem::path root;
    // The campaign's metadata, including id and name. `on_universe` is ignored here; attach the
    // writer's sink to the live CampaignConfig after create().
    CampaignConfig campaign;
    BuildInfo build;
};

// Parent-side writer. Create it before Campaign::run, attach sink(), then call finish() with the
// report. Every write is parent-only: workers never touch the artifact store.
class CampaignArtifactWriter {
  public:
    CampaignArtifactWriter(const CampaignArtifactWriter&) = delete;
    CampaignArtifactWriter& operator=(const CampaignArtifactWriter&) = delete;
    CampaignArtifactWriter(CampaignArtifactWriter&&) = default;
    CampaignArtifactWriter& operator=(CampaignArtifactWriter&&) = default;

    static std::expected<CampaignArtifactWriter, std::string> create(ArtifactOptions options) {
        CampaignArtifactWriter writer(std::move(options));
        if (writer.options_.root.empty()) {
            return std::unexpected("artifact root is required");
        }
        if (!export_detail::is_safe_component(writer.options_.campaign.id)) {
            return std::unexpected("campaign id must be a non-empty [A-Za-z0-9._-] component: \"" +
                                   writer.options_.campaign.id + "\"");
        }
        if (auto prepared = writer.prepare(); !prepared) {
            return std::unexpected(prepared.error());
        }
        return writer;
    }

    const std::filesystem::path& root() const { return options_.root; }
    const std::string& error() const { return error_; }
    uint64_t universes_written() const { return written_; }

    // Attach to CampaignConfig::on_universe. The writer must outlive the campaign.
    std::function<void(const detail::UniverseOutcome&)> sink() {
        return [this](const detail::UniverseOutcome& outcome) { write_universe(outcome); };
    }

    // Write one schema-v1 MinimizeReport document under minimizations/<seed>.json. Reports are
    // optional and parent-side: minimization runs after the campaign, so this must be called
    // after the universe that owns the seed has arrived. The caller renders the document with
    // print_minimize_json(); an empty body is refused rather than written.
    std::expected<void, std::string> write_minimization(uint64_t seed, std::string_view json) {
        if (finished_) return std::unexpected("the artifact writer is already finished");
        if (!error_.empty()) return std::unexpected(error_);
        if (json.empty()) {
            fail("minimization document is empty");
            return std::unexpected(error_);
        }
        if (std::find(seeds_.begin(), seeds_.end(), seed) == seeds_.end()) {
            fail("minimization seed " + std::to_string(seed) + " was not written by this campaign");
            return std::unexpected(error_);
        }
        // The artifact layout holds one report per seed. A caller that minimizes the same seed
        // for two findings must not silently replace the first document; the finding identity is
        // part of the report, not the filename. Per-finding documents are a future addition.
        if (std::find(minimized_seeds_.begin(), minimized_seeds_.end(), seed) !=
            minimized_seeds_.end()) {
            fail("a minimization report for seed " + std::to_string(seed) + " was already written");
            return std::unexpected(error_);
        }
        if (auto written =
                export_detail::write_atomic(options_.root / minimization_relative_path(seed), json);
            !written) {
            fail(written.error());
            return std::unexpected(error_);
        }
        minimized_seeds_.push_back(seed);
        return {};
    }

    std::expected<void, std::string> finish(const CampaignReport& report) {
        if (finished_) {
            if (error_.empty()) return {};
            return std::unexpected(error_);
        }
        finished_ = true;
        if (rows_.is_open()) {
            rows_.flush();
            rows_.close();
            if (!rows_ && error_.empty()) fail("flush(" + universe_index_path() + ") failed");
        }

        // A refused campaign has no universes by construction; it must not look like a complete
        // empty artifact to a consumer that reads only manifest.json.
        if (report.refused) {
            std::string message = "campaign refused";
            if (!report.findings.empty() && !report.findings[0].detail.empty()) {
                message += ": " + report.findings[0].detail;
            }
            fail(std::move(message));
        } else if (error_.empty()) {
            if (written_ != report.runs) {
                fail("artifact has " + std::to_string(written_) + " universes but the report has " +
                     std::to_string(report.runs));
            }
            // The manifest identifies the run, so it must describe the report that was actually
            // produced. ArtifactOptions is a pre-run snapshot; a caller copying one config and
            // running another must not get a green manifest next to a different campaign.json.
            const CampaignConfig& declared = options_.campaign;
            if (declared.base_seed != report.base_seed) {
                fail("manifest base_seed does not match the report");
            } else if (declared.trials != report.runs) {
                fail("manifest trials " + std::to_string(declared.trials) +
                     " does not match report runs " + std::to_string(report.runs));
            } else if (declared.node_count != report.node_count) {
                fail("manifest node_count " + std::to_string(declared.node_count) +
                     " does not match the report");
            } else if (declared.parallel != report.parallel) {
                fail("manifest parallel " + std::to_string(declared.parallel) +
                     " does not match the report");
            } else if (declared.verify != report.verify) {
                fail("manifest verify does not match the report");
            } else if ((declared.verify || declared.trace) != report.trace) {
                fail("manifest trace intent does not match the report");
            } else if (declared.trace_capacity != report.trace_capacity) {
                fail("manifest trace_capacity does not match the report");
            } else if ((declared.events || declared.state) != report.events) {
                fail("manifest events intent does not match the report");
            } else if ((declared.events || declared.state
                            ? (declared.event_capacity != 0 ? declared.event_capacity : 4096)
                            : 0) != report.event_capacity) {
                fail("manifest event_capacity does not match the report");
            } else if (declared.state != report.state) {
                fail("manifest state intent does not match the report");
            } else if (declared.program != report.program) {
                fail("manifest program does not match the report");
            } else if (!report.id.empty() && declared.id != report.id) {
                fail("manifest campaign_id does not match the report");
            } else if (!report.name.empty() && declared.name != report.name) {
                fail("manifest name does not match the report");
            }
        }

        CampaignReport shown = report;
        if (shown.id.empty()) shown.id = options_.campaign.id;
        if (shown.name.empty()) shown.name = options_.campaign.name;
        std::ostringstream campaign_text;
        print_campaign_json(campaign_text, shown);
        if (auto written =
                export_detail::write_atomic(options_.root / kCampaignFile, campaign_text.str());
            !written) {
            fail(written.error());
        }

        completed_unix_ns_ = export_detail::unix_time_ns();
        if (auto manifest = write_manifest(); !manifest && error_.empty()) {
            fail(manifest.error());
        }
        if (!error_.empty()) return std::unexpected(error_);
        return {};
    }

  private:
    explicit CampaignArtifactWriter(ArtifactOptions options)
        : options_(std::move(options)), created_unix_ns_(export_detail::unix_time_ns()) {}

    void fail(std::string message) {
        if (error_.empty()) error_ = std::move(message);
    }

    std::string universe_index_path() const {
        return (options_.root / kUniverseIndexFile).string();
    }

    std::expected<void, std::string> prepare() {
        std::error_code error;
        bool has_existing_artifacts = false;
        const std::filesystem::path& root = options_.root;
        if (std::filesystem::exists(root, error)) {
            if (error) {
                return std::unexpected("exists(" + root.string() + "): " + error.message());
            }
            if (!std::filesystem::is_directory(root, error) || error) {
                return std::unexpected("artifact root is not a directory: " + root.string());
            }
            const bool empty = std::filesystem::is_empty(root, error);
            if (error) {
                return std::unexpected("is_empty(" + root.string() + "): " + error.message());
            }
            if (!empty) {
                const std::filesystem::path manifest = root / kManifestFile;
                if (!std::filesystem::exists(manifest, error) || error) {
                    return std::unexpected("artifact root is not empty and has no " +
                                           std::string(kManifestFile) + ": " + root.string());
                }
                const auto existing_id = export_detail::read_manifest_campaign_id(manifest);
                if (!existing_id) {
                    return std::unexpected("cannot read campaign_id from " + manifest.string());
                }
                if (*existing_id != options_.campaign.id) {
                    return std::unexpected("artifact root already contains campaign \"" +
                                           *existing_id + "\", not \"" + options_.campaign.id +
                                           "\"");
                }
                has_existing_artifacts = true;
            }
        }
        // Disclaim completeness before deleting anything: if the cleanup or a later write fails,
        // the old complete manifest must not survive next to a partially removed artifact.
        if (auto manifest = write_manifest(); !manifest) return manifest;
        if (has_existing_artifacts) {
            // Re-exporting the same campaign must not leave stale universe/trace files from a
            // larger previous run: a complete manifest plus leftover rows is a lying artifact.
            for (const char* name : {kUniverseDir, kTraceDir, kEventDir, kMinimizationDir,
                                     kUniverseIndexFile, kCampaignFile}) {
                std::filesystem::remove_all(root / name, error);
                if (error) {
                    return std::unexpected("remove_all(" + (root / name).string() +
                                           "): " + error.message());
                }
            }
        }
        std::filesystem::create_directories(root / kUniverseDir, error);
        if (error) {
            return std::unexpected("create_directories(" + (root / kUniverseDir).string() +
                                   "): " + error.message());
        }
        std::filesystem::create_directories(root / kTraceDir, error);
        if (error) {
            return std::unexpected("create_directories(" + (root / kTraceDir).string() +
                                   "): " + error.message());
        }
        std::filesystem::create_directories(root / kEventDir, error);
        if (error) {
            return std::unexpected("create_directories(" + (root / kEventDir).string() +
                                   "): " + error.message());
        }
        std::filesystem::create_directories(root / kMinimizationDir, error);
        if (error) {
            return std::unexpected("create_directories(" + (root / kMinimizationDir).string() +
                                   "): " + error.message());
        }
        rows_.open(root / kUniverseIndexFile, std::ios::binary | std::ios::trunc);
        if (!rows_) {
            return std::unexpected("open(" + universe_index_path() + ") failed");
        }
        return {};
    }

    std::expected<void, std::string> write_manifest() {
        std::ostringstream text;
        print_manifest_json(text, options_.campaign, options_.build, created_unix_ns_,
                            completed_unix_ns_, completed_unix_ns_ > 0 && error_.empty(), error_);
        return export_detail::write_atomic(options_.root / kManifestFile, text.str());
    }

    void write_universe(const detail::UniverseOutcome& outcome) {
        if (!error_.empty()) return;
        if (outcome.index != written_) {
            fail("universes arrived out of order or with a gap: expected index " +
                 std::to_string(written_) + ", got " + std::to_string(outcome.index));
            return;
        }
        const UniverseSummary summary = summarize(outcome);
        if (summary.trace_present) {
            if (auto written = export_detail::write_atomic(options_.root / summary.trace_path,
                                                           outcome.trace_bytes);
                !written) {
                fail(written.error());
                return;
            }
        }
        if (!outcome.events_error.empty()) {
            fail("universe " + std::to_string(summary.index) +
                 " event stream: " + outcome.events_error);
            return;
        }
        if (summary.events_present) {
            if (auto written = export_detail::write_atomic(options_.root / summary.events_path,
                                                           outcome.event_bytes);
                !written) {
                fail(written.error());
                return;
            }
        }

        std::ostringstream universe_text;
        print_universe_json(universe_text, summary);
        const std::filesystem::path universe_path =
            options_.root / kUniverseDir / (std::to_string(summary.index) + ".json");
        if (auto written = export_detail::write_atomic(universe_path, universe_text.str());
            !written) {
            fail(written.error());
            return;
        }

        std::ostringstream row_text;
        print_universe_row_json(row_text, summary);
        rows_ << row_text.str() << '\n';
        if (!rows_) {
            fail("append(" + universe_index_path() + ") failed");
            return;
        }
        seeds_.push_back(summary.seed);
        ++written_;
    }

    ArtifactOptions options_;
    std::ofstream rows_;
    int64_t created_unix_ns_ = 0;
    int64_t completed_unix_ns_ = 0;
    uint64_t written_ = 0;
    std::vector<uint64_t> seeds_;
    std::vector<uint64_t> minimized_seeds_;
    std::string error_;
    bool finished_ = false;
};

} // namespace cosmos
