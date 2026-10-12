#pragma once

#include <tlg/tlg.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>
#include <utility>

namespace tlg::detail {

inline constexpr std::array<std::uint8_t, 11> magic5{
    'T','L','G','5','.','0',0,'r','a','w',0x1a};
inline constexpr std::array<std::uint8_t, 11> magic6{
    'T','L','G','6','.','0',0,'r','a','w',0x1a};
inline constexpr std::array<std::uint8_t, 11> magic_sds{
    'T','L','G','0','.','0',0,'s','d','s',0x1a};

struct Failure { Error error; };

[[noreturn]] inline void fail(ErrorCode code, std::string message,
                              std::size_t offset = 0) {
    throw Failure{{code, offset, std::move(message)}};
}

inline void require(bool condition, ErrorCode code, std::string_view message,
                    std::size_t offset = 0) {
    if (!condition) fail(code, std::string(message), offset);
}

inline std::size_t checked_image_size(std::uint32_t width, std::uint32_t height,
                                     PixelLayout layout, const DecodeOptions& options,
                                     std::size_t dimension_offset = 12) {
    const auto c = channels(layout);
    require(c == 1 || c == 3 || c == 4, ErrorCode::invalid_header,
            "channel count must be 1, 3, or 4", 11);
    require(width != 0 && height != 0, ErrorCode::invalid_header,
            "image dimensions must be positive", dimension_offset);
    require(std::uint64_t{width} <= std::numeric_limits<std::size_t>::max() - 7 &&
            std::uint64_t{height} <= std::numeric_limits<std::size_t>::max() - 7,
            ErrorCode::resource_limit, "dimensions overflow block rounding", dimension_offset);
    const auto pixels = std::uint64_t{width} * height;
    require(pixels <= options.max_pixels, ErrorCode::resource_limit,
            "image exceeds pixel limit", dimension_offset);
    require(pixels <= std::numeric_limits<std::size_t>::max() / c,
            ErrorCode::resource_limit, "image size overflows address space", dimension_offset);
    const auto bytes = static_cast<std::size_t>(pixels) * c;
    require(bytes <= options.max_decoded_bytes, ErrorCode::resource_limit,
            "image exceeds decoded byte limit", dimension_offset);
    return bytes;
}

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data, std::size_t base = 0)
        : data_(data), base_(base) {}

    [[nodiscard]] std::size_t position() const noexcept { return base_ + cursor_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - cursor_; }
    [[nodiscard]] bool empty() const noexcept { return cursor_ == data_.size(); }

    std::span<const std::uint8_t> take(std::size_t count) {
        require(count <= remaining(), ErrorCode::truncated_input,
                "input ended before the declared field length", position());
        const auto result = data_.subspan(cursor_, count);
        cursor_ += count;
        return result;
    }
    std::uint8_t u8() { return take(1)[0]; }
    std::uint32_t u32() {
        const auto b = take(4);
        return std::uint32_t{b[0]} | (std::uint32_t{b[1]} << 8)
             | (std::uint32_t{b[2]} << 16) | (std::uint32_t{b[3]} << 24);
    }
    void expect(std::span<const std::uint8_t> expected) {
        const auto at = position();
        const auto actual = take(expected.size());
        require(std::ranges::equal(actual, expected), ErrorCode::invalid_header,
                "invalid file signature", at);
    }
private:
    std::span<const std::uint8_t> data_;
    std::size_t base_{};
    std::size_t cursor_{};
};

struct ByteWriter {
    std::vector<std::uint8_t> data;
    void u8(std::uint8_t value) { data.push_back(value); }
    void u32(std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            data.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void append(std::span<const std::uint8_t> bytes) {
        data.insert(data.end(), bytes.begin(), bytes.end());
    }
    void patch_u32(std::size_t offset, std::uint32_t value) {
        require(offset <= data.size() && data.size() - offset >= 4,
                ErrorCode::invalid_argument, "invalid patch offset");
        for (unsigned i = 0; i < 4; ++i)
            data[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
};

inline std::uint32_t checked_u32(std::size_t value) {
    require(value <= std::numeric_limits<std::uint32_t>::max(),
            ErrorCode::resource_limit, "field exceeds the 32-bit format limit");
    return static_cast<std::uint32_t>(value);
}

std::vector<std::uint8_t> encode5(ImageView image, const EncodeOptions& options);
std::vector<std::uint8_t> encode6(ImageView image, const EncodeOptions& options);
Image decode5(std::span<const std::uint8_t> data, const DecodeOptions& options);
Image decode6(std::span<const std::uint8_t> data, const DecodeOptions& options);

} // namespace tlg::detail
