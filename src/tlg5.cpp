#include "internal.hpp"
#include "slide.hpp"

#include <algorithm>
#include <array>

namespace tlg::detail {
namespace {

constexpr std::size_t encoded_block_height = 4;

PixelLayout read_layout(std::uint8_t colors) {
    switch (colors) {
    case 1: return PixelLayout::gray;
    case 3: return PixelLayout::rgb;
    case 4: return PixelLayout::rgba;
    default: fail(ErrorCode::invalid_header, "TLG5 channel count must be 1, 3, or 4", 11);
    }
}

// The layout is selected once per block. A constant channel count lets the
// compiler unroll these tiny loops instead of dispatching for every pixel.
template<std::size_t colors>
void predict(ImageView image, std::size_t first_row, std::size_t end_row,
             std::array<std::vector<std::uint8_t>, 4>& planes) {
    const auto width = std::size_t(image.width);
    const auto stride = width * colors;
    std::size_t i = 0;
    for (auto y = first_row; y < end_row; ++y) {
        const auto* row = image.pixels.data() + y * stride;
        const auto* upper = y == 0 ? nullptr : row - stride;
        std::array<std::uint8_t, colors> previous{};
        for (std::size_t x = 0; x < width; ++x, ++i) {
            std::array<std::uint8_t, colors> difference{};
            for (std::size_t c = 0; c < colors; ++c) {
                const auto offset = x * colors + c;
                const auto vertical = static_cast<std::uint8_t>(row[offset] - (upper ? upper[offset] : 0));
                difference[c] = static_cast<std::uint8_t>(vertical - previous[c]);
                previous[c] = vertical;
            }
            // Predict vertically, then horizontally. Color planes use
            // (B-G), G, (R-G), A; public pixels remain RGB/RGBA.
            if constexpr (colors == 1) {
                planes[0][i] = difference[0];
            } else {
                planes[0][i] = static_cast<std::uint8_t>(difference[2] - difference[1]);
                planes[1][i] = difference[1];
                planes[2][i] = static_cast<std::uint8_t>(difference[0] - difference[1]);
                if constexpr (colors == 4) planes[3][i] = difference[3];
            }
        }
    }
}

template<std::size_t colors>
void reconstruct(Image& image, std::size_t first_row, std::size_t end_row,
                 const std::array<std::span<const std::uint8_t>, 4>& planes) {
    const auto width = std::size_t(image.width);
    const auto stride = width * colors;
    std::size_t i = 0;
    for (auto y = first_row; y < end_row; ++y) {
        auto* row = image.pixels.data() + y * stride;
        const auto* upper = y == 0 ? nullptr : row - stride;
        std::array<std::uint8_t, colors> previous{};
        for (std::size_t x = 0; x < width; ++x, ++i) {
            std::array<std::uint8_t, colors> difference{};
            if constexpr (colors == 1) {
                difference[0] = planes[0][i];
            } else {
                const auto green = planes[1][i];
                difference[0] = static_cast<std::uint8_t>(planes[2][i] + green);
                difference[1] = green;
                difference[2] = static_cast<std::uint8_t>(planes[0][i] + green);
                if constexpr (colors == 4) difference[3] = planes[3][i];
            }
            for (std::size_t c = 0; c < colors; ++c) {
                const auto offset = x * colors + c;
                previous[c] = static_cast<std::uint8_t>(previous[c] + difference[c]);
                row[offset] = static_cast<std::uint8_t>(previous[c] + (upper ? upper[offset] : 0));
            }
        }
    }
}

} // namespace

std::vector<std::uint8_t> encode5(ImageView image) {
    const auto colors = channels(image.layout);
    const auto width = std::size_t(image.width);
    const auto height = std::size_t(image.height);
    const auto block_count = height / encoded_block_height + (height % encoded_block_height != 0);

    ByteWriter writer;
    writer.append(magic5);
    writer.u8(static_cast<std::uint8_t>(colors));
    writer.u32(image.width);
    writer.u32(image.height);
    writer.u32(static_cast<std::uint32_t>(encoded_block_height));
    const auto table_offset = writer.data.size();
    for (std::size_t i = 0; i < block_count; ++i) writer.u32(0);

    std::array<std::vector<std::uint8_t>, 4> planes;
    const auto plane_capacity = width * std::min(height, encoded_block_height);
    for (std::size_t c = 0; c < colors; ++c) planes[c].resize(plane_capacity);
    SlideEncoder slide;

    for (std::size_t block = 0; block < block_count; ++block) {
        const auto first_row = block * encoded_block_height;
        const auto end_row = first_row + std::min(encoded_block_height, height - first_row);
        const auto plane_length = width * (end_row - first_row);
        switch (image.layout) {
        case PixelLayout::gray: predict<1>(image, first_row, end_row, planes); break;
        case PixelLayout::rgb: predict<3>(image, first_row, end_row, planes); break;
        case PixelLayout::rgba: predict<4>(image, first_row, end_row, planes); break;
        }

        const auto block_start = writer.data.size();
        for (std::size_t c = 0; c < colors; ++c) {
            const auto raw = std::span<const std::uint8_t>(planes[c]).first(plane_length);
            slide.store();
            const auto compressed = slide.encode(raw);
            if (compressed.size() < raw.size()) {
                writer.u8(0);
                writer.u32(checked_u32(compressed.size()));
                writer.append(compressed);
            } else {
                // Raw planes do not advance the decoder's dictionary. Rolling
                // back here keeps later compressed channels in the same state.
                slide.restore();
                writer.u8(1);
                writer.u32(checked_u32(raw.size()));
                writer.append(raw);
            }
        }
        writer.patch_u32(table_offset + block * 4, checked_u32(writer.data.size() - block_start));
    }
    return std::move(writer.data);
}

Image decode5(std::span<const std::uint8_t> data, const DecodeOptions& options) {
    require(data.size() <= options.max_input_bytes, ErrorCode::resource_limit,
            "TLG5 input exceeds the input byte limit");
    ByteReader reader(data);
    reader.expect(magic5);
    const auto layout = read_layout(reader.u8());
    const auto width_field = reader.u32();
    const auto height_field = reader.u32();
    const auto block_height_field = reader.u32();
    require(block_height_field != 0, ErrorCode::invalid_header,
            "TLG5 block height must be positive", 20);
    const auto output_size = checked_image_size(width_field, height_field, layout, options);

    const auto width = std::size_t(width_field);
    const auto height = std::size_t(height_field);
    const auto block_height = std::size_t(block_height_field);
    const auto colors = channels(layout);
    // TLG5 permits any positive block height, including a single whole-image
    // block. Division avoids overflow from height + block_height - 1.
    const auto block_count = height / block_height + (height % block_height != 0);
    require(block_count <= reader.remaining() / 4, ErrorCode::truncated_input,
            "TLG5 block size table is truncated", reader.position());
    const auto table_offset = reader.position();
    ByteReader sizes(reader.take(block_count * 4), table_offset);

    // Validate every block and compressed token stream before trusting the
    // dimensions enough to allocate the full image or its channel buffers.
    auto framing = reader;
    auto framing_sizes = sizes;
    for (std::size_t block = 0, first_row = 0; block < block_count; ++block) {
        const auto rows = std::min(block_height, height - first_row);
        const auto plane_length = width * rows;
        const auto block_size = std::size_t(framing_sizes.u32());
        const auto block_offset = framing.position();
        ByteReader block_reader(framing.take(block_size), block_offset);
        for (std::size_t c = 0; c < colors; ++c) {
            const auto flag_offset = block_reader.position();
            const auto flag = block_reader.u8();
            require(flag <= 1, ErrorCode::corrupt_data,
                    "TLG5 compression flag must be 0 or 1", flag_offset);
            const auto size = std::size_t(block_reader.u32());
            if (flag == 1) {
                require(size == plane_length, ErrorCode::corrupt_data,
                        "TLG5 raw plane has an incorrect length", flag_offset + 1);
                block_reader.take(size);
            } else {
                const auto compressed_offset = block_reader.position();
                const auto compressed = block_reader.take(size);
                try { validate_slide(compressed, plane_length); }
                catch (Failure& failure) {
                    failure.error.offset += compressed_offset;
                    throw;
                }
            }
        }
        require(block_reader.empty(), ErrorCode::corrupt_data,
                "TLG5 block size does not match its channel payloads", block_reader.position());
        first_row += rows;
    }
    require(!options.reject_trailing_data || framing.empty(), ErrorCode::corrupt_data,
            "TLG5 file has trailing bytes", framing.position());

    Image image{width_field, height_field, layout, std::vector<std::uint8_t>(output_size)};
    SlideDecoder slide;
    std::array<std::vector<std::uint8_t>, 4> storage;
    std::array<std::span<const std::uint8_t>, 4> planes;

    for (std::size_t block = 0, first_row = 0; block < block_count; ++block) {
        const auto rows = std::min(block_height, height - first_row);
        const auto end_row = first_row + rows;
        const auto plane_length = width * rows;
        const auto block_size = std::size_t(sizes.u32());
        const auto block_offset = reader.position();
        // Channel parsing is bounded by this block, so a corrupt channel size
        // cannot silently consume the next block's headers.
        ByteReader block_reader(reader.take(block_size), block_offset);
        for (std::size_t c = 0; c < colors; ++c) {
            const auto flag_offset = block_reader.position();
            const auto flag = block_reader.u8();
            require(flag <= 1, ErrorCode::corrupt_data,
                    "TLG5 compression flag must be 0 or 1", flag_offset);
            const auto size = std::size_t(block_reader.u32());
            if (flag == 1) {
                require(size == plane_length, ErrorCode::corrupt_data,
                        "TLG5 raw plane has an incorrect length", flag_offset + 1);
                planes[c] = block_reader.take(size); // Raw data needs no copy.
            } else {
                const auto compressed_offset = block_reader.position();
                const auto compressed = block_reader.take(size);
                try {
                    storage[c] = slide.decode(compressed, plane_length);
                } catch (Failure& failure) {
                    failure.error.offset += compressed_offset;
                    throw;
                }
                planes[c] = storage[c];
            }
        }
        require(block_reader.empty(), ErrorCode::corrupt_data,
                "TLG5 block size does not match its channel payloads", block_reader.position());

        switch (layout) {
        case PixelLayout::gray: reconstruct<1>(image, first_row, end_row, planes); break;
        case PixelLayout::rgb: reconstruct<3>(image, first_row, end_row, planes); break;
        case PixelLayout::rgba: reconstruct<4>(image, first_row, end_row, planes); break;
        }
        first_row = end_row;
    }
    require(!options.reject_trailing_data || reader.empty(), ErrorCode::corrupt_data,
            "TLG5 file has trailing bytes", reader.position());
    return image;
}

} // namespace tlg::detail
