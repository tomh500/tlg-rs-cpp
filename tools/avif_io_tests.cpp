#include "avif_io.hpp"

#include <avif/avif.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

unsigned checks = 0;
void expect(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

template<class Function>
void rejects(Function&& operation, const char* message) {
    bool failed = false;
    try { operation(); }
    catch (const std::runtime_error&) { failed = true; }
    expect(failed, message);
}

void check(avifResult result) {
    if (result != AVIF_RESULT_OK) throw std::runtime_error(avifResultToString(result));
}

using ImagePtr = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>;
using EncoderPtr = std::unique_ptr<avifEncoder, decltype(&avifEncoderDestroy)>;

struct Output {
    avifRWData data = AVIF_DATA_EMPTY;
    ~Output() { avifRWDataFree(&data); }
};

ImagePtr gray_image(std::uint32_t width, std::uint32_t height, unsigned depth = 8) {
    ImagePtr image(avifImageCreate(width, height, depth, AVIF_PIXEL_FORMAT_YUV400), avifImageDestroy);
    if (!image) throw std::bad_alloc();
    image->yuvRange = AVIF_RANGE_FULL;
    image->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_BT601;
    check(avifImageAllocatePlanes(image.get(), AVIF_PLANES_YUV));
    return image;
}

std::vector<std::uint8_t> external_encode(avifImage* image, bool sequence = false) {
    EncoderPtr encoder(avifEncoderCreate(), avifEncoderDestroy);
    if (!encoder) throw std::bad_alloc();
    encoder->maxThreads = 1;
    encoder->speed = 8;
    encoder->quality = AVIF_QUALITY_LOSSLESS;
    encoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
    Output output;
    if (sequence) {
        check(avifEncoderAddImage(encoder.get(), image, 1, AVIF_ADD_IMAGE_FLAG_NONE));
        check(avifEncoderAddImage(encoder.get(), image, 1, AVIF_ADD_IMAGE_FLAG_NONE));
        check(avifEncoderFinish(encoder.get(), &output.data));
    } else check(avifEncoderWrite(encoder.get(), image, &output.data));
    return {output.data.data, output.data.data + output.data.size};
}

void round_trips() {
    std::uint32_t state = 0x7e66ab23u;
    const auto next = [&] {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        return static_cast<std::uint8_t>(state);
    };
    for (const auto layout : {tlg::PixelLayout::gray, tlg::PixelLayout::rgb, tlg::PixelLayout::rgba}) {
        for (const auto& [width, height] : std::array<std::pair<unsigned, unsigned>, 5>{{
                 {1, 1}, {3, 5}, {16, 16}, {17, 9}, {33, 19}}}) {
            for (unsigned pattern = 0; pattern < 4; ++pattern) {
                tlg::Image source{width, height, layout, {}};
                source.pixels.resize(std::size_t{width} * height * tlg::channels(layout));
                for (auto& value : source.pixels) value = next();
                if (layout == tlg::PixelLayout::rgba) {
                    for (std::size_t i = 3; i < source.pixels.size(); i += 4)
                        if (pattern < 2) source.pixels[i] = pattern == 0 ? 255 : 0;
                }
                if (pattern == 2) std::fill(source.pixels.begin(), source.pixels.end(), std::uint8_t{127});
                const auto snapshot = source.pixels;
                const auto encoded = tlg_tool::encode_avif(source.view());
                expect(tlg_tool::is_avif(encoded), "encoded file is not recognized as AVIF");
                const auto decoded = tlg_tool::read_avif(encoded);
                expect(decoded.width == width && decoded.height == height && decoded.layout == layout,
                       "AVIF dimensions or layout did not round trip");
                expect(decoded.pixels == source.pixels, "AVIF pixels did not round trip losslessly");
                expect(source.pixels == snapshot, "AVIF conversion changed its borrowed input");
                for (const auto format : {tlg::Format::tlg5, tlg::Format::tlg6}) {
                    const auto wire = tlg::encode(decoded.view(), tlg::EncodeOptions{format});
                    expect(wire.has_value(), "AVIF pixels could not be encoded as TLG");
                    const auto restored = tlg::decode(*wire);
                    expect(restored && restored->image.pixels == source.pixels && restored->image.layout == layout,
                           "AVIF -> TLG conversion changed pixels or layout");
                }
            }
        }
    }
}

void metadata_and_precision() {
    auto image = gray_image(3, 2);
    for (std::size_t y = 0; y < 2; ++y)
        for (std::size_t x = 0; x < 3; ++x)
            image->yuvPlanes[0][y * image->yuvRowBytes[0] + x] = static_cast<std::uint8_t>(y * 3 + x);
    image->transformFlags = AVIF_TRANSFORM_IROT;
    image->irot.angle = 1;
    auto rotated = tlg_tool::read_avif(external_encode(image.get()));
    expect(rotated.width == 2 && rotated.height == 3 && rotated.pixels == std::vector<std::uint8_t>{2, 5, 1, 4, 0, 3},
           "AVIF anticlockwise rotation is wrong");
    image->transformFlags |= AVIF_TRANSFORM_IMIR;
    image->imir.axis = 1;
    auto mirrored = tlg_tool::read_avif(external_encode(image.get()));
    expect(mirrored.pixels == std::vector<std::uint8_t>{5, 2, 4, 1, 3, 0}, "AVIF rotation followed by horizontal mirror is wrong");
    image->imir.axis = 0;
    mirrored = tlg_tool::read_avif(external_encode(image.get()));
    expect(mirrored.pixels == std::vector<std::uint8_t>{0, 3, 1, 4, 2, 5}, "AVIF rotation followed by vertical mirror is wrong");
    image->transformFlags = AVIF_TRANSFORM_IROT;
    image->irot.angle = 3;
    rotated = tlg_tool::read_avif(external_encode(image.get()));
    expect(rotated.pixels == std::vector<std::uint8_t>{3, 0, 4, 1, 5, 2}, "AVIF clockwise rotation is wrong");

    auto cropped = gray_image(4, 4);
    for (std::size_t y = 0; y < 4; ++y)
        for (std::size_t x = 0; x < 4; ++x)
            cropped->yuvPlanes[0][y * cropped->yuvRowBytes[0] + x] = static_cast<std::uint8_t>(y * 4 + x);
    const avifCropRect rectangle{1, 1, 2, 2};
    avifDiagnostics diagnostics{};
    expect(avifCleanApertureBoxFromCropRect(&cropped->clap, &rectangle, 4, 4, &diagnostics) != AVIF_FALSE,
           "test crop could not be expressed as AVIF metadata");
    cropped->transformFlags = AVIF_TRANSFORM_CLAP;
    const auto crop = tlg_tool::read_avif(external_encode(cropped.get()));
    expect(crop.width == 2 && crop.height == 2 && crop.pixels == std::vector<std::uint8_t>{5, 6, 9, 10}, "AVIF crop is wrong");

    auto high_depth = gray_image(2, 2, 10);
    constexpr std::array<std::uint16_t, 4> samples{0, 4, 511, 1023};
    for (std::size_t y = 0; y < 2; ++y)
        for (std::size_t x = 0; x < 2; ++x)
            std::memcpy(high_depth->yuvPlanes[0] + y * high_depth->yuvRowBytes[0] + x * 2, &samples[y * 2 + x], 2);
    const auto high_file = external_encode(high_depth.get());
    rejects([&] { (void)tlg_tool::read_avif(high_file); }, "10-bit AVIF was silently quantized");
    const auto quantized = tlg_tool::read_avif(high_file, true);
    expect(quantized.layout == tlg::PixelLayout::gray && quantized.pixels.front() == 0 && quantized.pixels.back() == 255,
           "explicit AVIF precision reduction has wrong range or layout");

    const auto animation = external_encode(high_depth.get(), true);
    rejects([&] { (void)tlg_tool::read_avif(animation, true); }, "animated AVIF silently became its first frame");

    ImagePtr premultiplied(avifImageCreate(2, 2, 8, AVIF_PIXEL_FORMAT_YUV444), avifImageDestroy);
    if (!premultiplied) throw std::bad_alloc();
    premultiplied->yuvRange = AVIF_RANGE_FULL;
    premultiplied->matrixCoefficients = AVIF_MATRIX_COEFFICIENTS_IDENTITY;
    premultiplied->alphaPremultiplied = AVIF_TRUE;
    std::array<std::uint8_t, 16> premultiplied_pixels{0, 0, 0, 0, 32, 16, 8, 64, 64, 32, 16, 128, 127, 63, 31, 255};
    avifRGBImage premultiplied_rgb;
    avifRGBImageSetDefaults(&premultiplied_rgb, premultiplied.get());
    premultiplied_rgb.format = AVIF_RGB_FORMAT_RGBA;
    premultiplied_rgb.avoidLibYUV = AVIF_TRUE;
    premultiplied_rgb.alphaPremultiplied = AVIF_TRUE;
    premultiplied_rgb.rowBytes = 8;
    premultiplied_rgb.pixels = premultiplied_pixels.data();
    check(avifImageRGBToYUV(premultiplied.get(), &premultiplied_rgb));
    const auto premultiplied_file = external_encode(premultiplied.get());
    rejects([&] { (void)tlg_tool::read_avif(premultiplied_file); }, "premultiplied AVIF alpha was silently unpremultiplied");
    expect(tlg_tool::read_avif(premultiplied_file, true).layout == tlg::PixelLayout::rgba,
           "explicit unpremultiplication lost the AVIF alpha layout");
    image->transformFlags = AVIF_TRANSFORM_PASP;
    image->pasp = {2, 1};
    const auto nonsquare = external_encode(image.get());
    rejects([&] { (void)tlg_tool::read_avif(nonsquare); }, "non-square AVIF aspect ratio was silently discarded");
}

void malformed_inputs() {
    tlg::Image source{7, 5, tlg::PixelLayout::rgba, std::vector<std::uint8_t>(7 * 5 * 4, 127)};
    const auto encoded = tlg_tool::encode_avif(source.view());
    for (const std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{8}, std::size_t{16}, encoded.size() / 2})
        rejects([&] { (void)tlg_tool::read_avif(std::span{encoded}.first(size)); }, "truncated AVIF was accepted");
    expect(!tlg_tool::is_avif({}), "empty input looks like AVIF");
    const std::array<std::uint8_t, 8> pnm{'P', '6', '\n', '1', ' ', '1', '\n', 0};
    expect(!tlg_tool::is_avif(pnm), "PNM input looks like AVIF");

    // Mutate the container header, rather than allocating an enormous raster.
    // The decoder must enforce these bounds during Parse, before AV1 decoding.
    const std::array<std::uint8_t, 4> ispe{'i', 's', 'p', 'e'};
    const auto property = std::search(encoded.begin(), encoded.end(), ispe.begin(), ispe.end());
    expect(property != encoded.end() && encoded.end() - property >= 16, "test AVIF is missing its dimensions property");
    const auto dimensions = static_cast<std::size_t>(property - encoded.begin()) + 8;
    const auto patched = [&](std::uint32_t width, std::uint32_t height) {
        auto output = encoded;
        for (unsigned byte = 0; byte < 4; ++byte) {
            output[dimensions + byte] = static_cast<std::uint8_t>(width >> (24 - byte * 8));
            output[dimensions + 4 + byte] = static_cast<std::uint8_t>(height >> (24 - byte * 8));
        }
        return output;
    };
    const auto huge_dimension = patched(65'537, 1);
    rejects([&] { (void)tlg_tool::read_avif(huge_dimension); }, "AVIF header dimension bound was not enforced");
    const auto huge_pixels = patched(16'385, 16'385);
    rejects([&] { (void)tlg_tool::read_avif(huge_pixels); }, "AVIF header pixel bound was not enforced");
    auto bad = source.view();
    bad.width = 0;
    rejects([&] { (void)tlg_tool::encode_avif(bad); }, "zero AVIF width was accepted");
    bad = source.view(); bad.width = 65'537;
    rejects([&] { (void)tlg_tool::encode_avif(bad); }, "oversized AVIF dimension was accepted");
    bad = source.view(); bad.layout = static_cast<tlg::PixelLayout>(2);
    rejects([&] { (void)tlg_tool::encode_avif(bad); }, "invalid AVIF pixel layout was accepted");
    bad = source.view(); bad.pixels = bad.pixels.first(bad.pixels.size() - 1);
    rejects([&] { (void)tlg_tool::encode_avif(bad); }, "AVIF byte-count mismatch was accepted");
    const auto lossy = tlg_tool::encode_avif(source.view(), true);
    const auto lossy_image = tlg_tool::read_avif(lossy);
    expect(lossy_image.width == source.width && lossy_image.height == source.height && lossy_image.layout == source.layout,
           "explicit lossy AVIF changed dimensions or layout");
}

} // namespace

int main() try {
    round_trips();
    metadata_and_precision();
    malformed_inputs();
    std::cout << "PASS " << checks << " AVIF lossless, TLG, transform, precision, animation, and malformed-input checks\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << "FAIL AVIF: " << error.what() << '\n';
    return 1;
}
