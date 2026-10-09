#include <tlg/tlg.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
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
    if (argc > 4) throw std::runtime_error("usage: tlg_bench [width height rounds]");
    const auto width = argc > 1 ? parse_positive(argv[1]) : 512U;
    const auto height = argc > 2 ? parse_positive(argv[2]) : 512U;
    const auto rounds = argc > 3 ? parse_positive(argv[3]) : 3U;
    if (static_cast<std::uint64_t>(width) * height > 268435456ULL)
        throw std::runtime_error("benchmark dimensions exceed 268435456 pixels");
    std::cout << "implementation,format,pattern,width,height,channels,rounds,encoded_bytes,encode_ms,decode_ms,encode_mib_s,decode_mib_s\n";
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
    std::size_t checksum{};
    for (const std::string_view pattern : {"flat", "gradient", "random"}) {
        const auto raw = pixels(width, height, pattern);
        const tlg::ImageView image{width, height, tlg::PixelLayout::rgba, raw};
        for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
            const tlg::EncodeOptions options{format};
            auto encoded = tlg::encode(image, options);
            if (!encoded) throw std::runtime_error(encoded.error().message);
            auto decoded = tlg::decode(*encoded);
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
                auto output = tlg::decode(*encoded);
                if (!output) throw std::runtime_error(output.error().message);
                checksum += output->image.pixels.front();
            }
            const auto decode_seconds = std::chrono::duration<double>(Clock::now() - decode_start).count() / rounds;
            if (!(encode_seconds > 0 && decode_seconds > 0))
                throw std::runtime_error("timer resolution exceeded; increase rounds");
            const auto mib = static_cast<double>(raw.size()) / 1048576.0;
            std::cout << "cpp," << (format == tlg::Format::tlg5 ? 5 : 6) << ',' << pattern << ','
                      << width << ',' << height << ",4," << rounds << ',' << encoded->size() << ','
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
