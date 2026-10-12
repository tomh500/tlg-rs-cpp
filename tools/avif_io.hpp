#pragma once

#include <tlg/tlg.hpp>

namespace tlg_tool {

[[nodiscard]] bool is_avif(std::span<const std::uint8_t> prefix) noexcept;
// 10/12-bit input requires allow_lossy because the TLG API stores 8-bit pixels.
// Still-image crop, rotation and mirroring are applied to the returned pixels.
[[nodiscard]] tlg::Image read_avif(std::span<const std::uint8_t> encoded, bool allow_lossy = false);
// Default encoding is lossless and verifies the decoded bytes before returning.
[[nodiscard]] std::vector<std::uint8_t> encode_avif(tlg::ImageView image, bool allow_lossy = false);

} // namespace tlg_tool
