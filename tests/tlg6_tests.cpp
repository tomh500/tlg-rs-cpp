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

} // namespace

int main() try {
    residual_differential();
    arbitrary_bit_streams();
    std::cout << "PASS 100000 residual blocks, 800000 exact Rice/writer/reader comparisons, "
                 "300000 filter comparisons, 2000 complete color coefficient checks, "
                 "10000 arbitrary bit streams\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
}
