#include "cosmos/gen.hpp"
#include "cosmos/random.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void must(bool ok) { assert(ok); }

void test_range_is_bounded_and_reproducible() {
    cosmos::Rng first(1234);
    cosmos::Rng second(1234);
    bool saw_low = false;
    bool saw_high = false;
    for (int i = 0; i < 100000; ++i) {
        const int a = cosmos::gen::range(first, -50, 50);
        const int b = cosmos::gen::range(second, -50, 50);
        must(a == b);
        must(a >= -50 && a <= 50);
        saw_low = saw_low || a == -50;
        saw_high = saw_high || a == 50;
    }
    must(saw_low && saw_high);
    std::cout << "[PASS] test_range_is_bounded_and_reproducible" << std::endl;
}

void test_range_handles_full_width_types() {
    cosmos::Rng rng(99);
    bool saw_negative = false;
    bool saw_positive = false;
    bool saw_unsigned_high = false;
    for (int i = 0; i < 10000; ++i) {
        const int64_t signed_value = cosmos::gen::range<int64_t>(
            rng, std::numeric_limits<int64_t>::min(), std::numeric_limits<int64_t>::max());
        saw_negative = saw_negative || signed_value < 0;
        saw_positive = saw_positive || signed_value > 0;

        const uint64_t unsigned_value =
            cosmos::gen::range<uint64_t>(rng, 0, std::numeric_limits<uint64_t>::max());
        saw_unsigned_high = saw_unsigned_high || unsigned_value > (UINT64_MAX / 2);
    }
    must(saw_negative && saw_positive && saw_unsigned_high);

    cosmos::Rng a(7);
    cosmos::Rng b(7);
    must(cosmos::gen::range(a, 7, 7) == 7);
    (void)b.next(); // a singleton still consumes exactly one draw
    must(a.next() == b.next());
    std::cout << "[PASS] test_range_handles_full_width_types" << std::endl;
}

void test_range_narrow_types() {
    cosmos::Rng rng(2024);
    bool saw_min = false;
    bool saw_max = false;
    bool saw_unsigned_min = false;
    bool saw_unsigned_max = false;
    for (int i = 0; i < 10000; ++i) {
        const int8_t signed_value = cosmos::gen::range<int8_t>(
            rng, std::numeric_limits<int8_t>::min(), std::numeric_limits<int8_t>::max());
        saw_min = saw_min || signed_value == std::numeric_limits<int8_t>::min();
        saw_max = saw_max || signed_value == std::numeric_limits<int8_t>::max();

        const uint8_t unsigned_value =
            cosmos::gen::range<uint8_t>(rng, 0, std::numeric_limits<uint8_t>::max());
        saw_unsigned_min = saw_unsigned_min || unsigned_value == 0;
        saw_unsigned_max = saw_unsigned_max || unsigned_value == UINT8_MAX;
    }
    must(saw_min && saw_max && saw_unsigned_min && saw_unsigned_max);
    std::cout << "[PASS] test_range_narrow_types" << std::endl;
}

void test_index_and_coin() {
    cosmos::Rng rng(4321);
    must(cosmos::gen::index(rng, 1) == 0);
    for (int i = 0; i < 10000; ++i) {
        must(cosmos::gen::index(rng, 5) < 5);
        must(!cosmos::gen::coin(rng, 0.0));
        must(cosmos::gen::coin(rng, 1.0));
    }
    bool saw_true = false;
    bool saw_false = false;
    for (int i = 0; i < 1000; ++i) {
        const bool value = cosmos::gen::coin(rng, 0.5);
        saw_true = saw_true || value;
        saw_false = saw_false || !value;
    }
    must(saw_true && saw_false);

    // Deterministic endpoints do not spend a draw.
    cosmos::Rng a(9);
    cosmos::Rng b(9);
    must(!cosmos::gen::coin(a, 0.0));
    must(cosmos::gen::coin(a, 1.0));
    must(a.next() == b.next());
    std::cout << "[PASS] test_index_and_coin" << std::endl;
}

void test_one_of_covers_every_choice() {
    cosmos::Rng rng(2468);
    const std::array<int, 3> choices{10, 20, 30};
    bool seen[3] = {};
    for (int i = 0; i < 1000; ++i) {
        const int selected = cosmos::gen::one_of(rng, choices);
        for (int index = 0; index < 3; ++index)
            seen[index] = seen[index] || selected == choices[index];
    }
    must(seen[0] && seen[1] && seen[2]);

    const std::vector<std::string> names{"alpha", "beta"};
    bool saw_alpha = false;
    bool saw_beta = false;
    for (int i = 0; i < 100; ++i) {
        const std::string selected = cosmos::gen::one_of(rng, names);
        saw_alpha = saw_alpha || selected == "alpha";
        saw_beta = saw_beta || selected == "beta";
    }
    must(saw_alpha && saw_beta);

    const char* literal = cosmos::gen::one_of(rng, {"x", "y"});
    must(std::string(literal) == "x" || std::string(literal) == "y");
    std::cout << "[PASS] test_one_of_covers_every_choice" << std::endl;
}

void test_string_respects_length_and_alphabet() {
    cosmos::Rng rng(1357);
    const std::string value = cosmos::gen::string(rng, 32, "ab");
    must(value.size() == 32);
    for (char c : value)
        must(c == 'a' || c == 'b');

    const std::string ranged = cosmos::gen::string(rng, 4, 12);
    must(ranged.size() >= 4 && ranged.size() <= 12);

    // A one-character alphabet is the supported way to fill; a scalar char is rejected above.
    must(cosmos::gen::string(rng, 4, "z") == "zzzz");
    for (char c : ranged)
        must(cosmos::gen::kDefaultAlphabet.find(c) != std::string_view::npos);

    must(cosmos::gen::string(rng, 0).empty());

    char buffer[8] = {};
    cosmos::gen::fill_string(rng, std::span<char>(buffer), "xy");
    for (char c : buffer)
        must(c == 'x' || c == 'y');

    // An empty destination draws nothing.
    cosmos::Rng a(77);
    cosmos::Rng b(77);
    cosmos::gen::fill_string(a, std::span<char>(), "xy");
    must(a.next() == b.next());

    // An empty alphabet is total in release: NULs, and no draws.
    cosmos::Rng c(78);
    cosmos::Rng d(78);
    const std::string empty_alphabet = cosmos::gen::string(c, 3, "");
    must(empty_alphabet == std::string(3, '\0'));
    must(c.next() == d.next());
    std::cout << "[PASS] test_string_respects_length_and_alphabet" << std::endl;
}

void test_bytes_fills_the_whole_buffer() {
    constexpr std::size_t kSize = 37; // deliberately not a multiple of 8
    std::array<std::byte, kSize> first;
    std::array<std::byte, kSize> second;
    first.fill(std::byte{0xff});
    second.fill(std::byte{0xff});
    std::array<std::byte, kSize> sentinel;
    sentinel.fill(std::byte{0xff});

    cosmos::Rng rng(8642);
    cosmos::Rng expected(8642);
    std::array<std::byte, kSize> reference{};
    for (std::size_t offset = 0; offset < kSize; offset += sizeof(uint64_t)) {
        const uint64_t word = expected.next();
        const std::size_t chunk = std::min(kSize - offset, sizeof(word));
        std::memcpy(reference.data() + offset, &word, chunk);
    }

    cosmos::gen::bytes(rng, first);
    must(first == reference);
    must(first != sentinel);

    cosmos::Rng again(8642);
    cosmos::gen::bytes(again, second);
    must(first == second);

    // Five eight-byte draws for 37 bytes; an empty call adds none.
    must(rng.next() == expected.next());
    cosmos::gen::bytes(rng, std::span<std::byte>());
    must(rng.next() == expected.next());
    std::cout << "[PASS] test_bytes_fills_the_whole_buffer" << std::endl;
}

void test_exponential_is_non_negative_and_has_the_requested_mean() {
    cosmos::Rng rng(97531);
    must(cosmos::gen::exponential(rng, cosmos::Duration{0}).ns == 0);
    must(cosmos::gen::exponential(rng, cosmos::Duration{-1}).ns == 0);

    constexpr int kSamples = 200000;
    const cosmos::Duration mean{1'000'000}; // 1ms
    int64_t total = 0;
    for (int i = 0; i < kSamples; ++i) {
        const cosmos::Duration sample = cosmos::gen::exponential(rng, mean);
        must(sample.ns >= 0);
        total += sample.ns;
    }
    const double observed = static_cast<double>(total) / kSamples;
    must(std::fabs(observed - static_cast<double>(mean.ns)) < static_cast<double>(mean.ns) * 0.05);

    // A mean with no distribution must not consume a draw.
    cosmos::Rng a(17);
    cosmos::Rng b(17);
    must(cosmos::gen::exponential(a, cosmos::Duration{0}).ns == 0);
    must(cosmos::gen::exponential(a, cosmos::Duration{-1}).ns == 0);
    must(a.next() == b.next());
    std::cout << "[PASS] test_exponential_is_non_negative_and_has_the_requested_mean" << std::endl;
}

void test_shuffle_is_a_permutation_and_is_reproducible() {
    cosmos::Rng first(1212);
    cosmos::Rng second(1212);
    std::vector<int> a(100);
    std::vector<int> b(100);
    for (int i = 0; i < 100; ++i)
        a[i] = b[i] = i;
    const std::vector<int> original = a;
    cosmos::gen::shuffle(first, a);
    cosmos::gen::shuffle(second, b);
    must(a == b);
    must(a != original); // a no-op implementation must not pass

    std::vector<int> sorted = a;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < 100; ++i)
        must(sorted[i] == i);

    // A permutation alone also admits the identity; this pins that the fixed seed moved most
    // elements rather than only checking the multiset.
    std::size_t moved = 0;
    for (int i = 0; i < 100; ++i)
        moved += a[i] != original[i] ? 1 : 0;
    must(moved > 50);

    std::vector<int> empty;
    cosmos::gen::shuffle(first, empty);
    std::vector<int> one{7};
    cosmos::gen::shuffle(first, one);
    must(one[0] == 7);
    std::cout << "[PASS] test_shuffle_is_a_permutation_and_is_reproducible" << std::endl;
}

void test_weighted_one_of_respects_the_weights() {
    cosmos::Rng rng(3333);
    int zeros = 0;
    int ones = 0;
    for (int i = 0; i < 100000; ++i) {
        const int value = cosmos::gen::weighted_one_of<int>(rng, {{0, 1.0}, {1, 3.0}});
        must(value == 0 || value == 1);
        if (value == 0) ++zeros;
        if (value == 1) ++ones;
    }
    must(zeros > 22000 && zeros < 28000);
    must(ones > 72000 && ones < 78000);

    // A zero-weight bucket, including the first one, is unreachable.
    for (int i = 0; i < 10000; ++i) {
        must(cosmos::gen::weighted_one_of<int>(rng, {{0, 0.0}, {1, 1.0}}) == 1);
        must(cosmos::gen::weighted_one_of<int>(rng, {{0, 1.0}, {1, 0.0}}) == 0);
    }
    std::cout << "[PASS] test_weighted_one_of_respects_the_weights" << std::endl;
}

} // namespace

int main() {
    test_range_is_bounded_and_reproducible();
    test_range_handles_full_width_types();
    test_range_narrow_types();
    test_index_and_coin();
    test_one_of_covers_every_choice();
    test_string_respects_length_and_alphabet();
    test_bytes_fills_the_whole_buffer();
    test_exponential_is_non_negative_and_has_the_requested_mean();
    test_shuffle_is_a_permutation_and_is_reproducible();
    test_weighted_one_of_respects_the_weights();
    std::cout << "All generator tests passed." << std::endl;
    return 0;
}
