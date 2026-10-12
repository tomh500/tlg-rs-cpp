#include "image_io.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cctype>
#include <limits>
#include <stdexcept>

#ifdef TLG_TOOL_WIC
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace tlg_tool {
namespace {

constexpr std::size_t pixel_limit = 268'435'456;
constexpr std::size_t byte_limit = std::size_t{1} << 30;

std::size_t image_size(std::uint32_t width, std::uint32_t height, tlg::PixelLayout layout) {
    const auto count = std::uint64_t{width} * height;
    const auto colors = tlg::channels(layout);
    if (width == 0 || height == 0 || count > pixel_limit || count > byte_limit / colors)
        throw std::runtime_error("image exceeds the tool's pixel or 1 GiB byte limit");
    return static_cast<std::size_t>(count) * colors;
}

bool space(std::uint8_t byte) noexcept {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' || byte == '\f';
}

std::uint32_t number(std::string_view text, bool allow_zero = false) {
    std::uint32_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || (!allow_zero && value == 0))
        throw std::runtime_error("invalid positive integer in PNM header");
    return value;
}

class PnmReader {
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0;
public:
    explicit PnmReader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    void skip_comment() {
        // The terminating CR/LF is whitespace, not part of the comment.
        // In particular it may be the one byte that separates a raw raster.
        while (position_ < bytes_.size() && bytes_[position_] != '\r' && bytes_[position_] != '\n')
            ++position_;
    }

    void skip_space() {
        while (position_ < bytes_.size()) {
            if (space(bytes_[position_])) { ++position_; continue; }
            if (bytes_[position_] != '#') break;
            skip_comment();
        }
    }

    void finish_ascii() {
        skip_space();
        if (position_ != bytes_.size()) throw std::runtime_error("extra samples after PNM raster");
    }

    std::uint32_t bitmap_bit() {
        skip_space();
        if (position_ == bytes_.size()) throw std::runtime_error("truncated plain PBM raster");
        const auto byte = bytes_[position_++];
        if (byte != '0' && byte != '1') throw std::runtime_error("plain PBM pixels must be '0' or '1'");
        return byte - '0';
    }

    void finish_bitmap() const {
        // P1 explicitly permits arbitrary trailing material after whitespace.
        // Without that boundary an extra digit is still a malformed raster.
        if (position_ < bytes_.size() && !space(bytes_[position_]))
            throw std::runtime_error("extra data is not separated from the plain PBM raster");
    }

    std::size_t remaining() const { return bytes_.size() - position_; }

    std::string_view token() {
        skip_space();
        const auto first = position_;
        while (position_ < bytes_.size() && !space(bytes_[position_]) && bytes_[position_] != '#')
            ++position_;
        if (first == position_) throw std::runtime_error("truncated PNM header");
        return {reinterpret_cast<const char*>(bytes_.data() + first), position_ - first};
    }

    void raster_separator(std::size_t raster_size = std::numeric_limits<std::size_t>::max()) {
        if (position_ < bytes_.size() && bytes_[position_] == '#') skip_comment();
        if (position_ == bytes_.size() || !space(bytes_[position_]))
            throw std::runtime_error("missing whitespace before PNM pixels");
        // One separator belongs to the header. More may be actual pixel bytes;
        // greedily eating whitespace here would quietly nibble a dark image.
        // With a known raster length, CR + a first pixel of value 10 is a
        // perfectly good single separator. CRLF only wins when both bytes fit.
        if (bytes_[position_++] == '\r' && position_ < bytes_.size() && bytes_[position_] == '\n' &&
            (raster_size == std::numeric_limits<std::size_t>::max() ||
             bytes_.size() - position_ == raster_size + 1))
            ++position_;
    }

    std::string_view line() {
        const auto first = position_;
        while (position_ < bytes_.size() && bytes_[position_] != '\n') ++position_;
        if (position_ == bytes_.size()) throw std::runtime_error("truncated PAM header");
        auto end = position_++;
        if (end > first && bytes_[end - 1] == '\r') --end;
        return {reinterpret_cast<const char*>(bytes_.data() + first), end - first};
    }

    std::span<const std::uint8_t> raster(std::size_t size) const {
        if (bytes_.size() - position_ != size)
            throw std::runtime_error("PNM raster length does not match its dimensions");
        return bytes_.subspan(position_, size);
    }
};

std::string_view trim(std::string_view value) {
    while (!value.empty() && space(static_cast<std::uint8_t>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && space(static_cast<std::uint8_t>(value.back()))) value.remove_suffix(1);
    return value;
}

#ifdef TLG_TOOL_WIC
using Microsoft::WRL::ComPtr;

void check(HRESULT result, std::string_view operation) {
    if (SUCCEEDED(result)) return;
    std::array<char, 16> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                        static_cast<std::uint32_t>(result), 16);
    throw std::runtime_error(std::string(operation) + " failed (WIC 0x" +
                             std::string(buffer.data(), converted.ptr) + ")");
}

class WicContext {
    bool initialized_ = false;
public:
    ComPtr<IWICImagingFactory> factory;
    WicContext() {
        const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (initialized != RPC_E_CHANGED_MODE) check(initialized, "initialize image backend");
        initialized_ = SUCCEEDED(initialized);
        try {
            check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(factory.GetAddressOf())), "create image backend");
        } catch (...) {
            if (initialized_) CoUninitialize();
            throw;
        }
    }
    ~WicContext() { factory.Reset(); if (initialized_) CoUninitialize(); }
};

GUID pixel_format(tlg::PixelLayout layout) {
    if (layout == tlg::PixelLayout::gray) return GUID_WICPixelFormat8bppGray;
    if (layout == tlg::PixelLayout::rgb) return GUID_WICPixelFormat24bppRGB;
    return GUID_WICPixelFormat32bppRGBA;
}

void scalar_property(IPropertyBag2* properties, wchar_t* name, VARIANT value) {
    PROPBAG2 property{};
    property.pstrName = name;
    check(properties->Write(1, &property, &value), "set image encoding option");
}
#endif

} // namespace

std::string extension(const std::filesystem::path& path) {
    const auto encoded = path.extension().u8string();
    std::string result(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    std::ranges::transform(result, result.begin(), [](unsigned char byte) {
        return byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte + ('a' - 'A')) : static_cast<char>(byte);
    });
    return result;
}

bool is_pnm(std::span<const std::uint8_t> bytes) noexcept {
    return bytes.size() >= 3 && bytes[0] == 'P' &&
           bytes[1] >= '1' && bytes[1] <= '7' &&
           (space(bytes[2]) || (bytes[1] != '7' && bytes[2] == '#'));
}

bool is_pnm_extension(std::string_view value) noexcept {
    return value == ".pbm" || value == ".pgm" || value == ".ppm" || value == ".pam" || value == ".pnm";
}

bool is_image_extension(std::string_view value) noexcept {
    if (is_pnm_extension(value)) return true;
#ifdef TLG_TOOL_WIC
    return value == ".png" || value == ".bmp" || value == ".tif" || value == ".tiff" ||
           value == ".jpg" || value == ".jpeg";
#else
    return false;
#endif
}

tlg::Image read_pnm(std::span<const std::uint8_t> bytes, bool allow_lossy) {
    if (bytes.size() > byte_limit) throw std::runtime_error("portable image exceeds the 1 GiB input limit");
    PnmReader reader(bytes);
    const auto magic = reader.token();
    std::uint32_t width{}, height{}, maximum{}, depth{};
    tlg::PixelLayout layout{};
    bool ascii = false, bitmap = false, gray_alpha = false;
    if (magic.size() == 2 && magic[0] == 'P' && magic[1] >= '1' && magic[1] <= '6') {
        ascii = magic[1] <= '3';
        bitmap = magic == "P1" || magic == "P4";
        width = number(reader.token());
        height = number(reader.token());
        maximum = bitmap ? 1 : number(reader.token());
        layout = magic == "P3" || magic == "P6" ? tlg::PixelLayout::rgb : tlg::PixelLayout::gray;
        if (maximum > 65535) throw std::runtime_error("PNM MAXVAL exceeds 65535");
        const auto size = image_size(width, height, layout);
        if (!ascii) reader.raster_separator(bitmap ? ((std::size_t(width) + 7) / 8) * height :
                                           size * (maximum > 255 ? 2 : 1));
    } else if (magic == "P7") {
        reader.raster_separator();
        std::string_view tuple;
        bool has_maximum = false;
        while (true) {
            auto line = trim(reader.line());
            if (const auto comment = line.find('#'); comment != std::string_view::npos)
                line = trim(line.substr(0, comment));
            if (line.empty()) continue;
            if (line == "ENDHDR") break;
            const auto separator = line.find_first_of(" \t");
            if (separator == std::string_view::npos) throw std::runtime_error("invalid PAM header field");
            const auto key = line.substr(0, separator);
            const auto value = trim(line.substr(separator));
            if (key == "WIDTH" && width == 0) width = number(value);
            else if (key == "HEIGHT" && height == 0) height = number(value);
            else if (key == "DEPTH" && depth == 0) depth = number(value);
            else if (key == "MAXVAL" && !has_maximum) { maximum = number(value); has_maximum = true; }
            else if (key == "TUPLTYPE" && tuple.empty()) tuple = value;
            else throw std::runtime_error("duplicate or unsupported PAM header field");
        }
        if (depth == 1 && (tuple.empty() || tuple == "GRAYSCALE" || tuple == "BLACKANDWHITE")) layout = tlg::PixelLayout::gray;
        else if (depth == 3 && (tuple.empty() || tuple == "RGB")) layout = tlg::PixelLayout::rgb;
        else if (depth == 4 && (tuple.empty() || tuple == "RGB_ALPHA")) layout = tlg::PixelLayout::rgba;
        else if (depth == 2 && (tuple.empty() || tuple == "GRAYSCALE_ALPHA" || tuple == "BLACKANDWHITE_ALPHA")) {
            layout = tlg::PixelLayout::rgba; gray_alpha = true;
        } else throw std::runtime_error("unsupported PAM tuple layout");
        if ((tuple == "BLACKANDWHITE" || tuple == "BLACKANDWHITE_ALPHA") && maximum != 1)
            throw std::runtime_error("PAM black/white tuples require MAXVAL 1");
    } else throw std::runtime_error("supported portable image formats are binary P5, P6, and P7");
    if (maximum == 0 || maximum > 65535) throw std::runtime_error("PNM MAXVAL must be 1..65535");
    const auto output_size = image_size(width, height, layout);
    const auto sample_count = gray_alpha ? std::size_t(width) * height * 2 : output_size;
    const auto sample_bytes = maximum > 255 ? 2u : 1u;
    if (ascii) {
        if (sample_count > reader.remaining() / (bitmap ? 1 : 2))
            throw std::runtime_error("PNM dimensions cannot fit the ASCII raster");
    } else (void)reader.raster(bitmap ? ((std::size_t(width) + 7) / 8) * height : sample_count * sample_bytes);
    tlg::Image image{width, height, layout, std::vector<std::uint8_t>(output_size)};
    auto normalize = [maximum, allow_lossy](std::uint32_t value) {
        if (value > maximum) throw std::runtime_error("PNM sample exceeds MAXVAL");
        if (!allow_lossy && (value * 255) % maximum != 0)
            throw std::runtime_error("PNM sample cannot map exactly to 8 bits; pass --allow-lossy to normalize");
        return static_cast<std::uint8_t>((value * 255 + maximum / 2) / maximum);
    };
    if (ascii) {
        for (auto& pixel : image.pixels) {
            const auto value = bitmap ? reader.bitmap_bit() : number(reader.token(), true);
            pixel = normalize(bitmap ? (value > 1 ? throw std::runtime_error("invalid PBM bit") : 1 - value) : value);
        }
        if (bitmap) reader.finish_bitmap();
        else reader.finish_ascii();
    } else if (bitmap) {
        const auto stride = (std::size_t(width) + 7) / 8;
        const auto raster = reader.raster(stride * height);
        for (std::size_t y = 0; y < height; ++y)
            for (std::size_t x = 0; x < width; ++x)
                image.pixels[y * width + x] = (raster[y * stride + x / 8] >> (7 - x % 8)) & 1 ? 0 : 255;
    } else {
        const auto raster = reader.raster(sample_count * sample_bytes);
        auto sample = [&](std::size_t index) {
            const auto value = sample_bytes == 1 ? raster[index] :
                               (std::uint32_t(raster[index * 2]) << 8) | raster[index * 2 + 1];
            return normalize(value);
        };
        if (gray_alpha) {
            for (std::size_t i = 0; i < sample_count / 2; ++i) {
                const auto gray = sample(i * 2);
                image.pixels[i * 4] = image.pixels[i * 4 + 1] = image.pixels[i * 4 + 2] = gray;
                image.pixels[i * 4 + 3] = sample(i * 2 + 1);
            }
        } else for (std::size_t i = 0; i < sample_count; ++i) image.pixels[i] = sample(i);
    }
    return image;
}

std::vector<std::uint8_t> encode_pnm(tlg::ImageView image, std::string_view value) {
    const auto colors = tlg::channels(image.layout);
    if (value == ".pbm") {
        if (colors != 1 || !std::ranges::all_of(image.pixels, [](auto pixel) { return pixel == 0 || pixel == 255; }))
            throw std::invalid_argument("PBM requires black/white Gray pixels; use PGM for other shades");
        const auto header = "P4\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n";
        const auto stride = (std::size_t(image.width) + 7) / 8;
        std::vector<std::uint8_t> bytes(header.begin(), header.end());
        const auto offset = bytes.size();
        bytes.resize(offset + stride * image.height);
        for (std::size_t y = 0; y < image.height; ++y)
            for (std::size_t x = 0; x < image.width; ++x)
                if (image.pixels[y * image.width + x] == 0) bytes[offset + y * stride + x / 8] |= 1 << (7 - x % 8);
        return bytes;
    }
    const bool pam = value == ".pam" || (value == ".pnm" && colors == 4);
    if ((value == ".pgm" && colors != 1) || (value == ".ppm" && colors != 3) ||
        (!pam && colors == 4))
        throw std::invalid_argument("output extension cannot preserve this pixel layout; use .pam");
    std::string header;
    if (pam) {
        header = "P7\nWIDTH " + std::to_string(image.width) + "\nHEIGHT " + std::to_string(image.height) +
                 "\nDEPTH " + std::to_string(colors) + "\nMAXVAL 255\nTUPLTYPE " +
                 (colors == 1 ? "GRAYSCALE" : colors == 3 ? "RGB" : "RGB_ALPHA") + "\nENDHDR\n";
    } else header = std::string(colors == 1 ? "P5\n" : "P6\n") + std::to_string(image.width) + " " +
                    std::to_string(image.height) + "\n255\n";
    std::vector<std::uint8_t> bytes;
    bytes.reserve(header.size() + image.pixels.size());
    bytes.insert(bytes.end(), header.begin(), header.end());
    bytes.insert(bytes.end(), image.pixels.begin(), image.pixels.end());
    return bytes;
}

#ifdef TLG_TOOL_WIC
tlg::Image read_wic(const std::filesystem::path& path, bool allow_lossy) {
    WicContext context;
    ComPtr<IWICBitmapDecoder> decoder;
    check(context.factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
          WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()), "open input image");
    UINT frames{};
    check(decoder->GetFrameCount(&frames), "read image frame count");
    if (frames != 1) throw std::runtime_error("multi-frame images require selecting a frame before conversion");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, frame.GetAddressOf()), "read image frame");
    UINT width{}, height{};
    check(frame->GetSize(&width, &height), "read image dimensions");
    WICPixelFormatGUID source_format{};
    check(frame->GetPixelFormat(&source_format), "read image pixel format");

    ComPtr<IWICComponentInfo> component;
    check(context.factory->CreateComponentInfo(source_format, component.GetAddressOf()), "inspect image pixel format");
    ComPtr<IWICPixelFormatInfo> pixel_info;
    check(component.As(&pixel_info), "inspect image channel depths");
    UINT channel_count{};
    check(pixel_info->GetChannelCount(&channel_count), "read image channel count");
    for (UINT channel = 0; channel < channel_count; ++channel) {
        std::array<BYTE, 32> mask{};
        UINT written{};
        check(pixel_info->GetChannelMask(channel, static_cast<UINT>(mask.size()), mask.data(), &written),
              "read image channel precision");
        unsigned bits = 0;
        for (UINT byte = 0; byte < written; ++byte) bits += std::popcount(mask[byte]);
        if (bits > 8 && !allow_lossy)
            throw std::runtime_error("input has channels above 8 bits; pass --allow-lossy to normalize");
    }

    bool gray = IsEqualGUID(source_format, GUID_WICPixelFormat8bppGray) ||
                IsEqualGUID(source_format, GUID_WICPixelFormat4bppGray) ||
                IsEqualGUID(source_format, GUID_WICPixelFormat2bppGray) ||
                IsEqualGUID(source_format, GUID_WICPixelFormatBlackWhite);
    ComPtr<IWICPixelFormatInfo2> pixel_info2;
    check(component.As(&pixel_info2), "inspect image transparency");
    BOOL transparent{};
    check(pixel_info2->SupportsTransparency(&transparent), "read image transparency support");
    if (IsEqualGUID(source_format, GUID_WICPixelFormat1bppIndexed) ||
        IsEqualGUID(source_format, GUID_WICPixelFormat2bppIndexed) ||
        IsEqualGUID(source_format, GUID_WICPixelFormat4bppIndexed) ||
        IsEqualGUID(source_format, GUID_WICPixelFormat8bppIndexed)) {
        ComPtr<IWICPalette> palette;
        check(context.factory->CreatePalette(palette.GetAddressOf()), "create input palette");
        check(frame->CopyPalette(palette.Get()), "read input palette");
        check(palette->HasAlpha(&transparent), "read palette transparency");
        BOOL grayscale{};
        check(palette->IsGrayscale(&grayscale), "read palette grayscale status");
        gray = grayscale && !transparent;
    }
    const auto layout = gray ? tlg::PixelLayout::gray :
                        transparent ? tlg::PixelLayout::rgba : tlg::PixelLayout::rgb;
    tlg::Image image{width, height, layout, std::vector<std::uint8_t>(image_size(width, height, layout))};
    ComPtr<IWICFormatConverter> converter;
    check(context.factory->CreateFormatConverter(converter.GetAddressOf()), "create pixel converter");
    check(converter->Initialize(frame.Get(), pixel_format(layout), WICBitmapDitherTypeNone, nullptr,
                               0, WICBitmapPaletteTypeCustom), "convert input pixels");
    check(converter->CopyPixels(nullptr, width * static_cast<UINT>(tlg::channels(layout)),
          static_cast<UINT>(image.pixels.size()), image.pixels.data()), "read input pixels");
    return image;
}

void write_wic(const std::filesystem::path& path, tlg::ImageView image,
               std::string_view value, bool allow_lossy) {
    const bool jpeg = value == ".jpg" || value == ".jpeg";
    if (jpeg && !allow_lossy) throw std::invalid_argument("JPEG is lossy; pass --allow-lossy to request it");
    if (jpeg && image.layout == tlg::PixelLayout::rgba)
        throw std::invalid_argument("JPEG cannot preserve alpha; use PNG, TIFF, or PAM");
    WicContext context;
    GUID container{};
    if (value == ".png") container = GUID_ContainerFormatPng;
    else if (value == ".bmp") container = GUID_ContainerFormatBmp;
    else if (value == ".tif" || value == ".tiff") container = GUID_ContainerFormatTiff;
    else if (jpeg) container = GUID_ContainerFormatJpeg;
    else throw std::invalid_argument("unsupported image output extension");
    ComPtr<IWICStream> stream;
    check(context.factory->CreateStream(stream.GetAddressOf()), "create image output stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "open image output");
    ComPtr<IWICBitmapEncoder> encoder;
    check(context.factory->CreateEncoder(container, nullptr, encoder.GetAddressOf()), "create image encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "initialize image encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    check(encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf()), "create output image frame");
    if (value == ".bmp" && image.layout == tlg::PixelLayout::rgba) {
        VARIANT property{}; property.vt = VT_BOOL; property.boolVal = VARIANT_TRUE;
        scalar_property(properties.Get(), const_cast<wchar_t*>(L"EnableV5Header32bppBGRA"), property);
    }
    if (value == ".tif" || value == ".tiff") {
        VARIANT property{}; property.vt = VT_UI1; property.bVal = WICTiffCompressionLZW;
        scalar_property(properties.Get(), const_cast<wchar_t*>(L"TiffCompressionMethod"), property);
    }
    if (jpeg) {
        VARIANT property{}; property.vt = VT_R4; property.fltVal = 1.0f;
        scalar_property(properties.Get(), const_cast<wchar_t*>(L"ImageQuality"), property);
    }
    check(frame->Initialize(properties.Get()), "initialize output image frame");
    check(frame->SetSize(image.width, image.height), "set output dimensions");
    auto selected_format = pixel_format(image.layout);
    check(frame->SetPixelFormat(&selected_format), "select output pixel format");
    ComPtr<IWICPalette> palette;
    if (IsEqualGUID(selected_format, GUID_WICPixelFormat8bppIndexed)) {
        if (image.layout != tlg::PixelLayout::gray)
            throw std::runtime_error("image encoder requires color quantization; choose PNG or PAM");
        check(context.factory->CreatePalette(palette.GetAddressOf()), "create grayscale output palette");
        check(palette->InitializePredefined(WICBitmapPaletteTypeFixedGray256, FALSE), "initialize grayscale output palette");
        check(frame->SetPalette(palette.Get()), "set grayscale output palette");
    }
    ComPtr<IWICBitmap> source;
    check(context.factory->CreateBitmapFromMemory(image.width, image.height, pixel_format(image.layout),
          image.width * static_cast<UINT>(tlg::channels(image.layout)), static_cast<UINT>(image.pixels.size()),
          const_cast<BYTE*>(image.pixels.data()), source.GetAddressOf()), "prepare output pixels");
    ComPtr<IWICFormatConverter> converter;
    check(context.factory->CreateFormatConverter(converter.GetAddressOf()), "create output converter");
    check(converter->Initialize(source.Get(), selected_format, WICBitmapDitherTypeNone, palette.Get(), 0,
          WICBitmapPaletteTypeCustom), "convert output pixels");
    check(frame->WriteSource(converter.Get(), nullptr), "write output pixels");
    check(frame->Commit(), "finish output image frame");
    check(encoder->Commit(), "finish output image");
    // WIC can negotiate a different format. A lossless request earns its name
    // only if decoding gives back every channel, including transparent RGB.
    stream.Reset(); frame.Reset(); properties.Reset(); encoder.Reset(); converter.Reset(); source.Reset();
    if (!jpeg) {
        const auto restored = read_wic(path);
        const auto source_colors = tlg::channels(image.layout);
        const auto restored_colors = tlg::channels(restored.layout);
        bool equal = restored.width == image.width && restored.height == image.height;
        for (std::size_t i = 0; equal && i < std::size_t(image.width) * image.height; ++i) {
            for (std::size_t c = 0; c < (source_colors == 1 ? 3 : source_colors); ++c) {
                const auto actual = c == 3 && restored_colors < 4 ? 255 :
                                    restored.pixels[i * restored_colors + (restored_colors == 1 ? 0 : c)];
                equal &= actual == image.pixels[i * source_colors + (source_colors == 1 ? 0 : c)];
            }
        }
        if (!equal) throw std::runtime_error("image backend changed pixels; choose PNG or PAM to preserve this layout");
    }
}
#endif

} // namespace tlg_tool
