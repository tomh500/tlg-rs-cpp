#pragma once
#include "image_io.hpp"

namespace tlg_tool {
[[nodiscard]] bool is_portable_image(std::span<const std::uint8_t> bytes, std::string_view extension) noexcept;
[[nodiscard]] bool is_portable_extension(std::string_view extension) noexcept;
[[nodiscard]] tlg::Image read_portable_image(std::span<const std::uint8_t> bytes, std::string_view extension,
                                           bool allow_lossy = false);
[[nodiscard]] std::vector<std::uint8_t> encode_portable_image(tlg::ImageView image, std::string_view extension);
} // namespace tlg_tool
