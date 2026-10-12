#include "portable_io.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace tlg_tool {
namespace {

// Byte layouts: https://qoiformat.org/qoi-specification.pdf and
// https://git.suckless.org/farbfeld/file/FORMAT.html.
// TGA follows the Truevision 2.0 header/packet layout; image origins are
// normalized here so the codec always gets the same top-to-bottom pixels.

class Reader {
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint8_t u8() { if (offset_ == bytes_.size()) throw std::runtime_error("truncated image payload"); return bytes_[offset_++]; }
    std::uint16_t le16() { auto a = u8(); return a | (std::uint16_t(u8()) << 8); }
    std::uint16_t be16() { auto a = u8(); return (std::uint16_t(a) << 8) | u8(); }
    std::uint32_t le32() { auto a = le16(); return a | (std::uint32_t(le16()) << 16); }
    std::uint32_t be32() { auto a = be16(); return (std::uint32_t(a) << 16) | be16(); }
    void skip(std::size_t size) { if (size > bytes_.size() - offset_) throw std::runtime_error("truncated image header"); offset_ += size; }
    bool empty() const { return offset_ == bytes_.size(); }
    std::size_t remaining() const { return bytes_.size() - offset_; }
};

std::size_t pixel_count(std::uint32_t width, std::uint32_t height, unsigned colors) {
    const auto count = std::uint64_t(width) * height;
    if (width == 0 || height == 0 || count > 268'435'456 || count > (std::uint64_t{1} << 30) / colors)
        throw std::runtime_error("image exceeds the tool's pixel or 1 GiB byte limit");
    return static_cast<std::size_t>(count);
}

bool starts(std::span<const std::uint8_t> bytes, std::string_view magic) noexcept {
    return bytes.size() >= magic.size() && std::equal(magic.begin(), magic.end(), bytes.begin());
}

void be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (unsigned shift : {24u, 16u, 8u, 0u}) out.push_back(static_cast<std::uint8_t>(value >> shift));
}

using Pixel = std::array<std::uint8_t, 4>;
unsigned hash(const Pixel& pixel) { return (pixel[0] * 3 + pixel[1] * 5 + pixel[2] * 7 + pixel[3] * 11) & 63; }
Pixel get_pixel(tlg::ImageView image, std::size_t index) {
    const auto colors = tlg::channels(image.layout);
    const auto* pixel = image.pixels.data() + index * colors;
    return colors == 1 ? Pixel{pixel[0], pixel[0], pixel[0], 255} :
           Pixel{pixel[0], pixel[1], pixel[2], colors == 4 ? pixel[3] : std::uint8_t{255}};
}

tlg::Image read_qoi(std::span<const std::uint8_t> bytes) {
    Reader reader(bytes); reader.skip(4);
    const auto width = reader.be32(), height = reader.be32();
    const auto colors = reader.u8();
    if (colors != 3 && colors != 4) throw std::runtime_error("QOI channels must be 3 or 4");
    if (reader.u8() > 1) throw std::runtime_error("invalid QOI colorspace");
    const auto count = pixel_count(width, height, colors);
    if (bytes.size() < 22 || count > (bytes.size() - 22) * 62)
        throw std::runtime_error("QOI pixel count cannot fit its payload");
    tlg::Image image{width, height, colors == 3 ? tlg::PixelLayout::rgb : tlg::PixelLayout::rgba,
                     std::vector<std::uint8_t>(count * colors)};
    std::array<Pixel, 64> index{};
    Pixel pixel{0, 0, 0, 255};
    std::size_t output = 0;
    while (output < count) {
        const auto operation = reader.u8();
        unsigned run = 1;
        if (operation == 0xfe) { pixel[0] = reader.u8(); pixel[1] = reader.u8(); pixel[2] = reader.u8(); }
        else if (operation == 0xff) for (auto& color : pixel) color = reader.u8();
        else switch (operation & 0xc0) {
        case 0x00: pixel = index[operation & 63]; break;
        case 0x40:
            pixel[0] += ((operation >> 4) & 3) - 2;
            pixel[1] += ((operation >> 2) & 3) - 2;
            pixel[2] += (operation & 3) - 2;
            break;
        case 0x80: {
            const auto second = reader.u8();
            const int green = (operation & 63) - 32;
            pixel[0] += green + (second >> 4) - 8;
            pixel[1] += green;
            pixel[2] += green + (second & 15) - 8;
            break;
        }
        case 0xc0: run = (operation & 63) + 1; break;
        }
        if (run > count - output) throw std::runtime_error("QOI run exceeds image dimensions");
        index[hash(pixel)] = pixel;
        while (run-- != 0) {
            std::copy_n(pixel.begin(), colors, image.pixels.begin() + output * colors);
            ++output;
        }
    }
    for (unsigned i = 0; i < 8; ++i)
        if (reader.u8() != (i == 7 ? 1 : 0)) throw std::runtime_error("invalid QOI end marker");
    if (!reader.empty()) throw std::runtime_error("QOI file has trailing data");
    return image;
}

std::vector<std::uint8_t> encode_qoi(tlg::ImageView image) {
    const auto colors = image.layout == tlg::PixelLayout::rgba ? 4 : 3;
    std::vector<std::uint8_t> out{'q','o','i','f'};
    be32(out, image.width); be32(out, image.height);
    out.push_back(static_cast<std::uint8_t>(colors)); out.push_back(0);
    std::array<Pixel, 64> index{};
    Pixel previous{0, 0, 0, 255};
    unsigned run = 0;
    const auto count = std::size_t(image.width) * image.height;
    for (std::size_t i = 0; i < count; ++i) {
        const auto pixel = get_pixel(image, i);
        if (pixel == previous) {
            if (++run == 62 || i + 1 == count) { out.push_back(static_cast<std::uint8_t>(0xc0 | (run - 1))); run = 0; }
            continue;
        }
        if (run != 0) { out.push_back(static_cast<std::uint8_t>(0xc0 | (run - 1))); run = 0; }
        const auto slot = hash(pixel);
        if (index[slot] == pixel) out.push_back(static_cast<std::uint8_t>(slot));
        else {
            index[slot] = pixel;
            if (pixel[3] != previous[3]) {
                out.push_back(0xff); out.insert(out.end(), pixel.begin(), pixel.end());
            } else {
                const auto red = static_cast<std::int8_t>(pixel[0] - previous[0]);
                const auto green = static_cast<std::int8_t>(pixel[1] - previous[1]);
                const auto blue = static_cast<std::int8_t>(pixel[2] - previous[2]);
                const auto rg = red - green, bg = blue - green;
                if (red >= -2 && red <= 1 && green >= -2 && green <= 1 && blue >= -2 && blue <= 1)
                    out.push_back(static_cast<std::uint8_t>(0x40 | ((red + 2) << 4) | ((green + 2) << 2) | (blue + 2)));
                else if (green >= -32 && green <= 31 && rg >= -8 && rg <= 7 && bg >= -8 && bg <= 7) {
                    out.push_back(static_cast<std::uint8_t>(0x80 | (green + 32)));
                    out.push_back(static_cast<std::uint8_t>(((rg + 8) << 4) | (bg + 8)));
                } else { out.push_back(0xfe); out.insert(out.end(), pixel.begin(), pixel.begin() + 3); }
            }
        }
        previous = pixel;
    }
    out.insert(out.end(), {0,0,0,0,0,0,0,1});
    return out;
}

tlg::Image read_ff(std::span<const std::uint8_t> bytes, bool allow_lossy) {
    Reader reader(bytes); reader.skip(8);
    const auto width = reader.be32(), height = reader.be32();
    const auto count = pixel_count(width, height, 4);
    if (bytes.size() - 16 != count * 8) throw std::runtime_error("farbfeld raster length is incorrect");
    tlg::Image image{width, height, tlg::PixelLayout::rgba, std::vector<std::uint8_t>(count * 4)};
    for (auto& pixel : image.pixels) {
        const auto value = reader.be16();
        if (!allow_lossy && value % 257 != 0)
            throw std::runtime_error("farbfeld sample cannot map exactly to 8 bits; pass --allow-lossy to normalize");
        pixel = static_cast<std::uint8_t>((std::uint32_t(value) + 128) / 257);
    }
    return image;
}

tlg::Image read_tga(std::span<const std::uint8_t> bytes) {
    // A footer is not extra raster. Otherwise a chopped image could "borrow"
    // its missing colors from the footer and look successfully decoded.
    constexpr std::array<std::uint8_t, 18> footer_magic{'T','R','U','E','V','I','S','I','O','N','-','X','F','I','L','E','.',0};
    if (bytes.size() >= 26 && std::ranges::equal(bytes.last(18), footer_magic)) {
        const auto footer_start = bytes.size() - 26;
        Reader footer(bytes.last(26));
        const auto extension = footer.le32(), developer = footer.le32();
        auto raster_end = footer_start;
        for (const auto offset : {extension, developer}) if (offset != 0) {
            if (offset < 18 || offset > footer_start) throw std::runtime_error("TGA footer points outside the file body");
            raster_end = std::min(raster_end, static_cast<std::size_t>(offset));
        }
        bytes = bytes.first(raster_end);
    }
    Reader reader(bytes);
    const auto id = reader.u8(), map_type = reader.u8(), type = reader.u8();
    const auto map_first = reader.le16(), map_size = reader.le16();
    const auto map_bits = reader.u8();
    reader.skip(4);
    const auto width = reader.le16(), height = reader.le16();
    const auto bits = reader.u8(), descriptor = reader.u8();
    const auto basic = type & 7;
    const bool rle = type == 9 || type == 10 || type == 11;
    if ((basic < 1 || basic > 3) || (!rle && type != basic) || map_type > 1 || (descriptor & 0xc0) != 0)
        throw std::runtime_error("unsupported TGA image type/interleaving");
    if ((basic == 1 && (map_type != 1 || (bits != 8 && bits != 16))) ||
        (basic == 2 && bits != 15 && bits != 16 && bits != 24 && bits != 32) ||
        (basic == 3 && bits != 8 && bits != 16)) throw std::runtime_error("unsupported TGA pixel depth");
    const auto attributes = descriptor & 15;
    const auto color_depth = basic == 1 ? map_bits : bits;
    if ((basic == 3 && bits == 16 && attributes != 8) ||
        (color_depth == 32 && attributes != 0 && attributes != 8) ||
        (color_depth == 24 && attributes != 0) ||
        ((color_depth == 15 || color_depth == 16) && basic != 3 && attributes > 1) ||
        (color_depth == 8 && attributes != 0 && attributes != 8))
        throw std::runtime_error("unsupported TGA attribute bits");
    const bool alpha = basic == 1 ? map_bits == 32 || (map_bits == 16 && (descriptor & 15) != 0) :
                       basic == 3 ? bits == 16 : bits == 32 || (bits == 16 && (descriptor & 15) != 0);
    const auto layout = basic == 3 && !alpha ? tlg::PixelLayout::gray :
                        alpha ? tlg::PixelLayout::rgba : tlg::PixelLayout::rgb;
    const auto colors = tlg::channels(layout);
    const auto count = pixel_count(width, height, static_cast<unsigned>(colors));
    reader.skip(id);
    auto color = [&](unsigned depth) {
        Pixel pixel{0, 0, 0, 255};
        if (depth == 16 || depth == 15) {
            const auto value = reader.le16();
            for (unsigned c = 0; c < 3; ++c) {
                const auto five = (value >> ((2 - c) * 5)) & 31;
                pixel[c] = static_cast<std::uint8_t>((five << 3) | (five >> 2));
            }
            if (alpha && depth == 16) pixel[3] = value & 0x8000 ? 255 : 0;
        } else if (depth == 24 || depth == 32) {
            pixel[2] = reader.u8(); pixel[1] = reader.u8(); pixel[0] = reader.u8();
            if (depth == 32) pixel[3] = reader.u8();
        } else throw std::runtime_error("unsupported TGA palette depth");
        return pixel;
    };
    std::vector<Pixel> palette;
    if (map_type == 1) {
        palette.reserve(map_size);
        for (unsigned i = 0; i < map_size; ++i) palette.push_back(color(map_bits));
    }
    auto pixel = [&] {
        if (basic == 1) {
            const auto entry = bits == 8 ? reader.u8() : reader.le16();
            if (entry < map_first || entry - map_first >= palette.size()) throw std::runtime_error("TGA palette index is out of range");
            return palette[entry - map_first];
        }
        if (basic == 3) { const auto gray = reader.u8(); return Pixel{gray, gray, gray, bits == 16 ? reader.u8() : std::uint8_t{255}}; }
        return color(bits);
    };
    const auto sample_bytes = (bits + 7) / 8;
    if ((!rle && count > reader.remaining() / sample_bytes) ||
        (rle && count > (reader.remaining() / (sample_bytes + 1)) * 128))
        throw std::runtime_error("TGA dimensions cannot fit the encoded raster");
    tlg::Image image{width, height, layout, std::vector<std::uint8_t>(count * colors)};
    std::size_t consumed = 0;
    while (consumed < count) {
        const auto packet = rle ? reader.u8() : std::uint8_t{};
        auto run = rle ? (packet & 127) + 1u : 1u;
        if (run > count - consumed) throw std::runtime_error("TGA RLE packet exceeds image dimensions");
        const auto repeated = rle && (packet & 128) != 0;
        Pixel current{};
        if (repeated) current = pixel();
        while (run-- != 0) {
            if (!repeated) current = pixel();
            auto x = consumed % width, y = consumed / width;
            if ((descriptor & 0x10) != 0) x = width - 1 - x;
            if ((descriptor & 0x20) == 0) y = height - 1 - y;
            std::copy_n(current.begin(), colors, image.pixels.begin() + (y * width + x) * colors);
            ++consumed;
        }
    }
    return image;
}

} // namespace

bool is_portable_extension(std::string_view extension) noexcept {
    return extension == ".qoi" || extension == ".ff" || extension == ".tga";
}

bool is_portable_image(std::span<const std::uint8_t> bytes, std::string_view extension) noexcept {
    return starts(bytes, "qoif") || starts(bytes, "farbfeld") || extension == ".tga";
}

tlg::Image read_portable_image(std::span<const std::uint8_t> bytes, std::string_view extension, bool allow_lossy) {
    if (bytes.size() > (std::size_t{1} << 30)) throw std::runtime_error("portable image exceeds the 1 GiB input limit");
    if (starts(bytes, "qoif")) return read_qoi(bytes);
    if (starts(bytes, "farbfeld")) return read_ff(bytes, allow_lossy);
    if (extension == ".tga") return read_tga(bytes);
    throw std::runtime_error("unsupported portable image signature");
}

std::vector<std::uint8_t> encode_portable_image(tlg::ImageView image, std::string_view extension) {
    if (extension == ".qoi") return encode_qoi(image);
    if (extension == ".ff") {
        if (std::uint64_t(image.width) * image.height > ((std::uint64_t{1} << 30) - 16) / 8)
            throw std::runtime_error("farbfeld output exceeds the 1 GiB byte limit");
        std::vector<std::uint8_t> out{'f','a','r','b','f','e','l','d'};
        be32(out, image.width); be32(out, image.height);
        for (std::size_t i = 0; i < std::size_t(image.width) * image.height; ++i)
            for (const auto channel : get_pixel(image, i)) { out.push_back(channel); out.push_back(channel); }
        return out;
    }
    if (extension == ".tga") {
        if (image.width > 65535 || image.height > 65535) throw std::invalid_argument("TGA dimensions must fit 16 bits");
        const auto colors = tlg::channels(image.layout);
        std::vector<std::uint8_t> out(18);
        out[2] = colors == 1 ? 3 : 2;
        out[12] = static_cast<std::uint8_t>(image.width); out[13] = static_cast<std::uint8_t>(image.width >> 8);
        out[14] = static_cast<std::uint8_t>(image.height); out[15] = static_cast<std::uint8_t>(image.height >> 8);
        out[16] = static_cast<std::uint8_t>(colors * 8); out[17] = colors == 4 ? 0x28 : 0x20;
        for (std::size_t i = 0; i < std::size_t(image.width) * image.height; ++i) {
            const auto pixel = get_pixel(image, i);
            if (colors == 1) out.push_back(pixel[0]);
            else { out.push_back(pixel[2]); out.push_back(pixel[1]); out.push_back(pixel[0]); if (colors == 4) out.push_back(pixel[3]); }
        }
        return out;
    }
    throw std::invalid_argument("unsupported portable image extension");
}

} // namespace tlg_tool
