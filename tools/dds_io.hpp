#pragma once
#include "image_io.hpp"
namespace tlg_tool {
[[nodiscard]] bool is_dds(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] tlg::Image read_dds(std::span<const std::uint8_t> bytes, bool allow_lossy = false);
[[nodiscard]] std::vector<std::uint8_t> encode_dds(tlg::ImageView image);
} // namespace tlg_tool
