#pragma once

#include <cstdint>
#include <string>

namespace vmp {

struct FileFingerprint {
    std::uintmax_t size = 0;
    std::int64_t writeTimeTicks = 0;
    bool hasSize = false;
    bool hasWriteTime = false;
};

std::uint64_t Fnv1a64(const std::wstring& text);
FileFingerprint QueryFileFingerprint(const std::wstring& source);
std::wstring CacheRootForSource(const std::wstring& source);
std::wstring BuildThumbCachePath(const std::wstring& source);
std::wstring BuildThumbCachePath(const std::wstring& source, const FileFingerprint& fingerprint);
std::wstring BuildUiThumbCachePath(const std::wstring& source);
std::wstring BuildUiThumbCachePath(const std::wstring& source, const FileFingerprint& fingerprint);
std::wstring BuildFavoriteMetadataPath(const std::wstring& source);
bool ReadFavoriteMetadata(const std::wstring& source);

} // namespace vmp
