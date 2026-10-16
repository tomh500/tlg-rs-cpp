#include <tlg/tlg.hpp>
#include "image_io.hpp"
#include "portable_io.hpp"
#include "dds_io.hpp"
#ifdef TLG_TOOL_AVIF
#include "avif_io.hpp"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#ifndef TLG_TOOL_VERSION
#define TLG_TOOL_VERSION "1.2.0"
#endif

namespace {

constexpr std::string_view help = R"(tlg_tool - lossless TLG5/TLG6 codec

  tlg_tool encode [-v 5|6] [--tag KEY=VALUE]... INPUT_IMAGE OUTPUT.tlg
  tlg_tool encode [--format 5|6] --width W --height H --layout gray|rgb|rgba
                  [--tag KEY=VALUE]... INPUT.raw OUTPUT.tlg
  tlg_tool transcode [-v 5|6] [--tag KEY=VALUE]... INPUT.tlg OUTPUT.tlg
  tlg_tool decode [--allow-lossy] INPUT.tlg OUTPUT_IMAGE_OR.raw
  tlg_tool info INPUT.tlg
  tlg_tool detect INPUT

Algorithm profile (encode, transcode, decode, info):
  --profile reference|optimized|compact  C++ algorithm profile (default optimized)
Encoding options:
  -v, --version, --format 5|6          TLG version (default 6)
  --slide-parse greedy|optimal        Slide parser (default greedy)
  --slide-match adaptive|primary      Slide match index (default adaptive)
  --slide-memory BYTES                Optimal parser memory ceiling
  --filter-search factored|reference|refined  TLG6 filter search (default factored)
  --refine-work BYTES                 Residual scoring budget (default 67108864)
Decoding options:
  --slide-decode bulk|scalar          Slide decoder (default bulk)

Raw pixels are packed Gray/RGB/RGBA bytes, top to bottom. No row padding.
Images are detected by contents; TGA falls back to its filename extension.
Portable PBM/PGM/PPM/PAM, QOI, TGA, farbfeld, and DDS are always available.
DDS reads raw RGB/Gray/RGBA and BC1/BC2/BC3; it writes uncompressed pixels.
High-depth input requires exact mapping to 8 bits or explicit --allow-lossy.
TLG transcodes preserve metadata; --tag replaces or adds the specified key.
Use .raw for raw decoding, .pgm for Gray, .ppm for RGB, .pam for any layout.
Unknown output extensions are rejected. JPEG output requires --allow-lossy.
detect inspects only the header; info validates and decodes the full file.
Tags use UTF-8 byte lengths. Existing outputs are replaced after conversion succeeds.
Exit codes: 0 success, 1 codec/I/O failure (or detect found no TLG), 2 command-line error.
)";

constexpr std::array<std::uint8_t, 11> magic_sds{'T','L','G','0','.','0',0,'s','d','s',0x1a};
constexpr std::size_t byte_limit = std::size_t{1} << 30;

std::filesystem::path utf8_path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

template<class T>
T positive_number(std::string_view value, std::string_view name) {
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0)
        throw std::invalid_argument(std::string(name) + " must be a positive integer");
    return result;
}

void different_paths(const std::filesystem::path& input, const std::filesystem::path& output) {
    std::error_code error;
    if (std::filesystem::equivalent(input, output, error))
        throw std::invalid_argument("input and output refer to the same file");
    const auto in_absolute = std::filesystem::absolute(input).lexically_normal();
    const auto out_absolute = std::filesystem::absolute(output).lexically_normal();
    if (in_absolute == out_absolute)
        throw std::invalid_argument("input and output refer to the same path");
}

int report(const tlg::Error& error) {
    std::cerr << tlg::error_name(error.code) << " at byte " << error.offset
              << ": " << error.message << '\n';
    return 1;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path,
                                   std::size_t expected = std::numeric_limits<std::size_t>::max()) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open input file");
    const auto length = file.tellg();
    if (length < 0) throw std::runtime_error("cannot determine input length");
    const auto count = static_cast<std::uint64_t>(length);
    if (count > byte_limit) throw std::runtime_error("input exceeds the tool's 1 GiB byte limit");
    if (expected != std::numeric_limits<std::size_t>::max() && count != expected)
        throw std::runtime_error("raw input length does not match dimensions and layout");
    file.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(count));
    if (!bytes.empty())
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("cannot read complete input");
    return bytes;
}

std::vector<std::uint8_t> read_prefix(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open input file");
    std::vector<std::uint8_t> prefix(256);
    file.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
    if (file.bad()) throw std::runtime_error("cannot read input header");
    prefix.resize(static_cast<std::size_t>(file.gcount()));
    return prefix;
}

std::optional<tlg::Format> detect(std::span<const std::uint8_t> prefix, bool& sds) {
    sds = false;
    if (prefix.size() < 11) return {};
    sds = std::ranges::equal(prefix.first(11), magic_sds);
    const auto result = tlg::detect_format(prefix);
    if (result) return *result;
    if (!sds && result.error().code == tlg::ErrorCode::unsupported_format) return {};
    throw std::runtime_error(result.error().message);
}

void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot open output file");
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("cannot write complete output");
}

class StagedOutput {
    std::filesystem::path path_;
public:
    explicit StagedOutput(const std::filesystem::path& output) {
        std::random_device random;
        const auto parent = std::filesystem::absolute(output).parent_path();
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            path_ = parent / (".tlg-tool-" + std::to_string(random()) + "-" +
                              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp");
            std::ofstream reserve(path_, std::ios::binary | std::ios::out | std::ios::noreplace);
            if (reserve) return;
        }
        throw std::runtime_error("cannot reserve a temporary output beside the destination");
    }
    ~StagedOutput() { std::error_code error; std::filesystem::remove(path_, error); }
    const std::filesystem::path& path() const noexcept { return path_; }
    void finish(const std::filesystem::path& output) {
        if (std::filesystem::file_size(path_) > byte_limit)
            throw std::runtime_error("completed output exceeds the tool's 1 GiB byte limit");
#ifdef _WIN32
        if (!MoveFileExW(path_.c_str(), std::filesystem::absolute(output).c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace destination with completed output");
#else
        std::filesystem::rename(path_, output);
#endif
    }
};

void save_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    if (bytes.size() > byte_limit) throw std::runtime_error("output exceeds the tool's 1 GiB byte limit");
    StagedOutput staged(path);
    write_bytes(staged.path(), bytes);
    staged.finish(path);
}

void print_quoted(std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    std::cout << '"';
    for (const unsigned char byte : value) {
        if (byte == '"' || byte == '\\') std::cout << '\\' << static_cast<char>(byte);
        else if (byte < 0x20 || byte == 0x7f)
            std::cout << "\\u00" << hex[byte >> 4] << hex[byte & 15];
        else std::cout << static_cast<char>(byte);
    }
    std::cout << '"';
}

struct Arguments {
    tlg::EncodeOptions encode;
    tlg::DecodeOptions decode;
    tlg::Tags tags;
    std::uint32_t width{}, height{};
    tlg::PixelLayout layout{};
    bool allow_lossy = false;
    std::vector<std::string> paths;
};

Arguments parse(const std::vector<std::string>& args) {
    const bool encoding = args[1] == "encode" || args[1] == "transcode";
    Arguments parsed;
    bool options_ended = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (!options_ended && arg == "--") { options_ended = true; continue; }
        if (!options_ended && arg == "--allow-lossy" && (args[1] == "decode" || args[1] == "encode")) {
            parsed.allow_lossy = true; continue;
        }
        if (options_ended || (!arg.starts_with("--") && arg != "-v")) {
            parsed.paths.push_back(arg); continue;
        }
        if (++i == args.size()) throw std::invalid_argument("missing value for " + arg);
        const auto& value = args[i];
        if (args[1] != "detect" && arg == "--profile") {
            // These are our C++ reference paths. Calling them an exact Rust
            // replica would be a bit too generous to the word "exact".
            if (value != "reference" && value != "optimized" && value != "compact")
                throw std::invalid_argument("profile must be reference, optimized, or compact");
            const bool reference = value == "reference";
            const bool compact = value == "compact";
            parsed.encode.compare_slide_parses = compact;
            parsed.encode.slide_parse = compact ? tlg::SlideParse::optimal : tlg::SlideParse::greedy;
            parsed.encode.slide_match = reference ? tlg::SlideMatch::primary : tlg::SlideMatch::adaptive;
            parsed.encode.tlg6_filter_search = reference ? tlg::Tlg6FilterSearch::reference : tlg::Tlg6FilterSearch::factored;
            if (compact) parsed.encode.tlg6_filter_search = tlg::Tlg6FilterSearch::refined;
            parsed.decode.slide_decode = reference ? tlg::SlideDecode::scalar : tlg::SlideDecode::bulk;
        } else if (encoding && (arg == "--format" || arg == "--version" || arg == "-v")) {
            if (value == "5") parsed.encode.format = tlg::Format::tlg5;
            else if (value == "6") parsed.encode.format = tlg::Format::tlg6;
            else throw std::invalid_argument("format must be 5 or 6");
        } else if (encoding && arg == "--slide-parse") {
            if (value == "greedy") parsed.encode.slide_parse = tlg::SlideParse::greedy;
            else if (value == "optimal") parsed.encode.slide_parse = tlg::SlideParse::optimal;
            else throw std::invalid_argument("slide parser must be greedy or optimal");
        } else if (encoding && arg == "--slide-match") {
            if (value == "adaptive") parsed.encode.slide_match = tlg::SlideMatch::adaptive;
            else if (value == "primary") parsed.encode.slide_match = tlg::SlideMatch::primary;
            else throw std::invalid_argument("slide match index must be adaptive or primary");
        } else if (encoding && arg == "--filter-search") {
            if (value == "factored") parsed.encode.tlg6_filter_search = tlg::Tlg6FilterSearch::factored;
            else if (value == "reference") parsed.encode.tlg6_filter_search = tlg::Tlg6FilterSearch::reference;
            else if (value == "refined") parsed.encode.tlg6_filter_search = tlg::Tlg6FilterSearch::refined;
            else throw std::invalid_argument("filter search must be factored, reference, or refined");
        } else if (encoding && arg == "--refine-work") {
            parsed.encode.max_tlg6_refinement_work = positive_number<std::size_t>(value, "refinement work budget");
        } else if (encoding && arg == "--slide-memory") {
            parsed.encode.max_slide_working_bytes = positive_number<std::size_t>(value, "Slide memory ceiling");
        } else if (args[1] != "detect" && arg == "--slide-decode") {
            if (value == "bulk") parsed.decode.slide_decode = tlg::SlideDecode::bulk;
            else if (value == "scalar") parsed.decode.slide_decode = tlg::SlideDecode::scalar;
            else throw std::invalid_argument("slide decoder must be bulk or scalar");
        } else if (args[1] == "encode" && arg == "--width") {
            parsed.width = positive_number<std::uint32_t>(value, "width");
        } else if (args[1] == "encode" && arg == "--height") {
            parsed.height = positive_number<std::uint32_t>(value, "height");
        } else if (args[1] == "encode" && arg == "--layout") {
            if (value == "gray") parsed.layout = tlg::PixelLayout::gray;
            else if (value == "rgb") parsed.layout = tlg::PixelLayout::rgb;
            else if (value == "rgba") parsed.layout = tlg::PixelLayout::rgba;
            else throw std::invalid_argument("layout must be gray, rgb, or rgba");
        } else if (encoding && arg == "--tag") {
            const auto separator = value.find('=');
            if (separator == std::string::npos) throw std::invalid_argument("tag must be KEY=VALUE");
            if (!parsed.tags.emplace(value.substr(0, separator), value.substr(separator + 1)).second)
                throw std::invalid_argument("duplicate tag key");
        } else throw std::invalid_argument("unknown option: " + arg);
    }
    return parsed;
}

int execute(const std::vector<std::string>& args) {
    if ((args.size() == 2 && (args[1] == "--help" || args[1] == "-h")) ||
        (args.size() == 3 && (args[2] == "--help" || args[2] == "-h"))) {
        std::cout << help;
#ifdef TLG_TOOL_WIC
        std::cout << "WIC image backend: PNG/BMP/TIFF/JPEG input and output (Windows codecs).\n";
#endif
#ifdef TLG_TOOL_AVIF
        std::cout << "AVIF image backend: optional libavif input and output.\n";
#endif
        return 0;
    }
    if (args.size() == 2 && args[1] == "--version") {
        std::cout << "tlg_tool " TLG_TOOL_VERSION "\n"; return 0;
    }
    if (args.size() < 2) throw std::invalid_argument("missing command; use --help");
    const auto& command = args[1];
    if (command != "encode" && command != "transcode" && command != "decode" &&
        command != "info" && command != "detect")
        throw std::invalid_argument("unknown command: " + command);
    auto parsed = parse(args);
    const bool encoding = command == "encode" || command == "transcode";
    const bool one_path = command == "info" || command == "detect";
    if (parsed.paths.size() != (one_path ? 1u : 2u))
        throw std::invalid_argument(command + (one_path ? " requires one input path" : " requires input and output paths"));
    const auto input = utf8_path(parsed.paths[0]);
    const auto output = one_path ? std::filesystem::path{} : utf8_path(parsed.paths[1]);
    if (!one_path) different_paths(input, output);
    if (command == "detect") {
        bool sds{};
        const auto format = detect(read_prefix(input), sds);
        if (!format) { std::cout << "not TLG\n"; return 1; }
        std::cout << (sds ? "SDS " : "") << "TLG" << (*format == tlg::Format::tlg5 ? 5 : 6) << '\n';
        return 0;
    }
    if (encoding) {
        tlg::Image image;
        tlg::Tags tags;
        if (parsed.width != 0 || parsed.height != 0 || tlg::channels(parsed.layout) != 0) {
            if (parsed.width == 0 || parsed.height == 0 || tlg::channels(parsed.layout) == 0)
                throw std::invalid_argument("raw encoding requires width, height, and layout together");
            const auto count = std::uint64_t{parsed.width} * parsed.height;
            if (count > 268'435'456 || count > byte_limit / tlg::channels(parsed.layout))
                throw std::invalid_argument("raw image exceeds the tool's pixel or 1 GiB byte limit");
            image = {parsed.width, parsed.height, parsed.layout,
                     read_bytes(input, static_cast<std::size_t>(count) * tlg::channels(parsed.layout))};
        } else {
            const auto prefix = read_prefix(input);
            bool sds{};
            if (detect(prefix, sds)) {
                auto decoded = tlg::read_file(input, parsed.decode);
                if (!decoded) return report(decoded.error());
                image = std::move(decoded->image);
                tags = std::move(decoded->tags);
            } else if (command == "transcode") {
                throw std::runtime_error("transcode requires a TLG5, TLG6, or SDS input");
            } else if (tlg_tool::is_pnm(prefix)) {
                image = tlg_tool::read_pnm(read_bytes(input), parsed.allow_lossy);
            } else if (tlg_tool::is_portable_image(prefix, tlg_tool::extension(input))) {
                image = tlg_tool::read_portable_image(read_bytes(input), tlg_tool::extension(input), parsed.allow_lossy);
            } else if (tlg_tool::is_dds(prefix)) {
                image = tlg_tool::read_dds(read_bytes(input), parsed.allow_lossy);
#ifdef TLG_TOOL_AVIF
            } else if (tlg_tool::is_avif(prefix) || tlg_tool::extension(input) == ".avif" ||
                       (prefix.size() >= 12 && prefix[4] == 'f' && prefix[5] == 't' && prefix[6] == 'y' && prefix[7] == 'p')) {
                image = tlg_tool::read_avif(read_bytes(input), parsed.allow_lossy);
#endif
            } else {
#ifdef TLG_TOOL_WIC
                if (std::filesystem::file_size(input) > byte_limit)
                    throw std::runtime_error("input exceeds the tool's 1 GiB byte limit");
                image = tlg_tool::read_wic(input, parsed.allow_lossy);
#else
                throw std::runtime_error("unsupported image input; use a portable image format listed by --help, or specify dimensions and layout for raw pixels");
#endif
            }
        }
        for (auto& [key, value] : parsed.tags) tags.insert_or_assign(key, std::move(value));
        const auto encoded = tlg::encode(image.view(), parsed.encode, tags);
        if (!encoded) return report(encoded.error());
        save_bytes(output, *encoded);
        return 0;
    }
    const auto output_extension = one_path ? std::string{} : tlg_tool::extension(output);
    if (!one_path && output_extension != ".raw" && !tlg_tool::is_image_extension(output_extension) &&
        !tlg_tool::is_portable_extension(output_extension) && output_extension != ".dds"
#ifdef TLG_TOOL_AVIF
        && output_extension != ".avif"
#endif
        )
        throw std::invalid_argument("unsupported output extension; use .raw, .pgm, .ppm, .pam, or an enabled image format");
    if ((output_extension == ".jpg" || output_extension == ".jpeg") && !parsed.allow_lossy)
        throw std::invalid_argument("JPEG is lossy; pass --allow-lossy to request it");
    const auto decoded = tlg::read_file(input, parsed.decode);
    if (!decoded) return report(decoded.error());
    if (command == "info") {
        std::cout << "TLG" << (decoded->format == tlg::Format::tlg5 ? 5 : 6)
                  << ' ' << decoded->image.width << 'x' << decoded->image.height
                  << ' ' << tlg::channels(decoded->image.layout) << " channels\n";
        for (const auto& [key, value] : decoded->tags) {
            print_quoted(key); std::cout << " = "; print_quoted(value); std::cout << '\n';
        }
    } else if (output_extension == ".raw") save_bytes(output, decoded->image.pixels);
    else if (tlg_tool::is_pnm_extension(output_extension))
        save_bytes(output, tlg_tool::encode_pnm(decoded->image.view(), output_extension));
    else if (tlg_tool::is_portable_extension(output_extension))
        save_bytes(output, tlg_tool::encode_portable_image(decoded->image.view(), output_extension));
    else if (output_extension == ".dds") save_bytes(output, tlg_tool::encode_dds(decoded->image.view()));
#ifdef TLG_TOOL_AVIF
    else if (output_extension == ".avif")
        save_bytes(output, tlg_tool::encode_avif(decoded->image.view(), parsed.allow_lossy));
#endif
    else {
#ifdef TLG_TOOL_WIC
        StagedOutput staged(output);
        tlg_tool::write_wic(staged.path(), decoded->image.view(), output_extension, parsed.allow_lossy);
        staged.finish(output);
#else
        throw std::invalid_argument("this build does not have the Windows image backend");
#endif
    }
    return 0;
}

int checked_execute(const std::vector<std::string>& args) {
    try { return execute(args); }
    catch (const std::invalid_argument& error) { std::cerr << error.what() << '\n'; return 2; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        const auto utf8 = std::filesystem::path(argv[i]).u8string();
        args.emplace_back(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    }
    return checked_execute(args);
}
#else
int main(int argc, char** argv) {
    return checked_execute(std::vector<std::string>(argv, argv + argc));
}
#endif
