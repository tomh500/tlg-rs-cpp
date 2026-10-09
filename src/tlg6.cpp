#include "internal.hpp"
#include "slide.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>

#if !defined(TLG_FORCE_SCALAR) && (defined(_M_X64) || defined(__SSE2__))
#include <emmintrin.h>
#define TLG6_HAS_SSE2 1
#endif

namespace tlg::detail {
namespace {

constexpr std::uint32_t bit_length_mask = 0x3fffffff;

// The adaptive Rice parameter is part of the TLG6 wire format. Each column
// expands to 1024 entries; a is halved after every four nonzero residuals.
constexpr auto golomb_table = [] {
    constexpr int lengths[4][9] = {
        {3, 7, 15, 27, 63, 108, 223, 448, 130},
        {3, 5, 13, 24, 51, 95, 192, 384, 257},
        {2, 5, 12, 21, 39, 86, 155, 320, 384},
        {2, 3, 9, 18, 33, 61, 129, 258, 511}};
    std::array<std::array<std::uint8_t, 4>, 1024> table{};
    for (std::size_t n = 0; n < 4; ++n) {
        std::size_t a = 0;
        for (std::uint8_t k = 0; k < 9; ++k)
            for (int j = 0; j < lengths[n][k]; ++j) table[a++][n] = k;
    }
    return table;
}();

constexpr auto filter_dictionary = [] {
    std::array<std::uint8_t, 4096> text{};
    std::size_t p = 0;
    for (std::uint8_t i = 0; i < 32; ++i)
        for (std::uint8_t j = 0; j < 16; ++j) {
            for (int k = 0; k < 4; ++k) text[p++] = i;
            for (int k = 0; k < 4; ++k) text[p++] = j;
        }
    return text;
}();

class BitReader {
public:
    BitReader(std::span<const std::uint8_t> data, std::uint32_t length, std::size_t base)
        : data_(data), length_(length), base_(base) {}

    std::size_t offset() const noexcept { return base_ + position_ / 8; }

    std::uint32_t get(unsigned count) {
        require(count <= 32 && count <= length_ - position_, ErrorCode::truncated_input,
                "TLG6 entropy stream ends inside a code", base_ + position_ / 8);
        if (count == 0) return 0;
        if (count == 1) {
            const auto bit = (data_[position_ / 8] >> (position_ & 7)) & 1;
            ++position_;
            return bit;
        }
        if (count <= 8) {
            const auto byte = position_ / 8;
            const unsigned shift = static_cast<unsigned>(position_ & 7);
            auto value = std::uint32_t{data_[byte]} >> shift;
            if (shift + count > 8) value |= std::uint32_t{data_[byte + 1]} << (8 - shift);
            position_ += count;
            return value & ((std::uint32_t{1} << count) - 1);
        }
        // At most five bytes are needed for an unaligned 32-bit value. Reading
        // only existing bytes avoids padding copies and all speculative OOB reads.
        const auto byte = position_ / 8;
        std::uint64_t window = 0;
        const auto available = (count + (position_ & 7) + 7) / 8;
        for (std::size_t i = 0; i < available; ++i)
            window |= std::uint64_t{data_[byte + i]} << (8 * i);
        window >>= position_ & 7;
        position_ += count;
        return static_cast<std::uint32_t>(window & ((std::uint64_t{1} << count) - 1));
    }

    std::uint32_t gamma(std::size_t remaining) {
        unsigned zeros = 0;
        while (get(1) == 0) {
            ++zeros;
            require(zeros < 32 && (std::uint64_t{1} << zeros) <= remaining,
                    ErrorCode::corrupt_data, "TLG6 run exceeds the block row",
                    base_ + position_ / 8);
        }
        const auto count = (std::uint32_t{1} << zeros) + get(zeros);
        require(count <= remaining, ErrorCode::corrupt_data,
                "TLG6 run exceeds the block row", base_ + position_ / 8);
        return count;
    }

    std::uint32_t rice(unsigned k) {
        // TLG6's escape is byte-relative, not a fixed 32-bit unary limit:
        // zero bits through byte+3, the quotient byte at byte+4, then byte+5.
        const unsigned cap = 32 - static_cast<unsigned>(position_ & 7);
        const auto byte = position_ / 8;
        std::uint32_t window = 0;
        if (data_.size() - byte >= 4) {
            std::memcpy(&window, data_.data() + byte, 4);
            if constexpr (std::endian::native == std::endian::big)
                window = std::byteswap(window);
        } else {
            for (std::size_t i = 0; i < data_.size() - byte; ++i)
                window |= std::uint32_t{data_[byte + i]} << (8 * i);
        }
        window >>= position_ & 7;
        const auto available = std::min<std::size_t>(cap, length_ - position_);
        if (available < cap)
            window &= static_cast<std::uint32_t>((std::uint64_t{1} << available) - 1);
        if (window != 0) {
            const auto zeros = static_cast<unsigned>(std::countr_zero(window));
            const auto consumed = zeros + 1 + k;
            if (consumed <= available) {
                const auto remainder = k == 0 ? 0 : (window >> (zeros + 1)) & ((std::uint32_t{1} << k) - 1);
                position_ += consumed;
                return (zeros << k) + remainder;
            }
            position_ += zeros + 1;
            return (zeros << k) + get(k);
        }
        require(available == cap, ErrorCode::truncated_input,
                "TLG6 entropy stream ends inside a unary code", base_ + position_ / 8);
        position_ += cap;
        const auto quotient = get(8);
        return (quotient << k) + get(k);
    }

    void finish() const {
        require(position_ == length_, ErrorCode::corrupt_data,
                "TLG6 entropy stream has unused bits", base_ + position_ / 8);
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t length_;
    std::size_t base_;
    std::size_t position_ = 0;
};

struct BitCounter {
    std::size_t position = 0;
    void put(std::uint32_t, unsigned count) noexcept { position += count; }
};

struct BitWriter {
    std::size_t position = 0;
    std::vector<std::uint8_t> data;

    void put(std::uint32_t value, unsigned count) {
        if (count == 0) return;
        require(position <= bit_length_mask && count <= bit_length_mask - position,
                ErrorCode::resource_limit, "TLG6 channel exceeds its 30-bit length field");
        const auto end = position + count;
        data.resize((end + 7) / 8, 0);
        if (value == 0) { position = end; return; }
        if (count <= 8) {
            const auto byte = position / 8;
            const unsigned shift = static_cast<unsigned>(position & 7);
            value &= (std::uint32_t{1} << count) - 1;
            data[byte] |= static_cast<std::uint8_t>(value << shift);
            if (shift + count > 8) data[byte + 1] |= static_cast<std::uint8_t>(value >> (8 - shift));
            position = end;
            return;
        }
        while (count != 0) {
            const unsigned offset = static_cast<unsigned>(position & 7);
            const unsigned take = std::min(count, 8 - offset);
            const auto mask = (std::uint32_t{1} << take) - 1;
            data[position / 8] |= static_cast<std::uint8_t>((value & mask) << offset);
            value >>= take;
            position += take;
            count -= take;
        }
    }
};

template<class Sink>
void put_gamma(Sink& sink, std::uint32_t value) {
    const unsigned bits = std::bit_width(value) - 1;
    sink.put(0, bits);
    sink.put(1, 1);
    sink.put(value, bits);
}

template<class Sink>
void encode_channel(Sink& sink, std::span<const std::uint8_t> values) {
    sink.put(values.front() != 0, 1);
    unsigned a = 0;
    unsigned n = 3;
    std::size_t i = 0;
    while (i < values.size()) {
        const bool nonzero = values[i] != 0;
        auto end = i + 1;
        if (nonzero) {
            end = static_cast<std::size_t>(std::find(values.data() + end,
                                                     values.data() + values.size(), std::uint8_t{0})
                                           - values.data());
        } else while (end < values.size() && values[end] == 0) ++end;
        put_gamma(sink, static_cast<std::uint32_t>(end - i));
        if (nonzero) {
            for (; i < end; ++i) {
                // Zigzag without a sign-dependent branch: random byte data
                // otherwise makes filter trials pay for a misprediction per value.
                const unsigned byte = values[i];
                const unsigned m = ((byte << 1) ^ (0x1ffu & (0u - (byte >> 7)))) - 1;
                const unsigned k = golomb_table[a][n];
                const unsigned quotient = m >> k;
                const unsigned cap = 32 - static_cast<unsigned>(sink.position & 7);
                if (quotient >= cap) {
                    sink.put(0, cap);
                    sink.put(quotient, 8);
                } else {
                    sink.put(0, quotient);
                    sink.put(1, 1);
                }
                sink.put(m, k);
                a += m >> 1;
                if (n == 0) { a >>= 1; n = 3; } else --n;
            }
        } else i = end;
    }
}

void decode_channel(BitReader& bits, std::span<std::uint8_t> output) {
    bool nonzero = bits.get(1) != 0;
    unsigned a = 0;
    unsigned n = 3;
    std::size_t i = 0;
    while (i < output.size()) {
        const auto count = bits.gamma(output.size() - i);
        const auto end = i + count;
        if (nonzero) {
            const auto decode_value = [&](unsigned phase) {
                const unsigned k = golomb_table[a][phase];
                const auto m = bits.rice(k);
                require(m <= 254, ErrorCode::corrupt_data,
                        "TLG6 residual is outside the signed-byte range", bits.offset());
                const auto sign = (m & 1) - 1;
                output[i++] = static_cast<std::uint8_t>(((m >> 1) ^ sign) + sign + 1);
                a += m >> 1;
            };
            while (n != 3 && i < end) {
                decode_value(n);
                if (n == 0) { a >>= 1; n = 3; } else --n;
            }
            // Four is the algorithm's natural adaptation period. Expanding
            // complete periods removes three phase branches per four values.
            while (end - i >= 4) {
                decode_value(3); decode_value(2); decode_value(1); decode_value(0);
                a >>= 1;
            }
            while (i < end) {
                decode_value(n);
                if (n == 0) { a >>= 1; n = 3; } else --n;
            }
        } else {
            std::fill(output.begin() + i, output.begin() + end, std::uint8_t{0});
            i = end;
        }
        nonzero = !nonzero;
    }
    bits.finish();
}

std::uint8_t med(std::uint8_t left, std::uint8_t above, std::uint8_t diagonal) noexcept {
    const auto low = std::min(left, above);
    const auto high = std::max(left, above);
    if (diagonal >= high) return low;
    if (diagonal <= low) return high;
    return static_cast<std::uint8_t>(int{left} + above - diagonal);
}

std::uint8_t avg(std::uint8_t left, std::uint8_t above) noexcept {
    return static_cast<std::uint8_t>((unsigned{left} + above + 1) / 2);
}

// Every operation is in Z/256Z. Using unsigned bytes makes wraparound explicit
// and avoids signed overflow, negative left shifts, and channel carry.
void color_filter(std::uint8_t& b, std::uint8_t& g, std::uint8_t& r, unsigned code) noexcept {
    switch (code) {
    case 0: break;
    case 1: r -= g; b -= g; break;
    case 2: r -= g; g -= b; break;
    case 3: b -= g; g -= r; break;
    case 4: r -= g; g -= b; b -= r; break;
    case 5: g -= b; b -= r; break;
    case 6: b -= g; break;
    case 7: g -= b; break;
    case 8: r -= g; break;
    case 9: b -= g; g -= r; r -= b; break;
    case 10: g -= r; b -= r; break;
    case 11: r -= b; g -= b; break;
    case 12: g -= r; r -= b; break;
    case 13: g -= r; r -= b; b -= g; break;
    case 14: r -= b; b -= g; g -= r; break;
    case 15: r -= 2 * b; g -= 2 * b; break;
    }
}

void inverse_filter(std::uint8_t& b, std::uint8_t& g, std::uint8_t& r, unsigned code) noexcept {
    // Reverse the order as well as the sign of the triangular transforms.
    switch (code) {
    case 0: break;
    case 1: b += g; r += g; break;
    case 2: g += b; r += g; break;
    case 3: g += r; b += g; break;
    case 4: b += r; g += b; r += g; break;
    case 5: b += r; g += b; break;
    case 6: b += g; break;
    case 7: g += b; break;
    case 8: r += g; break;
    case 9: r += b; g += r; b += g; break;
    case 10: b += r; g += r; break;
    case 11: g += b; r += b; break;
    case 12: r += b; g += r; break;
    case 13: b += g; r += b; g += r; break;
    case 14: g += r; b += g; r += b; break;
    case 15: g += 2 * b; r += 2 * b; break;
    }
}

std::uint32_t packed_ge(std::uint32_t a, std::uint32_t b) noexcept {
    const auto complement = ~b;
    const auto carries = ((a & complement) + (((a ^ complement) >> 1) & 0x7f7f7f7f)) & 0x80808080;
    return ((carries >> 7) + 0x7f7f7f7f) ^ 0x7f7f7f7f;
}

std::uint32_t packed_med(std::uint32_t left, std::uint32_t above, std::uint32_t diagonal) noexcept {
    const auto swap = (left ^ above) & packed_ge(left, above);
    const auto low = left ^ swap;
    const auto high = above ^ swap;
    const auto lower_edge = packed_ge(diagonal, high);
    const auto upper_edge = packed_ge(low, diagonal);
    const auto middle = ~(lower_edge | upper_edge);
    return (lower_edge & low) | (upper_edge & high)
         | ((high & middle) - (diagonal & middle) + (low & middle));
}

std::uint32_t packed_avg(std::uint32_t a, std::uint32_t b) noexcept {
    return (a & b) + (((a ^ b) & 0xfefefefe) >> 1) + ((a ^ b) & 0x01010101);
}

std::uint32_t packed_add(std::uint32_t a, std::uint32_t b) noexcept {
    // Strip the high bit before adding, then restore it modulo two. No sum in
    // the stripped lanes can carry across a byte boundary.
    return ((a & 0x7f7f7f7f) + (b & 0x7f7f7f7f)) ^ ((a ^ b) & 0x80808080);
}

std::uint32_t predict_add(std::uint32_t left, std::uint32_t above,
                          std::uint32_t diagonal, std::uint32_t residual, bool use_avg) noexcept {
    if (left == above) return packed_add(left, residual);
#if defined(TLG6_HAS_SSE2)
    // SSE2 is baseline on x86-64. MED = left + above - clamp(diagonal,
    // min(left,above), max(left,above)), independently in each unsigned byte.
    const auto l = _mm_cvtsi32_si128(static_cast<int>(left));
    const auto u = _mm_cvtsi32_si128(static_cast<int>(above));
    const auto d = _mm_cvtsi32_si128(static_cast<int>(diagonal));
    const auto r = _mm_cvtsi32_si128(static_cast<int>(residual));
    const auto prediction = use_avg ? _mm_avg_epu8(l, u)
        : _mm_sub_epi8(_mm_add_epi8(l, u),
                      _mm_min_epu8(_mm_max_epu8(d, _mm_min_epu8(l, u)), _mm_max_epu8(l, u)));
    return static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_add_epi8(prediction, r)));
#else
    return packed_add(use_avg ? packed_avg(left, above) : packed_med(left, above, diagonal), residual);
#endif
}

template<unsigned Colors>
std::uint32_t load_pixel(const std::uint8_t* p) noexcept {
    if constexpr (Colors == 3) return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8)
                                         | (std::uint32_t{p[2]} << 16);
    else {
        std::uint32_t result;
        std::memcpy(&result, p, 4);
        if constexpr (std::endian::native == std::endian::big) result = std::byteswap(result);
        return result;
    }
}

template<unsigned Colors>
void store_pixel(std::uint8_t* p, std::uint32_t value) noexcept {
    if constexpr (Colors == 3) {
        p[0] = static_cast<std::uint8_t>(value);
        p[1] = static_cast<std::uint8_t>(value >> 8);
        p[2] = static_cast<std::uint8_t>(value >> 16);
    } else {
        if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
        std::memcpy(p, &value, 4);
    }
}

using Block = std::array<std::array<std::uint8_t, 64>, 4>;

unsigned native_channel(unsigned channel, unsigned colors) noexcept {
    return colors == 1 || channel == 3 ? channel : 2 - channel;
}

// Blocks snake vertically; rows snake horizontally. The same bijection is used
// in both directions, including the final partial block and odd block height.
std::size_t block_index(std::size_t bx, std::size_t row, std::size_t col,
                        std::size_t width, std::size_t height, std::size_t y) noexcept {
    const auto stored_row = (bx & 1) != 0 ? height - row - 1 : row;
    const auto stored_col = ((y + row) & 1) != 0 ? width - col - 1 : col;
    return stored_row * width + stored_col;
}

std::array<Block, 2> predict_block(ImageView image, std::size_t x, std::size_t y) {
    std::array<Block, 2> result{};
    const unsigned colors = static_cast<unsigned>(image.layout);
    const auto bw = std::min<std::size_t>(8, image.width - x);
    const auto bh = std::min<std::size_t>(8, image.height - y);
    const auto stride = std::size_t{image.width} * colors;
    for (std::size_t row = 0; row < bh; ++row) {
        for (std::size_t col = 0; col < bw; ++col) {
            const auto index = block_index(x / 8, row, col, bw, bh, y);
            const auto pixel = (y + row) * stride + (x + col) * colors;
            for (unsigned c = 0; c < colors; ++c) {
                const auto p = pixel + native_channel(c, colors);
                const auto left = x + col != 0 ? image.pixels[p - colors] : std::uint8_t{0};
                const auto above = y + row != 0 ? image.pixels[p - stride] : std::uint8_t{0};
                const auto diagonal = x + col != 0 && y + row != 0
                                    ? image.pixels[p - stride - colors] : std::uint8_t{0};
                result[0][c][index] = static_cast<std::uint8_t>(image.pixels[p] - med(left, above, diagonal));
                result[1][c][index] = static_cast<std::uint8_t>(image.pixels[p] - avg(left, above));
            }
        }
    }
    return result;
}

template<unsigned Code>
void filter_block_code(Block& block, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i)
        color_filter(block[0][i], block[1][i], block[2][i], Code);
}

void filter_block(Block& block, std::size_t count, unsigned code) noexcept {
    // Dispatch once per block so each byte loop has a constant transform and
    // the compiler can vectorize across up to 64 independent residuals.
    switch (code) {
    case 0: break;
    case 1: filter_block_code<1>(block, count); break;
    case 2: filter_block_code<2>(block, count); break;
    case 3: filter_block_code<3>(block, count); break;
    case 4: filter_block_code<4>(block, count); break;
    case 5: filter_block_code<5>(block, count); break;
    case 6: filter_block_code<6>(block, count); break;
    case 7: filter_block_code<7>(block, count); break;
    case 8: filter_block_code<8>(block, count); break;
    case 9: filter_block_code<9>(block, count); break;
    case 10: filter_block_code<10>(block, count); break;
    case 11: filter_block_code<11>(block, count); break;
    case 12: filter_block_code<12>(block, count); break;
    case 13: filter_block_code<13>(block, count); break;
    case 14: filter_block_code<14>(block, count); break;
    case 15: filter_block_code<15>(block, count); break;
    }
}

template<unsigned Code>
void inverse_block_code(std::uint8_t* b, std::uint8_t* g, std::uint8_t* r, std::size_t count) noexcept {
    for (std::size_t i = 0; i < count; ++i) inverse_filter(b[i], g[i], r[i], Code);
}

void inverse_block(std::uint8_t* b, std::uint8_t* g, std::uint8_t* r,
                    std::size_t count, unsigned code) noexcept {
    switch (code) {
    case 0: break;
    case 1: inverse_block_code<1>(b, g, r, count); break;
    case 2: inverse_block_code<2>(b, g, r, count); break;
    case 3: inverse_block_code<3>(b, g, r, count); break;
    case 4: inverse_block_code<4>(b, g, r, count); break;
    case 5: inverse_block_code<5>(b, g, r, count); break;
    case 6: inverse_block_code<6>(b, g, r, count); break;
    case 7: inverse_block_code<7>(b, g, r, count); break;
    case 8: inverse_block_code<8>(b, g, r, count); break;
    case 9: inverse_block_code<9>(b, g, r, count); break;
    case 10: inverse_block_code<10>(b, g, r, count); break;
    case 11: inverse_block_code<11>(b, g, r, count); break;
    case 12: inverse_block_code<12>(b, g, r, count); break;
    case 13: inverse_block_code<13>(b, g, r, count); break;
    case 14: inverse_block_code<14>(b, g, r, count); break;
    case 15: inverse_block_code<15>(b, g, r, count); break;
    }
}

std::size_t channel_cost(std::span<const std::uint8_t> values) {
    BitCounter counter;
    encode_channel(counter, values);
    return counter.position;
}

std::uint8_t choose_filter(const std::array<Block, 2>& candidates,
                           std::size_t count, unsigned colors) {
    std::size_t best_cost = std::numeric_limits<std::size_t>::max();
    std::uint8_t best = 0;
    for (unsigned predictor = 0; predictor < 2; ++predictor) {
        const auto alpha_cost = colors == 4
            ? channel_cost(std::span{candidates[predictor][3]}.first(count)) : 0;
        for (unsigned code = 0; code < (colors >= 3 ? 16u : 1u); ++code) {
            auto trial = candidates[predictor]; // stack storage, no allocation per trial
            filter_block(trial, count, code);
            auto cost = alpha_cost;
            for (unsigned c = 0; c < std::min(colors, 3u); ++c) {
                cost += channel_cost(std::span{trial[c]}.first(count));
                if (cost >= best_cost) break;
            }
            if (cost < best_cost) {
                best_cost = cost;
                best = static_cast<std::uint8_t>(2 * code + predictor);
            }
        }
    }
    return best;
}

template<unsigned Colors>
void reconstruct_stripe(Image& image, const std::array<std::vector<std::uint8_t>, 4>& stripe,
                        std::span<const std::uint8_t> filters, std::size_t y, std::size_t bh) {
    const auto blocks_x = (std::size_t{image.width} + 7) / 8;
    const auto stride = std::size_t{image.width} * Colors;
    for (std::size_t row = 0; row < bh; ++row) {
        auto* destination = image.pixels.data() + (y + row) * stride;
        const auto* above_line = y + row != 0 ? destination - stride : nullptr;
        std::uint32_t left = 0;
        std::uint32_t diagonal = 0;
        const bool reverse = ((y + row) & 1) != 0;
        for (std::size_t bx = 0; bx < blocks_x; ++bx) {
            const auto x = bx * 8;
            const auto bw = std::min<std::size_t>(8, image.width - x);
            const auto stored_row = (bx & 1) != 0 ? bh - row - 1 : row;
            const auto start = x * bh + stored_row * bw;
            const auto type = filters[(y / 8) * blocks_x + bx];
            for (std::size_t col = 0; col < bw; ++col) {
                const auto index = start + (reverse ? bw - col - 1 : col);
                auto b = stripe[0][index];
                std::uint8_t g = 0;
                std::uint8_t r = 0;
                if constexpr (Colors >= 3) { g = stripe[1][index]; r = stripe[2][index]; }
                if constexpr (Colors == 1) {
                    inverse_filter(b, g, r, type >> 1);
                    const auto above = above_line != nullptr ? above_line[x + col] : std::uint8_t{0};
                    const auto prediction = (type & 1) == 0
                        ? med(static_cast<std::uint8_t>(left), above, static_cast<std::uint8_t>(diagonal))
                        : avg(static_cast<std::uint8_t>(left), above);
                    const auto value = static_cast<std::uint8_t>(prediction + b);
                    destination[x + col] = value;
                    left = value;
                    diagonal = above;
                } else {
                    const auto offset = (x + col) * Colors;
                    const auto above = above_line != nullptr ? load_pixel<Colors>(above_line + offset) : 0;
                    auto residual = std::uint32_t{r} | (std::uint32_t{g} << 8) | (std::uint32_t{b} << 16);
                    if constexpr (Colors == 4) residual |= std::uint32_t{stripe[3][index]} << 24;
                    left = predict_add(left, above, diagonal, residual, (type & 1) != 0);
                    store_pixel<Colors>(destination + offset, left);
                    diagonal = above;
                }
            }
        }
    }
}

} // namespace

std::vector<std::uint8_t> encode6(ImageView image) {
    const unsigned colors = static_cast<unsigned>(image.layout);
    require(std::size_t{image.width} * std::min<std::size_t>(8, image.height)
                <= std::numeric_limits<std::uint32_t>::max(),
            ErrorCode::resource_limit, "TLG6 block row exceeds the 32-bit run-length range");
    const auto blocks_x = (std::size_t{image.width} + 7) / 8;
    const auto blocks_y = (std::size_t{image.height} + 7) / 8;
    std::vector<std::uint8_t> filters;
    filters.reserve(blocks_x * blocks_y);

    // Two passes avoid buffering a second complete compressed image before its
    // filter table is known. Only a stripe of eight rows is retained in pass 2.
    for (std::size_t y = 0; y < image.height; y += 8) {
        const auto bh = std::min<std::size_t>(8, image.height - y);
        for (std::size_t x = 0; x < image.width; x += 8) {
            const auto bw = std::min<std::size_t>(8, image.width - x);
            filters.push_back(choose_filter(predict_block(image, x, y), bw * bh, colors));
        }
    }
    SlideEncoder slide(filter_dictionary);
    const auto compressed_filters = slide.encode(filters);
    require(compressed_filters.size() <= std::numeric_limits<std::uint32_t>::max(),
            ErrorCode::resource_limit, "TLG6 filter table is too large");
    ByteWriter writer;
    writer.append(magic6);
    writer.u8(static_cast<std::uint8_t>(colors));
    writer.u8(0); writer.u8(0); writer.u8(0);
    writer.u32(image.width); writer.u32(image.height);
    const auto max_length_offset = writer.data.size();
    writer.u32(0);
    writer.u32(static_cast<std::uint32_t>(compressed_filters.size()));
    writer.append(compressed_filters);

    std::array<std::vector<std::uint8_t>, 4> stripe;
    for (unsigned c = 0; c < colors; ++c)
        stripe[c].resize(std::size_t{image.width} * std::min<std::size_t>(8, image.height));
    std::uint32_t max_length = 0;
    for (std::size_t y = 0; y < image.height; y += 8) {
        const auto bh = std::min<std::size_t>(8, image.height - y);
        for (std::size_t x = 0; x < image.width; x += 8) {
            const auto bw = std::min<std::size_t>(8, image.width - x);
            const auto type = filters[(y / 8) * blocks_x + x / 8];
            auto block = predict_block(image, x, y)[type & 1];
            filter_block(block, bw * bh, type >> 1);
            for (unsigned c = 0; c < colors; ++c)
                std::copy_n(block[c].begin(), bw * bh, stripe[c].begin() + x * bh);
        }
        for (unsigned c = 0; c < colors; ++c) {
            BitWriter bits;
            bits.data.reserve(std::size_t{image.width} * bh + 16);
            encode_channel(bits, std::span{stripe[c]}.first(std::size_t{image.width} * bh));
            const auto length = static_cast<std::uint32_t>(bits.position);
            max_length = std::max(max_length, length);
            writer.u32(length);
            writer.append(bits.data);
        }
    }
    writer.patch_u32(max_length_offset, max_length);
    return std::move(writer.data);
}

Image decode6(std::span<const std::uint8_t> bytes, const DecodeOptions& options) {
    require(bytes.size() <= options.max_input_bytes, ErrorCode::resource_limit,
            "TLG6 input exceeds the configured byte limit");
    ByteReader reader(bytes);
    reader.expect(magic6);
    const unsigned colors = reader.u8();
    require(colors == 1 || colors == 3 || colors == 4, ErrorCode::unsupported_format,
            "TLG6 supports one, three, or four channels", reader.position() - 1);
    const auto data_flag = reader.u8();
    const auto color_type = reader.u8();
    const auto external_table = reader.u8();
    require(data_flag == 0 && color_type == 0 && external_table == 0,
            ErrorCode::unsupported_format, "Unsupported TLG6 header flags", 12);
    Image image;
    image.width = reader.u32();
    image.height = reader.u32();
    image.layout = static_cast<PixelLayout>(colors);
    const auto image_bytes = checked_image_size(image.width, image.height, image.layout, options, 15);
    const auto maximum_bits = reader.u32();
    require(maximum_bits > 0 && maximum_bits <= bit_length_mask, ErrorCode::corrupt_data,
            "Invalid TLG6 maximum entropy bit length", reader.position() - 4);
    const auto blocks_x = (std::size_t{image.width} + 7) / 8;
    const auto blocks_y = (std::size_t{image.height} + 7) / 8;
    const auto filter_size = reader.u32();
    const auto filter_offset = reader.position();
    const auto compressed_filters = reader.take(filter_size);
    // Verify all entropy frame lengths before allocating filters and pixels.
    auto framing = reader;
    for (std::size_t row = 0; row < blocks_y; ++row) {
        for (unsigned c = 0; c < colors; ++c) {
            const auto length_field = framing.u32();
            require((length_field >> 30) == 0, ErrorCode::unsupported_format,
                    "Unsupported TLG6 entropy coding method", framing.position() - 4);
            require(length_field > 0 && length_field <= maximum_bits, ErrorCode::corrupt_data,
                    "TLG6 channel length exceeds its header maximum", framing.position() - 4);
            framing.take((std::size_t{length_field} + 7) / 8);
        }
    }
    require(!options.reject_trailing_data || framing.empty(), ErrorCode::corrupt_data,
            "Unexpected bytes after the TLG6 image", framing.position());
    SlideDecoder slide(filter_dictionary);
    const auto filters = [&] {
        try { return slide.decode(compressed_filters, blocks_x * blocks_y); }
        catch (Failure& failure) {
            failure.error.offset += filter_offset;
            throw;
        }
    }();
    for (const auto type : filters)
        require(type < 32, ErrorCode::corrupt_data, "Invalid TLG6 color filter", filter_offset);
    image.pixels.resize(image_bytes);
    std::array<std::vector<std::uint8_t>, 4> stripe;
    for (unsigned c = 0; c < colors; ++c)
        stripe[c].resize(std::size_t{image.width} * std::min<std::size_t>(8, image.height));

    for (std::size_t y = 0; y < image.height; y += 8) {
        const auto bh = std::min<std::size_t>(8, image.height - y);
        const auto count = std::size_t{image.width} * bh;
        for (unsigned c = 0; c < colors; ++c) {
            const auto length_field = reader.u32();
            require((length_field >> 30) == 0, ErrorCode::unsupported_format,
                    "Unsupported TLG6 entropy coding method", reader.position() - 4);
            require(length_field > 0 && length_field <= maximum_bits, ErrorCode::corrupt_data,
                    "TLG6 channel length exceeds its header maximum", reader.position() - 4);
            const auto base = reader.position();
            BitReader bits(reader.take((std::size_t{length_field} + 7) / 8), length_field, base);
            decode_channel(bits, std::span{stripe[c]}.first(count));
        }
        if (colors >= 3) {
            for (std::size_t bx = 0; bx < blocks_x; ++bx) {
                const auto x = bx * 8;
                const auto bw = std::min<std::size_t>(8, image.width - x);
                const auto start = x * bh;
                inverse_block(stripe[0].data() + start, stripe[1].data() + start,
                              stripe[2].data() + start, bw * bh,
                              filters[(y / 8) * blocks_x + bx] >> 1);
            }
        }
        switch (colors) {
        case 1: reconstruct_stripe<1>(image, stripe, filters, y, bh); break;
        case 3: reconstruct_stripe<3>(image, stripe, filters, y, bh); break;
        case 4: reconstruct_stripe<4>(image, stripe, filters, y, bh); break;
        }
    }
    require(!options.reject_trailing_data || reader.empty(), ErrorCode::corrupt_data,
            "Unexpected bytes after the TLG6 image", reader.position());
    return image;
}

} // namespace tlg::detail
