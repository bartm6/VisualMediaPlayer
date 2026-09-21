#include "ImageSafety.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vmp {

SafeDecodeSize CalculateSafeDecodeSize(std::uint32_t sourceWidth,
                                       std::uint32_t sourceHeight,
                                       int requestedMaxWidth,
                                       int requestedMaxHeight) {
    SafeDecodeSize result;
    if (sourceWidth == 0 || sourceHeight == 0) return result;

    double width = static_cast<double>(sourceWidth);
    double height = static_cast<double>(sourceHeight);

    if (requestedMaxWidth > 0 && requestedMaxHeight > 0) {
        const double requestedScale = std::min(
            static_cast<double>(requestedMaxWidth) / width,
            static_cast<double>(requestedMaxHeight) / height);
        width = std::max(1.0, std::round(width * requestedScale));
        height = std::max(1.0, std::round(height * requestedScale));
    }

    double safetyScale = 1.0;
    if (width > static_cast<double>(kMaxDecodedImageDimension))
        safetyScale = std::min(safetyScale, static_cast<double>(kMaxDecodedImageDimension) / width);
    if (height > static_cast<double>(kMaxDecodedImageDimension))
        safetyScale = std::min(safetyScale, static_cast<double>(kMaxDecodedImageDimension) / height);

    const long double pixels = static_cast<long double>(width) * static_cast<long double>(height);
    if (pixels > static_cast<long double>(kMaxDecodedImagePixels)) {
        const double pixelScale = std::sqrt(
            static_cast<double>(static_cast<long double>(kMaxDecodedImagePixels) / pixels));
        safetyScale = std::min(safetyScale, pixelScale);
    }

    if (safetyScale < 1.0) {
        width = std::max(1.0, std::floor(width * safetyScale));
        height = std::max(1.0, std::floor(height * safetyScale));
        result.scaledForSafety = true;
    }

    if (width > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        height > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) return result;

    result.width = static_cast<std::uint32_t>(width);
    result.height = static_cast<std::uint32_t>(height);
    if (result.width == 0 || result.height == 0) return SafeDecodeSize{};
    const std::uint64_t finalPixels = static_cast<std::uint64_t>(result.width) * result.height;
    if (finalPixels > kMaxDecodedImagePixels) return SafeDecodeSize{};
    result.valid = true;
    return result;
}

} // namespace vmp
