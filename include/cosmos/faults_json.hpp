#pragma once

// JSON and fingerprint views of a FaultConfig (docs/ui-design.md Appendix A, U0.1/U0.2).
//
// The realized per-universe config is not the campaign template: FaultConfig::sample() varies it
// from the Swarm stream, and D14 says the UI must show what a seed actually ran. The universe
// exporter therefore embeds this object verbatim, and the config hash is computed from a canonical
// binary view so it does not depend on JSON formatting choices.

#include "cosmos/faults.hpp"
#include "cosmos/json.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

namespace cosmos {

constexpr const char* mode_name(FaultMode mode) {
    switch (mode) {
    case FaultMode::Safety:
        return "Safety";
    case FaultMode::Liveness:
        return "Liveness";
    }
    return "?";
}

namespace config_detail {

inline void append_u8(std::string& bytes, uint8_t value) {
    bytes.push_back(static_cast<char>(value));
}

inline void append_u32(std::string& bytes, uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

inline void append_u64(std::string& bytes, uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

inline void append_i64(std::string& bytes, int64_t value) {
    append_u64(bytes, static_cast<uint64_t>(value));
}

inline void append_double(std::string& bytes, double value) {
    // -0.0 and 0.0 are the same rate/weight to the injector; do not let the sign bit split the
    // identity of two otherwise identical configs.
    append_u64(bytes, std::bit_cast<uint64_t>(value == 0.0 ? 0.0 : value));
}

// The fingerprint covers the fields that change a universe's decisions, and the order is fixed.
// A new field must be appended with a version bump; otherwise two different configs can share an
// identity, which is exactly the kind of silent mismatch D11 exists to prevent.
inline constexpr uint8_t kConfigFingerprintVersion = 1;

inline std::string fingerprint_bytes(const FaultConfig& config) {
    std::string bytes;
    append_u8(bytes, kConfigFingerprintVersion);
    append_u8(bytes, static_cast<uint8_t>(config.mode));
    append_u64(bytes, config.enabled.to_ullong());
    append_u64(bytes, config.activated_sites.to_ullong());

    for (size_t slot = 0; slot < kSiteCount; ++slot) {
        const std::optional<FaultRule>& rule = config.rules[slot];
        append_u8(bytes, rule.has_value() ? 1 : 0);
        if (!rule.has_value()) continue;
        append_double(bytes, rule->rate);
        append_u64(bytes, rule->skip_first);
        append_u64(bytes, rule->max_injections);
        append_u8(bytes, rule->fire_on_eligible_call.has_value() ? 1 : 0);
        if (rule->fire_on_eligible_call.has_value()) {
            append_u64(bytes, *rule->fire_on_eligible_call);
        }
        append_i64(bytes, rule->amount.ns);
        append_u8(bytes, static_cast<uint8_t>(rule->outcomes.count));
        for (const SiteOutcome& outcome : rule->outcomes) {
            append_u8(bytes, static_cast<uint8_t>(outcome.kind));
            append_double(bytes, outcome.weight);
        }
    }

    append_u32(bytes, config.max_crashed_nodes);
    append_u32(bytes, config.min_healthy_quorum);

    append_u64(bytes, static_cast<uint64_t>(config.scheduled_episodes.size()));
    for (const ScheduledEpisode& episode : config.scheduled_episodes) {
        append_i64(bytes, episode.at.ns);
        append_i64(bytes, episode.until_heal.ns);
        if (const auto* crash = std::get_if<CrashNode>(&episode.spec)) {
            append_u8(bytes, 0);
            append_u32(bytes, crash->id);
        } else if (const auto* split = std::get_if<Partition>(&episode.spec)) {
            append_u8(bytes, 1);
            append_u64(bytes, static_cast<uint64_t>(split->a.size()));
            for (NodeId id : split->a)
                append_u32(bytes, id);
            append_u64(bytes, static_cast<uint64_t>(split->b.size()));
            for (NodeId id : split->b)
                append_u32(bytes, id);
        } else if (const auto* pause = std::get_if<PauseNode>(&episode.spec)) {
            append_u8(bytes, 2);
            append_u32(bytes, pause->id);
        }
    }

    append_u64(bytes, static_cast<uint64_t>(config.knobs.size()));
    for (const Knob& knob : config.knobs) {
        append_u64(bytes, knob.id);
        append_i64(bytes, knob.value);
    }

    append_i64(bytes, config.warmup_until.ns);
    append_i64(bytes, config.quiesce_after.ns);
    return bytes;
}

inline uint64_t fnv1a_64(std::string_view bytes) {
    uint64_t hash = 14695981039346656037ULL;
    for (const char raw : bytes) {
        hash ^= static_cast<uint8_t>(raw);
        hash *= 1099511628211ULL;
    }
    return hash;
}

} // namespace config_detail

// Stable identity for a realized config. Two universes print the same hash only when every field
// that can change a decision is the same.
inline std::string config_fingerprint(const FaultConfig& config) {
    return json::hex(config_detail::fnv1a_64(config_detail::fingerprint_bytes(config)));
}

// The order is fixed: enum order for classes, slot order for sites and rules, vector order for
// episodes and knobs. A UI can therefore diff two exports textually.
inline void print_config_json(std::ostream& out, const FaultConfig& config) {
    const json::classic_locale_guard locale(out);
    out << '{';
    out << "\"mode\": ";
    json::string(out, mode_name(config.mode));

    out << ", \"enabled_classes\": [";
    bool first = true;
    for (size_t index = 0; index < kFaultClassCount; ++index) {
        if (!config.enabled.test(index)) continue;
        if (!first) out << ", ";
        first = false;
        json::string(out, name_of(static_cast<FaultClass>(index)));
    }
    out << ']';

    out << ", \"activated_sites\": [";
    first = true;
    for (SiteId site : kAllSites) {
        if (!config.is_site_activated(site)) continue;
        if (!first) out << ", ";
        first = false;
        json::string(out, name_of(site));
    }
    out << ']';

    out << ", \"limits\": {\"max_crashed_nodes\": " << config.max_crashed_nodes
        << ", \"min_healthy_quorum\": " << config.min_healthy_quorum << '}';

    out << ", \"warmup_ns\": " << config.warmup_until.ns;
    out << ", \"quiesce_after_ns\": ";
    if (config.quiesce_after == Time::max()) {
        out << "null";
    } else {
        out << config.quiesce_after.ns;
    }

    out << ", \"rules\": {";
    first = true;
    for (size_t slot = 0; slot < kSiteCount; ++slot) {
        const std::optional<FaultRule>& rule = config.rules[slot];
        if (!rule.has_value()) continue;
        if (!first) out << ", ";
        first = false;
        json::string(out, name_of(site_at_slot(slot)));
        out << ": {\"rate\": ";
        json::number(out, rule->rate);
        out << ", \"skip_first\": " << rule->skip_first << ", \"max_injections\": ";
        json::optional_number(out, rule->max_injections != UINT64_MAX, rule->max_injections);
        out << ", \"amount_ns\": " << rule->amount.ns << ", \"fire_on_eligible_call\": ";
        json::optional_number(out, rule->fire_on_eligible_call.has_value(),
                              rule->fire_on_eligible_call.value_or(0));
        out << ", \"outcomes\": [";
        bool first_outcome = true;
        for (const SiteOutcome& outcome : rule->outcomes) {
            if (!first_outcome) out << ", ";
            first_outcome = false;
            out << "{\"kind\": ";
            json::string(out, name_of(outcome.kind));
            out << ", \"weight\": ";
            json::number(out, outcome.weight);
            out << '}';
        }
        out << "]}";
    }
    out << '}';

    out << ", \"episodes\": [";
    first = true;
    for (const ScheduledEpisode& episode : config.scheduled_episodes) {
        if (!first) out << ", ";
        first = false;
        out << "{\"at_ns\": " << episode.at.ns << ", \"until_heal_ns\": " << episode.until_heal.ns;
        if (const auto* crash = std::get_if<CrashNode>(&episode.spec)) {
            out << ", \"kind\": \"crash\", \"node\": " << crash->id;
        } else if (const auto* split = std::get_if<Partition>(&episode.spec)) {
            out << ", \"kind\": \"partition\", \"a\": [";
            for (size_t index = 0; index < split->a.size(); ++index) {
                if (index != 0) out << ", ";
                out << split->a[index];
            }
            out << "], \"b\": [";
            for (size_t index = 0; index < split->b.size(); ++index) {
                if (index != 0) out << ", ";
                out << split->b[index];
            }
            out << ']';
        } else if (const auto* pause = std::get_if<PauseNode>(&episode.spec)) {
            out << ", \"kind\": \"pause\", \"node\": " << pause->id;
        }
        out << '}';
    }
    out << ']';

    out << ", \"knobs\": [";
    for (size_t index = 0; index < config.knobs.size(); ++index) {
        if (index != 0) out << ", ";
        out << "{\"id\": " << config.knobs[index].id << ", \"value\": ";
        out << config.knobs[index].value << '}';
    }
    out << ']';
    out << '}';
}

inline std::string config_json(const FaultConfig& config) {
    std::ostringstream text;
    print_config_json(text, config);
    return text.str();
}

} // namespace cosmos
