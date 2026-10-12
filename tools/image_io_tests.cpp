#include "image_io.hpp"
#include "portable_io.hpp"
#include "dds_io.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {

unsigned failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}

void pnm_golden(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> pixels,
                tlg::PixelLayout layout, std::uint32_t width, std::uint32_t height) {
    const auto image = tlg_tool::read_pnm(bytes);
    check(image.width == width && image.height == height && image.layout == layout,
          "PNM header did not preserve dimensions/layout");
    check(std::ranges::equal(image.pixels, pixels), "PNM separator ate or changed raster bytes");
    for (const auto extension : {".pam", ".pnm"}) {
        const auto restored = tlg_tool::read_pnm(tlg_tool::encode_pnm(image.view(), extension));
        check(restored.width == width && restored.height == height && restored.layout == layout &&
              restored.pixels == image.pixels, "portable image roundtrip did not preserve pixels");
    }
}

void read_case(std::string_view header, std::span<const std::uint8_t> pixels,
               tlg::PixelLayout layout, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> bytes(header.begin(), header.end());
    bytes.insert(bytes.end(), pixels.begin(), pixels.end());
    pnm_golden(bytes, pixels, layout, width, height);
}

void reject(std::string_view text) {
    const std::vector<std::uint8_t> bytes(text.begin(), text.end());
    try {
        (void)tlg_tool::read_pnm(bytes);
        check(false, "malformed PNM was accepted");
    } catch (const std::runtime_error&) {}
}

unsigned pnm_grammar_cases() {
    // Netpbm's comment ends before CR or LF. Before the raw raster's one
    // separator a comment is header; after it '#' and whitespace are pixels.
    // https://netpbm.sourceforge.net/doc/pbm.html (June 2024 correction)
    unsigned cases = 0;
    auto positive = [&](std::string_view wire, std::span<const std::uint8_t> expected,
                        tlg::PixelLayout layout, std::uint32_t width) {
        const auto bytes = std::span{reinterpret_cast<const std::uint8_t*>(wire.data()), wire.size()};
        check(tlg_tool::is_pnm(bytes), "legal PNM magic/comment was not detected by contents");
        pnm_golden(bytes, expected, layout, width, 1);
        ++cases;
    };
    for (const auto terminator : {std::string_view{"\n"}, std::string_view{"\r"}, std::string_view{"\r\n"}}) {
        for (const std::uint8_t first_pixel : {9, 10, 13, 32, 35}) {
            const std::array<std::uint8_t, 1> expected{first_pixel};
            auto last_comment = std::string("P5 1 1 255# final header comment") + std::string(terminator);
            last_comment.push_back(static_cast<char>(first_pixel));
            positive(last_comment, expected, tlg::PixelLayout::gray, 1);
            auto earlier_comments = std::string("P5\n# header comment") + std::string(terminator) +
                "1 # width comment" + std::string(terminator) + "1 # height comment" +
                std::string(terminator) + "255\n";
            earlier_comments.push_back(static_cast<char>(first_pixel));
            positive(earlier_comments, expected, tlg::PixelLayout::gray, 1);
        }
        const std::array<std::uint8_t, 3> rgb{10, 32, 35};
        auto ppm = std::string("P6 1 1 255# final header comment") + std::string(terminator);
        ppm.append(reinterpret_cast<const char*>(rgb.data()), rgb.size());
        positive(ppm, rgb, tlg::PixelLayout::rgb, 1);
        const std::array<std::uint8_t, 8> bitmap{255,255,0,255,255,255,0,0};
        auto pbm = std::string("P4 8 1# final header comment") + std::string(terminator) + '#';
        positive(pbm, bitmap, tlg::PixelLayout::gray, 8);
    }
    const std::array<std::uint8_t, 4> plain{255,0,255,0};
    positive("P1\n4 1\n0101\n", plain, tlg::PixelLayout::gray, 4);
    positive("P1\n4 1\n0101", plain, tlg::PixelLayout::gray, 4);
    positive("P1\n4 1\n01 0\t1\n", plain, tlg::PixelLayout::gray, 4);
    positive("P1\n4# width comment\r1# height comment\r0101\n", plain, tlg::PixelLayout::gray, 4);
    positive("P1\n4 1\n0101 arbitrary trailing junk 001101", plain, tlg::PixelLayout::gray, 4);
    positive("P1\n4 1\n0101\r\nanything, including # and 0/1", plain, tlg::PixelLayout::gray, 4);
    positive("P1# magic comment\n4 1\n0101\n", plain, tlg::PixelLayout::gray, 4);
    const std::array<std::uint8_t, 1> gray{65};
    positive("P5# magic comment\r1 1\n255\nA", gray, tlg::PixelLayout::gray, 1);
    const std::array<std::uint8_t, 3> rgb{9,32,35};
    positive("P6# magic comment\r\n1 1\n255\n\t #", rgb, tlg::PixelLayout::rgb, 1);
    for (const auto invalid : {
        "P1\n4 1\n01010", "P1\n4 1\n01011", "P1\n4 1\n01012",
        "P1\n4 1\n010", "P1\n4 1\n0121\n", "P5 1 1 255# unterminated comment",
        "P2 1 1 255\n0 arbitrary junk", "P3 1 1 255\n0 1 2 arbitrary junk"}) {
        reject(invalid);
        ++cases;
    }
    return cases;
}

void golden(std::span<const std::uint8_t> wire, std::span<const std::uint8_t> expected,
            std::string_view extension, tlg::PixelLayout layout, std::uint32_t width, std::uint32_t height) {
    const auto image = tlg_tool::read_portable_image(wire, extension);
    check(image.width == width && image.height == height && image.layout == layout &&
          std::ranges::equal(image.pixels, expected), "portable image did not match its independent wire golden");
    const auto encoded = tlg_tool::encode_portable_image(image.view(), extension);
    const auto restored = tlg_tool::read_portable_image(encoded, extension);
    check(restored.pixels == image.pixels && restored.layout == layout, "portable image roundtrip changed pixels/layout");
    for (std::size_t length = 0; length < wire.size(); ++length) {
        try {
            (void)tlg_tool::read_portable_image(wire.first(length), extension);
            check(false, "truncated portable image was accepted");
        } catch (const std::runtime_error&) {}
    }
}

void format_cases() {
    // Independent wire examples from the formats' byte layouts, not encoder
    // roundtrips. Two mutually agreeable bugs are still two bugs.
    const std::vector<std::uint8_t> qoi{
        'q','o','i','f',0,0,0,5,0,0,0,1,4,0,
        0xff,255,17,93,0,0x7f,0xaa,0x97,0x1d,0xc0,0,0,0,0,0,0,0,1};
    const std::array<std::uint8_t, 20> qoi_pixels{255,17,93,0,0,18,94,0,11,28,103,0,255,17,93,0,255,17,93,0};
    golden(qoi, qoi_pixels, ".qoi", tlg::PixelLayout::rgba, 5, 1);
    const tlg::ImageView qoi_image{5, 1, tlg::PixelLayout::rgba, qoi_pixels};
    check(tlg_tool::encode_portable_image(qoi_image, ".qoi") == qoi, "QOI opcodes differ from the specification golden");

    const std::vector<std::uint8_t> ff{'f','a','r','b','f','e','l','d',0,0,0,1,0,0,0,1,255,255,17,17,93,93,0,0};
    const std::array<std::uint8_t, 4> rgba{255,17,93,0};
    golden(ff, rgba, ".ff", tlg::PixelLayout::rgba, 1, 1);
    check(tlg_tool::encode_portable_image({1, 1, tlg::PixelLayout::rgba, rgba}, ".ff") == ff,
          "farbfeld output is not big endian RGBA16 with exact 8-bit expansion");
    auto high_depth = ff; high_depth[17] = 254;
    try { (void)tlg_tool::read_portable_image(high_depth, ".ff"); check(false, "farbfeld quantized without explicit authorization"); }
    catch (const std::runtime_error&) {}
    check(tlg_tool::read_portable_image(high_depth, ".ff", true).pixels[0] == 255,
          "explicit farbfeld normalization failed");

    const std::vector<std::uint8_t> tga{
        0,0,2,0,0,0,0,0,0,0,0,0,2,0,2,0,24,0x10,
        255,0,0,0,255,0,255,255,255,0,0,255};
    const std::array<std::uint8_t, 12> rgb{255,0,0,255,255,255,0,255,0,0,0,255};
    golden(tga, rgb, ".tga", tlg::PixelLayout::rgb, 2, 2);
    const std::vector<std::uint8_t> tga_palette{
        0,1,9,5,0,2,0,32,0,0,0,0,2,0,1,0,8,0x28,
        93,17,255,0,255,0,0,255,1,5,6};
    const std::array<std::uint8_t, 8> palette_pixels{255,17,93,0,0,0,255,255};
    golden(tga_palette, palette_pixels, ".tga", tlg::PixelLayout::rgba, 2, 1);
    const std::vector<std::uint8_t> tga_rle{0,0,10,0,0,0,0,0,0,0,0,0,2,0,1,0,32,0x28,0x81,93,17,255,0};
    const std::array<std::uint8_t, 8> repeated{255,17,93,0,255,17,93,0};
    golden(tga_rle, repeated, ".tga", tlg::PixelLayout::rgba, 2, 1);
    auto footer_as_raster = tga;
    footer_as_raster.resize(18 + 9); // one of four RGB pixels is missing
    footer_as_raster.insert(footer_as_raster.end(), {0,0,0,0,0,0,0,0,'T','R','U','E','V','I','S','I','O','N','-','X','F','I','L','E','.',0});
    try { (void)tlg_tool::read_portable_image(footer_as_raster, ".tga"); check(false, "TGA footer was consumed as missing raster bytes"); }
    catch (const std::runtime_error&) {}

    const std::array<std::uint8_t, 3> scaled{0,17,255};
    const std::string_view ascii_gray = "P2 3 1 15\n0 1 15 # tail\n";
    check(std::ranges::equal(tlg_tool::read_pnm({reinterpret_cast<const std::uint8_t*>(ascii_gray.data()), ascii_gray.size()}).pixels, scaled),
          "ASCII PGM samples/comments are incorrect");
    const std::string_view ascii = "P3 1 1 15\n0 1 15\n";
    const auto normalized = tlg_tool::read_pnm({reinterpret_cast<const std::uint8_t*>(ascii.data()), ascii.size()});
    check(std::ranges::equal(normalized.pixels, scaled), "PNM exact MAXVAL normalization failed");
    const std::vector<std::uint8_t> pgm16{'P','5',' ','1',' ','1',' ','6','5','5','3','5','\n',17,17};
    check(tlg_tool::read_pnm(pgm16).pixels[0] == 17, "PNM big endian 16-bit exact normalization failed");
    const std::vector<std::uint8_t> pbm{'P','4',' ','9',' ','1','\n',0x80,0x80};
    const auto bitmap = tlg_tool::read_pnm(pbm);
    check(bitmap.pixels.size() == 9 && bitmap.pixels[0] == 0 && bitmap.pixels[1] == 255 && bitmap.pixels[8] == 0,
          "PBM row padding or black/white polarity is incorrect");
    check(tlg_tool::read_pnm(tlg_tool::encode_pnm(bitmap.view(), ".pbm")).pixels == bitmap.pixels,
          "PBM binary output did not preserve pixels");
}

void dds_cases() {
    auto header = [](char variant, unsigned width, unsigned height) {
        std::vector<std::uint8_t> bytes(128);
        auto field = [&](unsigned offset, unsigned value) {
            for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
        };
        bytes[0] = 'D'; bytes[1] = 'D'; bytes[2] = 'S'; bytes[3] = ' ';
        field(4, 124); field(8, 0x81007); field(12, height); field(16, width);
        field(76, 32); field(80, 4); field(108, 0x1000);
        bytes[84] = 'D'; bytes[85] = 'X'; bytes[86] = 'T'; bytes[87] = variant;
        return bytes;
    };
    auto transparent = header('1', 3, 2);
    transparent.insert(transparent.end(), {0,0,255,255,255,255,255,255});
    const auto bc1 = tlg_tool::read_dds(transparent);
    check(bc1.width == 3 && bc1.height == 2 && bc1.layout == tlg::PixelLayout::rgba &&
          std::ranges::all_of(bc1.pixels, [](auto value) { return value == 0; }),
          "BC1 transparency or partial edge-block crop is incorrect");
    auto bc2_wire = header('3', 4, 4);
    bc2_wire.insert(bc2_wire.end(), {0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe,0,0xf8,0,0,0,0,0,0});
    const auto bc2 = tlg_tool::read_dds(bc2_wire);
    for (unsigned i = 0; i < 16; ++i)
        check(bc2.pixels[i * 4] == 255 && bc2.pixels[i * 4 + 1] == 0 && bc2.pixels[i * 4 + 2] == 0 &&
              bc2.pixels[i * 4 + 3] == i * 17, "BC2 explicit alpha order is incorrect");
    auto bc3_wire = header('5', 4, 4);
    bc3_wire.insert(bc3_wire.end(), {0,255,0x3e,0,0,0,0,0,0,0xf8,0,0,0,0,0,0});
    const auto bc3 = tlg_tool::read_dds(bc3_wire);
    check(bc3.pixels[0] == 255 && bc3.pixels[3] == 0 && bc3.pixels[7] == 255 && bc3.pixels[11] == 0,
          "BC3 alpha endpoint/interpolation table is incorrect");
    for (const auto& wire : {transparent, bc2_wire, bc3_wire}) {
        for (std::size_t length = 0; length < wire.size(); ++length) {
            try { (void)tlg_tool::read_dds(std::span(wire).first(length)); check(false, "truncated DDS was accepted"); }
            catch (const std::runtime_error&) {}
        }
    }
    const std::array<std::uint8_t, 4> rgba{255,17,93,0};
    const auto raw = tlg_tool::encode_dds({1,1,tlg::PixelLayout::rgba,rgba});
    check(raw.size() == 132 && raw[4] == 124 && raw[76] == 32 && raw[80] == 0x41 && raw[88] == 32 &&
          raw[92] == 255 && raw[97] == 255 && raw[102] == 255 && raw[107] == 255 &&
          std::equal(rgba.begin(), rgba.end(), raw.begin() + 128), "DDS raw writer masks/header differ from the wire specification");
    check(std::ranges::equal(tlg_tool::read_dds(raw).pixels, rgba), "DDS raw RGBA roundtrip failed");
    auto invalid = raw; invalid[96] = 255;
    try { (void)tlg_tool::read_dds(invalid); check(false, "overlapping DDS masks were accepted"); }
    catch (const std::runtime_error&) {}
}

} // namespace

int main() {
    try {
        const std::array<std::uint8_t, 1> newline{10};
        const std::array<std::uint8_t, 1> carriage_return{13};
        const std::array<std::uint8_t, 3> comment_like{9, 32, 35};
        const std::array<std::uint8_t, 4> transparent_rgba{255, 17, 93, 0};
        read_case("P5 1 1 255\r", newline, tlg::PixelLayout::gray, 1, 1);
        read_case("P5 1 1 255\r\n", newline, tlg::PixelLayout::gray, 1, 1);
        read_case("P5 1 1 255\n", carriage_return, tlg::PixelLayout::gray, 1, 1);
        read_case("P6\r\n# tiny image\r\n1 1\r\n255\r\n", comment_like, tlg::PixelLayout::rgb, 1, 1);
        read_case("P6 1 1 255\r", comment_like, tlg::PixelLayout::rgb, 1, 1);
        read_case("P7\r\nWIDTH 1\r\nHEIGHT 1\r\nDEPTH 4\r\nMAXVAL 255\r\nTUPLTYPE RGB_ALPHA\r\nENDHDR\r\n",
                  transparent_rgba, tlg::PixelLayout::rgba, 1, 1);
        reject("P5 0 1 255\na");
        reject("P5 1 1 65535\nab");
        reject("P6 1 1 255\nab");
        reject("P6 1 1 255\nabcd");
        reject("P5 4294967295 4294967295 255\na");
        reject("P7\nWIDTH 1\nWIDTH 2\nHEIGHT 1\nDEPTH 3\nMAXVAL 255\nENDHDR\nabc");
        reject("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 5\nMAXVAL 255\nENDHDR\nabcde");
        reject("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 3\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\nabc");
        reject("P7\nWIDTH 1\nHEIGHT 1\nDEPTH 3\nMAXVAL 255\nENDHDR");
        format_cases();
        dds_cases();
        std::cout << pnm_grammar_cases() << " PNM grammar regression cases passed\n";
    } catch (const std::exception& error) {
        check(false, error.what());
    }
    if (failures != 0) return 1;
    std::cout << "portable image parsing and byte-exact roundtrips passed\n";
    return 0;
}
