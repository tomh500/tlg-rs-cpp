#include <tlg/tlg.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::uint32_t parse_positive(std::string_view value) {
    std::uint32_t result{};
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (ec != std::errc{} || end != value.data() + value.size() || result == 0)
        throw std::runtime_error("dimensions and rounds must be positive integers");
    return result;
}
std::vector<std::uint8_t> pixels(std::uint32_t width, std::uint32_t height, std::string_view pattern) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
    std::uint32_t state = 0xc0ffee12U;
    std::size_t pos{};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                state ^= state << 13; state ^= state >> 17; state ^= state << 5;
                if (pattern == "flat") out[pos++] = c == 3 ? 255 : 127;
                else if (pattern == "gradient") out[pos++] = static_cast<std::uint8_t>(
                    c == 0 ? x : c == 1 ? y : c == 2 ? x + y : 255);
                else out[pos++] = static_cast<std::uint8_t>(state >> 24);
            }
        }
    }
    return out;
}
}

int main(int argc, char** argv) try {
    if (argc > 10) throw std::runtime_error("usage: tlg_bench [width height rounds [greedy|optimal [factored|reference|refined|compact [bulk|scalar [raw_file|- [channels [adaptive|primary]]]]]]]");
    const auto width = argc > 1 ? parse_positive(argv[1]) : 512U;
    const auto height = argc > 2 ? parse_positive(argv[2]) : 512U;
    const auto rounds = argc > 3 ? parse_positive(argv[3]) : 3U;
    tlg::EncodeOptions options;
    tlg::DecodeOptions decode_options;
    if (argc > 4) {
        const std::string_view choice = argv[4];
        if (choice == "optimal") options.slide_parse = tlg::SlideParse::optimal;
        else if (choice != "greedy") throw std::runtime_error("unknown Slide parser");
    }
    if (argc > 5) {
        const std::string_view choice = argv[5];
        if (choice == "reference") options.tlg6_filter_search = tlg::Tlg6FilterSearch::reference;
        else if (choice == "refined") options.tlg6_filter_search = tlg::Tlg6FilterSearch::refined;
        else if (choice == "compact") {
            options.tlg6_filter_search = tlg::Tlg6FilterSearch::refined;
            options.compare_slide_parses = true;
        }
        else if (choice != "factored") throw std::runtime_error("unknown filter search");
    }
    if (argc > 6) {
        const std::string_view choice = argv[6];
        if (choice == "scalar") decode_options.slide_decode = tlg::SlideDecode::scalar;
        else if (choice != "bulk") throw std::runtime_error("unknown Slide decoder");
    }
    const auto channels = argc > 8 ? parse_positive(argv[8]) : 4U;
    const bool from_file = argc > 7 && std::string_view(argv[7]) != "-";
    if (argc > 9) {
        const std::string_view choice = argv[9];
        if (choice == "primary") options.slide_match = tlg::SlideMatch::primary;
        else if (choice != "adaptive") throw std::runtime_error("unknown Slide match search");
    }
    if (channels != 1 && channels != 3 && channels != 4) throw std::runtime_error("channels must be 1, 3, or 4");
    if (static_cast<std::uint64_t>(width) * height > 268435456ULL)
        throw std::runtime_error("benchmark dimensions exceed 268435456 pixels");
    std::cout << "implementation,format,pattern,width,height,channels,rounds,encoded_bytes,encode_ms,decode_ms,encode_mib_s,decode_mib_s\n";
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    std::size_t checksum{};
    for (const std::string_view pattern : {"flat", "gradient", "random", "file"}) {
        if (from_file != (pattern == "file")) continue;
        auto raw = pattern == "file" ? std::vector<std::uint8_t>{} : pixels(width, height, pattern);
        if (pattern == "file") {
            std::ifstream input(argv[7], std::ios::binary | std::ios::ate);
            if (!input || input.tellg() != static_cast<std::streamoff>(std::uint64_t{width} * height * channels))
                throw std::runtime_error("raw input size does not match dimensions and channels");
            raw.resize(static_cast<std::size_t>(input.tellg()));
            input.seekg(0);
            input.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            if (!input) throw std::runtime_error("cannot read raw benchmark input");
        }
        const tlg::ImageView image{width, height, static_cast<tlg::PixelLayout>(pattern == "file" ? channels : 4), raw};
        for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
            options.format = format;
            auto encoded = tlg::encode(image, options);
            if (!encoded) throw std::runtime_error(encoded.error().message);
            auto decoded = tlg::decode(*encoded, decode_options);
            if (!decoded) throw std::runtime_error(decoded.error().message);
            if (decoded->image.pixels != raw) throw std::runtime_error("benchmark round trip failed");
            const auto encode_start = Clock::now();
            for (std::uint32_t i = 0; i < rounds; ++i) {
                auto output = tlg::encode(image, options);
                if (!output) throw std::runtime_error(output.error().message);
                checksum += output->size();
            }
            const auto encode_seconds = std::chrono::duration<double>(Clock::now() - encode_start).count() / rounds;
            const auto decode_start = Clock::now();
            for (std::uint32_t i = 0; i < rounds; ++i) {
                auto output = tlg::decode(*encoded, decode_options);
                if (!output) throw std::runtime_error(output.error().message);
                checksum += output->image.pixels.front();
            }
            const auto decode_seconds = std::chrono::duration<double>(Clock::now() - decode_start).count() / rounds;
            if (!(encode_seconds > 0 && decode_seconds > 0))
                throw std::runtime_error("timer resolution exceeded; increase rounds");
            const auto mib = static_cast<double>(raw.size()) / 1048576.0;
            std::cout << "cpp," << (format == tlg::Format::tlg5 ? 5 : 6) << ',' << pattern << ','
                      << width << ',' << height << ',' << tlg::channels(image.layout) << ',' << rounds << ',' << encoded->size() << ','
                      << encode_seconds * 1000.0 << ',' << decode_seconds * 1000.0 << ','
                      << mib / encode_seconds << ',' << mib / decode_seconds << '\n';
        }
    }
    // Keep every timed result observable, including in optimized and LTO builds.
    if (checksum == std::numeric_limits<std::size_t>::max()) std::cerr << checksum;
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
