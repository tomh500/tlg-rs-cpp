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
