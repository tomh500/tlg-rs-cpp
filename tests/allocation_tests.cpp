#include <tlg/tlg.hpp>
#include "internal.hpp"
#include "slide.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
bool watching{};
std::size_t largest{};
constexpr std::size_t allocation_budget = 65536;
}

// Intercept normal allocations in this executable only. A malformed small
// stream must fail parsing before it tries to allocate the claimed image.
void* operator new(std::size_t size) {
    if (watching) {
        largest = std::max(largest, size);
        if (size > allocation_budget) throw std::bad_alloc{};
    }
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
void rejects_without_large_allocation(std::span<const std::uint8_t> bytes,
                                      tlg::ErrorCode code, const tlg::DecodeOptions& options = {}) {
    largest = 0;
    watching = true;
    const auto result = tlg::decode(bytes, options);
    watching = false;
    if (result || result.error().code != code || largest > allocation_budget) {
        throw std::runtime_error("small invalid input allocated " + std::to_string(largest)
            + " bytes; expected " + tlg::error_name(code));
    }
}
void put32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}
std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    tlg::detail::ByteReader reader(std::span(bytes).subspan(offset, 4));
    return reader.u32();
}
}

int main() try {
    tlg::detail::ByteWriter tlg5;
    tlg5.append(tlg::detail::magic5);
    tlg5.u8(1); tlg5.u32(1 << 20); tlg5.u32(1); tlg5.u32(1);
    tlg5.u32(0); // A complete size table, but no channel headers.
    rejects_without_large_allocation(tlg5.data, tlg::ErrorCode::truncated_input);

    tlg5.data.resize(24);
    tlg5.u32(6); tlg5.u8(0); tlg5.u32(1); tlg5.u8(0);
    // One flag byte cannot produce the declared one-megabyte plane.
    rejects_without_large_allocation(tlg5.data, tlg::ErrorCode::truncated_input);

    // Build a valid large compressed image before enabling the allocation cap.
    const std::vector<std::uint8_t> pixels(1024 * 1024);
    auto encoded = tlg::encode({1024, 1024, tlg::PixelLayout::gray, pixels});
    if (!encoded) throw std::runtime_error(encoded.error().message);
    auto truncated = *encoded;
    truncated.resize(31 + get32(truncated, 27)); // All filters, no entropy streams.
    rejects_without_large_allocation(truncated, tlg::ErrorCode::truncated_input);
    // A short invalid filter stream must not allocate its declared filter table.
    tlg::detail::ByteWriter invalid_filters;
    invalid_filters.append(tlg::detail::magic6);
    invalid_filters.u8(1); invalid_filters.u8(0); invalid_filters.u8(0); invalid_filters.u8(0);
    invalid_filters.u32(1 << 20); invalid_filters.u32(1); invalid_filters.u32(1);
    invalid_filters.u32(1); invalid_filters.u8(0);
    invalid_filters.u32(1); invalid_filters.u8(0); // Complete entropy framing.
    rejects_without_large_allocation(invalid_filters.data, tlg::ErrorCode::truncated_input);

    tlg::detail::ByteWriter sds;
    sds.append(tlg::detail::magic_sds);
    sds.u32(static_cast<std::uint32_t>(encoded->size())); sds.append(*encoded);
    sds.append(std::array<std::uint8_t, 4>{'j','u','n','k'}); sds.u32(0);
    auto limits = tlg::DecodeOptions{};
    limits.max_metadata_bytes = 0;
    rejects_without_large_allocation(sds.data, tlg::ErrorCode::resource_limit, limits);

    // Preserve the decoded error location while preflighting embedded streams.
    put32(sds.data, sds.data.size() - 4, 1);
    rejects_without_large_allocation(sds.data, tlg::ErrorCode::truncated_input);
    std::cout << "PASS malformed TLG5/TLG6/SDS rejected before large allocations\n";
    return 0;
} catch (const std::exception& error) {
    watching = false;
    std::cerr << error.what() << '\n';
    return 1;
}
