#include <tlg/tlg.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

template<class T>
concept CanBorrowImage = requires(T&& image) { std::forward<T>(image).view(); };
static_assert(CanBorrowImage<tlg::Image&> && CanBorrowImage<const tlg::Image&>);
static_assert(!CanBorrowImage<tlg::Image> && !CanBorrowImage<const tlg::Image>);

namespace {
std::size_t checks{};
void require(bool condition, std::string_view expression, int line) {
    ++checks;
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + std::string(expression));
}
#define REQUIRE(expr) require(static_cast<bool>(expr), #expr, __LINE__)

std::filesystem::path source_dir() {
#ifdef TLG_TEST_SOURCE_DIR
    return TLG_TEST_SOURCE_DIR;
#else
    return std::filesystem::path(__FILE__).parent_path();
#endif
}
std::vector<std::uint8_t> load(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open fixture: " + path.string());
    return {std::istreambuf_iterator<char>(input), {}};
}
void put32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value) {
    REQUIRE(offset + 4 <= data.size());
    for (unsigned i = 0; i < 4; ++i) data[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
std::uint32_t get32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    REQUIRE(offset + 4 <= data.size());
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(data[offset + i]) << (i * 8);
    return value;
}
std::vector<std::uint8_t> pixels(std::uint32_t width, std::uint32_t height, tlg::PixelLayout layout, unsigned pattern) {
    const auto channels = static_cast<unsigned>(layout);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * channels);
    std::uint32_t state = 0xc0ffee12U;
    std::size_t pos{};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (unsigned c = 0; c < channels; ++c) {
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                switch (pattern) {
                case 0: out[pos++] = 0; break;
                case 1: out[pos++] = c == 3 ? 255 : 127; break;
                case 2: out[pos++] = static_cast<std::uint8_t>(c == 0 ? x : c == 1 ? y : c == 2 ? x + y : 255); break;
                case 3: out[pos++] = static_cast<std::uint8_t>(state >> 24); break;
                default: out[pos++] = ((x ^ y) & 1U) ? 255 : 0; break;
                }
            }
        }
    }
    return out;
}
void round_trip(std::uint32_t width, std::uint32_t height, tlg::PixelLayout layout, tlg::Format format, unsigned pattern) {
    const auto raw = pixels(width, height, layout, pattern);
    const tlg::ImageView image{width, height, layout, raw};
    const auto encoded = tlg::encode(image, tlg::EncodeOptions{format});
    REQUIRE(encoded);
    const auto decoded = tlg::decode(*encoded);
    REQUIRE(decoded);
    REQUIRE(decoded->image.width == width);
    REQUIRE(decoded->image.height == height);
    REQUIRE(decoded->image.layout == layout);
    REQUIRE(decoded->image.pixels == raw);
    REQUIRE(decoded->tags.empty());
    REQUIRE(decoded->format == format);
    REQUIRE(tlg::detect_format(*encoded) == format);
    REQUIRE(tlg::detect_format(std::span(*encoded).first(11)) == format);
    REQUIRE(decoded->image.view().pixels.size() == raw.size());
    const auto reencoded = tlg::encode(decoded->image.view(), tlg::EncodeOptions{format});
    REQUIRE(reencoded);
    REQUIRE(*reencoded == *encoded);
}
void round_trips() {
    constexpr std::array dimensions{1U, 2U, 3U, 7U, 8U, 9U, 15U, 16U, 17U, 31U, 32U, 33U};
    for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
        for (const auto layout : {tlg::PixelLayout::gray, tlg::PixelLayout::rgb, tlg::PixelLayout::rgba}) {
            for (const auto width : dimensions) {
                for (const auto height : dimensions) round_trip(width, height, layout, format, 3);
            }
            for (unsigned pattern = 0; pattern < 5; ++pattern) {
                round_trip(1, 129, layout, format, pattern);
                round_trip(129, 1, layout, format, pattern);
                round_trip(65, 73, layout, format, pattern);
            }
        }
    }
}
void algorithm_choices() {
    // Comparing two implementations catches mistakes that a round trip can
    // happily hide. Both sides agreeing on the same bug would be awkward.
    constexpr std::array dimensions{1U, 3U, 7U, 8U, 9U, 17U, 33U};
    for (const auto layout : {tlg::PixelLayout::gray, tlg::PixelLayout::rgb, tlg::PixelLayout::rgba}) {
        for (const auto width : dimensions) {
            for (const auto height : dimensions) {
                for (unsigned pattern = 0; pattern < 5; ++pattern) {
                    const auto raw = pixels(width, height, layout, pattern);
                    const tlg::ImageView image{width, height, layout, raw};
                    tlg::EncodeOptions factored{tlg::Format::tlg6};
                    auto reference = factored;
                    reference.tlg6_filter_search = tlg::Tlg6FilterSearch::reference;
                    const auto a = tlg::encode(image, factored);
                    const auto b = tlg::encode(image, reference);
                    REQUIRE(a && b);
                    REQUIRE(*a == *b);
                    for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
                        factored.format = format;
                        const auto greedy = tlg::encode(image, factored);
                        auto options = factored;
                        options.slide_parse = tlg::SlideParse::optimal;
                        const auto optimal = tlg::encode(image, options);
                        REQUIRE(greedy && optimal);
                        if (format == tlg::Format::tlg6) REQUIRE(optimal->size() <= greedy->size());
                        // TLG5 store/restore decisions can change later dictionaries;
                        // the per-chunk optimum is not a whole-file size theorem.
                        for (const auto mode : {tlg::SlideDecode::bulk, tlg::SlideDecode::scalar}) {
                            tlg::DecodeOptions decode_options;
                            decode_options.slide_decode = mode;
                            const auto decoded = tlg::decode(*optimal, decode_options);
                            REQUIRE(decoded);
                            REQUIRE(decoded->image.pixels == raw);
                        }
                    }
                }
            }
        }
    }
    const auto raw = pixels(33, 33, tlg::PixelLayout::rgba, 3);
    const tlg::ImageView image{33, 33, tlg::PixelLayout::rgba, raw};
    tlg::EncodeOptions options;
    options.slide_parse = static_cast<tlg::SlideParse>(99);
    auto invalid = tlg::encode(image, options);
    REQUIRE(!invalid && invalid.error().code == tlg::ErrorCode::invalid_argument);
    options = {};
    options.tlg6_filter_search = static_cast<tlg::Tlg6FilterSearch>(99);
    invalid = tlg::encode(image, options);
    REQUIRE(!invalid && invalid.error().code == tlg::ErrorCode::invalid_argument);
    options = {};
    options.slide_match = static_cast<tlg::SlideMatch>(99);
    invalid = tlg::encode(image, options);
    REQUIRE(!invalid && invalid.error().code == tlg::ErrorCode::invalid_argument);
    options = {};
    options.slide_parse = tlg::SlideParse::optimal;
    options.max_slide_working_bytes = 1;
    invalid = tlg::encode(image, options);
    REQUIRE(!invalid && invalid.error().code == tlg::ErrorCode::resource_limit);
    const auto encoded = tlg::encode(image);
    REQUIRE(encoded);
    tlg::DecodeOptions decode_options;
    decode_options.slide_decode = static_cast<tlg::SlideDecode>(99);
    const auto bad_decode = tlg::decode(*encoded, decode_options);
    REQUIRE(!bad_decode && bad_decode.error().code == tlg::ErrorCode::invalid_argument);
}
void metadata() {
    const tlg::Tags tags{{"author", "\xe8\xbd\xa9\xe9\x85\xb1"},
        {"a:=,\xe6\x96\x87", "value,:=\n\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x99\x82"},
        {"", ""}, {"binary", std::string("a\0b,:=", 6)}};
    const auto raw = pixels(17, 9, tlg::PixelLayout::rgba, 2);
    for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
        const tlg::ImageView image{17, 9, tlg::PixelLayout::rgba, raw};
        auto encoded = tlg::encode(image, tlg::EncodeOptions{format}, tags);
        REQUIRE(encoded);
        const auto decoded = tlg::decode(*encoded);
        REQUIRE(decoded);
        REQUIRE(decoded->image.pixels == raw);
        REQUIRE(decoded->tags == tags);
        REQUIRE(decoded->format == format);
        REQUIRE(tlg::detect_format(*encoded) == format);
        REQUIRE(tlg::detect_format(std::span(*encoded).first(26)) == format);
        for (std::size_t n = 0; n < 26; ++n)
            REQUIRE(!tlg::detect_format(std::span(*encoded).first(n)));
        auto invalid_probe = *encoded;
        put32(invalid_probe, 11, 10);
        const auto bad_probe = tlg::detect_format(invalid_probe);
        REQUIRE(!bad_probe && bad_probe.error().code == tlg::ErrorCode::corrupt_data);
        const auto repeated = tlg::encode(image, tlg::EncodeOptions{format}, tags);
        REQUIRE(repeated);
        REQUIRE(*repeated == *encoded);
        const auto tag_start = 15 + get32(*encoded, 11);
        REQUIRE(tag_start + 8 < encoded->size());
        auto malformed = *encoded;
        malformed[tag_start + 8] = 'x';
        REQUIRE(!tlg::decode(malformed));
        malformed = *encoded;
        put32(malformed, tag_start + 4, std::numeric_limits<std::uint32_t>::max());
        REQUIRE(!tlg::decode(malformed));
        auto limit = tlg::DecodeOptions{};
        limit.max_metadata_bytes = 1;
        auto limited = tlg::decode(*encoded, limit);
        REQUIRE(!limited);
        REQUIRE(limited.error().code == tlg::ErrorCode::resource_limit);
    }
}
void invalid_arguments() {
    const std::vector<std::uint8_t> raw(4);
    for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
        const tlg::EncodeOptions options{format};
        for (const auto image : {
            tlg::ImageView{0, 1, tlg::PixelLayout::rgba, {}},
            tlg::ImageView{1, 0, tlg::PixelLayout::rgba, {}},
            tlg::ImageView{1, 1, tlg::PixelLayout::rgb, raw},
            tlg::ImageView{2, 1, tlg::PixelLayout::rgba, raw},
            tlg::ImageView{1, 1, static_cast<tlg::PixelLayout>(2), raw},
            tlg::ImageView{std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), tlg::PixelLayout::rgba, raw}}) {
            auto result = tlg::encode(image, options);
            REQUIRE(!result);
            REQUIRE(result.error().code == tlg::ErrorCode::invalid_argument || result.error().code == tlg::ErrorCode::resource_limit);
        }
        auto limited_options = options;
        limited_options.max_pixels = 0;
        auto limited = tlg::encode(tlg::ImageView{1, 1, tlg::PixelLayout::rgba, raw}, limited_options);
        REQUIRE(!limited);
        REQUIRE(limited.error().code == tlg::ErrorCode::resource_limit);
        limited_options = options;
        limited_options.max_pixel_bytes = 3;
        limited = tlg::encode(tlg::ImageView{1, 1, tlg::PixelLayout::rgba, raw}, limited_options);
        REQUIRE(!limited);
        REQUIRE(limited.error().code == tlg::ErrorCode::resource_limit);
        limited_options.max_pixels = 1;
        limited_options.max_pixel_bytes = 4;
        REQUIRE(tlg::encode(tlg::ImageView{1, 1, tlg::PixelLayout::rgba, raw}, limited_options));
    }
    REQUIRE(!tlg::encode(tlg::ImageView{1, 1, tlg::PixelLayout::rgba, raw},
                        tlg::EncodeOptions{static_cast<tlg::Format>(99)}));
    REQUIRE(!tlg::decode({}));
    REQUIRE(!tlg::decode(raw));
}
void malformed_and_limits() {
    const auto raw = pixels(9, 9, tlg::PixelLayout::rgb, 2);
    const tlg::ImageView image{9, 9, tlg::PixelLayout::rgb, raw};
    for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
        const auto encoded = tlg::encode(image, tlg::EncodeOptions{format});
        REQUIRE(encoded);
        for (std::size_t length = 0; length < encoded->size(); ++length) {
            auto decoded = tlg::decode(std::span(*encoded).first(length));
            REQUIRE(!decoded);
            REQUIRE(decoded.error().offset <= length);
        }
        auto bad = *encoded; bad[0] ^= 0xff;
        REQUIRE(!tlg::decode(bad));
        bad = *encoded; bad[11] = 2;
        REQUIRE(!tlg::decode(bad));
        const auto width_offset = format == tlg::Format::tlg5 ? 12U : 15U;
        const auto height_offset = format == tlg::Format::tlg5 ? 16U : 19U;
        bad = *encoded; put32(bad, width_offset, 0);
        REQUIRE(!tlg::decode(bad));
        bad = *encoded; put32(bad, height_offset, 0);
        REQUIRE(!tlg::decode(bad));
        bad = *encoded; put32(bad, width_offset, std::numeric_limits<std::uint32_t>::max());
        auto huge = tlg::decode(bad);
        REQUIRE(!huge);
        REQUIRE(huge.error().code == tlg::ErrorCode::resource_limit);
        bad = *encoded; bad.push_back(0x42);
        REQUIRE(!tlg::decode(bad));
        auto relaxed = tlg::DecodeOptions{}; relaxed.reject_trailing_data = false;
        const auto trailing = tlg::decode(bad, relaxed);
        REQUIRE(trailing);
        REQUIRE(trailing->image.pixels == raw);
        for (unsigned kind = 0; kind < 3; ++kind) {
            auto limits = tlg::DecodeOptions{};
            if (kind == 0) limits.max_pixels = 80;
            if (kind == 1) limits.max_decoded_bytes = raw.size() - 1;
            if (kind == 2) limits.max_input_bytes = encoded->size() - 1;
            auto limited = tlg::decode(*encoded, limits);
            REQUIRE(!limited);
            REQUIRE(limited.error().code == tlg::ErrorCode::resource_limit);
        }
        auto exact_limits = tlg::DecodeOptions{};
        exact_limits.max_pixels = 81;
        exact_limits.max_decoded_bytes = raw.size();
        exact_limits.max_input_bytes = encoded->size();
        REQUIRE(tlg::decode(*encoded, exact_limits));
        if (format == tlg::Format::tlg5) {
            bad = *encoded; put32(bad, 20, 0);
            REQUIRE(!tlg::decode(bad));
            bad = *encoded; put32(bad, 24, std::numeric_limits<std::uint32_t>::max());
            REQUIRE(!tlg::decode(bad));
        } else {
            for (const auto flag : {12U, 13U, 14U}) {
                bad = *encoded; bad[flag] = 1;
                REQUIRE(!tlg::decode(bad));
            }
            bad = *encoded; put32(bad, 27, std::numeric_limits<std::uint32_t>::max());
            REQUIRE(!tlg::decode(bad));
            bad = *encoded; put32(bad, 23, 0);
            REQUIRE(!tlg::decode(bad));
        }
        const auto wrapped = tlg::encode(image, tlg::EncodeOptions{format}, tlg::Tags{{"key", "value"}});
        REQUIRE(wrapped);
        const auto empty_sds_size = 15 + get32(*wrapped, 11);
        // SDS with a complete image and no chunks is itself a valid document.
        for (std::size_t length = 0; length < wrapped->size(); ++length) {
            const auto prefix = tlg::decode(std::span(*wrapped).first(length));
            if (length == empty_sds_size) {
                REQUIRE(prefix);
                REQUIRE(prefix->image.pixels == raw);
                REQUIRE(prefix->tags.empty());
            } else REQUIRE(!prefix);
        }
        bad = *wrapped; put32(bad, 11, std::numeric_limits<std::uint32_t>::max());
        REQUIRE(!tlg::decode(bad));
    }
}
void tlg5_wire_examples() {
    // Handwritten uncompressed Gray stream: three vertical residuals 5,4,-7
    // reconstruct pixels 5,9,2 independently of our encoder's block height.
    const std::array<std::uint8_t, 3> residuals{5, 4, 249};
    for (const auto block_height : {1U, 2U, 3U, 99U}) {
        std::vector<std::uint8_t> wire{'T', 'L', 'G', '5', '.', '0', 0, 'r', 'a', 'w', 0x1a, 1};
        const auto append32 = [&wire](std::uint32_t value) {
            for (unsigned byte = 0; byte < 4; ++byte) wire.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
        };
        append32(1); append32(3); append32(block_height);
        for (unsigned y = 0; y < 3; y += block_height) append32(5 + std::min(block_height, 3 - y));
        for (unsigned y = 0; y < 3; y += block_height) {
            const auto count = std::min(block_height, 3 - y);
            wire.push_back(1); append32(count);
            for (unsigned offset = 0; offset < count; ++offset) wire.push_back(residuals[y + offset]);
        }
        const auto decoded = tlg::decode(wire);
        REQUIRE(decoded);
        REQUIRE(decoded->image.width == 1);
        REQUIRE(decoded->image.height == 3);
        REQUIRE(decoded->image.layout == tlg::PixelLayout::gray);
        REQUIRE(decoded->image.pixels == std::vector<std::uint8_t>({5, 9, 2}));
    }
    // Wider rows verify both horizontal and vertical differences; height 9
    // produces partial tail blocks for block heights 7 and 99.
    const std::vector<std::uint8_t> gray{
        5, 23, 91, 8, 29, 93, 12, 37, 97,
        19, 43, 101, 22, 47, 107, 25, 53, 113,
        31, 61, 127, 37, 67, 139, 41, 71, 149};
    std::vector<std::uint8_t> differences(gray.size());
    for (unsigned y = 0; y < 9; ++y) {
        std::uint8_t previous{};
        for (unsigned x = 0; x < 3; ++x) {
            const auto pos = static_cast<std::size_t>(y) * 3 + x;
            const auto vertical = static_cast<std::uint8_t>(gray[pos] - (y == 0 ? 0 : gray[pos - 3]));
            differences[pos] = static_cast<std::uint8_t>(vertical - previous);
            previous = vertical;
        }
    }
    for (const auto block_height : {1U, 3U, 7U, 9U, 99U}) {
        std::vector<std::uint8_t> wire{'T', 'L', 'G', '5', '.', '0', 0, 'r', 'a', 'w', 0x1a, 1};
        const auto append32 = [&wire](std::uint32_t value) {
            for (unsigned byte = 0; byte < 4; ++byte) wire.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
        };
        append32(3); append32(9); append32(block_height);
        for (unsigned y = 0; y < 9; y += block_height) append32(5 + 3 * std::min(block_height, 9 - y));
        for (unsigned y = 0; y < 9; y += block_height) {
            const auto count = 3 * std::min(block_height, 9 - y);
            wire.push_back(1); append32(count);
            const auto pos = static_cast<std::size_t>(y) * 3;
            wire.insert(wire.end(), differences.begin() + pos, differences.begin() + pos + count);
        }
        const auto decoded = tlg::decode(wire);
        REQUIRE(decoded);
        REQUIRE(decoded->image.width == 3);
        REQUIRE(decoded->image.height == 9);
        REQUIRE(decoded->image.layout == tlg::PixelLayout::gray);
        REQUIRE(decoded->image.pixels == gray);
    }
}
void mutation_sweep() {
    auto limits = tlg::DecodeOptions{};
    limits.max_pixels = 4096;
    limits.max_decoded_bytes = 16384;
    limits.max_input_bytes = 16384;
    limits.max_metadata_bytes = 4096;
    std::uint32_t state = 0x13579bdfU;
    const auto next = [&state] {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return state;
    };
    const auto raw = pixels(17, 9, tlg::PixelLayout::rgba, 3);
    const tlg::ImageView image{17, 9, tlg::PixelLayout::rgba, raw};
    for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
        const auto encoded = tlg::encode(image, tlg::EncodeOptions{format}, tlg::Tags{{"key", "value"}});
        REQUIRE(encoded);
        for (unsigned attempt = 0; attempt < 2000; ++attempt) {
            auto mutated = *encoded;
            const auto modifications = 1 + next() % 4;
            for (unsigned i = 0; i < modifications; ++i) mutated[next() % mutated.size()] ^= static_cast<std::uint8_t>(1U << (next() % 8));
            const auto decoded = tlg::decode(mutated, limits);
            if (decoded) {
                REQUIRE(decoded->image.width > 0);
                REQUIRE(decoded->image.height > 0);
                REQUIRE(decoded->image.pixels.size() == static_cast<std::size_t>(decoded->image.width) * decoded->image.height * tlg::channels(decoded->image.layout));
            } else {
                REQUIRE(!decoded.error().message.empty());
                REQUIRE(decoded.error().offset <= mutated.size());
            }
        }
    }
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        std::vector<std::uint8_t> garbage(next() % 128);
        for (auto& byte : garbage) byte = static_cast<std::uint8_t>(next());
        REQUIRE(!tlg::decode(garbage, limits));
    }
}
void rust_fixtures() {
    struct Fixture { const char* name; std::uint32_t width; std::uint32_t height; tlg::PixelLayout layout; };
    constexpr Fixture fixtures[]{
        {"gray_1x1_zero", 1, 1, tlg::PixelLayout::gray},
        {"gray_19x13_random", 19, 13, tlg::PixelLayout::gray},
        {"rgb_9x11_gradient", 9, 11, tlg::PixelLayout::rgb},
        {"rgb_32x24_flat", 32, 24, tlg::PixelLayout::rgb},
        {"rgba_17x9_random", 17, 9, tlg::PixelLayout::rgba},
        {"rgba_32x24_gradient", 32, 24, tlg::PixelLayout::rgba},
    };
    const auto dir = source_dir() / "fixtures";
    for (const auto& fixture : fixtures) {
        const auto raw = load(dir / (std::string(fixture.name) + ".raw"));
        for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
            const auto encoded = load(dir / (std::string(fixture.name) + (format == tlg::Format::tlg5 ? ".tlg5" : ".tlg6")));
            const auto decoded = tlg::decode(encoded);
            REQUIRE(decoded);
            REQUIRE(decoded->image.width == fixture.width);
            REQUIRE(decoded->image.height == fixture.height);
            REQUIRE(decoded->image.layout == fixture.layout);
            REQUIRE(decoded->image.pixels == raw);
            REQUIRE(decoded->format == format);
        }
    }
    const tlg::Tags tags{{"author", "\xe8\xbd\xa9\xe9\x85\xb1"},
        {"a:=,\xe6\x96\x87", "value,:=\n\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x99\x82"}, {"", ""}};
    for (const auto* name : {"metadata.sds5", "metadata.sds6"}) {
        const auto decoded = tlg::decode(load(dir / name));
        REQUIRE(decoded);
        REQUIRE(decoded->image.pixels == load(dir / "rgb_9x11_gradient.raw"));
        REQUIRE(decoded->tags == tags);
    }
    const auto bundle = load(dir / "tlg6_filters.bin");
    const std::array<std::uint8_t, 8> bundle_magic{'T', 'L', 'G', '6', 'O', 'R', 'C', 'L'};
    REQUIRE(bundle.size() >= 18);
    REQUIRE(std::ranges::equal(std::span(bundle).first(8), bundle_magic));
    REQUIRE(get32(bundle, 8) == 3);
    REQUIRE(get32(bundle, 12) == 3);
    REQUIRE(bundle[16] == 4);
    REQUIRE(bundle[17] == 32);
    std::size_t cursor = 18;
    for (unsigned code = 0; code < 32; ++code) {
        REQUIRE(cursor < bundle.size());
        REQUIRE(bundle[cursor++] == code);
        const auto encoded_length = get32(bundle, cursor); cursor += 4;
        const auto pixel_length = get32(bundle, cursor); cursor += 4;
        REQUIRE(pixel_length == 36);
        REQUIRE(encoded_length <= bundle.size() - cursor);
        const auto wire = std::span(bundle).subspan(cursor, encoded_length); cursor += encoded_length;
        REQUIRE(pixel_length <= bundle.size() - cursor);
        const auto expected = std::span(bundle).subspan(cursor, pixel_length); cursor += pixel_length;
        const auto decoded = tlg::decode(wire);
        REQUIRE(decoded);
        REQUIRE(decoded->image.width == 3);
        REQUIRE(decoded->image.height == 3);
        REQUIRE(decoded->image.layout == tlg::PixelLayout::rgba);
        REQUIRE(std::ranges::equal(decoded->image.pixels, expected));
        REQUIRE(decoded->format == tlg::Format::tlg6);
    }
    REQUIRE(cursor == bundle.size());
}
void file_io() {
    const auto raw = pixels(17, 9, tlg::PixelLayout::rgba, 3);
    const tlg::ImageView image{17, 9, tlg::PixelLayout::rgba, raw};
    const auto unique = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() / ("tlg_cpp_test_" + unique);
    REQUIRE(std::filesystem::create_directory(directory));
    const auto path = directory / "image.tlg";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(path.parent_path(), ignored);
        }
    } cleanup{path};
    for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
        REQUIRE(tlg::write_file(path, image, tlg::EncodeOptions{format}, tlg::Tags{{"key", "value"}}));
        const auto decoded = tlg::read_file(path);
        REQUIRE(decoded);
        REQUIRE(decoded->image.pixels == raw);
        REQUIRE(decoded->tags == tlg::Tags({{"key", "value"}}));
        REQUIRE(decoded->format == format);
        const auto original_file = load(path);
        const auto invalid_image = tlg::ImageView{16, 9, tlg::PixelLayout::rgba, raw};
        REQUIRE(!tlg::write_file(path, invalid_image));
        REQUIRE(load(path) == original_file);
    }
    auto missing = tlg::read_file(directory / "nonexistent" / "missing.tlg");
    REQUIRE(!missing);
    REQUIRE(missing.error().code == tlg::ErrorCode::io_error);
    auto unwritable = tlg::write_file(directory / "nonexistent" / "image.tlg", image);
    REQUIRE(!unwritable);
    REQUIRE(unwritable.error().code == tlg::ErrorCode::io_error);
}
}

int main() {
    const std::array<std::pair<std::string_view, void(*)()>, 9> tests{{
        {"round trips across block boundaries", round_trips}, {"length-prefixed deterministic metadata", metadata},
        {"exact filter factoring and selectable Slide algorithms", algorithm_choices},
        {"invalid encoder arguments", invalid_arguments}, {"malformed streams, truncation, and resource limits", malformed_and_limits},
        {"handwritten TLG5 variable block heights", tlg5_wire_examples}, {"bounded deterministic mutation sweep", mutation_sweep},
        {"immutable Rust interoperability fixtures", rust_fixtures}, {"file I/O", file_io}}};
    unsigned failures{};
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << checks << " checks, " << failures << " failing groups\n";
    return failures == 0 ? 0 : 1;
}
