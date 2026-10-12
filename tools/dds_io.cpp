#include "dds_io.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace tlg_tool {
namespace {
// DDS header and BC layouts are specified by Microsoft and Khronos:
// https://learn.microsoft.com/windows/win32/direct3ddds/dds-header
// https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt
constexpr std::size_t byte_limit = std::size_t{1} << 30;

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) throw std::runtime_error("truncated DDS header");
    return std::uint32_t(bytes[offset]) | (std::uint32_t(bytes[offset + 1]) << 8) |
           (std::uint32_t(bytes[offset + 2]) << 16) | (std::uint32_t(bytes[offset + 3]) << 24);
}
void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
std::uint32_t fourcc(char a, char b, char c, char d) {
    return std::uint32_t(a) | (std::uint32_t(b) << 8) | (std::uint32_t(c) << 16) | (std::uint32_t(d) << 24);
}
using Pixel = std::array<std::uint8_t, 4>;
Pixel rgb565(unsigned value) {
    return {static_cast<std::uint8_t>((((value >> 11) & 31) * 255 + 15) / 31),
            static_cast<std::uint8_t>((((value >> 5) & 63) * 255 + 31) / 63),
            static_cast<std::uint8_t>(((value & 31) * 255 + 15) / 31), 255};
}
std::array<Pixel, 16> bc_block(std::span<const std::uint8_t> block, unsigned variant) {
    const auto color_offset = variant == 1 ? 0u : 8u;
    const auto endpoints = le32(block, color_offset);
    const auto first = endpoints & 65535, second = endpoints >> 16;
    std::array<Pixel, 4> colors{rgb565(first), rgb565(second), Pixel{}, Pixel{}};
    if (variant != 1 || first > second) {
        for (unsigned c = 0; c < 3; ++c) {
            colors[2][c] = static_cast<std::uint8_t>((2 * colors[0][c] + colors[1][c]) / 3);
            colors[3][c] = static_cast<std::uint8_t>((colors[0][c] + 2 * colors[1][c]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (unsigned c = 0; c < 3; ++c) colors[2][c] = static_cast<std::uint8_t>((colors[0][c] + colors[1][c]) / 2);
        colors[2][3] = 255; // BC1's fourth entry is transparent black, not opaque black.
    }
    const auto indices = le32(block, color_offset + 4);
    std::array<Pixel, 16> pixels{};
    for (unsigned i = 0; i < 16; ++i) pixels[i] = colors[(indices >> (2 * i)) & 3];
    if (variant == 2) {
        for (unsigned i = 0; i < 16; ++i) pixels[i][3] = ((block[i / 2] >> ((i % 2) * 4)) & 15) * 17;
    } else if (variant == 3) {
        std::array<unsigned, 8> alpha{block[0], block[1], 0,0,0,0,0,255};
        const auto last = alpha[0] > alpha[1] ? 8u : 6u;
        const auto denominator = last - 1;
        for (unsigned i = 2; i < last; ++i) alpha[i] = ((last - i) * alpha[0] + (i - 1) * alpha[1]) / denominator;
        std::uint64_t table = 0;
        for (unsigned i = 0; i < 6; ++i) table |= std::uint64_t(block[i + 2]) << (8 * i);
        for (unsigned i = 0; i < 16; ++i) pixels[i][3] = static_cast<std::uint8_t>(alpha[(table >> (3 * i)) & 7]);
    }
    return pixels;
}

std::uint8_t masked(std::uint32_t pixel, std::uint32_t mask, bool allow_lossy) {
    if (mask == 0) return 255;
    const auto shift = std::countr_zero(mask);
    const auto maximum = mask >> shift;
    if ((maximum & (maximum + 1)) != 0) throw std::runtime_error("DDS channel masks must be contiguous");
    const auto value = (pixel & mask) >> shift;
    const auto scaled = std::uint64_t(value) * 255;
    if (std::popcount(mask) > 8 && !allow_lossy && scaled % maximum != 0)
        throw std::runtime_error("DDS channel cannot map exactly to 8 bits; pass --allow-lossy to normalize");
    return static_cast<std::uint8_t>((scaled + maximum / 2) / maximum);
}
} // namespace

bool is_dds(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= 4 && bytes[0] == 'D' && bytes[1] == 'D' && bytes[2] == 'S' && bytes[3] == ' ';
}

tlg::Image read_dds(std::span<const std::uint8_t> bytes, bool allow_lossy) {
    if (!is_dds(bytes) || bytes.size() > byte_limit) throw std::runtime_error("invalid or oversized DDS input");
    if (le32(bytes, 4) != 124 || le32(bytes, 76) != 32) throw std::runtime_error("invalid DDS header size");
    const auto flags = le32(bytes, 8), height = le32(bytes, 12), width = le32(bytes, 16);
    const auto pitch_field = le32(bytes, 20), depth = le32(bytes, 24), mipmaps = le32(bytes, 28);
    if (depth > 1 || mipmaps > 1 || le32(bytes, 112) != 0)
        throw std::runtime_error("DDS arrays, cubemaps, volumes, and mip chains require selecting one image first");
    const auto count = std::uint64_t(width) * height;
    if (width == 0 || height == 0 || count > 268'435'456 || count > byte_limit / 4)
        throw std::runtime_error("DDS dimensions exceed the decoded image limit");
    auto pixel_flags = le32(bytes, 80), code = le32(bytes, 84), bits = le32(bytes, 88);
    std::array<std::uint32_t, 4> masks{le32(bytes, 92), le32(bytes, 96), le32(bytes, 100), le32(bytes, 104)};
    unsigned variant = 0;
    std::size_t offset = 128;
    bool gray = (pixel_flags & 0x20000) != 0;
    if ((pixel_flags & 4) != 0) {
        if (code == fourcc('D','X','T','1')) variant = 1;
        else if (code == fourcc('D','X','T','3')) variant = 2;
        else if (code == fourcc('D','X','T','5')) variant = 3;
        else if (code == fourcc('D','X','1','0')) {
            const auto dxgi = le32(bytes, 128);
            if (le32(bytes, 132) != 3 || (le32(bytes, 136) & 4) != 0 || le32(bytes, 140) != 1 ||
                (le32(bytes, 144) & 7) == 2) throw std::runtime_error("unsupported DDS DX10 image/alpha mode");
            offset = 148;
            if (dxgi == 71 || dxgi == 72) variant = 1;
            else if (dxgi == 74 || dxgi == 75) variant = 2;
            else if (dxgi == 77 || dxgi == 78) variant = 3;
            else if (dxgi == 28 || dxgi == 29) { bits = 32; masks = {255,65280,16711680,4278190080u}; }
            else if (dxgi == 87 || dxgi == 91) { bits = 32; masks = {16711680,65280,255,4278190080u}; }
            else if (dxgi == 88 || dxgi == 93) { bits = 32; masks = {16711680,65280,255,0}; }
            else if (dxgi == 61) { bits = 8; masks = {255,0,0,0}; gray = true; }
            else throw std::runtime_error("DDS DX10 supports RGBA8/BGRA8/R8 and BC1/BC2/BC3");
            pixel_flags = variant ? 4 : gray ? 0x20000 : 0x40;
        } else throw std::runtime_error("DDS supports DXT1/DXT3/DXT5 and selected DX10 formats");
    }
    if (bytes.size() < offset) throw std::runtime_error("truncated DDS header");
    if (variant != 0) {
        const auto block_width = (std::size_t(width) + 3) / 4, block_height = (std::size_t(height) + 3) / 4;
        const auto block_size = variant == 1 ? 8u : 16u;
        if (bytes.size() - offset != block_width * block_height * block_size) throw std::runtime_error("DDS block payload length is incorrect");
        tlg::Image image{width, height, tlg::PixelLayout::rgba, std::vector<std::uint8_t>(static_cast<std::size_t>(count) * 4)};
        bool opaque = true;
        for (std::size_t by = 0; by < block_height; ++by) for (std::size_t bx = 0; bx < block_width; ++bx) {
            const auto pixels = bc_block(bytes.subspan(offset, block_size), variant); offset += block_size;
            for (std::size_t y = 0; y < 4 && by * 4 + y < height; ++y)
                for (std::size_t x = 0; x < 4 && bx * 4 + x < width; ++x) {
                    const auto& pixel = pixels[y * 4 + x]; opaque &= pixel[3] == 255;
                    std::copy(pixel.begin(), pixel.end(), image.pixels.begin() + ((by * 4 + y) * width + bx * 4 + x) * 4);
                }
        }
        if (variant == 1 && opaque) {
            for (std::size_t i = 0; i < count; ++i) std::copy_n(image.pixels.begin() + i * 4, 3, image.pixels.begin() + i * 3);
            image.pixels.resize(static_cast<std::size_t>(count) * 3); image.layout = tlg::PixelLayout::rgb;
        }
        return image;
    }
    if ((pixel_flags & (0x40 | 0x20000)) == 0 || (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        throw std::runtime_error("unsupported uncompressed DDS pixel format");
    if (masks[0] == 0 || (!gray && (masks[1] == 0 || masks[2] == 0))) throw std::runtime_error("DDS is missing color channel masks");
    std::uint32_t used = 0;
    for (const auto mask : masks) {
        if ((mask & used) != 0 || (bits < 32 && (mask >> bits) != 0)) throw std::runtime_error("DDS channel masks overlap or exceed pixel depth");
        if (mask != 0) { const auto shifted = mask >> std::countr_zero(mask); if ((shifted & (shifted + 1)) != 0) throw std::runtime_error("non-contiguous DDS channel mask"); }
        used |= mask;
    }
    const auto colors = masks[3] != 0 ? 4u : gray ? 1u : 3u;
    const auto layout = colors == 4 ? tlg::PixelLayout::rgba : colors == 1 ? tlg::PixelLayout::gray : tlg::PixelLayout::rgb;
    const auto minimum_pitch = std::size_t(width) * (bits / 8);
    const auto pitch = (flags & 8) != 0 ? std::size_t(pitch_field) : minimum_pitch;
    if (pitch < minimum_pitch || pitch > byte_limit / height || bytes.size() - offset != pitch * height)
        throw std::runtime_error("DDS pitch or raster length is incorrect");
    tlg::Image image{width, height, layout, std::vector<std::uint8_t>(static_cast<std::size_t>(count) * colors)};
    for (std::size_t y = 0; y < height; ++y) for (std::size_t x = 0; x < width; ++x) {
        std::uint32_t pixel = 0;
        for (unsigned i = 0; i < bits / 8; ++i) pixel |= std::uint32_t(bytes[offset + y * pitch + x * (bits / 8) + i]) << (i * 8);
        for (unsigned c = 0; c < colors; ++c) image.pixels[(y * width + x) * colors + c] = masked(pixel, masks[gray && c < 3 ? 0 : c], allow_lossy);
    }
    return image;
}

std::vector<std::uint8_t> encode_dds(tlg::ImageView image) {
    const auto colors = tlg::channels(image.layout);
    if (image.pixels.size() > byte_limit - 128) throw std::runtime_error("DDS output exceeds the 1 GiB byte limit");
    std::vector<std::uint8_t> bytes(128);
    bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
    put32(bytes, 4, 124); put32(bytes, 8, 0x100f); put32(bytes, 12, image.height); put32(bytes, 16, image.width);
    put32(bytes, 20, static_cast<std::uint32_t>(image.width * colors));
    put32(bytes, 76, 32); put32(bytes, 80, colors == 1 ? 0x20000 : colors == 4 ? 0x41 : 0x40);
    put32(bytes, 88, static_cast<std::uint32_t>(colors * 8)); put32(bytes, 92, 255);
    if (colors >= 3) { put32(bytes, 96, 65280); put32(bytes, 100, 16711680); }
    if (colors == 4) put32(bytes, 104, 4278190080u);
    put32(bytes, 108, 0x1000);
    bytes.insert(bytes.end(), image.pixels.begin(), image.pixels.end());
    return bytes;
}
} // namespace tlg_tool
