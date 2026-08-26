#include "LibraryScanner.h"
#include "MediaCachePaths.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace vmp {
namespace {

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

template <std::size_t N>
bool MatchesExtension(const std::wstring& extension, const wchar_t* const (&extensions)[N]) {
    const std::wstring lower = ToLower(extension);
    for (const wchar_t* candidate : extensions) {
        if (lower == candidate) return true;
    }
    return false;
}

} // namespace

bool IsVideoExtension(const std::wstring& extension) {
    static const wchar_t* const kExtensions[] = {
        L".mp4", L".m4v", L".mkv", L".mk3d", L".webm", L".avi", L".divx", L".mov", L".qt", L".wmv", L".asf", L".mpg", L".mpeg", L".mpe", L".mpv", L".mpv2", L".m1v", L".m2v", L".m2p", L".ts", L".m2t", L".mts", L".m2ts", L".tp", L".trp", L".vob", L".vro", L".ogv", L".ogm", L".flv", L".f4v", L".f4p", L".3gp", L".3g2", L".3gp2", L".3gpp", L".rm", L".rmvb", L".rv", L".mxf", L".gxf", L".dv", L".dif", L".dvr-ms", L".wtv", L".mod", L".tod", L".amv", L".ivf", L".y4m", L".nut", L".nsv", L".roq", L".smk", L".bik", L".bk2", L".mjpeg", L".mjpg", L".mjp", L".h264", L".264", L".avc", L".h265", L".265", L".hevc", L".vp8", L".vp9", L".av1", L".r3d", L".braw", L".ari", L".cine", L".crm", L".insv", L".lrv", L".360", L".evo", L".mj2"
    };
    return MatchesExtension(extension, kExtensions);
}

bool IsImageExtension(const std::wstring& extension) {
    static const wchar_t* const kExtensions[] = {
        L".jpg", L".jpeg", L".jpe", L".jfif", L".jif", L".jfi", L".png", L".apng", L".bmp", L".dib", L".gif", L".tif", L".tiff", L".webp", L".heic", L".heif", L".hif", L".avif", L".avifs", L".jxl", L".jp2", L".j2k", L".j2c", L".jpf", L".jpx", L".jpm", L".jxr", L".wdp", L".hdp", L".tga", L".targa", L".icb", L".vda", L".vst", L".dds", L".pcx", L".ico", L".cur", L".mng", L".psd", L".psb", L".exr", L".hdr", L".rgbe", L".pic", L".pfm", L".pnm", L".ppm", L".pgm", L".pbm", L".pam", L".qoi", L".sgi", L".rgb", L".rgba", L".bw", L".ras", L".sun", L".xbm", L".xpm", L".svg", L".svgz", L".dng", L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".srf", L".sr2", L".raf", L".orf", L".rw2", L".rwl", L".pef", L".x3f", L".3fr", L".fff", L".iiq", L".erf", L".mef", L".mos", L".mrw", L".kdc", L".dcr", L".raw", L".srw", L".bay", L".cap", L".eip", L".mdc", L".rwz"
    };
    return MatchesExtension(extension, kExtensions);
}

LibraryScanResult ScanLibrary(const std::wstring& root, const std::atomic<bool>& cancelRequested) {
    LibraryScanResult result;
    result.root = fs::path(root).lexically_normal().wstring();
    if (result.root.empty()) return result;

    std::error_code ec;
    fs::recursive_directory_iterator it(result.root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        result.reliable = false;
        ec.clear();
    }

    for (; it != end;) {
        if (cancelRequested.load(std::memory_order_acquire)) {
            result.cancelled = true;
            return result;
        }
        ++result.visitedEntries;
        if (ec) {
            result.reliable = false;
            ec.clear();
        }

        const fs::path path = it->path();
        std::error_code entryError;
        const bool isDirectory = it->is_directory(entryError);
        if (entryError) {
            result.reliable = false;
        } else if (isDirectory) {
            if (ToLower(path.filename().wstring()) == L".visualmediaplayer-cache") {
                result.cacheRoots.push_back(path.lexically_normal().wstring());
                it.disable_recursion_pending();
            } else {
                result.folders.push_back(path.lexically_normal().wstring());
            }
        } else {
            entryError.clear();
            const bool isFile = it->is_regular_file(entryError);
            if (entryError) {
                result.reliable = false;
            } else if (isFile) {
                const std::wstring extension = path.extension().wstring();
                const bool video = IsVideoExtension(extension);
                const bool image = !video && IsImageExtension(extension);
                if (video || image) {
                    ScannedMediaFile media;
                    media.path = path.lexically_normal().wstring();
                    media.kind = video ? ScannedMediaKind::Video : ScannedMediaKind::Image;
                    const FileFingerprint fingerprint = QueryFileFingerprint(media.path);
                    media.cachePath = BuildThumbCachePath(media.path, fingerprint);
                    media.uiCachePath = BuildUiThumbCachePath(media.path, fingerprint);
                    media.favorite = ReadFavoriteMetadata(media.path);
                    result.media.push_back(std::move(media));
                }
            }
        }

        it.increment(ec);
        if (ec) {
            result.reliable = false;
            ec.clear();
        }
    }
    return result;
}

} // namespace vmp
