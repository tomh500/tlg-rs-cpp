#include "internal.hpp"

#include <charconv>
#include <fstream>
#include <new>
#include <stdexcept>
#include <type_traits>

namespace tlg {
namespace {

template<class T, class F>
Result<T> guarded(F&& operation) {
    try {
        if constexpr (std::is_void_v<T>) {
            operation();
            return {};
        } else {
            return operation();
        }
    } catch (const detail::Failure& failure) {
        return std::unexpected(failure.error);
    } catch (const std::bad_alloc&) {
        return std::unexpected(Error{ErrorCode::resource_limit, 0, "memory allocation failed"});
    } catch (const std::length_error&) {
        return std::unexpected(Error{ErrorCode::resource_limit, 0, "container size exceeds address space"});
    } catch (const std::ios_base::failure& failure) {
        return std::unexpected(Error{ErrorCode::io_error, 0, failure.what()});
    }
}

Format detect_format(std::span<const std::uint8_t> bytes) {
    detail::require(bytes.size() >= 11, ErrorCode::truncated_input, "missing TLG signature");
    const auto signature = bytes.first(11);
    if (std::ranges::equal(signature, detail::magic5)) return Format::tlg5;
    if (std::ranges::equal(signature, detail::magic6)) return Format::tlg6;
    detail::fail(ErrorCode::unsupported_format, "expected TLG5 or TLG6 raw signature");
}

Image decode_raw(std::span<const std::uint8_t> bytes, Format format,
                 const DecodeOptions& options) {
    return format == Format::tlg5 ? detail::decode5(bytes, options)
                                : detail::decode6(bytes, options);
}

std::size_t decimal_length(detail::ByteReader& reader) {
    std::size_t result = 0;
    bool any = false;
    while (true) {
        const auto at = reader.position();
        const auto byte = reader.u8();
        if (byte == ':') {
            detail::require(any, ErrorCode::corrupt_data, "empty tag length", at);
            return result;
        }
        detail::require(byte >= '0' && byte <= '9', ErrorCode::corrupt_data,
                        "tag length must be decimal followed by ':'", at);
        const auto digit = static_cast<std::size_t>(byte - '0');
        detail::require(result <= (std::numeric_limits<std::size_t>::max() - digit) / 10,
                        ErrorCode::corrupt_data, "tag length overflows address space", at);
        result = result * 10 + digit;
        any = true;
    }
}

std::string tag_string(detail::ByteReader& reader) {
    const auto bytes = reader.take(decimal_length(reader));
    return std::string(bytes.begin(), bytes.end());
}

void read_tags(std::span<const std::uint8_t> bytes, Tags& tags, std::size_t base) {
    detail::ByteReader reader(bytes, base);
    while (!reader.empty()) {
        auto key = tag_string(reader);
        const auto equals_offset = reader.position();
        detail::require(reader.u8() == '=', ErrorCode::corrupt_data,
                        "missing '=' after tag key", equals_offset);
        auto value = tag_string(reader);
        const auto comma_offset = reader.position();
        detail::require(reader.u8() == ',', ErrorCode::corrupt_data,
                        "missing ',' after tag value", comma_offset);
        const auto [it, inserted] = tags.emplace(std::move(key), std::move(value));
        (void)it;
        detail::require(inserted, ErrorCode::corrupt_data, "duplicate metadata key", reader.position());
    }
}

void append_text(detail::ByteWriter& writer, std::string_view value) {
    writer.append({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}

void write_tag_string(detail::ByteWriter& writer, std::string_view value) {
    std::array<char, 32> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.size());
    append_text(writer, {buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())});
    writer.u8(':');
    append_text(writer, value);
}

} // namespace

Result<std::vector<std::uint8_t>> encode(ImageView image, const EncodeOptions& options,
                                       const Tags& tags) {
    return guarded<std::vector<std::uint8_t>>([&] {
        const auto c = channels(image.layout);
        detail::require(c == 1 || c == 3 || c == 4, ErrorCode::invalid_argument,
                        "channel count must be 1, 3, or 4");
        detail::require(image.width != 0 && image.height != 0, ErrorCode::invalid_argument,
                        "image dimensions must be positive");
        const auto count = std::uint64_t{image.width} * image.height;
        detail::require(count <= std::numeric_limits<std::size_t>::max() / c,
                        ErrorCode::invalid_argument, "image size overflows address space");
        detail::require(static_cast<std::size_t>(count) * c == image.pixels.size(),
                        ErrorCode::invalid_argument, "pixel buffer length does not match dimensions and layout");
        auto limits = DecodeOptions{};
        limits.max_pixels = options.max_pixels;
        limits.max_decoded_bytes = options.max_pixel_bytes;
        (void)detail::checked_image_size(image.width, image.height, image.layout, limits);
        detail::require(options.format == Format::tlg5 || options.format == Format::tlg6,
                        ErrorCode::invalid_argument, "unknown encoding format");
        auto raw = options.format == Format::tlg5 ? detail::encode5(image) : detail::encode6(image);
        if (tags.empty()) return raw;
        detail::ByteWriter metadata;
        for (const auto& [key, value] : tags) {
            write_tag_string(metadata, key);
            metadata.u8('=');
            write_tag_string(metadata, value);
            metadata.u8(',');
        }
        detail::ByteWriter writer;
        writer.append(detail::magic_sds);
        writer.u32(detail::checked_u32(raw.size()));
        writer.append(raw);
        append_text(writer, "tags");
        writer.u32(detail::checked_u32(metadata.data.size()));
        writer.append(metadata.data);
        return std::move(writer.data);
    });
}

Result<Document> decode(std::span<const std::uint8_t> data, const DecodeOptions& options) {
    return guarded<Document>([&] {
        detail::require(data.size() <= options.max_input_bytes, ErrorCode::resource_limit,
                        "input exceeds encoded byte limit");
        detail::require(data.size() >= 11, ErrorCode::truncated_input, "missing TLG signature");
        if (!std::ranges::equal(data.first(11), detail::magic_sds)) {
            const auto format = detect_format(data);
            return Document{decode_raw(data, format, options), {}, format};
        }
        detail::ByteReader reader(data);
        reader.expect(detail::magic_sds);
        const auto raw_size = reader.u32();
        const auto raw = reader.take(raw_size);
        Document document;
        try {
            document.format = detect_format(raw);
            // The SDS raw_size is a hard boundary, regardless of outer trailing-data policy.
            auto raw_options = options;
            raw_options.reject_trailing_data = true;
            document.image = decode_raw(raw, document.format, raw_options);
        } catch (detail::Failure& failure) {
            failure.error.offset += 15;
            throw;
        }
        const auto metadata_start = reader.position();
        detail::require(reader.remaining() <= options.max_metadata_bytes, ErrorCode::resource_limit,
                        "SDS chunks exceed metadata byte limit", metadata_start);
        while (!reader.empty()) {
            const auto name = reader.take(4);
            const auto size = reader.u32();
            const auto base = reader.position();
            const auto payload = reader.take(size);
            constexpr std::array<std::uint8_t, 4> tags_name{'t','a','g','s'};
            if (std::ranges::equal(name, tags_name)) read_tags(payload, document.tags, base);
            // The SDS container permits extension chunks; their size still bounds every read.
        }
        return document;
    });
}

Result<Document> read_file(const std::filesystem::path& path, const DecodeOptions& options) {
    return guarded<Document>([&]() -> Document {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        detail::require(static_cast<bool>(stream), ErrorCode::io_error, "cannot open input file");
        const auto end = stream.tellg();
        detail::require(end >= 0, ErrorCode::io_error, "cannot determine input length");
        const auto length = static_cast<std::uint64_t>(end);
        detail::require(length <= options.max_input_bytes &&
                        length <= std::numeric_limits<std::size_t>::max() &&
                        length <= static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()),
                        ErrorCode::resource_limit, "file exceeds encoded byte limit");
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        stream.seekg(0);
        if (!bytes.empty())
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        detail::require(static_cast<bool>(stream), ErrorCode::io_error, "cannot read complete input file");
        auto result = decode(bytes, options);
        if (!result) throw detail::Failure{std::move(result.error())};
        return std::move(*result);
    });
}

Result<void> write_file(const std::filesystem::path& path, ImageView image,
                       const EncodeOptions& options, const Tags& tags) {
    auto result = encode(image, options, tags);
    if (!result) return std::unexpected(std::move(result.error()));
    return guarded<void>([&] {
        detail::require(result->size() <= static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()),
                        ErrorCode::resource_limit, "output exceeds stream size limit");
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        detail::require(static_cast<bool>(stream), ErrorCode::io_error, "cannot open output file");
        stream.write(reinterpret_cast<const char*>(result->data()),
                     static_cast<std::streamsize>(result->size()));
        stream.close();
        detail::require(static_cast<bool>(stream), ErrorCode::io_error, "cannot write complete output file");
    });
}

const char* error_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::invalid_argument: return "invalid_argument";
    case ErrorCode::invalid_header: return "invalid_header";
    case ErrorCode::truncated_input: return "truncated_input";
    case ErrorCode::corrupt_data: return "corrupt_data";
    case ErrorCode::unsupported_format: return "unsupported_format";
    case ErrorCode::resource_limit: return "resource_limit";
    case ErrorCode::io_error: return "io_error";
    }
    return "unknown_error";
}

} // namespace tlg
