#include "../src/slide.hpp"
#include "../src/internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using tlg::detail::SlideDecoder;
using tlg::detail::SlideEncoder;
using tlg::SlideDecode;
using tlg::SlideParse;
using tlg::SlideMatch;
std::size_t checks{};
void check(bool condition, std::string_view expression, int line) {
    ++checks;
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + std::string(expression));
}
#define CHECK(expr) check(static_cast<bool>(expr), #expr, __LINE__)

// Deliberately simple wire-format oracle. It shares no matching, buffering,
// snapshot, or fast-copy code with the library and keeps state between chunks.
struct ReferenceDecoder {
    std::array<std::uint8_t, 4096> dictionary{};
    std::size_t cursor{};
    explicit ReferenceDecoder(std::span<const std::uint8_t> initial = {}) {
        std::copy(initial.begin(), initial.end(), dictionary.begin());
    }
    std::vector<std::uint8_t> decode(std::span<const std::uint8_t> input) {
        std::vector<std::uint8_t> out;
        std::size_t pos{};
        const auto emit = [&](std::uint8_t byte) {
            out.push_back(byte);
            dictionary[cursor] = byte;
            cursor = (cursor + 1) % dictionary.size();
        };
        while (pos < input.size()) {
            const auto flags = input[pos++];
            for (unsigned token = 0; token < 8 && pos < input.size(); ++token) {
                if ((flags & (1U << token)) == 0) emit(input[pos++]);
                else {
                    CHECK(input.size() - pos >= 2);
                    const auto low = input[pos++];
                    const auto high = input[pos++];
                    auto source = static_cast<std::size_t>(low) + (high % 16) * 256;
                    auto count = static_cast<std::size_t>(high / 16) + 3;
                    if (count == 18) { CHECK(pos < input.size()); count += input[pos++]; }
                    for (std::size_t i = 0; i < count; ++i) {
                        emit(dictionary[source]);
                        source = (source + 1) % dictionary.size();
                    }
                }
            }
        }
        return out;
    }
};
std::vector<std::uint8_t> data_for(unsigned kind, std::size_t size, std::uint32_t seed = 0x12345678U) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        switch (kind) {
        case 0: data[i] = 0; break;
        case 1: data[i] = 255; break;
        case 2: data[i] = static_cast<std::uint8_t>(i % 251); break;
        case 3: data[i] = static_cast<std::uint8_t>((seed >> 24) % 4); break;
        default: data[i] = static_cast<std::uint8_t>(seed >> 24); break;
        }
    }
    return data;
}
std::vector<std::uint8_t> literals(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> stream;
    for (std::size_t pos = 0; pos < input.size(); pos += 8) {
        stream.push_back(0);
        const auto group = input.subspan(pos, std::min<std::size_t>(8, input.size() - pos));
        stream.insert(stream.end(), group.begin(), group.end());
    }
    return stream;
}
void wire_examples() {
    const auto decode = [](std::initializer_list<std::uint8_t> bytes, std::size_t size) {
        return SlideDecoder{}.decode(std::span(bytes.begin(), bytes.size()), size);
    };
    CHECK(SlideEncoder{}.encode({}).empty());
    CHECK(SlideDecoder{}.decode({}, 0).empty());
    CHECK(decode({0, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 0, 'i'}, 9) == std::vector<std::uint8_t>({'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'}));
    CHECK(decode({1, 0, 0}, 3) == std::vector<std::uint8_t>(3, 0));
    CHECK(decode({1, 0, 0xe0}, 17) == std::vector<std::uint8_t>(17, 0));
    CHECK(decode({1, 0, 0xf0, 0}, 18) == std::vector<std::uint8_t>(18, 0));
    CHECK(decode({1, 0, 0xf0, 255}, 273) == std::vector<std::uint8_t>(273, 0));
    CHECK(decode({4, 'a', 'b', 1, 0}, 5) == std::vector<std::uint8_t>({'a', 'b', 'b', 'b', 'b'}));
    CHECK(decode({4, 'a', 'b', 0, 0x30}, 8) == std::vector<std::uint8_t>({'a', 'b', 'a', 'b', 'a', 'b', 'a', 'b'}));
    CHECK(decode({2, 'x', 0, 0xf0, 255}, 274) == std::vector<std::uint8_t>(274, 'x'));
    for (const auto length : {4096U, 4104U}) {
        auto raw = data_for(2, length);
        auto stream = literals(raw);
        if (length == 4096) {
            stream.insert(stream.end(), {1, 255, 15});
            raw.insert(raw.end(), 3, raw.back());
        } else {
            stream.insert(stream.end(), {1, 254, 0x2f});
            const std::array tail{raw[4094], raw[4095], raw[4096], raw[4097], raw[4098]};
            raw.insert(raw.end(), tail.begin(), tail.end());
        }
        CHECK(SlideDecoder{}.decode(stream, raw.size()) == raw);
    }
    for (std::size_t extra = 0; extra < 16; ++extra) {
        const auto length = 4096 + extra * 8;
        auto raw = data_for(2, length);
        auto stream = literals(raw);
        const auto slot = length % 4096;
        stream.insert(stream.end(), {1, static_cast<std::uint8_t>(slot), static_cast<std::uint8_t>((slot >> 8) | 0x20)});
        const std::array tail{raw[length - 4096], raw[length - 4095], raw[length - 4094], raw[length - 4093], raw[length - 4092]};
        raw.insert(raw.end(), tail.begin(), tail.end());
        CHECK(SlideDecoder{}.decode(stream, raw.size()) == raw);
    }
    SlideDecoder persistent;
    CHECK(persistent.decode(std::array<std::uint8_t, 6>{0, 'h', 'e', 'l', 'l', 'o'}, 5) == std::vector<std::uint8_t>({'h', 'e', 'l', 'l', 'o'}));
    CHECK(persistent.decode(std::array<std::uint8_t, 3>{1, 1, 0}, 3) == std::vector<std::uint8_t>({'e', 'l', 'l'}));
}
void round_trips() {
    for (unsigned kind = 0; kind < 5; ++kind) {
        for (std::size_t size = 0; size <= 64; ++size) {
            const auto raw = data_for(kind, size);
            const auto encoded = SlideEncoder{}.encode(raw);
            CHECK(encoded.size() <= raw.size() + (raw.size() + 7) / 8);
            CHECK(SlideDecoder{}.decode(encoded, raw.size()) == raw);
            CHECK(ReferenceDecoder{}.decode(encoded) == raw);
        }
        for (const auto size : {272U, 273U, 274U, 275U, 4095U, 4096U, 4097U, 8192U, 9001U}) {
            const auto raw = data_for(kind, size);
            const auto encoded = SlideEncoder{}.encode(raw);
            CHECK(SlideDecoder{}.decode(encoded, raw.size()) == raw);
            CHECK(ReferenceDecoder{}.decode(encoded) == raw);
            CHECK(SlideEncoder{}.encode(raw) == encoded);
        }
        for (unsigned seed = 1; seed <= 3; ++seed) {
            const auto raw = data_for(kind, 24000, seed);
            SlideEncoder encoder;
            SlideDecoder decoder;
            ReferenceDecoder reference;
            std::size_t pos{};
            std::uint32_t rng = seed;
            while (pos < raw.size()) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                const auto size = std::min<std::size_t>(1 + rng % 997, raw.size() - pos);
                const auto chunk = std::span(raw).subspan(pos, size);
                const auto encoded = encoder.encode(chunk);
                const auto actual = decoder.decode(encoded, size);
                CHECK(std::ranges::equal(actual, chunk));
                CHECK(reference.decode(encoded) == actual);
                CHECK(encoder.encode({}).empty());
                CHECK(decoder.decode({}, 0).empty());
                pos += size;
            }
        }
    }
}
void snapshots_and_initial_dictionary() {
    const auto dictionary = data_for(4, 4096);
    auto raw = std::vector<std::uint8_t>(dictionary.begin() + 200, dictionary.begin() + 700);
    raw.insert(raw.end(), dictionary.begin(), dictionary.begin() + 500);
    SlideEncoder encoder(dictionary);
    SlideDecoder decoder(dictionary);
    ReferenceDecoder reference(dictionary);
    for (unsigned attempt = 0; attempt < 24; ++attempt) {
        encoder.store();
        const auto discarded = encoder.encode(data_for(attempt % 5, 7000, attempt + 1));
        CHECK(!discarded.empty());
        encoder.restore();
        const auto encoded = encoder.encode(raw);
        CHECK(decoder.decode(encoded, raw.size()) == raw);
        CHECK(reference.decode(encoded) == raw);
        // Repeated restore is supported; it reuses the last snapshot.
        encoder.restore();
        CHECK(encoder.encode(raw) == encoded);
    }
    const std::array<std::uint8_t, 3> match{1, 0xf8, 0x2f};
    std::vector<std::uint8_t> tail(dictionary.begin() + 4088, dictionary.begin() + 4093);
    CHECK(SlideDecoder(dictionary).decode(match, 5) == tail);
}

std::vector<std::uint8_t> match_token(std::size_t slot, std::size_t length) {
    std::vector<std::uint8_t> bytes{1, static_cast<std::uint8_t>(slot),
        static_cast<std::uint8_t>((slot >> 8) | ((length >= 18 ? 15 : length - 3) << 4))};
    if (length >= 18) bytes.push_back(static_cast<std::uint8_t>(length - 18));
    return bytes;
}

void decoder_algorithms() {
    const auto dictionary = data_for(4, 4096, 123);
    // All 12-bit distances, both sides of byte-copy/length-extension boundaries.
    // Prefix literals make matches straddle old dictionary and current output.
    for (std::size_t distance = 1; distance <= 4096; ++distance) {
        for (const auto length : {3U, 7U, 8U, 15U, 16U, 17U, 18U, 31U, 63U, 127U, 255U, 272U, 273U}) {
            const auto prefix = data_for(4, 8, static_cast<std::uint32_t>(distance));
            auto stream = literals(prefix);
            const auto token = match_token((8 + 4096 - distance) & 4095, length);
            stream.insert(stream.end(), token.begin(), token.end());
            ReferenceDecoder reference(dictionary);
            const auto expected = reference.decode(stream);
            SlideDecoder bulk(dictionary, SlideDecode::bulk);
            SlideDecoder scalar(dictionary, SlideDecode::scalar);
            CHECK(bulk.decode(stream, 8 + length) == expected);
            CHECK(scalar.decode(stream, 8 + length) == expected);
            const auto next = match_token((reference.cursor + 4096 - distance) & 4095, 273);
            const auto expected_next = reference.decode(next);
            CHECK(bulk.decode(next, 273) == expected_next);
            CHECK(scalar.decode(next, 273) == expected_next);
        }
    }
    // Commit tails of exactly a ring, several rings, and a wrapped partial ring.
    for (const auto size : {1U, 7U, 8U, 4095U, 4096U, 4097U, 8192U, 8201U}) {
        SlideDecoder bulk(dictionary, SlideDecode::bulk), scalar(dictionary, SlideDecode::scalar);
        ReferenceDecoder reference(dictionary);
        const auto raw = data_for(4, size);
        const auto stream = literals(raw);
        CHECK(bulk.decode(stream, raw.size()) == reference.decode(stream));
        CHECK(scalar.decode(stream, raw.size()) == raw);
        for (const auto distance : {1U, 2U, 7U, 8U, 17U, 272U, 273U, 4095U, 4096U}) {
            const auto token = match_token((reference.cursor + 4096 - distance) & 4095, 273);
            const auto expected = reference.decode(token);
            CHECK(bulk.decode(token, 273) == expected);
            CHECK(scalar.decode(token, 273) == expected);
        }
    }
}

// Exhaustive independent match oracle + elementary DAG DP. No hash chains or
// range-minimum tree, so this can catch a plausible but suboptimal parser.
std::size_t optimal_size_reference(std::span<const std::uint8_t> input,
                                   std::span<const std::uint8_t> initial = {}) {
    std::array<std::uint8_t, 4096> dictionary{};
    std::copy(initial.begin(), initial.end(), dictionary.begin());
    std::vector<std::size_t> longest(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto cap = std::min<std::size_t>(273, input.size() - i);
        for (std::size_t distance = 1; distance <= 4096 && longest[i] < cap; ++distance) {
            std::size_t length = 0;
            while (length < cap) {
                const auto expected = length < distance
                    ? dictionary[(i + 4096 - distance + length) & 4095]
                    : input[i + length - distance];
                if (input[i + length] != expected) break;
                ++length;
            }
            if (length >= 3) longest[i] = std::max(longest[i], length);
        }
        dictionary[i & 4095] = input[i];
    }
    std::vector<std::array<std::size_t, 8>> cost(input.size() + 1);
    for (auto i = input.size(); i-- != 0;) {
        for (std::size_t phase = 0; phase < 8; ++phase) {
            const auto next = (phase + 1) % 8;
            cost[i][phase] = cost[i + 1][next] + 1 + (phase == 0);
            for (std::size_t length = 3; length <= longest[i]; ++length) {
                cost[i][phase] = std::min(cost[i][phase],
                    cost[i + length][next] + (length < 18 ? 2 : 3) + (phase == 0));
            }
        }
    }
    return cost[0][0];
}

void optimal_parsing() {
    // All binary strings through length 11, including every flag phase.
    for (std::size_t size = 0; size <= 11; ++size) {
        for (std::size_t bits = 0; bits < (std::size_t{1} << size); ++bits) {
            std::vector<std::uint8_t> raw(size);
            for (std::size_t i = 0; i < size; ++i) raw[i] = static_cast<std::uint8_t>((bits >> i) & 1);
            const auto encoded = SlideEncoder({}, SlideParse::optimal).encode(raw);
            CHECK(encoded.size() == optimal_size_reference(raw));
            CHECK(ReferenceDecoder{}.decode(encoded) == raw);
        }
    }
    const auto dictionary = data_for(4, 4096);
    for (const auto size : {17U, 18U, 19U, 32U, 63U, 272U, 273U, 274U, 511U, 512U, 513U}) {
        for (unsigned kind = 0; kind < 5; ++kind) {
            const auto raw = data_for(kind, size);
            const auto optimal = SlideEncoder({}, SlideParse::optimal).encode(raw);
            CHECK(optimal.size() == optimal_size_reference(raw));
            CHECK(optimal.size() <= SlideEncoder{}.encode(raw).size());
            CHECK(SlideDecoder({}, SlideDecode::scalar).decode(optimal, raw.size()) == raw);
            CHECK(SlideDecoder{}.decode(optimal, raw.size()) == raw);
        }
        auto raw = std::vector<std::uint8_t>(dictionary.begin(), dictionary.begin() + size);
        const auto optimal = SlideEncoder(dictionary, SlideParse::optimal).encode(raw);
        CHECK(optimal.size() == optimal_size_reference(raw, dictionary));
        CHECK(ReferenceDecoder(dictionary).decode(optimal) == raw);
    }
    // Repeated zero bytes used to need an extra literal in upstream Rust.
    CHECK(SlideEncoder{}.encode(std::vector<std::uint8_t>(273)).size() == 4);
    CHECK(SlideEncoder({}, SlideParse::optimal).encode(std::vector<std::uint8_t>(273)).size() == 4);
    const std::vector<std::uint8_t> one_run(274, 'x');
    CHECK(SlideEncoder{}.encode(one_run).size() == 5);

    std::vector<std::uint8_t> greedy_trap(80);
    std::uint32_t state = 1;
    for (auto& value : greedy_trap) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        value = static_cast<std::uint8_t>(state % 3);
    }
    const auto trap_greedy = SlideEncoder{}.encode(greedy_trap);
    const auto trap_optimal = SlideEncoder({}, SlideParse::optimal).encode(greedy_trap);
    CHECK(trap_optimal.size() < trap_greedy.size());
    CHECK(trap_optimal.size() == optimal_size_reference(greedy_trap));
    CHECK(ReferenceDecoder{}.decode(trap_optimal) == greedy_trap);

    for (unsigned kind = 0; kind < 5; ++kind) {
        const auto raw = data_for(kind, 9001);
        SlideEncoder greedy(dictionary), optimal(dictionary, SlideParse::optimal);
        SlideDecoder bulk(dictionary), scalar(dictionary, SlideDecode::scalar);
        ReferenceDecoder reference(dictionary);
        for (std::size_t pos = 0; pos < raw.size();) {
            const auto size = std::min<std::size_t>(997, raw.size() - pos);
            const auto chunk = std::span(raw).subspan(pos, size);
            optimal.store();
            const auto encoded = optimal.encode(chunk);
            CHECK(encoded.size() <= greedy.encode(chunk).size());
            CHECK(std::ranges::equal(reference.decode(encoded), chunk));
            CHECK(std::ranges::equal(bulk.decode(encoded, size), chunk));
            CHECK(std::ranges::equal(scalar.decode(encoded, size), chunk));
            optimal.restore();
            CHECK(optimal.encode(chunk) == encoded);
            pos += size;
        }
    }
}

void matching_algorithms() {
    std::vector<std::uint8_t> dense(12288);
    for (std::size_t i = 0; i < dense.size(); i += 8) {
        dense[i] = 'a'; dense[i + 1] = 'b'; dense[i + 2] = 'c';
        dense[i + 3] = static_cast<std::uint8_t>((i / 8) % 128);
        dense[i + 4] = static_cast<std::uint8_t>((i / 1024) % 251);
        dense[i + 5] = 17; dense[i + 6] = 239; dense[i + 7] = 5;
    }
    const std::vector<std::uint8_t> trap{'a', 'b', 'c', 255, 'a', 'b', 'c', 255};
    std::vector<std::uint8_t> period_three(901);
    for (std::size_t i = 0; i < period_three.size(); ++i)
        period_three[i] = static_cast<std::uint8_t>('a' + i % 3);
    for (const auto parse : {SlideParse::greedy, SlideParse::optimal}) {
        for (const bool initialized : {false, true}) {
            const auto dictionary = initialized ? std::span(dense).first(4096) : std::span<const std::uint8_t>{};
            SlideEncoder primary(dictionary, parse, std::size_t{256} << 20, SlideMatch::primary);
            SlideEncoder adaptive(dictionary, parse, std::size_t{256} << 20, SlideMatch::adaptive);
            primary.store(); adaptive.store();
            CHECK(primary.encode(dense) == adaptive.encode(dense));
            CHECK(primary.encode(trap) == adaptive.encode(trap));
            CHECK(primary.encode(period_three) == adaptive.encode(period_three));
            // Restoring a pre-activation snapshot must clear stale long keys,
            // including the completely cold dictionary and wrapped cursors.
            primary.restore(); adaptive.restore();
            CHECK(primary.encode(trap) == adaptive.encode(trap));
            for (unsigned trial = 0; trial < 24; ++trial) {
                primary.store(); adaptive.store();
                const auto discarded = data_for(trial % 5, 6000, trial + 1);
                CHECK(primary.encode(discarded) == adaptive.encode(discarded));
                primary.restore(); adaptive.restore();
                const auto raw = data_for(trial % 5, 4095 + trial, trial + 19);
                const auto expected = primary.encode(raw);
                const auto actual = adaptive.encode(raw);
                CHECK(actual == expected);
                primary.restore(); adaptive.restore();
                CHECK(primary.encode(raw) == expected);
                CHECK(adaptive.encode(raw) == actual);
            }
        }
    }
    // Empty rollback uses age instead of clearing the hash table. Exercise a
    // real 31-bit rebase without allocating or pushing two gigabytes of input.
    SlideEncoder expired;
    expired.store();
    static_cast<void>(expired.encode(dense));
    static_cast<void>(expired.encode(trap));
    expired.restore();
    for (std::size_t i = 0; i < 530000; ++i) expired.restore();
    CHECK(expired.encode(period_three) == SlideEncoder({}, SlideParse::greedy,
        std::size_t{256} << 20, SlideMatch::primary).encode(period_three));
}
template<class F> void rejects(F&& action, tlg::ErrorCode expected) {
    bool rejected{};
    try { action(); }
    catch (const tlg::detail::Failure& failure) { rejected = true; CHECK(failure.error.code == expected); }
    CHECK(rejected);
}
void malformed() {
    for (const auto bytes : {std::vector<std::uint8_t>{}, {0}, {1}, {1, 0}, {1, 0, 0xf0}, {2, 'z', 0}}) {
        rejects([&] { static_cast<void>(SlideDecoder{}.decode(bytes, 5)); }, tlg::ErrorCode::truncated_input);
    }
    rejects([] { static_cast<void>(SlideDecoder{}.decode(std::array<std::uint8_t, 3>{1, 0, 0}, 2)); }, tlg::ErrorCode::corrupt_data);
    rejects([] { static_cast<void>(SlideDecoder{}.decode(std::array<std::uint8_t, 4>{1, 0, 0xf0, 255}, 272)); }, tlg::ErrorCode::corrupt_data);
    rejects([] { static_cast<void>(SlideDecoder{}.decode(std::array<std::uint8_t, 3>{0, 'a', 'b'}, 1)); }, tlg::ErrorCode::corrupt_data);
    rejects([] { static_cast<void>(SlideDecoder{}.decode(std::array<std::uint8_t, 1>{0}, 0)); }, tlg::ErrorCode::corrupt_data);
    rejects([] { SlideEncoder{}.restore(); }, tlg::ErrorCode::invalid_argument);
    rejects([] { SlideEncoder invalid(std::array<std::uint8_t, 1>{0}); }, tlg::ErrorCode::invalid_argument);
    rejects([] { SlideDecoder invalid(std::array<std::uint8_t, 1>{0}); }, tlg::ErrorCode::invalid_argument);
    rejects([] { SlideEncoder invalid({}, static_cast<SlideParse>(-1)); }, tlg::ErrorCode::invalid_argument);
    rejects([] { SlideEncoder invalid({}, SlideParse::greedy, 0, static_cast<SlideMatch>(-1)); }, tlg::ErrorCode::invalid_argument);
    rejects([] { SlideDecoder invalid({}, static_cast<SlideDecode>(-1)); }, tlg::ErrorCode::invalid_argument);
    for (const auto algorithm : {SlideDecode::bulk, SlideDecode::scalar}) {
        const auto dictionary = data_for(4, 4096);
        SlideDecoder persistent(dictionary, algorithm);
        const auto prefix = std::array<std::uint8_t, 4>{0, 'a', 'b', 'c'};
        static_cast<void>(persistent.decode(prefix, 3));
        rejects([&] { static_cast<void>(persistent.decode(std::array<std::uint8_t, 3>{0, 'z', 'x'}, 1)); }, tlg::ErrorCode::corrupt_data);
        CHECK(persistent.decode(match_token(0, 3), 3) == std::vector<std::uint8_t>({'a', 'b', 'c'}));
    }
    SlideEncoder limited({}, SlideParse::optimal, 0);
    CHECK(limited.encode({}).empty());
    rejects([&] { static_cast<void>(limited.encode(std::array<std::uint8_t, 1>{'x'})); }, tlg::ErrorCode::resource_limit);
    // The scratch limit is checked before precomputation can consume the input.
    const auto small_dictionary = data_for(4, 4096);
    constexpr auto tree_bytes = std::size_t{1024 * 16} * sizeof(std::size_t);
    SlideEncoder limited_existing(small_dictionary, SlideParse::optimal, tree_bytes + 20 * 3);
    rejects([&] { static_cast<void>(limited_existing.encode(std::vector<std::uint8_t>(4, 'x'))); }, tlg::ErrorCode::resource_limit);
    const auto three = std::span(small_dictionary).subspan(100, 3);
    CHECK(limited_existing.encode(three) == SlideEncoder(small_dictionary, SlideParse::optimal).encode(three));
    const auto raw = data_for(4, 257);
    const auto encoded = SlideEncoder{}.encode(raw);
    for (std::size_t cut = 0; cut < encoded.size(); ++cut) {
        rejects([&] { static_cast<void>(SlideDecoder{}.decode(std::span(encoded).first(cut), raw.size())); }, tlg::ErrorCode::truncated_input);
    }
}
}

int main() try {
    wire_examples();
    round_trips();
    snapshots_and_initial_dictionary();
    decoder_algorithms();
    optimal_parsing();
    matching_algorithms();
    malformed();
    std::cout << "PASS Slide wire format, streaming, dictionary, snapshots, and malformed input: " << checks << " checks\n";
    return 0;
} catch (const tlg::detail::Failure& failure) {
    std::cerr << "unexpected Slide failure: " << failure.error.message << '\n';
    return 1;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
