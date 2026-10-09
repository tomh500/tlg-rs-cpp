#include "slide.hpp"
#include "internal.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#if defined(_MSC_VER)
#define TLG_SLIDE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define TLG_SLIDE_INLINE inline __attribute__((always_inline))
#else
#define TLG_SLIDE_INLINE inline
#endif

namespace tlg::detail {
namespace {

constexpr std::size_t hash3(std::uint8_t a, std::uint8_t b, std::uint8_t c) noexcept {
    const auto value = std::uint32_t(a) | (std::uint32_t(b) << 8) | (std::uint32_t(c) << 16);
    return (value * 0x9e3779b1u) >> 17;
}

TLG_SLIDE_INLINE std::size_t common_prefix(const std::uint8_t* a, const std::uint8_t* b,
                          std::size_t limit) noexcept {
    std::size_t i = 0;
    // memcpy permits unaligned loads without violating C++ aliasing rules.
    // The bound is the match cap, including for overlapping copies.
    while (limit - i >= sizeof(std::uint64_t)) {
        std::uint64_t lhs, rhs;
        std::memcpy(&lhs, a + i, sizeof(lhs));
        std::memcpy(&rhs, b + i, sizeof(rhs));
        auto difference = lhs ^ rhs;
        if (difference != 0) {
            if constexpr (std::endian::native == std::endian::big)
                difference = std::byteswap(difference);
            return i + std::countr_zero(difference) / 8;
        }
        i += sizeof(std::uint64_t);
    }
    while (i < limit && a[i] == b[i]) ++i;
    return i;
}

} // namespace

SlideEncoder::SlideEncoder(std::span<const std::uint8_t> dictionary) : heads_(keys) {
    require(dictionary.empty() || dictionary.size() == window, ErrorCode::invalid_argument,
            "Slide dictionary must be empty or exactly 4096 bytes");
    std::copy(dictionary.begin(), dictionary.end(), text_.begin());
    std::copy_n(text_.begin(), longest - 1, text_.begin() + window);
    if (!dictionary.empty()) {
        steps_ = window;
        rebuild();
    }
}

TLG_SLIDE_INLINE void SlideEncoder::push(std::uint8_t value) {
    const auto slot = std::size_t(position_) & mask;
    text_[slot] = value;
    if (slot < longest - 1) text_[slot + window] = value;

    // A new byte completes the three-byte key beginning two positions ago.
    const auto start = position_ - 2;
    const auto q = std::size_t(start) & mask;
    const auto key = hash3(text_[q], text_[(q + 1) & mask], value);
    previous_[q] = heads_[key];
    heads_[key] = start;
    ++position_;
    steps_ = std::min(steps_ + 1, window);
}

void SlideEncoder::rebuild() {
    std::fill(heads_.begin(), heads_.end(), 0);
    if (steps_ == 0) return;
    // Reinsert oldest first: hash-chain walks then visit most recent matches
    // first. Absolute positions avoid ambiguous links after ring wraparound.
    const auto first = steps_ < window ? std::uint32_t(2 * window - 2)
                                      : position_ - std::uint32_t(window);
    for (auto p = first; p <= position_ - 3; ++p) {
        const auto slot = std::size_t(p) & mask;
        const auto key = hash3(text_[slot], text_[(slot + 1) & mask], text_[(slot + 2) & mask]);
        previous_[slot] = heads_[key];
        heads_[key] = p;
    }
}

void SlideEncoder::rebase() {
    // Keep all live absolute positions positive so zero remains the sentinel.
    const auto shift = (position_ - std::uint32_t(2 * window)) & ~std::uint32_t(mask);
    for (auto& p : heads_) p = p > shift ? p - shift : 0;
    for (auto& p : previous_) p = p > shift ? p - shift : 0;
    position_ -= shift;
}

TLG_SLIDE_INLINE std::size_t SlideEncoder::probe(std::span<const std::uint8_t> input,
                                std::size_t slot, std::size_t distance,
                                std::size_t cap, std::size_t best) const {
    if (cap <= best) return 0;
    if (best >= 3) {
        const auto predicted = best < distance ? text_[slot + best] : input[best - distance];
        if (predicted != input[best]) return 0;
    }

    const auto prefix = std::min(distance, cap);
    auto length = common_prefix(text_.data() + slot, input.data(), prefix);
    if (length == distance && length < cap) {
        // A decoder copies one byte at a time. After distance bytes it reads
        // bytes produced by this same token: an input prefix repeats itself.
        length += common_prefix(input.data(), input.data() + distance, cap - distance);
    }
    return length >= 3 && length > best ? length : 0;
}

TLG_SLIDE_INLINE std::pair<std::size_t, std::size_t>
SlideEncoder::find_match(std::span<const std::uint8_t> input) const {
    if (input.size() < 3) return {0, 0};
    const auto cap = std::min(longest, input.size());
    std::size_t best = 0, best_slot = 0;

    // Their three-byte keys cross the cursor, so distances 1 and 2 are tested
    // directly. This makes long runs compress immediately after one literal.
    for (std::size_t distance = 1; distance <= 2; ++distance) {
        if (input[0] != input[distance]) continue;
        if (distance == 1 && input[0] != input[2]) continue;
        const auto slot = (std::size_t(position_) - distance) & mask;
        if (input[0] != text_[slot] || (distance == 2 && input[1] != text_[(slot + 1) & mask]))
            continue;
        const auto length = probe(input, slot, distance, cap, best);
        if (length != 0) { best = length; best_slot = slot; }
        if (best == cap) return {best_slot, best};
    }

    auto p = heads_[hash3(input[0], input[1], input[2])];
    while (p != 0) {
        const auto distance = std::size_t(position_ - p);
        if (distance > window) break;
        const auto slot = std::size_t(p) & mask;
        const auto length = probe(input, slot, distance, cap, best);
        if (length != 0) { best = length; best_slot = slot; }
        if (best == cap) break;
        p = previous_[slot];
    }
    // The cold part of a zero-initialized dictionary has no hash nodes yet.
    // All its three-byte keys are zero, except keys crossing the ring boundary.
    // Avoid building 4096 redundant nodes after every incompressible raw plane.
    if (steps_ < window && input[0] == 0) {
        const auto cursor = std::size_t(position_) & mask;
        if (input[1] == 0 && steps_ <= window - 2) {
            const auto first = input[2] == 0 ? cursor : window - 2;
            for (auto slot = first; slot <= window - 2; ++slot) {
                const auto distance = window + cursor - slot;
                const auto length = probe(input, slot, distance, cap, best);
                if (length != 0) { best = length; best_slot = slot; }
                if (best == cap) return {best_slot, best};
            }
        }
        const auto slot = window - 1;
        const auto length = probe(input, slot, cursor + 1, cap, best);
        if (length != 0) { best = length; best_slot = slot; }
    }
    return {best_slot, best};
}

std::vector<std::uint8_t> SlideEncoder::encode(std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> output;
    const auto flags = input.size() / 8 + (input.size() % 8 != 0);
    require(input.size() <= output.max_size() - flags, ErrorCode::resource_limit,
            "Slide input is too large");
    output.resize(input.size() + flags);
    std::size_t written = 0;

    std::array<std::uint8_t, 25> group{}; // 1 flag + 8 long-form tokens
    std::size_t used = 1;
    unsigned flag = 1;
    std::size_t i = 0;
    while (i < input.size()) {
        if (position_ >= (1u << 31)) rebase();
        const auto [slot, length] = find_match(input.subspan(i));
        if (length != 0) {
            group[0] |= std::uint8_t(flag);
            group[used++] = std::uint8_t(slot);
            group[used++] = std::uint8_t((slot >> 8) | ((length >= 18 ? 15 : length - 3) << 4));
            if (length >= 18) group[used++] = std::uint8_t(length - 18);
            for (std::size_t n = 0; n < length; ++n) push(input[i++]);
        } else {
            group[used++] = input[i];
            push(input[i++]);
        }

        flag <<= 1;
        if (flag == 256) {
            std::memcpy(output.data() + written, group.data(), used);
            written += used;
            group[0] = 0;
            used = 1;
            flag = 1;
        }
    }
    if (flag != 1) {
        std::memcpy(output.data() + written, group.data(), used);
        written += used;
    }
    output.resize(written);
    return output;
}

void SlideEncoder::store() {
    std::copy_n(text_.begin(), window, saved_text_.begin());
    saved_cursor_ = std::size_t(position_) & mask;
    saved_steps_ = steps_;
    has_snapshot_ = true;
}

void SlideEncoder::restore() {
    require(has_snapshot_, ErrorCode::invalid_argument, "Slide restore requires a stored dictionary");
    std::copy(saved_text_.begin(), saved_text_.end(), text_.begin());
    std::copy_n(text_.begin(), longest - 1, text_.begin() + window);
    position_ = std::uint32_t(2 * window + saved_cursor_);
    steps_ = saved_steps_;
    rebuild();
}

SlideDecoder::SlideDecoder(std::span<const std::uint8_t> dictionary) {
    require(dictionary.empty() || dictionary.size() == text_.size(), ErrorCode::invalid_argument,
            "Slide dictionary must be empty or exactly 4096 bytes");
    std::copy(dictionary.begin(), dictionary.end(), text_.begin());
}

void validate_slide(std::span<const std::uint8_t> input, std::size_t exact_output) {
    ByteReader reader(input);
    std::size_t written = 0;
    while (written < exact_output) {
        const auto flags = reader.u8();
        for (unsigned bit = 0; bit < 8 && written < exact_output; ++bit) {
            std::size_t length = 1;
            if ((flags & (1u << bit)) != 0) {
                reader.u8();
                const auto high = reader.u8();
                length = std::size_t(high >> 4) + 3;
                if ((high >> 4) == 15) length = 18 + reader.u8();
                require(length <= exact_output - written, ErrorCode::corrupt_data,
                        "Slide match exceeds the expected output size", reader.position());
            } else {
                reader.u8();
            }
            written += length;
        }
    }
    require(reader.empty(), ErrorCode::corrupt_data,
            "Slide stream has bytes beyond its expected output", reader.position());
}

std::vector<std::uint8_t> SlideDecoder::decode(std::span<const std::uint8_t> input,
                                             std::size_t exact_output) {
    validate_slide(input, exact_output);
    ByteReader reader(input);
    std::vector<std::uint8_t> output;
    require(exact_output <= output.max_size(), ErrorCode::resource_limit, "Slide output is too large");
    output.resize(exact_output);
    std::size_t written = 0;
    while (written < exact_output) {
        const auto flags = reader.u8();
        for (unsigned bit = 0; bit < 8 && written < exact_output; ++bit) {
            if ((flags & (1u << bit)) != 0) {
                const auto low = reader.u8();
                const auto high = reader.u8();
                auto slot = std::size_t(low | ((high & 15u) << 8));
                auto length = std::size_t(high >> 4) + 3;
                if ((high >> 4) == 15) length = 18 + reader.u8();
                require(length <= exact_output - written, ErrorCode::corrupt_data,
                        "Slide match exceeds the expected output size", reader.position());
                if (((cursor_ - slot) & 4095) == 1) {
                    // A distance-one overlap repeats the immediately preceding
                    // byte. Bulk fills preserve the same ring state as copying
                    // that byte through the token one position at a time.
                    const auto value = text_[slot];
                    std::fill_n(output.data() + written, length, value);
                    const auto first = std::min(length, text_.size() - cursor_);
                    std::fill_n(text_.begin() + cursor_, first, value);
                    std::fill_n(text_.begin(), length - first, value);
                    cursor_ = (cursor_ + length) & 4095;
                    written += length;
                    continue;
                }
                for (std::size_t n = 0; n < length; ++n) {
                    const auto value = text_[slot];
                    output[written++] = value;
                    text_[cursor_] = value;
                    cursor_ = (cursor_ + 1) & 4095;
                    slot = (slot + 1) & 4095;
                }
            } else {
                const auto value = reader.u8();
                output[written++] = value;
                text_[cursor_] = value;
                cursor_ = (cursor_ + 1) & 4095;
            }
        }
    }
    require(reader.empty(), ErrorCode::corrupt_data,
            "Slide stream has bytes beyond its expected output", reader.position());
    return output;
}

} // namespace tlg::detail

#undef TLG_SLIDE_INLINE
