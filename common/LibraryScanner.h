#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace vmp {

enum class ScannedMediaKind {
    Video,
    Image
};

struct ScannedMediaFile {
    std::wstring path;
    std::wstring cachePath;
    std::wstring uiCachePath;
    ScannedMediaKind kind = ScannedMediaKind::Video;
    bool favorite = false;
};

struct LibraryScanResult {
    std::wstring root;
    std::vector<std::wstring> folders;
    std::vector<ScannedMediaFile> media;
    std::vector<std::wstring> cacheRoots;
    std::uint64_t visitedEntries = 0;
    bool reliable = true;
    bool cancelled = false;
};

bool IsVideoExtension(const std::wstring& extension);
bool IsImageExtension(const std::wstring& extension);
LibraryScanResult ScanLibrary(const std::wstring& root, const std::atomic<bool>& cancelRequested);

} // namespace vmp
