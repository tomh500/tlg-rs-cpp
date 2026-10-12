#include "avif_io.hpp"

#include <avif/avif.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#if AVIF_VERSION < 1030000
#error "The optional AVIF backend requires libavif 1.3.0 or newer."
#endif

namespace tlg_tool {
namespace {

constexpr std::uint32_t pixel_limit = 268'435'456;
constexpr std::size_t byte_limit = std::size_t{1} << 30;
constexpr std::uint32_t dimension_limit = 65'536;

// libavif quite reasonably removes a wholly opaque alpha plane. This optional
// property remembers the source layout so that RGBA does not become RGB on a
// round trip. Other AVIF readers may ignore the non-essential UUID property.
constexpr std::array<std::uint8_t, 16> layout_uuid{
    0xd3, 0x39, 0x1f, 0x06, 0x14, 0x82, 0x4d, 0x4b,
    0x86, 0xd2, 0xdf, 0x22, 0xa9, 0x4d, 0x3c, 0xf7};

using ImagePtr = std::unique_ptr<avifImage, decltype(&avifImageDestroy)>;
using DecoderPtr = std::unique_ptr<avifDecoder, decltype(&avifDecoderDestroy)>;
using EncoderPtr = std::unique_ptr<avifEncoder, decltype(&avifEncoderDestroy)>;

struct AvifOutput {
    avifRWData data = AVIF_DATA_EMPTY;
    ~AvifOutput() { avifRWDataFree(&data); }
};

void check(avifResult result, const char* operation, const avifDiagnostics* diagnostics = nullptr) {
    if (result == AVIF_RESULT_OK) return;
    auto message = std::string(operation) + ": " + avifResultToString(result);
    if (diagnostics != nullptr && diagnostics->error[0] != '\0')
        message += std::string(" (") + diagnostics->error + ')';
    throw std::runtime_error(message);
}

int threads() noexcept {
    return static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
}

std::size_t image_size(std::uint32_t width, std::uint32_t height, tlg::PixelLayout layout) {
    const auto colors = tlg::channels(layout);
    const auto pixels = std::uint64_t{width} * height;
    if (colors != 1 && colors != 3 && colors != 4)
        throw std::runtime_error("AVIF pixels must use Gray, RGB, or RGBA layout");
    if (width == 0 || height == 0 || width > dimension_limit || height > dimension_limit ||
        pixels > pixel_limit || pixels > byte_limit / colors)
        throw std::runtime_error("AVIF image exceeds the pixel, dimension, or 1 GiB byte limit");
    return static_cast<std::size_t>(pixels) * colors;
}

tlg::PixelLayout pixel_layout(const avifImage& image, bool alpha_present) {
    auto layout = alpha_present ? tlg::PixelLayout::rgba
                : image.yuvFormat == AVIF_PIXEL_FORMAT_YUV400 ? tlg::PixelLayout::gray
                                                             : tlg::PixelLayout::rgb;
    bool found = false;
    for (std::size_t i = 0; i < image.numProperties; ++i) {
        const auto& property = image.properties[i];
        if (std::memcmp(property.boxtype, "uuid", 4) != 0 ||
            std::memcmp(property.usertype, layout_uuid.data(), layout_uuid.size()) != 0) continue;
        if (found || property.boxPayload.size != 2 || property.boxPayload.data[0] != 1)
            throw std::runtime_error("invalid AVIF source-layout property");
        found = true;
        layout = static_cast<tlg::PixelLayout>(property.boxPayload.data[1]);
        if (layout != tlg::PixelLayout::gray && layout != tlg::PixelLayout::rgb && layout != tlg::PixelLayout::rgba)
            throw std::runtime_error("invalid AVIF source-layout value");
        if ((layout != tlg::PixelLayout::rgba && alpha_present) ||
            (layout == tlg::PixelLayout::gray && image.yuvFormat != AVIF_PIXEL_FORMAT_YUV400))
            throw std::runtime_error("AVIF source-layout property disagrees with its image planes");
    }
    return layout;
}

void check_image(const avifImage& image, tlg::PixelLayout layout, bool allow_lossy) {
    image_size(image.width, image.height, layout);
    if (image.depth != 8 && image.depth != 10 && image.depth != 12)
        throw std::runtime_error("unsupported AVIF pixel depth");
    if (image.depth != 8 && !allow_lossy)
        throw std::runtime_error("AVIF input is above 8 bits; --allow-lossy is required to quantize it to TLG pixels");
    if (image.alphaPremultiplied && !allow_lossy)
        throw std::runtime_error("premultiplied AVIF alpha requires --allow-lossy to recover straight RGBA pixels");
    if ((image.transformFlags & AVIF_TRANSFORM_PASP) != 0 && image.pasp.hSpacing != image.pasp.vSpacing)
        throw std::runtime_error("AVIF non-square pixel aspect ratio is unsupported by the TLG pixel layout");
    if (image.yuvFormat == AVIF_PIXEL_FORMAT_NONE)
        throw std::runtime_error("AVIF image has no color planes");
}

avifRGBFormat rgb_format(tlg::PixelLayout layout) noexcept {
    if (layout == tlg::PixelLayout::gray) return AVIF_RGB_FORMAT_GRAY;
    return layout == tlg::PixelLayout::rgb ? AVIF_RGB_FORMAT_RGB : AVIF_RGB_FORMAT_RGBA;
}

tlg::Image apply_transforms(tlg::Image image, const avifImage& source) {
    avifCropRect crop{0, 0, image.width, image.height};
    if ((source.transformFlags & AVIF_TRANSFORM_CLAP) != 0) {
        avifDiagnostics diagnostics{};
        if (!avifCropRectFromCleanApertureBox(&crop, &source.clap, image.width, image.height, &diagnostics))
            throw std::runtime_error(std::string("invalid AVIF crop: ") + diagnostics.error);
    }
    const unsigned angle = (source.transformFlags & AVIF_TRANSFORM_IROT) != 0 ? source.irot.angle : 0;
    const bool mirrored = (source.transformFlags & AVIF_TRANSFORM_IMIR) != 0;
    if (angle > 3 || (mirrored && source.imir.axis > 1))
        throw std::runtime_error("invalid AVIF rotation or mirror transform");
    if (angle == 0 && !mirrored && crop.x == 0 && crop.y == 0 &&
        crop.width == image.width && crop.height == image.height) return image;

    tlg::Image transformed;
    transformed.width = (angle & 1) != 0 ? crop.height : crop.width;
    transformed.height = (angle & 1) != 0 ? crop.width : crop.height;
    transformed.layout = image.layout;
    transformed.pixels.resize(image_size(transformed.width, transformed.height, transformed.layout));
    const auto colors = tlg::channels(image.layout);
    for (std::uint32_t y = 0; y < crop.height; ++y) {
        for (std::uint32_t x = 0; x < crop.width; ++x) {
            auto dx = x, dy = y;
            switch (angle) {
            case 1: dx = y; dy = crop.width - x - 1; break;
            case 2: dx = crop.width - x - 1; dy = crop.height - y - 1; break;
            case 3: dx = crop.height - y - 1; dy = x; break;
            }
            if (mirrored) {
                if (source.imir.axis == 0) dy = transformed.height - dy - 1;
                else dx = transformed.width - dx - 1;
            }
            const auto from = (std::size_t{crop.y + y} * image.width + crop.x + x) * colors;
            const auto to = (std::size_t{dy} * transformed.width + dx) * colors;
            std::copy_n(image.pixels.data() + from, colors, transformed.pixels.data() + to);
        }
    }
    return transformed;
}

} // namespace

bool is_avif(std::span<const std::uint8_t> prefix) noexcept {
    const avifROData data{prefix.data(), prefix.size()};
    return avifPeekCompatibleFileType(&data) != AVIF_FALSE;
}

tlg::Image read_avif(std::span<const std::uint8_t> encoded, bool allow_lossy) {
    if (encoded.empty() || encoded.size() > byte_limit)
        throw std::runtime_error("AVIF input is empty or exceeds the tool's 1 GiB limit");
    DecoderPtr decoder(avifDecoderCreate(), avifDecoderDestroy);
    if (!decoder) throw std::bad_alloc();
    decoder->maxThreads = threads();
    decoder->imageSizeLimit = pixel_limit;
    decoder->imageDimensionLimit = dimension_limit;
    decoder->imageCountLimit = 1;
    decoder->allowProgressive = AVIF_FALSE;
    decoder->allowIncremental = AVIF_FALSE;
    decoder->ignoreExif = AVIF_TRUE;
    decoder->ignoreXMP = AVIF_TRUE;
    check(avifDecoderSetIOMemory(decoder.get(), encoded.data(), encoded.size()), "open AVIF data");
    check(avifDecoderParse(decoder.get()), "parse AVIF header", &decoder->diag);
    if (decoder->imageCount != 1 || decoder->imageSequenceTrackPresent)
        throw std::runtime_error("animated or multi-frame AVIF input is unsupported");

    // Inspect the container before the AV1 codec allocates its image planes.
    auto layout = pixel_layout(*decoder->image, decoder->alphaPresent != AVIF_FALSE);
    check_image(*decoder->image, layout, allow_lossy);
    check(avifDecoderNextImage(decoder.get()), "decode AVIF image", &decoder->diag);
    layout = pixel_layout(*decoder->image, decoder->alphaPresent != AVIF_FALSE);
    check_image(*decoder->image, layout, allow_lossy);
    tlg::Image output{decoder->image->width, decoder->image->height, layout, {}};
    output.pixels.resize(image_size(output.width, output.height, output.layout));
    avifRGBImage rgb;
    avifRGBImageSetDefaults(&rgb, decoder->image);
    rgb.depth = 8;
    rgb.format = rgb_format(layout);
    rgb.avoidLibYUV = AVIF_TRUE;
    rgb.alphaPremultiplied = AVIF_FALSE;
    rgb.maxThreads = threads();
    rgb.rowBytes = static_cast<std::uint32_t>(std::size_t{output.width} * tlg::channels(layout));
    rgb.pixels = output.pixels.data();
    check(avifImageYUVToRGB(decoder->image, &rgb), "convert AVIF pixels to the TLG layout");
    return apply_transforms(std::move(output), *decoder->image);
}

std::vector<std::uint8_t> encode_avif(tlg::ImageView input, bool allow_lossy) {
    const auto bytes = image_size(input.width, input.height, input.layout);
    if (input.pixels.size() != bytes)
        throw std::runtime_error("AVIF input byte count does not match its dimensions and layout");
    const bool gray = input.layout == tlg::PixelLayout::gray;
    ImagePtr image(avifImageCreate(input.width, input.height, 8,
                                  gray ? AVIF_PIXEL_FORMAT_YUV400 : AVIF_PIXEL_FORMAT_YUV444), avifImageDestroy);
    if (!image) throw std::bad_alloc();
    image->yuvRange = AVIF_RANGE_FULL;
    image->matrixCoefficients = gray ? AVIF_MATRIX_COEFFICIENTS_BT601 : AVIF_MATRIX_COEFFICIENTS_IDENTITY;
    image->colorPrimaries = AVIF_COLOR_PRIMARIES_BT709;
    image->transferCharacteristics = AVIF_TRANSFER_CHARACTERISTICS_SRGB;
    image->alphaPremultiplied = AVIF_FALSE;
    if (gray) {
        check(avifImageAllocatePlanes(image.get(), AVIF_PLANES_YUV), "allocate AVIF grayscale plane");
        for (std::size_t y = 0; y < input.height; ++y)
            std::copy_n(input.pixels.data() + y * input.width, input.width,
                        image->yuvPlanes[AVIF_CHAN_Y] + y * image->yuvRowBytes[AVIF_CHAN_Y]);
    } else {
        avifRGBImage rgb;
        avifRGBImageSetDefaults(&rgb, image.get());
        rgb.format = rgb_format(input.layout);
        rgb.avoidLibYUV = AVIF_TRUE;
        rgb.alphaPremultiplied = AVIF_FALSE;
        rgb.rowBytes = static_cast<std::uint32_t>(std::size_t{input.width} * tlg::channels(input.layout));
        // libavif borrows this RGB buffer for a read-only conversion.
        rgb.pixels = const_cast<std::uint8_t*>(input.pixels.data());
        check(avifImageRGBToYUV(image.get(), &rgb), "convert input pixels to lossless AVIF planes");
    }
    const std::array<std::uint8_t, 2> source_layout{1, static_cast<std::uint8_t>(input.layout)};
    check(avifImageAddUUIDProperty(image.get(), layout_uuid.data(), source_layout.data(), source_layout.size()),
          "record AVIF source pixel layout");

    EncoderPtr encoder(avifEncoderCreate(), avifEncoderDestroy);
    if (!encoder) throw std::bad_alloc();
    encoder->maxThreads = threads();
    encoder->speed = 6;
    encoder->quality = allow_lossy ? 80 : AVIF_QUALITY_LOSSLESS;
    encoder->qualityAlpha = AVIF_QUALITY_LOSSLESS;
    encoder->autoTiling = AVIF_TRUE;
    AvifOutput output;
    check(avifEncoderWrite(encoder.get(), image.get(), &output.data), "encode AVIF image", &encoder->diag);
    if (output.data.size > byte_limit)
        throw std::runtime_error("encoded AVIF exceeds the tool's 1 GiB limit");
    std::vector<std::uint8_t> encoded(output.data.data, output.data.data + output.data.size);
    if (!allow_lossy) {
        const auto decoded = read_avif(encoded);
        if (decoded.width != input.width || decoded.height != input.height || decoded.layout != input.layout ||
            !std::ranges::equal(decoded.pixels, input.pixels))
            throw std::runtime_error("AVIF lossless verification failed: decoded pixels differ from the source");
    }
    return encoded;
}

} // namespace tlg_tool
