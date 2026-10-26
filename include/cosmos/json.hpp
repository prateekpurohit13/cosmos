#pragma once

// Small JSON emission helpers for the artifact contract (docs/ui-design.md Appendix A).
//
// This is deliberately not a JSON library: Cosmos only writes JSON, never parses it, and the
// writers in faults_json.hpp / campaign.hpp / export.hpp need exactly three things that are easy
// to get wrong by hand -- escaping, finite doubles, and 64-bit identifiers. Keeping them here
// means a seed is never rounded through a JavaScript number by accident.
//
// The schema is versioned with a single integer. Additive fields do not bump it; a reader rejects
// a version it does not know rather than guessing.

#include <charconv>
#include <cmath>
#include <cstdint>
#include <locale>
#include <ostream>
#include <string>
#include <string_view>

namespace cosmos {

// v1 artifact schema, shared by campaign.json, manifest.json and universes/<index>.json.
inline constexpr int kArtifactSchemaVersion = 1;

namespace json {

// Artifact JSON must not depend on the application's global locale. A caller that installs a
// thousands-grouping numpunct would otherwise turn `"index": 1000` into `"index": 1'000`, which no
// JSON parser accepts. Restores the previous facet on scope exit.
class classic_locale_guard {
  public:
    explicit classic_locale_guard(std::ostream& out) : out_(out), previous_(out.getloc()) {
        out_.imbue(std::locale::classic());
    }
    ~classic_locale_guard() { out_.imbue(previous_); }

    classic_locale_guard(const classic_locale_guard&) = delete;
    classic_locale_guard& operator=(const classic_locale_guard&) = delete;

  private:
    std::ostream& out_;
    std::locale previous_;
};

// Serialises a 64-bit identifier as a decimal JSON string. JavaScript's JSON.parse loses
// precision above 2^53, and a truncated seed is not a repro identity.
inline std::string decimal(uint64_t value) { return std::to_string(value); }

inline std::string hex(uint64_t value) {
    char digits[19];
    digits[0] = '0';
    digits[1] = 'x';
    for (int index = 0; index < 16; ++index) {
        const int nibble = static_cast<int>((value >> ((15 - index) * 4)) & 0xF);
        digits[2 + index] = static_cast<char>(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    // 2 prefix bytes plus 16 hex digits; sizeof(digits) is 19 and would append the terminator
    // slot as an uninitialised char.
    return std::string(digits, 18);
}

// Length of a valid UTF-8 sequence starting at pos, or 0 for an invalid lead byte or a malformed
// continuation. JSON text must be valid UTF-8 (RFC 8259); a single Latin-1 byte from a filename or
// an exception message would otherwise make the whole artifact unparseable.
inline size_t utf8_sequence_length(std::string_view text, size_t pos) {
    const auto byte = [&](size_t index) { return static_cast<unsigned char>(text[index]); };
    const unsigned char lead = byte(pos);
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) {
        if (pos + 1 >= text.size() || (byte(pos + 1) & 0xC0) != 0x80) return 0;
        return 2;
    }
    if (lead >= 0xE0 && lead <= 0xEF) {
        if (pos + 2 >= text.size()) return 0;
        const unsigned char second = byte(pos + 1);
        const unsigned char third = byte(pos + 2);
        bool second_ok = false;
        if (lead == 0xE0) {
            second_ok = second >= 0xA0 && second <= 0xBF; // no overlong form
        } else if (lead == 0xED) {
            second_ok = second >= 0x80 && second <= 0x9F; // no surrogate
        } else {
            second_ok = second >= 0x80 && second <= 0xBF;
        }
        if (!second_ok || (third & 0xC0) != 0x80) return 0;
        return 3;
    }
    if (lead >= 0xF0 && lead <= 0xF4) {
        if (pos + 3 >= text.size()) return 0;
        const unsigned char second = byte(pos + 1);
        const unsigned char third = byte(pos + 2);
        const unsigned char fourth = byte(pos + 3);
        bool second_ok = false;
        if (lead == 0xF0) {
            second_ok = second >= 0x90 && second <= 0xBF; // no overlong form
        } else if (lead == 0xF4) {
            second_ok = second >= 0x80 && second <= 0x8F; // <= U+10FFFF
        } else {
            second_ok = second >= 0x80 && second <= 0xBF;
        }
        if (!second_ok || (third & 0xC0) != 0x80 || (fourth & 0xC0) != 0x80) return 0;
        return 4;
    }
    return 0;
}

inline void escape(std::ostream& out, std::string_view text) {
    for (size_t index = 0; index < text.size();) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        switch (byte) {
        case '"':
            out << "\\\"";
            ++index;
            continue;
        case '\\':
            out << "\\\\";
            ++index;
            continue;
        case '\b':
            out << "\\b";
            ++index;
            continue;
        case '\f':
            out << "\\f";
            ++index;
            continue;
        case '\n':
            out << "\\n";
            ++index;
            continue;
        case '\r':
            out << "\\r";
            ++index;
            continue;
        case '\t':
            out << "\\t";
            ++index;
            continue;
        default:
            break;
        }
        if (byte < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            out << "\\u00" << kHex[(byte >> 4) & 0xF] << kHex[byte & 0xF];
            ++index;
            continue;
        }
        const size_t length = utf8_sequence_length(text, index);
        if (length == 0) {
            // Replace one invalid byte at a time. Lossy is better than a document no strict parser
            // can read; the alternative is to reject the entire artifact.
            out << "\\ufffd";
            ++index;
            continue;
        }
        out.write(text.data() + index, static_cast<std::streamsize>(length));
        index += length;
    }
}

inline void string(std::ostream& out, std::string_view text) {
    out << '"';
    escape(out, text);
    out << '"';
}

// Non-finite values have no JSON representation; emit null rather than the non-standard tokens a
// naive stream insertion would produce. Config validation rejects them, but a hand-built report
// must not be able to make the artifact unparseable.
inline void number(std::ostream& out, double value) {
    if (!std::isfinite(value)) {
        out << "null";
        return;
    }
    if (value == 0.0) {
        // -0.0 is the same rate/weight as +0.0 and the fingerprint normalizes it; keep the text
        // form consistent so a textual diff never shows a change the hash says is absent.
        out << '0';
        return;
    }
    char buffer[64];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general);
    if (result.ec != std::errc{}) {
        out << "null";
        return;
    }
    out.write(buffer, result.ptr - buffer);
}

inline void number(std::ostream& out, uint64_t value) { out << value; }
inline void number(std::ostream& out, int64_t value) { out << value; }
inline void number(std::ostream& out, unsigned value) { out << value; }
inline void number(std::ostream& out, int value) { out << value; }
inline void boolean(std::ostream& out, bool value) { out << (value ? "true" : "false"); }

// Optional 64-bit counters: a null field means "unbounded" or "never", which is not the same as
// a very large number.
inline void optional_number(std::ostream& out, bool present, uint64_t value) {
    if (!present) {
        out << "null";
    } else {
        out << value;
    }
}

} // namespace json
} // namespace cosmos
