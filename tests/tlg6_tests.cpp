// Keep the production helpers in this translation unit so their private state
// can be checked against deliberately simple oracles. This target owns its
// TLG6 implementation; it must not also compile src/tlg6.cpp separately.
#include "../src/tlg6.cpp"

#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message, unsigned attempt) {
    if (!condition)
        throw std::runtime_error(std::string(message) + " at deterministic case " + std::to_string(attempt));
}

// Intentionally one bit at a time. The oracle earns no points for speed.
struct ReferenceBits {
    std::size_t position = 0;
    std::vector<std::uint8_t> data;

    void put(std::uint32_t value, unsigned count) {
        data.resize((position + count + 7) / 8, 0);
        for (unsigned i = 0; i < count; ++i) {
            data[position / 8] |= static_cast<std::uint8_t>(((value >> i) & 1) << (position & 7));
            ++position;
        }
    }
};

// An independent algebraic description checks the factoring table against
// the sequential production filters, including modulo-256 wraparound.
constexpr std::array<std::array<int, 3>, tlg::detail::expression_count> coefficients{{
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
    {1, -1, 0}, {0, -1, 1}, {-1, 1, 0}, {0, 1, -1},
    {1, 0, -1}, {-1, 0, 1}, {1, 1, -1}, {-1, 1, 1}, {1, -1, 1},
    {-2, 0, 1}, {-2, 1, 0}
}};

void check_coefficients(const tlg::detail::Block& block, unsigned attempt) {
    for (unsigned code = 0; code < 16; ++code) {
        auto transformed = block;
        tlg::detail::filter_block(transformed, 64, code);
        for (unsigned c = 0; c < 3; ++c) {
            const auto& terms = coefficients[tlg::detail::filter_expressions[code][c]];
            for (std::size_t i = 0; i < 64; ++i) {
                const auto expected = static_cast<std::uint8_t>(
                    terms[0] * block[0][i] + terms[1] * block[1][i] + terms[2] * block[2][i]);
                check(transformed[c][i] == expected, "factored color coefficients differ", attempt);
            }
        }
    }
}

void residual_differential() {
    std::mt19937 random(0x837245aU);
    for (unsigned attempt = 0; attempt < 100'000; ++attempt) {
        const auto count = std::size_t{1 + random() % 64};
        std::array<tlg::detail::Block, 2> blocks;
        for (auto& block : blocks) {
            for (auto& channel : block) {
                for (auto& byte : channel)
                    byte = random() % 10 < attempt % 11 ? 0 : static_cast<std::uint8_t>(random());
                const auto values = std::span{channel}.first(count);
                check(tlg::detail::channel_cost(values) == tlg::detail::reference_channel_cost(values),
                      "exact Rice cost differs", attempt);
                tlg::detail::BitWriter output;
                ReferenceBits reference;
                tlg::detail::encode_channel(output, values);
                tlg::detail::encode_channel(reference, values);
                output.finish();
                check(output.position == reference.position && output.data == reference.data,
                      "Rice writer bits differ", attempt);
                std::array<std::uint8_t, 64> restored;
                tlg::detail::BitReader input(output.data, static_cast<std::uint32_t>(output.position), 0);
                tlg::detail::decode_channel(input, std::span{restored}.first(count));
                check(std::ranges::equal(values, std::span{restored}.first(count)),
                      "Rice round trip differs", attempt);
            }
            if (attempt < 1'000) check_coefficients(block, attempt);
        }
        for (const unsigned colors : {1U, 3U, 4U})
            check(tlg::detail::choose_filter_factored(blocks, count, colors)
                    == tlg::detail::choose_filter_reference(blocks, count, colors),
                  "selected filter differs", attempt);
    }
}

void arbitrary_bit_streams() {
    std::mt19937 random(0x5591726U);
    for (unsigned attempt = 0; attempt < 10'000; ++attempt) {
        tlg::detail::BitWriter output;
        ReferenceBits reference;
        for (unsigned field = 0; field < 100; ++field) {
            const auto value = random();
            const unsigned count = random() % 33;
            output.put(value, count);
            reference.put(value, count);
        }
        output.finish();
        check(output.position == reference.position && output.data == reference.data,
              "arbitrary writer bits differ", attempt);
        const auto completed = output.data;
        output.finish();
        check(output.data == completed, "finishing twice appends extra bytes", attempt);
    }
}

void packed_prediction_regression() {
    // v1.1.0 fixes unsigned subtraction in Rust's packed byte addition.
    // Compare all byte pairs in every lane, with carries across neighboring
    // lanes, then compare both production predictors against scalar channels.
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned b = 0; b < 256; ++b) {
            const auto left = std::uint32_t{a} * 0x01010101u;
            const auto right = std::uint32_t{b} * 0x01010101u;
            const auto expected = std::uint32_t{(a + b) & 255} * 0x01010101u;
            check(tlg::detail::packed_add(left, right) == expected,
                  "packed modular addition differs", a * 256 + b);
        }
    }
    std::mt19937 random(0x1102026u);
    for (unsigned attempt = 0; attempt < 100'000; ++attempt) {
        const auto left = static_cast<std::uint32_t>(random());
        const auto above = static_cast<std::uint32_t>(random());
        const auto diagonal = static_cast<std::uint32_t>(random());
        const auto residual = static_cast<std::uint32_t>(random());
        for (const bool avg : {false, true}) {
            std::uint32_t expected = 0;
            for (unsigned shift = 0; shift < 32; shift += 8) {
                const auto a = static_cast<std::uint8_t>(left >> shift);
                const auto b = static_cast<std::uint8_t>(above >> shift);
                const auto c = static_cast<std::uint8_t>(diagonal >> shift);
                const auto prediction = avg ? tlg::detail::avg(a, b) : tlg::detail::med(a, b, c);
                const auto value = static_cast<std::uint8_t>(prediction + (residual >> shift));
                expected |= std::uint32_t{value} << shift;
            }
            check(tlg::detail::predict_add(left, above, diagonal, residual, avg) == expected,
                  "packed prediction and modular addition differ", attempt);
        }
    }
}

void stripe_objective_regression() {
    std::mt19937 random(0x600110u);
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        const auto width = static_cast<std::uint32_t>(1 + random() % 99);
        const auto height = static_cast<std::uint32_t>(1 + random() % 25);
        const unsigned colors = attempt % 3 == 0 ? 1 : attempt % 3 == 1 ? 3 : 4;
        const auto bx = (std::size_t{width} + 7) / 8;
        const auto by = (std::size_t{height} + 7) / 8;
        std::vector<std::uint8_t> raw(std::size_t{width} * height * colors);
        for (auto& byte : raw) byte = static_cast<std::uint8_t>(random() % (attempt % 2 == 0 ? 4 : 256));
        const tlg::ImageView image{width, height, static_cast<tlg::PixelLayout>(colors), raw};
        std::vector<std::uint8_t> filters(bx * by);
        for (auto& type : filters) type = static_cast<std::uint8_t>(random() % (colors == 1 ? 2 : 32));
        const auto original = filters;
        auto work = std::size_t{64} << 20;
        static_cast<void>(tlg::detail::refine_uniform_filters(image, filters, work));
        static_cast<void>(tlg::detail::refine_local_filters(image, filters, work));
        for (std::size_t y = 0; y < height; y += 8) {
            const auto bh = std::min<std::size_t>(8, height - y);
            // Serialize complete stripes into an independent one-bit writer.
            // This oracle does not call either of the production cost counters.
            const auto score = [&](std::span<const std::uint8_t> types) {
                std::array<std::vector<std::uint8_t>, 4> stripe;
                for (unsigned c = 0; c < colors; ++c) stripe[c].resize(width * bh);
                for (std::size_t x = 0; x < width; x += 8) {
                    const auto count = std::min<std::size_t>(8, width - x) * bh;
                    const auto type = types[x / 8];
                    auto block = tlg::detail::predict_block(image, x, y)[type & 1];
                    tlg::detail::filter_block(block, count, type >> 1);
                    for (unsigned c = 0; c < colors; ++c)
                        std::copy_n(block[c].begin(), count, stripe[c].begin() + x * bh);
                }
                std::pair<std::size_t, std::size_t> cost{};
                for (unsigned c = 0; c < colors; ++c) {
                    ReferenceBits bits;
                    tlg::detail::encode_channel(bits, stripe[c]);
                    cost.first += bits.data.size();
                    cost.second += bits.position;
                }
                return cost;
            };
            const auto result = score(std::span{filters}.subspan((y / 8) * bx, bx));
            check(result <= score(std::span{original}.subspan((y / 8) * bx, bx)),
                  "refinement increased a stripe's serialized cost", attempt);
            for (unsigned type = 0; type < (colors == 1 ? 2u : 32u); ++type) {
                const std::vector<std::uint8_t> uniform(bx, static_cast<std::uint8_t>(type));
                check(result <= score(uniform), "refinement missed a better uniform stripe", attempt);
            }
        }
    }
}

} // namespace

int main() try {
    residual_differential();
    arbitrary_bit_streams();
    packed_prediction_regression();
    stripe_objective_regression();
    std::cout << "PASS 100000 residual blocks, 800000 exact Rice/writer/reader comparisons, "
                 "300000 filter comparisons, 2000 complete color coefficient checks, "
                 "10000 arbitrary bit streams, 65536 modular additions, 200000 packed predictors, "
                 "30 exhaustive stripe-objective images\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
