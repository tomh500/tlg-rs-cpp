#include <tlg/tlg.hpp>

#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

constexpr std::string_view help = R"(tlg_tool - lossless TLG5/TLG6 codec

  tlg_tool encode --format 5|6 --width W --height H --layout gray|rgb|rgba
                  [--tag KEY=VALUE]... INPUT.raw OUTPUT.tlg
  tlg_tool decode INPUT.tlg OUTPUT.raw
  tlg_tool info INPUT.tlg

Raw pixels are packed Gray/RGB/RGBA bytes, top to bottom. No row padding.
Format defaults to 6. Tags use UTF-8 byte lengths. Output paths may be overwritten.
Exit codes: 0 success, 1 codec/I/O failure, 2 command-line error.
)";

std::filesystem::path utf8_path(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

std::uint32_t dimension(std::string_view value) {
    std::uint32_t result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result == 0)
        throw std::invalid_argument("dimensions must be positive 32-bit integers");
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

std::vector<std::uint8_t> read_raw(const std::filesystem::path& path, std::size_t expected) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("cannot open raw input file");
    const auto length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) != expected)
        throw std::runtime_error("raw input length does not match dimensions and layout");
    file.seekg(0);
    std::vector<std::uint8_t> bytes(expected);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) throw std::runtime_error("cannot read complete raw input");
    return bytes;
}

void write_raw(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) throw std::runtime_error("cannot open raw output file");
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.close();
    if (!file) throw std::runtime_error("cannot write complete raw output");
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

int execute(const std::vector<std::string>& args) {
    if (args.size() == 2 && (args[1] == "--help" || args[1] == "-h")) {
        std::cout << help;
        return 0;
    }
    if (args.size() < 2) throw std::invalid_argument("missing command; use --help");
    if (args[1] == "encode") {
        tlg::EncodeOptions options;
        tlg::Tags tags;
        std::uint32_t width{}, height{};
        tlg::PixelLayout layout{};
        std::vector<std::string> paths;
        bool options_ended = false;
        for (std::size_t i = 2; i < args.size(); ++i) {
            const auto& arg = args[i];
            if (!options_ended && arg == "--") { options_ended = true; continue; }
            if (!options_ended && arg.starts_with("--")) {
                if (arg != "--format" && arg != "--width" && arg != "--height" &&
                    arg != "--layout" && arg != "--tag")
                    throw std::invalid_argument("unknown option: " + arg);
                if (++i == args.size()) throw std::invalid_argument("missing value for " + arg);
                const auto& value = args[i];
                if (arg == "--format") {
                    if (value == "5") options.format = tlg::Format::tlg5;
                    else if (value == "6") options.format = tlg::Format::tlg6;
                    else throw std::invalid_argument("format must be 5 or 6");
                } else if (arg == "--width") width = dimension(value);
                else if (arg == "--height") height = dimension(value);
                else if (arg == "--layout") {
                    if (value == "gray") layout = tlg::PixelLayout::gray;
                    else if (value == "rgb") layout = tlg::PixelLayout::rgb;
                    else if (value == "rgba") layout = tlg::PixelLayout::rgba;
                    else throw std::invalid_argument("layout must be gray, rgb, or rgba");
                } else {
                    const auto separator = value.find('=');
                    if (separator == std::string::npos)
                        throw std::invalid_argument("tag must be KEY=VALUE");
                    if (!tags.emplace(value.substr(0, separator), value.substr(separator + 1)).second)
                        throw std::invalid_argument("duplicate tag key");
                }
            } else paths.push_back(arg);
        }
        if (width == 0 || height == 0 || tlg::channels(layout) == 0 || paths.size() != 2)
            throw std::invalid_argument("encode requires width, height, layout, input, and output");
        const auto count = std::uint64_t{width} * height;
        if (count > (std::uint64_t{1} << 30) / tlg::channels(layout))
            throw std::invalid_argument("raw image exceeds the tool's 1 GiB limit");
        const auto input = utf8_path(paths[0]);
        const auto output = utf8_path(paths[1]);
        different_paths(input, output);
        auto pixels = read_raw(input, static_cast<std::size_t>(count) * tlg::channels(layout));
        const auto result = tlg::write_file(output, {width, height, layout, pixels}, options, tags);
        return result ? 0 : report(result.error());
    }
    if (args[1] != "decode" && args[1] != "info")
        throw std::invalid_argument("unknown command: " + args[1]);
    const bool info = args[1] == "info";
    if (args.size() != (info ? 3u : 4u))
        throw std::invalid_argument(info ? "info requires one input path" : "decode requires input and output paths");
    const auto input = utf8_path(args[2]);
    if (!info) different_paths(input, utf8_path(args[3]));
    const auto decoded = tlg::read_file(input);
    if (!decoded) return report(decoded.error());
    if (!info) { write_raw(utf8_path(args[3]), decoded->image.pixels); return 0; }
    std::cout << "TLG" << (decoded->format == tlg::Format::tlg5 ? 5 : 6)
              << ' ' << decoded->image.width << 'x' << decoded->image.height
              << ' ' << tlg::channels(decoded->image.layout) << " channels\n";
    for (const auto& [key, value] : decoded->tags) {
        print_quoted(key);
        std::cout << " = ";
        print_quoted(value);
        std::cout << '\n';
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
