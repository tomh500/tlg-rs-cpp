#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace tlg {

enum class PixelLayout : std::uint8_t { gray = 1, rgb = 3, rgba = 4 };
enum class Format { tlg5, tlg6 };

// Pixels are tightly packed, top to bottom, in Gray/RGB/RGBA order.
// A view borrows its storage; encode never takes ownership or modifies it.
struct ImageView {
    std::uint32_t width{};
    std::uint32_t height{};
    PixelLayout layout{PixelLayout::rgba};
    std::span<const std::uint8_t> pixels;
};

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    PixelLayout layout{PixelLayout::rgba};
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] ImageView view() const & noexcept {
        return {width, height, layout, pixels};
    }
    ImageView view() const && = delete; // A temporary cannot own a borrowed view.
};

// Sorted keys make SDS output reproducible. Lengths count bytes, not characters.
using Tags = std::map<std::string, std::string, std::less<>>;

struct Document {
    Image image;
    Tags tags;
    Format format{Format::tlg6};
};

enum class ErrorCode {
    invalid_argument,
    invalid_header,
    truncated_input,
    corrupt_data,
    unsupported_format,
    resource_limit,
    io_error
};

struct Error {
    ErrorCode code;
    std::size_t offset; // Byte offset in the input, when applicable.
    std::string message;
};

template<class T>
using Result = std::expected<T, Error>;

struct DecodeOptions {
    std::size_t max_pixels = 268'435'456;
    std::size_t max_decoded_bytes = std::size_t{1} << 30;
    std::size_t max_input_bytes = std::size_t{1} << 30;
    std::size_t max_metadata_bytes = std::size_t{16} << 20;
    bool reject_trailing_data = true;
};

struct EncodeOptions {
    Format format = Format::tlg6;
    std::size_t max_pixels = 268'435'456;
    std::size_t max_pixel_bytes = std::size_t{1} << 30;
};

[[nodiscard]] Result<std::vector<std::uint8_t>> encode(
    ImageView image, const EncodeOptions& options = {}, const Tags& tags = {});
[[nodiscard]] Result<Document> decode(
    std::span<const std::uint8_t> data, const DecodeOptions& options = {});
[[nodiscard]] Result<Document> read_file(
    const std::filesystem::path& path, const DecodeOptions& options = {});
// Encoding completes before the destination is opened.
[[nodiscard]] Result<void> write_file(
    const std::filesystem::path& path, ImageView image,
    const EncodeOptions& options = {}, const Tags& tags = {});
[[nodiscard]] const char* error_name(ErrorCode code) noexcept;
[[nodiscard]] constexpr std::size_t channels(PixelLayout layout) noexcept {
    return static_cast<std::uint8_t>(layout);
}

} // namespace tlg
