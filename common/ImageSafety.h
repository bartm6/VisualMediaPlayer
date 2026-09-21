#pragma once

#include <cstdint>

namespace vmp {

struct SafeDecodeSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool scaledForSafety = false;
    bool valid = false;
};

inline constexpr std::uint64_t kMaxDecodedImagePixels = 64ull * 1024ull * 1024ull; // 256 MiB at BGRA32
inline constexpr std::uint32_t kMaxDecodedImageDimension = 16384u;

SafeDecodeSize CalculateSafeDecodeSize(std::uint32_t sourceWidth,
                                       std::uint32_t sourceHeight,
                                       int requestedMaxWidth,
                                       int requestedMaxHeight);

} // namespace vmp
