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

// Same wire format, different algorithms. Keep the simple paths available for
// experiments and for checking that a clever optimization really earns its keep.
enum class SlideParse { greedy, optimal };
enum class SlideMatch { adaptive, primary };
enum class SlideDecode { bulk, scalar };
enum class Tlg6FilterSearch { factored, reference, refined };

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
    SlideDecode slide_decode = SlideDecode::bulk;
};

struct EncodeOptions {
    Format format = Format::tlg6;
    std::size_t max_pixels = 268'435'456;
    std::size_t max_pixel_bytes = std::size_t{1} << 30;
    SlideParse slide_parse = SlideParse::greedy;
    Tlg6FilterSearch tlg6_filter_search = Tlg6FilterSearch::factored;
    // Limits optimal parsing's temporary state; fixed match indices and encoded
    // output are separate. The guarantee is the smallest individual Slide chunk.
    std::size_t max_slide_working_bytes = std::size_t{256} << 20;
    SlideMatch slide_match = SlideMatch::adaptive;
    // Refined search scores complete stripes, including byte padding, then
    // keeps the smaller complete file against the factored baseline.
    // Budget counts residual bytes examined by entropy scoring per Slide parse.
    std::size_t max_tlg6_refinement_work = std::size_t{64} << 20;
    // Compare complete greedy/optimal files, including TLG5 dictionary forks.
    // Requires enough optimal scratch for every Slide chunk.
    bool compare_slide_parses = false;
};

[[nodiscard]] Result<std::vector<std::uint8_t>> encode(
    ImageView image, const EncodeOptions& options = {}, const Tags& tags = {});
[[nodiscard]] Result<Document> decode(
    std::span<const std::uint8_t> data, const DecodeOptions& options = {});
// Signature-only inspection, including the raw image inside SDS. A successful
// probe does not validate the rest of the file; use decode for that.
[[nodiscard]] Result<Format> detect_format(std::span<const std::uint8_t> data);
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
