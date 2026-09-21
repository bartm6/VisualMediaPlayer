#include "MediaCachePaths.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace vmp {
namespace {

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

std::wstring BuildVersionedThumbPath(const std::wstring& source,
                                     const FileFingerprint& fingerprint,
                                     const wchar_t* signature,
                                     const wchar_t* suffix) {
    std::wstring input = std::wstring(signature) + source;
    if (fingerprint.hasSize) input += L"|" + std::to_wstring(fingerprint.size);
    if (fingerprint.hasWriteTime) input += L"|" + std::to_wstring(fingerprint.writeTimeTicks);
    const std::uint64_t hash = Fnv1a64(input);
    wchar_t name[64]{};
#if defined(_MSC_VER)
    swprintf_s(name, sizeof(name) / sizeof(name[0]), L"%016llx%ls", static_cast<unsigned long long>(hash), suffix);
#else
    std::swprintf(name, sizeof(name) / sizeof(name[0]), L"%016llx%ls", static_cast<unsigned long long>(hash), suffix);
#endif
    return (fs::path(CacheRootForSource(source)) / L"thumbs" / name).wstring();
}

} // namespace

std::uint64_t Fnv1a64(const std::wstring& text) {
    std::uint64_t hash = 1469598103934665603ull;
    for (wchar_t c : text) {
        hash ^= static_cast<std::uint64_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

FileFingerprint QueryFileFingerprint(const std::wstring& source) {
    FileFingerprint fingerprint;
    std::error_code ec;
    const auto size = fs::file_size(source, ec);
    if (!ec) {
        fingerprint.size = size;
        fingerprint.hasSize = true;
    }
    ec.clear();
    const auto writeTime = fs::last_write_time(source, ec);
    if (!ec) {
        fingerprint.writeTimeTicks = static_cast<std::int64_t>(writeTime.time_since_epoch().count());
        fingerprint.hasWriteTime = true;
    }
    return fingerprint;
}

std::wstring CacheRootForSource(const std::wstring& source) {
    return (fs::path(source).parent_path() / L".visualmediaplayer-cache").wstring();
}

std::wstring BuildThumbCachePath(const std::wstring& source) {
    return BuildThumbCachePath(source, QueryFileFingerprint(source));
}

std::wstring BuildThumbCachePath(const std::wstring& source, const FileFingerprint& fingerprint) {
    return BuildVersionedThumbPath(source, fingerprint,
        L"detail-info-native-v12-fullsource-10pct-sharedtime|", L".jpg");
}

std::wstring BuildUiThumbCachePath(const std::wstring& source) {
    return BuildUiThumbCachePath(source, QueryFileFingerprint(source));
}

std::wstring BuildUiThumbCachePath(const std::wstring& source, const FileFingerprint& fingerprint) {
    return BuildVersionedThumbPath(source, fingerprint,
        L"grid-v10-vr-crop-stereo-lock-640-at-10pct-exacttime|", L".ui.jpg");
}

std::wstring BuildFavoriteMetadataPath(const std::wstring& source) {
    const std::wstring normalized = ToLower(fs::path(source).lexically_normal().wstring());
    const std::uint64_t hash = Fnv1a64(L"favorite-v1|" + normalized);
    wchar_t name[40]{};
#if defined(_MSC_VER)
    swprintf_s(name, sizeof(name) / sizeof(name[0]), L"%016llx.favorite", static_cast<unsigned long long>(hash));
#else
    std::swprintf(name, sizeof(name) / sizeof(name[0]), L"%016llx.favorite", static_cast<unsigned long long>(hash));
#endif
    return (fs::path(CacheRootForSource(source)) / L"favorites" / name).wstring();
}

bool ReadFavoriteMetadata(const std::wstring& source) {
    const fs::path path = fs::path(BuildFavoriteMetadataPath(source));
    std::error_code ec;
    if (!fs::is_regular_file(path, ec) || ec) return false;
    std::ifstream in(path, std::ios::binary);
    std::string tag;
    return static_cast<bool>(in >> tag) && tag == "VMPFAV1";
}

} // namespace vmp
