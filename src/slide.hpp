#pragma once

#include <tlg/tlg.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace tlg::detail {

// Check token lengths and framing before allocating decompressed storage.
void validate_slide(std::span<const std::uint8_t> input, std::size_t exact_output);

// TLG's LZSS uses a 12-bit ring position and lengths in [3, 273]. Each
// encode/decode call begins a fresh flag group; the dictionary persists.
class SlideEncoder {
public:
    explicit SlideEncoder(std::span<const std::uint8_t> dictionary = {},
                          SlideParse parse = SlideParse::greedy,
                          std::size_t max_working_bytes = std::size_t{256} << 20,
                          SlideMatch match = SlideMatch::adaptive);
    [[nodiscard]] std::vector<std::uint8_t> encode(std::span<const std::uint8_t> input);
    void store();
    void restore();

private:
    static constexpr std::size_t window = 4096;
    static constexpr std::size_t longest = 273;
    static constexpr std::size_t mask = window - 1;
    static constexpr std::size_t keys = 1 << 15;

    // The mirrored prefix makes every non-overlapping match contiguous.
    std::array<std::uint8_t, window + longest - 1> text_{};
    std::vector<std::uint32_t> heads_;
    std::array<std::uint32_t, window> previous_{};
    std::vector<std::uint32_t> long_heads_;
    std::vector<std::uint32_t> long_previous_;
    std::uint32_t position_ = 2 * window;
    std::size_t steps_ = 0;
    SlideParse parse_;
    std::size_t max_working_bytes_;
    SlideMatch match_;

    std::array<std::uint8_t, window> saved_text_{};
    std::size_t saved_cursor_ = 0;
    std::size_t saved_steps_ = 0;
    bool has_snapshot_ = false;

    void push(std::uint8_t value);
    template<bool LongKeys> void push_byte(std::uint8_t value);
    void rebuild();
    void rebuild_long_keys();
    void rebase();
    [[nodiscard]] std::size_t probe(std::span<const std::uint8_t> input,
                                    std::size_t slot, std::size_t distance,
                                    std::size_t cap, std::size_t best) const;
    [[nodiscard]] std::pair<std::size_t, std::size_t>
    find_match(std::span<const std::uint8_t> input);
    [[nodiscard]] std::vector<std::uint8_t> encode_optimal(std::span<const std::uint8_t> input);
};

class SlideDecoder {
public:
    explicit SlideDecoder(std::span<const std::uint8_t> dictionary = {},
                          SlideDecode algorithm = SlideDecode::bulk);
    // exact_output is both the expected length and an allocation/output bound.
    // Malformed, truncated and overlong streams fail instead of yielding prefixes.
    [[nodiscard]] std::vector<std::uint8_t>
    decode(std::span<const std::uint8_t> input, std::size_t exact_output);
    // Internal framing fast path. The caller must first successfully run
    // validate_slide on these exact bytes and this exact output length.
    [[nodiscard]] std::vector<std::uint8_t>
    decode_prevalidated(std::span<const std::uint8_t> input, std::size_t exact_output);

private:
    std::array<std::uint8_t, 4096> text_{};
    std::size_t cursor_ = 0;
    SlideDecode algorithm_;
    [[nodiscard]] std::vector<std::uint8_t>
    decode_scalar(std::span<const std::uint8_t> input, std::size_t exact_output);
};

} // namespace tlg::detail
