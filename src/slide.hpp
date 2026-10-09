#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace tlg::detail {

// TLG's LZSS uses a 12-bit ring position and lengths in [3, 273]. Each
// encode/decode call begins a fresh flag group; the dictionary persists.
class SlideEncoder {
public:
    explicit SlideEncoder(std::span<const std::uint8_t> dictionary = {});
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
    std::uint32_t position_ = 2 * window;
    std::size_t steps_ = 0;

    std::array<std::uint8_t, window> saved_text_{};
    std::size_t saved_cursor_ = 0;
    std::size_t saved_steps_ = 0;
    bool has_snapshot_ = false;

    void push(std::uint8_t value);
    void rebuild();
    void rebase();
    [[nodiscard]] std::size_t probe(std::span<const std::uint8_t> input,
                                    std::size_t slot, std::size_t distance,
                                    std::size_t cap, std::size_t best) const;
    [[nodiscard]] std::pair<std::size_t, std::size_t>
    find_match(std::span<const std::uint8_t> input) const;
};

class SlideDecoder {
public:
    explicit SlideDecoder(std::span<const std::uint8_t> dictionary = {});
    // exact_output is both the expected length and an allocation/output bound.
    // Malformed, truncated and overlong streams fail instead of yielding prefixes.
    [[nodiscard]] std::vector<std::uint8_t>
    decode(std::span<const std::uint8_t> input, std::size_t exact_output);

private:
    std::array<std::uint8_t, 4096> text_{};
    std::size_t cursor_ = 0;
};

} // namespace tlg::detail
