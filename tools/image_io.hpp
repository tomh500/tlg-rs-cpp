#pragma once

#include <tlg/tlg.hpp>

#include <string_view>

namespace tlg_tool {

[[nodiscard]] std::string extension(const std::filesystem::path& path);
[[nodiscard]] bool is_pnm(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool is_pnm_extension(std::string_view extension) noexcept;
[[nodiscard]] bool is_image_extension(std::string_view extension) noexcept;
[[nodiscard]] tlg::Image read_pnm(std::span<const std::uint8_t> bytes, bool allow_lossy = false);
[[nodiscard]] std::vector<std::uint8_t> encode_pnm(tlg::ImageView image, std::string_view extension);

#ifdef TLG_TOOL_WIC
[[nodiscard]] tlg::Image read_wic(const std::filesystem::path& path, bool allow_lossy = false);
void write_wic(const std::filesystem::path& path, tlg::ImageView image,
               std::string_view extension, bool allow_lossy);
#endif

} // namespace tlg_tool
