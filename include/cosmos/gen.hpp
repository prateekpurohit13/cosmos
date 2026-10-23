#pragma once

// Property-testing-style value generators (docs/design.md §11). Every draw comes from the caller's
// Rng, so a workload is a pure function of the universe seed. The fill_* forms never allocate; the
// owning forms (string, one_of with a copying value type) allocate only through the selected type,
// and none touches a fault or schedule stream.

#include "cosmos/random.hpp"
#include "cosmos/time.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace cosmos::gen {

inline constexpr std::string_view kDefaultAlphabet = "abcdefghijklmnopqrstuvwxyz0123456789";

// Inclusive [lo, hi], signed or unsigned. The span is computed in unsigned arithmetic so the full
// range of a signed type (INT64_MIN..INT64_MAX) is representable rather than an overflow.
template <std::integral T>
    requires(!std::same_as<T, bool>)
T range(Rng& rng, T lo, T hi) {
    assert(lo <= hi);
    if (lo > hi) return lo; // total in release, like the other generators
    using U = std::make_unsigned_t<T>;
    const U ulo = static_cast<U>(lo);
    const U uhi = static_cast<U>(hi);
    const U span = static_cast<U>(uhi - ulo);
    const U offset = span == std::numeric_limits<U>::max()
                         ? static_cast<U>(rng.next())
                         : static_cast<U>(rng.range(0, static_cast<uint64_t>(span)));
    return static_cast<T>(static_cast<U>(ulo + offset));
}

// Mixed-width call sites (int bounds, size_t count) use this non-template form; the template above
// still wins for same-type arguments, preserving the narrower return type.
inline uint64_t range(Rng& rng, uint64_t lo, uint64_t hi) { return range<uint64_t>(rng, lo, hi); }

// [0, n). The primitive behind one_of, kept separate so a caller can index its own table without
// copying the selected value.
inline std::size_t index(Rng& rng, std::size_t n) {
    assert(n > 0);
    if (n == 0) return 0;
    return static_cast<std::size_t>(rng.range(0, static_cast<uint64_t>(n - 1)));
}

inline bool coin(Rng& rng, double p) {
    assert(std::isfinite(p) && p >= 0.0 && p <= 1.0);
    if (!std::isfinite(p)) return false;
    if (p <= 0.0) return false;
    if (p >= 1.0) return true;
    return rng.coin(p);
}

// Any random-access range: std::array, std::vector, std::span, std::initializer_list, or a C array.
// Returning by value keeps the common case free of lifetime traps; use index() when a copy matters.
template <std::ranges::random_access_range R>
    requires std::copy_constructible<std::ranges::range_value_t<R>> &&
             std::default_initializable<std::ranges::range_value_t<R>>
std::ranges::range_value_t<R> one_of(Rng& rng, const R& choices) {
    assert(!std::ranges::empty(choices));
    if (std::ranges::empty(choices)) return std::ranges::range_value_t<R>{};
    const std::size_t selected = index(rng, static_cast<std::size_t>(std::ranges::size(choices)));
    return *(std::ranges::begin(choices) +
             static_cast<std::ranges::range_difference_t<R>>(selected));
}

// Braced lists are not deducible through the generic overload, so they get their own.
template <typename T>
    requires std::default_initializable<T>
T one_of(Rng& rng, std::initializer_list<T> choices) {
    assert(choices.size() > 0);
    if (choices.size() == 0) return T{};
    return *(choices.begin() + static_cast<std::ptrdiff_t>(index(rng, choices.size())));
}

// Fills caller-owned storage without allocating, which matters when a test pins an exact
// eligible-call index at SiteId::malloc: a generated std::string would otherwise spend that index.
inline void fill_string(Rng& rng, std::span<char> out,
                        std::string_view alphabet = kDefaultAlphabet) {
    if (out.empty()) return;
    if (alphabet.empty()) {
        // Total in release too: an empty alphabet produces NUL bytes rather than leaving the
        // caller's buffer untouched (which looked like valid data for an owning string).
        std::fill(out.begin(), out.end(), '\0');
        return;
    }
    for (char& c : out)
        c = alphabet[index(rng, alphabet.size())];
}

inline std::string string(Rng& rng, std::size_t len, std::string_view alphabet = kDefaultAlphabet) {
    std::string out(len, '\0');
    fill_string(rng, std::span<char>(out), alphabet);
    return out;
}

// A scalar third argument is a mistake, not an alphabet: without this deleted overload
// string(rng, 10, 'a') silently selected the (min_len, max_len) form with max_len == 97.
template <typename T>
    requires(std::same_as<T, char> || std::same_as<T, signed char> ||
             std::same_as<T, unsigned char> || std::same_as<T, char8_t> ||
             std::same_as<T, char16_t> || std::same_as<T, char32_t> || std::same_as<T, wchar_t>)
std::string string(Rng&, std::size_t, T) = delete;

inline std::string string(Rng& rng, std::size_t min_len, std::size_t max_len,
                          std::string_view alphabet = kDefaultAlphabet) {
    assert(min_len <= max_len);
    if (min_len == max_len) return string(rng, min_len, alphabet); // no pointless length draw
    const uint64_t len =
        range<uint64_t>(rng, static_cast<uint64_t>(min_len), static_cast<uint64_t>(max_len));
    return string(rng, static_cast<std::size_t>(len), alphabet);
}

// Fills a byte buffer without allocating. Each eight-byte chunk, including a trailing partial one,
// consumes exactly one next() draw: the stream advances by ceil(out.size() / 8) draws. The bytes
// are copied in host byte order, so the same seed is byte-identical per architecture but not
// across endianness.
inline void bytes(Rng& rng, std::span<std::byte> out) {
    std::size_t offset = 0;
    while (offset < out.size()) {
        const uint64_t word = rng.next();
        const std::size_t chunk = std::min(out.size() - offset, sizeof(word));
        std::memcpy(out.data() + offset, &word, chunk);
        offset += chunk;
    }
}

// Exponential inter-arrival time with the given mean. A non-positive mean has no distribution to
// draw from and yields zero. A draw beyond int64 nanoseconds saturates to Duration::max() rather
// than overflowing. The log-based form is reproducible for one binary; like FaultConfig::sample,
// it inherits libm's cross-toolchain freedom.
inline Duration exponential(Rng& rng, Duration mean) {
    if (mean.ns <= 0) return Duration::zero();
    const double u = 1.0 - rng.uniform(); // (0, 1], so log(u) is finite
    const double ns = -std::log(u) * static_cast<double>(mean.ns);
    if (!(ns < static_cast<double>(std::numeric_limits<int64_t>::max()))) return Duration::max();
    return Duration{static_cast<int64_t>(ns)};
}

// Fisher-Yates over caller-owned storage. An lvalue is required: a temporary would be shuffled and
// discarded, and a const range could not be swapped.
template <std::ranges::random_access_range R>
    requires(!std::is_const_v<std::remove_reference_t<std::ranges::range_reference_t<R>>>)
void shuffle(Rng& rng, R& values) {
    const std::size_t n = static_cast<std::size_t>(std::ranges::size(values));
    for (std::size_t i = n; i > 1; --i) {
        const std::size_t j = index(rng, i);
        std::swap(*(std::ranges::begin(values) + static_cast<std::ptrdiff_t>(i - 1)),
                  *(std::ranges::begin(values) + static_cast<std::ptrdiff_t>(j)));
    }
}

// Weighted choice over caller-supplied values. All weights must be non-negative and at least one
// must be positive. The explicit T keeps braced initializer lists of pairs deducible:
//   gen::weighted_one_of<int>(rng, {{0, 50.0}, {1, 40.0}, {2, 10.0}})
template <typename T>
    requires std::default_initializable<T>
T weighted_one_of(Rng& rng, std::initializer_list<std::pair<T, double>> choices) {
    assert(choices.size() > 0);
    if (choices.size() == 0) return T{};

    double total = 0.0;
    for (const auto& [value, weight] : choices) {
        (void)value;
        assert(std::isfinite(weight) && weight >= 0.0);
        if (!std::isfinite(weight) || weight < 0.0) continue;
        total += weight;
    }
    assert(std::isfinite(total) && total > 0.0);
    if (!std::isfinite(total) || !(total > 0.0)) return choices.begin()->first;

    const double point = rng.uniform() * total;
    double cumulative = 0.0;
    const T* last_positive = &choices.begin()->first;
    for (const auto& [value, weight] : choices) {
        if (std::isfinite(weight) && weight > 0.0) last_positive = &value;
        cumulative += std::isfinite(weight) && weight > 0.0 ? weight : 0.0;
        if (point < cumulative) return value;
    }
    return *last_positive; // round-off can put point at total; never bias to the first bucket
}

} // namespace cosmos::gen
