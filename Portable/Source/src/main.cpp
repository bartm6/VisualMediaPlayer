#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <d2d1.h>
#include <d2d1_3.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <oleauto.h>
#include <gdiplus.h>
#include <wincodec.h>
#include <shlobj.h>
#include <psapi.h>
#include "../res/resource.h"
#include "../../../common/ImageSafety.h"
#include "../../../common/LibraryScanner.h"
#include "../../../common/MediaCachePaths.h"
#include "../../../common/MemoryCachePolicy.h"
#include "../../../common/SingleInstanceIpc.h"

#include <algorithm>
#include <cstring>
#include <climits>
#include <cwctype>
#include <iterator>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include <cwchar>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstdint>
#include <limits>
#include <regex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "msimg32.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

#if defined(VMP_PORTABLE_BUILD)
static constexpr wchar_t kVmpMainWindowClass[] = L"VisualMediaPlayerPortableMain";
static constexpr wchar_t kVmpInstanceMutex[] = L"Local\\VisualMediaPlayer.Portable.SingleInstance.v1";
static constexpr wchar_t kVmpIpcChannel[] = L"VisualMediaPlayer.Portable.SingleInstance.v2";
#else
static constexpr wchar_t kVmpMainWindowClass[] = L"VisualMediaPlayerMain";
static constexpr wchar_t kVmpInstanceMutex[] = L"Local\\VisualMediaPlayer.SingleInstance.v1";
static constexpr wchar_t kVmpIpcChannel[] = L"VisualMediaPlayer.SingleInstance.v2";
#endif

static constexpr UINT WM_APP_MEDIA_EVENT = WM_APP + 1;
static constexpr UINT WM_APP_SEEK_COMMIT = WM_APP + 2;
static constexpr UINT WM_APP_PLAYER_READY = WM_APP + 3;
static constexpr UINT WM_APP_SELECTED_WORK_DONE = WM_APP + 4;
static constexpr UINT WM_APP_MEDIA_ERROR = WM_APP + 5;
static constexpr UINT WM_APP_HOVER_AUDIO_EVENT = WM_APP + 6;
static constexpr float PI_F = 3.14159265358979323846f;

static std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}


static void TrimSortSpaces(std::wstring& s) {
    size_t first = 0;
    while (first < s.size() && iswspace(s[first])) ++first;
    size_t last = s.size();
    while (last > first && iswspace(s[last - 1])) --last;
    s = s.substr(first, last - first);
}

static bool ExtractTrailingSortNumber(std::wstring& text, std::wstring& digits) {
    TrimSortSpaces(text);
    if (text.empty() || text.back() != L')') return false;

    const size_t open = text.rfind(L" (");
    if (open == std::wstring::npos || open + 2 >= text.size() - 1) return false;

    const size_t digitsBegin = open + 2;
    const size_t digitsEnd = text.size() - 1;
    for (size_t i = digitsBegin; i < digitsEnd; ++i) {
        if (!iswdigit(text[i])) return false;
    }

    digits = text.substr(digitsBegin, digitsEnd - digitsBegin);
    text.erase(open);
    TrimSortSpaces(text);
    return true;
}

static int CompareSortNumbers(const std::wstring& a, const std::wstring& b) {
    size_t aFirst = 0, bFirst = 0;
    while (aFirst + 1 < a.size() && a[aFirst] == L'0') ++aFirst;
    while (bFirst + 1 < b.size() && b[bFirst] == L'0') ++bFirst;

    const size_t aLen = a.size() - aFirst;
    const size_t bLen = b.size() - bFirst;
    if (aLen != bLen) return aLen < bLen ? -1 : 1;

    const int valueCmp = a.compare(aFirst, aLen, b, bFirst, bLen);
    if (valueCmp != 0) return valueCmp < 0 ? -1 : 1;

    // Equal numeric values: keep the shorter spelling first, e.g. (1) before (01).
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    return 0;
}

struct MediaNameSortKey {
    std::wstring primary;
    std::wstring secondary;
    std::wstring number;
    std::wstring fallback;
    int group = 0;
    bool hasNumber = false;
};

static MediaNameSortKey BuildMediaNameSortKey(const std::wstring& title) {
    MediaNameSortKey key;
    key.fallback = ToLower(title);

    std::wstring text = key.fallback;
    TrimSortSpaces(text);
    key.hasNumber = ExtractTrailingSortNumber(text, key.number);

    // VR files named "name VR" or "name VR (x)" always come last in
    // the matching name family. The optional x is still sorted numerically.
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, L" vr") == 0) {
        key.primary = text.substr(0, text.size() - 3);
        TrimSortSpaces(key.primary);
        key.group = 3;
        return key;
    }

    // "name &" and "name & (x)" are their own groups before "name & name".
    if (text.size() >= 2 && text.compare(text.size() - 2, 2, L" &") == 0) {
        key.primary = text.substr(0, text.size() - 2);
        TrimSortSpaces(key.primary);
        key.group = 1;
        return key;
    }

    // "name & name" and "name & name (x)" follow the bare ampersand forms.
    const size_t amp = text.find(L" & ");
    if (amp != std::wstring::npos) {
        key.primary = text.substr(0, amp);
        key.secondary = text.substr(amp + 3);
        TrimSortSpaces(key.primary);
        TrimSortSpaces(key.secondary);
        key.group = 2;
        return key;
    }

    key.primary = text;
    key.group = 0;
    return key;
}

static std::vector<std::wstring> SplitMediaNameComponents(const std::wstring& text) {
    // A space is part of a name (for example "name lastname"). Only the explicit
    // " & " separator creates another media-name component. Never reinterpret words
    // inside one component as separate names for Library/search ordering.
    std::vector<std::wstring> parts;
    size_t start=0;
    while(start<=text.size()){
        const size_t amp=text.find(L" & ",start);
        std::wstring part=text.substr(start,amp==std::wstring::npos?std::wstring::npos:amp-start);
        TrimSortSpaces(part);
        if(!part.empty()) parts.push_back(std::move(part));
        if(amp==std::wstring::npos) break;
        start=amp+3;
    }
    return parts;
}

static MediaNameSortKey BuildSearchAwareMediaNameSortKey(const std::wstring& title, const std::wstring& searchText) {
    // Normal Library ordering is based on the first name component. During a text
    // search, rotate an ampersand-separated title when the searched name lives in a
    // later component. Prefer the complete search phrase first ("golden dog" stays
    // together); only fall back to individual search words when no component contains
    // the whole phrase. The file/display name itself is never changed.
    MediaNameSortKey normal = BuildMediaNameSortKey(title);
    std::wstring query = ToLower(searchText);
    TrimSortSpaces(query);
    if (query.empty()) return normal;

    std::wstring text = ToLower(title);
    TrimSortSpaces(text);
    std::wstring number;
    const bool hasNumber = ExtractTrailingSortNumber(text, number);

    bool vrSuffix = false;
    if (text.size() >= 3 && text.compare(text.size() - 3, 3, L" vr") == 0) {
        text.resize(text.size() - 3);
        TrimSortSpaces(text);
        vrSuffix = true;
    }

    bool trailingAmp = false;
    if (text.size() >= 2 && text.compare(text.size() - 2, 2, L" &") == 0) {
        text.resize(text.size() - 2);
        TrimSortSpaces(text);
        trailingAmp = true;
    }

    const std::vector<std::wstring> parts=SplitMediaNameComponents(text);
    if (parts.size() < 2) return normal;

    auto rotateTo=[&](size_t matched)->MediaNameSortKey{
        std::wstring rotated = parts[matched];
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i == matched) continue;
            rotated += L" & ";
            rotated += parts[i];
        }
        if (trailingAmp) rotated += L" &";
        if (vrSuffix) rotated += L" VR";
        if (hasNumber) { rotated += L" ("; rotated += number; rotated += L")"; }
        return BuildMediaNameSortKey(rotated);
    };

    // Exact component match is strongest, followed by a component containing the whole
    // phrase. If the first component has that same strength, preserve normal ordering.
    if (parts[0] == query) return normal;
    for (size_t i = 1; i < parts.size(); ++i) if (parts[i] == query) return rotateTo(i);
    if (parts[0].find(query) != std::wstring::npos) return normal;
    for (size_t i = 1; i < parts.size(); ++i)
        if (parts[i].find(query) != std::wstring::npos) return rotateTo(i);

    // Whole-phrase matching may fail for a broad multi-word search. In that case score
    // complete name components by how many query words they contain. Rotate only when a
    // later component is a strictly better match than the first component.
    std::vector<std::wstring> words;
    size_t wordStart=0;
    while(wordStart<query.size()){
        while(wordStart<query.size() && iswspace(query[wordStart])) ++wordStart;
        if(wordStart>=query.size()) break;
        size_t wordEnd=wordStart;
        while(wordEnd<query.size() && !iswspace(query[wordEnd])) ++wordEnd;
        std::wstring word=query.substr(wordStart,wordEnd-wordStart);
        TrimSortSpaces(word);
        if(!word.empty()) words.push_back(std::move(word));
        wordStart=wordEnd;
    }
    if(words.empty()) return normal;
    auto score=[&](const std::wstring& part){
        int value=0;
        for(const auto& word:words) if(part.find(word)!=std::wstring::npos) ++value;
        return value;
    };
    const int firstScore=score(parts[0]);
    int bestScore=firstScore;
    size_t matched=parts.size();
    for(size_t i=1;i<parts.size();++i){
        const int current=score(parts[i]);
        if(current>bestScore){bestScore=current;matched=i;}
    }
    if(matched==parts.size() || bestScore<=0) return normal;
    return rotateTo(matched);
}



static std::wstring StripLeadingImageResolutionPrefix(std::wstring title) {
    // Some generated/downloaded image filenames begin with dimensions such as
    // "2000x1333 ". Keep the real file/path unchanged; only hide that leading
    // resolution token from the user-facing image title/search text.
    size_t pos = 0;
    const size_t n = title.size();
    while (pos < n && iswdigit(title[pos])) ++pos;
    if (pos == 0 || pos >= n || (title[pos] != L'x' && title[pos] != L'X')) return title;
    ++pos;
    const size_t heightBegin = pos;
    while (pos < n && iswdigit(title[pos])) ++pos;
    if (pos == heightBegin || pos >= n || !iswspace(title[pos])) return title;
    while (pos < n && iswspace(title[pos])) ++pos;
    if (pos >= n) return title;
    return title.substr(pos);
}

static std::wstring NormalizeMarkerText(const std::wstring& input) {
    std::wstring out;
    out.reserve(input.size() + 2);
    bool lastWasSpace = true;
    for (wchar_t c : ToLower(input)) {
        if (iswalnum(c)) {
            out.push_back(c);
            lastWasSpace = false;
        } else if (!lastWasSpace) {
            out.push_back(L' ');
            lastWasSpace = true;
        }
    }
    if (!out.empty() && out.back() == L' ') out.pop_back();
    return L" " + out + L" ";
}

static bool HasMarker(const std::wstring& normalized, const wchar_t* marker) {
    return normalized.find(std::wstring(L" ") + marker + L" ") != std::wstring::npos;
}

static bool IsVideoExtension(const std::wstring& extRaw) {
    return vmp::IsVideoExtension(extRaw);
}

static std::wstring HrText(HRESULT hr) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"0x%08X", static_cast<unsigned>(hr));
    return buf;
}

static std::wstring PathToFileUrl(const std::wstring& path) {
    wchar_t out[32768]{};
    DWORD len = static_cast<DWORD>(std::size(out));
    if (SUCCEEDED(UrlCreateFromPathW(path.c_str(), out, &len, 0))) return out;
    return path;
}

struct VRInfo {
    bool vr = false;
    int layout = 0;      // 0 mono, 1 SBS, 2 top-bottom
    int projection = 0;  // 0 flat, 1 360, 2 180
    bool layoutExplicit = false; // only true when filename explicitly says SBS/LR/TB/OU
};

static VRInfo DetectVR(const std::wstring& file) {
    const std::wstring filename = fs::path(file).filename().wstring();
    const std::wstring markers = NormalizeMarkerText(filename);
    VRInfo v;

    // VR classification is filename-marker based. A standalone "VR" token marks
    // generic VR media; VR180/180VR remain explicit 180-degree markers.
    // The old "360" filename suffix is intentionally no longer a VR marker.
    const bool has180 = HasMarker(markers, L"vr180") || HasMarker(markers, L"180vr") ||
                        markers.find(L" vr 180 ") != std::wstring::npos ||
                        markers.find(L" 180 vr ") != std::wstring::npos;
    const bool hasVr = has180 || HasMarker(markers, L"vr");
    if (!hasVr) return v;

    v.vr = true;
    v.projection = has180 ? 2 : 1;

    const bool tb = HasMarker(markers, L"tb") || HasMarker(markers, L"ou") ||
                    markers.find(L" top bottom ") != std::wstring::npos ||
                    markers.find(L" over under ") != std::wstring::npos;
    const bool sbs = HasMarker(markers, L"sbs") || HasMarker(markers, L"lr") ||
                     markers.find(L" side by side ") != std::wstring::npos ||
                     markers.find(L" left right ") != std::wstring::npos;

    if (tb) { v.layout = 2; v.layoutExplicit = true; }
    else if (sbs) { v.layout = 1; v.layoutExplicit = true; }
    else v.layout = 0;

    // Explicit stereo packing is front-facing VR180 by default. Ambiguous files are
    // resolved once from decoded content later rather than by substring guesses.
    if (v.layout != 0) v.projection = 2;
    return v;
}

struct MediaItem {
    std::wstring path;
    // Normalized/lower-cased parent folder is computed once during discovery. Folder
    // navigation and scrollbar geometry must never rebuild filesystem paths per item.
    std::wstring parentFolderKey;
    std::wstring title;
    std::wstring cachePath;
    std::wstring uiCachePath;
    std::wstring searchText;
    VRInfo vr;
    bool isVideo = true;
    HBITMAP thumb = nullptr;
    int thumbW = 0;
    int thumbH = 0;
    // The GPU Library renderer keeps a device-dependent Direct2D copy of resident
    // thumbnails. It is recreated lazily if the render target changes.
    ComPtr<ID2D1Bitmap> libraryGpuThumb;
    HBITMAP libraryGpuThumbSource = nullptr;
    uint64_t libraryGpuGeneration = 0;
    // Details/Info uses the same hardware Direct2D target as Library, but keeps a
    // separate device-dependent copy because its hero image may be the full native banner.
    ComPtr<ID2D1Bitmap> detailsGpuThumb;
    HBITMAP detailsGpuThumbSource = nullptr;
    uint64_t detailsGpuGeneration = 0;
    bool thumbAttempted = false;
    bool thumbFromPrivateCache = false;
    ULONGLONG thumbLastUsed = 0;
    // Library thumbnail disk/decode requests are performed off the UI thread.
    // The epoch prevents stale work from a previous scroll position being applied.
    uint64_t thumbLoadRequestEpoch = 0;
    int thumbLoadRequestW = 0;
    int thumbLoadRequestH = 0;
    ULONGLONG thumbNextLoadAttempt = 0;
    HBITMAP detailThumb = nullptr;
    int detailThumbW = 0;
    int detailThumbH = 0;
    bool detailDecodeUnsupported = false;
    UINT sourceWidth = 0;
    UINT sourceHeight = 0;
    bool resolutionProbeAttempted = false;
    bool resolutionMetadataQueued = false;
    bool favorite = false;
};

struct LibraryFolder {
    std::wstring path;
    std::wstring parentFolderKey;
    std::wstring name;
};

struct PreviewFrame {
    int seconds = 0;                 // rounded/display label and deterministic cache filename
    double seekSeconds = 0.0;        // exact playback/hover timestamp represented by this still
    std::wstring path;
    HBITMAP bitmap = nullptr;
    ULONGLONG lastUsed = 0;
    int loadFailures = 0;
    ULONGLONG nextLoadAttempt = 0;
    // A larger display-sized decode may be prepared off the UI thread.  Keep the
    // currently visible bitmap until that replacement is ready so Ctrl+wheel zoom
    // never blocks on JPEG I/O/decompression or flashes a placeholder.
    int pendingDecodeW = 0;
    uint64_t pendingDecodeGeneration = 0;
    // Temporary GPU copy of the decoded preview. It is never written to disk and is
    // recreated lazily if the shared Direct2D render target is lost.
    ComPtr<ID2D1Bitmap> gpuBitmap;
    HBITMAP gpuBitmapSource = nullptr;
    uint64_t gpuGeneration = 0;
};

class MediaEngineNotify final : public IMFMediaEngineNotify {
public:
    explicit MediaEngineNotify(HWND hwnd) : hwnd_(hwnd) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP EventNotify(DWORD meEvent, DWORD_PTR param1, DWORD param2) override {
        PostMessageW(hwnd_, WM_APP_MEDIA_EVENT, meEvent, static_cast<LPARAM>(param1));
        (void)param2;
        return S_OK;
    }
private:
    std::atomic<ULONG> ref_{1};
    HWND hwnd_{};
};

class HoverAudioNotify final : public IMFMediaEngineNotify {
public:
    HoverAudioNotify(HWND hwnd,uint64_t generation) : hwnd_(hwnd),generation_(generation) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP EventNotify(DWORD meEvent, DWORD_PTR, DWORD) override {
        if(hwnd_) PostMessageW(hwnd_,WM_APP_HOVER_AUDIO_EVENT,meEvent,static_cast<LPARAM>(generation_));
        return S_OK;
    }
private:
    std::atomic<ULONG> ref_{1};
    HWND hwnd_{};
    uint64_t generation_=0;
};

class HoverPreviewAudioPlayer {
public:
    ~HoverPreviewAudioPlayer() { Stop(); }

    HRESULT Open(HWND eventWindow,const std::wstring& path,uint64_t generation) {
        Stop();
        if(!eventWindow || path.empty()) return E_INVALIDARG;

        ComPtr<IMFMediaEngineClassFactory> factory;
        HRESULT hr=CoCreateInstance(CLSID_MFMediaEngineClassFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
        if(FAILED(hr)) return hr;

        notify_.Attach(new HoverAudioNotify(eventWindow,generation));
        ComPtr<IMFAttributes> attrs;
        hr=MFCreateAttributes(&attrs,1);
        if(FAILED(hr)) { Stop(); return hr; }
        hr=attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK,notify_.Get());
        if(FAILED(hr)) { Stop(); return hr; }
        hr=factory->CreateInstance(MF_MEDIA_ENGINE_AUDIOONLY,attrs.Get(),&engine_);
        if(FAILED(hr)) { Stop(); return hr; }

        generation_=generation;
        engine_->SetAutoPlay(FALSE);
        engine_->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
        engine_->SetVolume(0.0);
        BSTR src=SysAllocString(PathToFileUrl(path).c_str());
        if(!src) { Stop(); return E_OUTOFMEMORY; }
        hr=engine_->SetSource(src);
        SysFreeString(src);
        if(FAILED(hr)) { Stop(); return hr; }
        hr=engine_->Load();
        if(FAILED(hr)) { Stop(); return hr; }
        return S_OK;
    }

    void HandleMediaEvent(DWORD ev,double currentPreviewSeconds,double volume) {
        if(!engine_) return;
        if(ev==MF_MEDIA_ENGINE_EVENT_ERROR) { Stop(); return; }
        if(ev!=MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA && ev!=MF_MEDIA_ENGINE_EVENT_CANPLAY) return;

        const double duration=engine_->GetDuration();
        double target=std::max(0.0,currentPreviewSeconds);
        if(std::isfinite(duration) && duration>0.0)
            target=std::clamp(target,0.0,std::max(0.0,duration-0.02));
        engine_->SetCurrentTime(target);
        if(!playing_) {
            engine_->SetVolume(std::clamp(volume,0.0,1.0));
            engine_->Play();
            playing_=true;
        }
    }

    void Stop() {
        if(engine_) {
            engine_->SetVolume(0.0);
            engine_->Pause();
            engine_->Shutdown();
        }
        engine_.Reset();
        notify_.Reset();
        generation_=0;
        playing_=false;
    }

    bool IsOpen() const { return engine_.Get()!=nullptr; }
    uint64_t Generation() const { return generation_; }

private:
    ComPtr<IMFMediaEngineNotify> notify_;
    ComPtr<IMFMediaEngine> engine_;
    uint64_t generation_=0;
    bool playing_=false;
};

class NativePlayer {
public:
    ~NativePlayer() { Shutdown(); }

    HRESULT Initialize(HWND eventWindow, HWND videoWindow) {
        eventWindow_ = eventWindow;
        videoWindow_ = videoWindow;

        RECT rc{}; GetClientRect(videoWindow_, &rc);
        UINT width = std::max<LONG>(1L, rc.right - rc.left);
        UINT height = std::max<LONG>(1L, rc.bottom - rc.top);

        DXGI_SWAP_CHAIN_DESC sc{};
        sc.BufferCount = 2;
        sc.BufferDesc.Width = width;
        sc.BufferDesc.Height = height;
        sc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc.OutputWindow = videoWindow_;
        sc.SampleDesc.Count = 1;
        sc.Windowed = TRUE;
        sc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL requested[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            requested, static_cast<UINT>(std::size(requested)), D3D11_SDK_VERSION,
            &sc, &swapChain_, &device_, &got, &context_);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                requested + 1, static_cast<UINT>(std::size(requested) - 1), D3D11_SDK_VERSION,
                &sc, &swapChain_, &device_, &got, &context_);
        }
        if (FAILED(hr)) return hr;
        swapChainW_ = width;
        swapChainH_ = height;

        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

        hr = CreateBackbuffer();
        if (FAILED(hr)) return hr;
        hr = CreatePipeline();
        if (FAILED(hr)) return hr;

        hr = MFCreateDXGIDeviceManager(&dxgiResetToken_, &dxgiManager_);
        if (FAILED(hr)) return hr;
        hr = dxgiManager_->ResetDevice(device_.Get(), dxgiResetToken_);
        if (FAILED(hr)) return hr;

        return CreateMediaEngine();
    }

    HRESULT Open(const std::wstring& path, const VRInfo& vr, double startSeconds = -1.0) {
        if (!engine_) {
            const HRESULT recreateHr=CreateMediaEngine();
            if(FAILED(recreateHr)) return recreateHr;
        }
        const bool switchingMedia = !path_.empty() && path_ != path && videoSRV_ && eyeW_ && eyeH_;
        if (switchingMedia) {
            transitionTexture_ = videoTexture_;
            transitionSRV_ = videoSRV_;
            transitionState_ = CurrentSurfaceState();
            transitionWaitingForFrame_ = true;
            transitionActive_ = false;
            transitionStart_ = 0;
        } else if (path_.empty() || path_ == path) {
            ClearVideoTransition();
        }
        vrInfo_ = vr;
        // VR always opens in the standard front-only 180 mode.  The 360 state is
        // user-enabled only, so the button stays dark until the user turns 360 on.
        projectionOverride_ = vr.vr ? 2 : 0;
        layoutDetectionDone_ = vr.layoutExplicit;
        layoutDetectionPending_ = false;
        layoutDetectionAttempts_ = 0;
        layoutMonoVotes_ = 0;
        stereoProbeGpu_.Reset();
        stereoProbeStaging_.Reset();
        stereoProbePending_ = false;
        stereoProbeW_ = stereoProbeH_ = 0;
        const bool startImmediately = startSeconds <= 0.001;
        pendingStartSeconds_ = startImmediately ? -1.0 : startSeconds;
        yaw_ = 0.0f;
        pitch_ = 0.0f;
        fovRadians_ = 65.0f * PI_F / 180.0f;
        ResetFlatZoom();
        nativeW_ = nativeH_ = eyeW_ = eyeH_ = 0;
        videoTexture_.Reset();
        videoSRV_.Reset();
        // Normal Play starts at 00:00, so queue playback immediately. Keep the
        // readiness-event Play call armed as a fallback in case an individual decoder
        // rejects the early Play request. Non-zero timeline starts still seek first.
        autoPlayWhenReady_ = true;
        path_ = path;
        engine_->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
        engine_->SetAutoPlay(startImmediately ? TRUE : FALSE);

        BSTR src = SysAllocString(PathToFileUrl(path).c_str());
        if (!src) return E_OUTOFMEMORY;
        HRESULT hr = engine_->SetSource(src);
        SysFreeString(src);
        if (FAILED(hr)) return hr;
        hr=engine_->Load();
        if(SUCCEEDED(hr) && startImmediately) engine_->Play();
        return hr;
    }

    void CloseSource() {
        // Pause is not enough for removable/encrypted volumes: Media Foundation may
        // retain the underlying source handle. Shutdown only the media engine here so
        // the file is definitively released while the D3D device/swap chain stay alive.
        if (engine_) {
            engine_->Pause();
            engine_->Shutdown();
        }
        engine_.Reset();
        notify_.Reset();
        path_.clear();
        pendingStartSeconds_=-1.0;
        autoPlayWhenReady_=false;
        nativeW_=nativeH_=eyeW_=eyeH_=0;
        videoSRV_.Reset();
        videoTexture_.Reset();
        ClearVideoTransition();
        ResetFlatZoom();
    }

    void Shutdown() {
        if (engine_) engine_->Shutdown();
        engine_.Reset();
        notify_.Reset();
        dxgiManager_.Reset();
        videoSRV_.Reset();
        videoTexture_.Reset();
        ClearVideoTransition();
        blendState_.Reset();
        stereoProbeGpu_.Reset();
        stereoProbeStaging_.Reset();
        stereoProbePending_ = false;
        renderTarget_.Reset();
        swapChain_.Reset();
        context_.Reset();
        device_.Reset();
    }

    void HandleMediaEvent(DWORD ev) {
        if (!engine_) return;
        switch (ev) {
        case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
        case MF_MEDIA_ENGINE_EVENT_CANPLAY:
        case MF_MEDIA_ENGINE_EVENT_FORMATCHANGE:
            EnsureVideoTexture();
            if (pendingStartSeconds_ >= 0.0) {
                const double duration = engine_->GetDuration();
                double target = pendingStartSeconds_;
                if (duration > 0.0) target = std::clamp(target, 0.0, duration);
                engine_->SetCurrentTime(target);
                pendingStartSeconds_ = -1.0;
            }
            if (autoPlayWhenReady_) {
                autoPlayWhenReady_ = false;
                engine_->Play();
            }
            PostMessageW(eventWindow_, WM_APP_PLAYER_READY, 0, 0);
            break;
        case MF_MEDIA_ENGINE_EVENT_ERROR:
            PostMessageW(eventWindow_, WM_APP_MEDIA_ERROR, 0, 0);
            break;
        default:
            break;
        }
    }

    void Render() {
        if (!context_ || !swapChain_ || !renderTarget_) return;
        if (engine_ && nativeW_ && nativeH_) {
            LONGLONG pts = 0;
            if (engine_->OnVideoStreamTick(&pts) == S_OK) {
                EnsureVideoTexture();

                // A 2:1 360 source is ambiguous: it can be a normal mono panorama or
                // two square stereo eyes packed side-by-side.  Inspect a tiny copy of
                // the first decoded frame so stereo VR is not accidentally rendered as
                // one double/mirrored panorama.  This runs only once per opened video.
                if (layoutDetectionPending_ && !layoutDetectionDone_) {
                    const int detected = DetectPackedStereoFromCurrentFrame();
                    if (detected == -2) {
                        // GPU readback is still pending. Do not stall playback and do not
                        // count this frame as a failed stereo-detection attempt.
                    } else if (detected == 1 || detected == 2) {
                        ++layoutDetectionAttempts_;
                        // A positive stereo match wins immediately. Never let a later
                        // frame undo the one-eye decision for this playback session.
                        vrInfo_.layout = detected;
                        vrInfo_.projection = 2;
                        layoutDetectionDone_ = true;
                        layoutDetectionPending_ = false;
                        yaw_ = 0.0f;
                        pitch_ = 0.0f;
                        EnsureVideoTexture();
                    } else {
                        ++layoutDetectionAttempts_;
                        if (detected == 0) ++layoutMonoVotes_;
                        // Do not classify an ambiguous VR video as mono from one frame.
                        // Stereo footage can contain cuts, fades and large disparity.
                        if (layoutDetectionAttempts_ >= 12 && layoutMonoVotes_ >= 4) {
                            vrInfo_.layout = 0;
                            layoutDetectionDone_ = true;
                            layoutDetectionPending_ = false;
                            EnsureVideoTexture();
                        } else if (layoutDetectionAttempts_ >= 24) {
                            // Final safety fallback for genuinely mono/inconclusive files.
                            vrInfo_.layout = 0;
                            layoutDetectionDone_ = true;
                            layoutDetectionPending_ = false;
                            EnsureVideoTexture();
                        }
                    }
                }

                if (videoTexture_) {
                    // Unpack the selected eye during Media Foundation's GPU blit.
                    // This keeps the VR shader working on the full per-eye resolution
                    // instead of uploading a double-wide/double-high stereo texture.
                    MFVideoNormalizedRect src{0.f, 0.f, 1.f, 1.f};
                    if (vrInfo_.layout == 1) src.right = 0.5f;       // SBS: left eye
                    else if (vrInfo_.layout == 2) src.bottom = 0.5f; // TB/OU: top eye
                    RECT dst{0, 0, static_cast<LONG>(eyeW_), static_cast<LONG>(eyeH_)};
                    MFARGB border{0,0,0,255};
                    const HRESULT transferHr=engine_->TransferVideoFrame(videoTexture_.Get(), &src, &dst, &border);
                    if (SUCCEEDED(transferHr) && transitionWaitingForFrame_ && transitionSRV_) {
                        transitionWaitingForFrame_ = false;
                        transitionActive_ = true;
                        transitionStart_ = GetTickCount64();
                    }
                }
            }
        }

        const float clear[4] = {0.008f, 0.010f, 0.014f, 1.0f};
        context_->OMSetRenderTargets(1, renderTarget_.GetAddressOf(), nullptr);
        context_->ClearRenderTargetView(renderTarget_.Get(), clear);

        if (transitionSRV_ && transitionWaitingForFrame_) {
            // Keep the outgoing frame visible while Media Foundation prepares the next
            // source. This removes the black/disappear gap, especially when switching
            // between flat and VR pipelines.
            DrawVideoSurface(transitionSRV_.Get(), transitionState_, 1.0f);
        } else if (transitionSRV_ && transitionActive_) {
            const ULONGLONG elapsed=GetTickCount64()-transitionStart_;
            const float raw=std::clamp(static_cast<float>(elapsed)/static_cast<float>(kMediaSwitchFadeMs),0.0f,1.0f);
            const float t=raw*raw*(3.0f-2.0f*raw); // smoothstep
            DrawVideoSurface(transitionSRV_.Get(), transitionState_, 1.0f);
            if(videoSRV_) DrawVideoSurface(videoSRV_.Get(), CurrentSurfaceState(), t);
            if(raw>=1.0f) ClearVideoTransition();
        } else if (videoSRV_) {
            DrawVideoSurface(videoSRV_.Get(), CurrentSurfaceState(), 1.0f);
        }

        swapChain_->Present(1, 0);
    }

    void Resize() {
        if (!swapChain_ || !context_ || !videoWindow_) return;

        RECT rc{}; GetClientRect(videoWindow_, &rc);
        const UINT w = static_cast<UINT>(std::max<LONG>(1L, rc.right - rc.left));
        const UINT h = static_cast<UINT>(std::max<LONG>(1L, rc.bottom - rc.top));

        // Live window sizing can deliver many WM_SIZE messages for the same effective
        // client dimensions. Avoid tearing down the backbuffer unless its size really
        // changed; this keeps resize interaction cheap enough to run while video plays.
        if (renderTarget_ && w == swapChainW_ && h == swapChainH_) return;

        // A bound RTV keeps a reference to the swap-chain backbuffer. Unbind it
        // before ResizeBuffers or DXGI can reject the resize with INVALID_CALL.
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        renderTarget_.Reset();
        context_->Flush();

        const HRESULT resizeHr = swapChain_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(resizeHr)) {
            swapChainW_ = w;
            swapChainH_ = h;
            CreateBackbuffer();
        } else {
            // A transient DXGI resize failure must not leave playback black. The old
            // swap-chain buffers are still valid when ResizeBuffers fails, so immediately
            // recreate an RTV for the existing buffer and try the new size on the next tick.
            CreateBackbuffer();
        }
    }

    void PlayPause() {
        if (!engine_) return;
        if (engine_->IsPaused()) engine_->Play(); else engine_->Pause();
    }
    void Play() { if (engine_) engine_->Play(); }
    void Pause() { if (engine_) engine_->Pause(); }
    bool IsPaused() const { return !engine_ || engine_->IsPaused(); }
    double CurrentTime() const { return engine_ ? engine_->GetCurrentTime() : 0.0; }
    double Duration() const { return engine_ ? engine_->GetDuration() : 0.0; }
    void Seek(double seconds) {
        if (!engine_) return;
        double d = Duration();
        if (d > 0.0) seconds = std::clamp(seconds, 0.0, d);
        engine_->SetCurrentTime(seconds);
    }
    void SetVolume(double v) { if (engine_) engine_->SetVolume(std::clamp(v, 0.0, 1.0)); }
    std::pair<UINT,UINT> NativeSize() const { return {nativeW_, nativeH_}; }
    std::pair<UINT,UINT> EyeSize() const { return {eyeW_, eyeH_}; }
    void SetNativePixelSizing(bool enabled) { nativePixelSizing_ = enabled; if(enabled) ResetFlatZoom(); }
    bool NativePixelSizing() const { return nativePixelSizing_; }
    bool FlatZoomActive() const {
        return !vrInfo_.vr && !nativePixelSizing_ &&
               (std::abs(flatZoom_-1.0f) > 0.001f ||
                std::abs(flatCenterU_-0.5f) > 0.001f ||
                std::abs(flatCenterV_-0.5f) > 0.001f);
    }
    void ResetFlatZoom() { flatZoom_=1.0f; flatCenterU_=0.5f; flatCenterV_=0.5f; }
    static float ClampFlatCenterAxis(float center, float fitScale, float zoom) {
        const float extent=std::max(0.0001f,fitScale*zoom);
        const float half=0.5f/extent;
        const float a=half, b=1.0f-half;
        return std::clamp(center,std::min(a,b),std::max(a,b));
    }
    void ClampFlatCenter(float sx, float sy, float zoom) {
        flatCenterU_=ClampFlatCenterAxis(flatCenterU_,sx,zoom);
        flatCenterV_=ClampFlatCenterAxis(flatCenterV_,sy,zoom);
    }
    bool FlatPointOverMedia(int x, int y) const {
        if(vrInfo_.vr || nativePixelSizing_ || !videoWindow_ || !eyeW_ || !eyeH_) return false;
        RECT rc{}; GetClientRect(videoWindow_,&rc);
        const float clientW=static_cast<float>(std::max<LONG>(1L,rc.right-rc.left));
        const float clientH=static_cast<float>(std::max<LONG>(1L,rc.bottom-rc.top));
        const float viewportAspect=clientW/clientH;
        const float sourceAspect=static_cast<float>(eyeW_)/static_cast<float>(eyeH_);
        float sx=1.0f,sy=1.0f;
        if(viewportAspect>sourceAspect) sx=sourceAspect/viewportAspect;
        else sy=viewportAspect/sourceAspect;
        sx=std::max(0.0001f,sx); sy=std::max(0.0001f,sy);
        const float zoom=std::clamp(flatZoom_,0.25f,8.0f);

        // Match the pixel shader exactly. These are the screen-space bounds where
        // source UVs remain inside [0,1]; everything outside is the black surround.
        const float left=clientW*(0.5f-flatCenterU_*sx*zoom);
        const float right=clientW*(0.5f+(1.0f-flatCenterU_)*sx*zoom);
        const float top=clientH*(0.5f-flatCenterV_*sy*zoom);
        const float bottom=clientH*(0.5f+(1.0f-flatCenterV_)*sy*zoom);
        const float px=static_cast<float>(x), py=static_cast<float>(y);
        return px>=std::max(0.0f,left) && px<std::min(clientW,right) &&
               py>=std::max(0.0f,top) && py<std::min(clientH,bottom);
    }
    void FlatWheelZoom(short delta, int x, int y) {
        if(vrInfo_.vr || nativePixelSizing_ || !videoWindow_ || !eyeW_ || !eyeH_ || delta==0) return;
        RECT rc{}; GetClientRect(videoWindow_,&rc);
        const float clientW=static_cast<float>(std::max<LONG>(1L,rc.right-rc.left));
        const float clientH=static_cast<float>(std::max<LONG>(1L,rc.bottom-rc.top));
        const float viewportAspect=clientW/clientH;
        const float sourceAspect=static_cast<float>(eyeW_)/static_cast<float>(eyeH_);
        float sx=1.0f,sy=1.0f;
        if(viewportAspect>sourceAspect) sx=sourceAspect/viewportAspect;
        else sy=viewportAspect/sourceAspect;
        sx=std::max(0.0001f,sx); sy=std::max(0.0001f,sy);

        const float oldZoom=std::clamp(flatZoom_,0.25f,8.0f);
        const float factor=std::pow(1.15f,static_cast<float>(delta)/120.0f);
        const float newZoom=std::clamp(oldZoom*factor,0.25f,8.0f);
        if(std::abs(newZoom-1.0f)<=0.001f){ ResetFlatZoom(); return; }

        const float clampedX=std::clamp(static_cast<float>(x),0.0f,clientW);
        const float clampedY=std::clamp(static_cast<float>(y),0.0f,clientH);
        const float px=(clampedX/clientW)*2.0f-1.0f;
        const float py=(clampedY/clientH)*2.0f-1.0f;
        const float baseU=(px/sx)*0.5f+0.5f;
        const float baseV=(py/sy)*0.5f+0.5f;
        const float sourceU=flatCenterU_+(baseU-0.5f)/oldZoom;
        const float sourceV=flatCenterV_+(baseV-0.5f)/oldZoom;
        float newCenterU=sourceU-(baseU-0.5f)/newZoom;
        float newCenterV=sourceV-(baseV-0.5f)/newZoom;

        flatZoom_=newZoom; flatCenterU_=newCenterU; flatCenterV_=newCenterV;
        // Above fit this prevents revealing black through a cropped axis. Below fit it
        // keeps the smaller media fully inside the client area while still allowing it
        // to be positioned anywhere within the available black surround.
        ClampFlatCenter(sx,sy,newZoom);
    }
    const VRInfo& VR() const { return vrInfo_; }
    int EffectiveProjection() const {
        return projectionOverride_ != 0 ? projectionOverride_ : vrInfo_.projection;
    }
    bool IsVr360Enabled() const { return vrInfo_.vr && EffectiveProjection() == 1; }
    bool IsMirroredBack360() const {
        // Stereo-packed VR content is fundamentally front-facing 180 data.
        // When the user enables 360, mirror the back hemisphere instead of
        // stretching the front 180 across the full sphere.
        return vrInfo_.vr && vrInfo_.layout != 0 && EffectiveProjection() == 1;
    }
    void ToggleVrBackside() {
        if (!vrInfo_.vr) return;

        // Keep the user's live 180/360 choice separate from automatic VR detection.
        // This prevents a later layout/projection probe from silently undoing the toggle.
        if (EffectiveProjection() == 1) {
            projectionOverride_ = 2;
            while (yaw_ > PI_F) yaw_ -= 2.0f * PI_F;
            while (yaw_ < -PI_F) yaw_ += 2.0f * PI_F;
            if (std::abs(yaw_) > (PI_F * 0.5f)) yaw_ = 0.0f;
        } else {
            projectionOverride_ = 1;
        }
    }

    void BeginDrag(int x, int y) {
        // Preserve VR mouse-look. Flat media can be repositioned at any wheel zoom,
        // including below fit-to-window, but a drag may start only on actual media
        // pixels. The black surround is deliberately not draggable.
        if (!vrInfo_.vr && !FlatPointOverMedia(x,y)) return;
        dragging_ = true; lastX_ = x; lastY_ = y; SetCapture(videoWindow_);
    }
    void Drag(int x, int y) {
        if (!dragging_) return;
        const int dx = x - lastX_, dy = y - lastY_;
        lastX_ = x; lastY_ = y;
        if (vrInfo_.vr) {
            // VR mouse-look: keep the approved horizontal direction, with gentler vertical movement.
            yaw_ -= dx * 0.0032f;
            // Vertical direction is intentionally opposite to the horizontal grab direction.
            pitch_ = std::clamp(pitch_ - dy * 0.0026f, -1.48f, 1.48f);
            return;
        }
        if (nativePixelSizing_ || !videoWindow_ || !eyeW_ || !eyeH_) return;
        RECT rc{}; GetClientRect(videoWindow_,&rc);
        const float clientW=static_cast<float>(std::max<LONG>(1L,rc.right-rc.left));
        const float clientH=static_cast<float>(std::max<LONG>(1L,rc.bottom-rc.top));
        const float viewportAspect=clientW/clientH;
        const float sourceAspect=static_cast<float>(eyeW_)/static_cast<float>(eyeH_);
        float sx=1.0f,sy=1.0f;
        if(viewportAspect>sourceAspect) sx=sourceAspect/viewportAspect;
        else sy=viewportAspect/sourceAspect;
        sx=std::max(0.0001f,sx); sy=std::max(0.0001f,sy);
        const float zoom=std::clamp(flatZoom_,0.25f,8.0f);
        // Grab-style panning: dragging the picture right/down moves the picture with
        // the pointer, which means sampling a little farther left/up in source space.
        flatCenterU_-=static_cast<float>(dx)/(clientW*sx*zoom);
        flatCenterV_-=static_cast<float>(dy)/(clientH*sy*zoom);
        ClampFlatCenter(sx,sy,zoom);
    }
    void EndDrag() {
        if (!dragging_) return;
        dragging_ = false;
        if (GetCapture() == videoWindow_) ReleaseCapture();
    }
    void CancelDrag() { dragging_ = false; }
    void Wheel(short delta) {
        if (!vrInfo_.vr) return;
        float deg = fovRadians_ * 180.f / PI_F;
        deg = std::clamp(deg - (delta / 120.f) * 5.f, 35.f, 110.f);
        fovRadians_ = deg * PI_F / 180.f;
    }

private:
    static constexpr ULONGLONG kMediaSwitchFadeMs = 220;
    struct SurfaceState {
        VRInfo vr{};
        int projectionOverride = 0;
        float yaw = 0.0f, pitch = 0.0f, fov = 65.0f * PI_F / 180.0f;
        UINT eyeW = 0, eyeH = 0;
        bool nativePixelSizing = false;
        float flatZoom = 1.0f, flatCenterU = 0.5f, flatCenterV = 0.5f;
    };
    struct Vertex { float x,y,u,v; };
    struct ShaderConstants {
        float yaw, pitch, fov, vrMode;
        float layout, projection, sourceAspect, viewportAspect;
        float mirrorBack, pad0, pad1, pad2;
    };

    SurfaceState CurrentSurfaceState() const {
        SurfaceState s;
        s.vr=vrInfo_; s.projectionOverride=projectionOverride_;
        s.yaw=yaw_; s.pitch=pitch_; s.fov=fovRadians_;
        s.eyeW=eyeW_; s.eyeH=eyeH_;
        s.nativePixelSizing=nativePixelSizing_;
        s.flatZoom=flatZoom_; s.flatCenterU=flatCenterU_; s.flatCenterV=flatCenterV_;
        return s;
    }

    void ClearVideoTransition() {
        transitionTexture_.Reset(); transitionSRV_.Reset();
        transitionWaitingForFrame_=false; transitionActive_=false; transitionStart_=0;
    }

    void DrawVideoSurface(ID3D11ShaderResourceView* srv,const SurfaceState& s,float opacity) {
        if(!srv || !context_ || !videoWindow_ || !s.eyeW || !s.eyeH) return;
        RECT rc{}; GetClientRect(videoWindow_,&rc);
        const float clientW=static_cast<float>(std::max<LONG>(1L,rc.right-rc.left));
        const float clientH=static_cast<float>(std::max<LONG>(1L,rc.bottom-rc.top));
        float renderW=clientW,renderH=clientH,renderX=0.0f,renderY=0.0f;
        if(s.nativePixelSizing && !s.vr.vr){
            const float sourceW=static_cast<float>(s.eyeW),sourceH=static_cast<float>(s.eyeH);
            // Native Size is literal 1:1 pixel mapping. Windowed mode is prevented from
            // becoming smaller than this surface; fullscreen may clip an oversized source
            // rather than silently scaling it below native dimensions.
            renderW=std::max(1.0f,sourceW);
            renderH=std::max(1.0f,sourceH);
            renderX=std::floor((clientW-renderW)*0.5f);
            renderY=std::floor((clientH-renderH)*0.5f);
        }
        D3D11_VIEWPORT vp{}; vp.TopLeftX=renderX;vp.TopLeftY=renderY;vp.Width=renderW;vp.Height=renderH;vp.MinDepth=0.f;vp.MaxDepth=1.f;
        context_->RSSetViewports(1,&vp);
        const int projection=s.projectionOverride!=0?s.projectionOverride:s.vr.projection;
        ShaderConstants c{};
        c.yaw=s.yaw;c.pitch=s.pitch;c.fov=s.fov;c.vrMode=s.vr.vr?1.0f:0.0f;
        c.layout=std::clamp(opacity,0.0f,1.0f);
        c.projection=static_cast<float>(projection);
        c.sourceAspect=s.eyeH?static_cast<float>(s.eyeW)/static_cast<float>(s.eyeH):1.0f;
        c.viewportAspect=vp.Height>0.f?vp.Width/vp.Height:1.0f;
        c.mirrorBack=(s.vr.vr && s.vr.layout!=0 && projection==1)?1.0f:0.0f;
        c.pad0=(!s.vr.vr && !s.nativePixelSizing)?s.flatZoom:1.0f;
        c.pad1=s.flatCenterU;c.pad2=s.flatCenterV;
        context_->UpdateSubresource(constantBuffer_.Get(),0,nullptr,&c,0,0);
        const float blendFactor[4]={0,0,0,0};
        context_->OMSetBlendState(blendState_.Get(),blendFactor,0xffffffffu);
        UINT stride=sizeof(Vertex),offset=0;
        context_->IASetInputLayout(inputLayout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetVertexBuffers(0,1,vertexBuffer_.GetAddressOf(),&stride,&offset);
        context_->VSSetShader(vs_.Get(),nullptr,0);
        context_->PSSetShader(ps_.Get(),nullptr,0);
        context_->PSSetShaderResources(0,1,&srv);
        context_->PSSetSamplers(0,1,sampler_.GetAddressOf());
        context_->PSSetConstantBuffers(0,1,constantBuffer_.GetAddressOf());
        context_->Draw(6,0);
        ID3D11ShaderResourceView* nullSrv=nullptr;
        context_->PSSetShaderResources(0,1,&nullSrv);
    }

    int DetectPackedStereoFromCurrentFrame() {
        if (!engine_ || !device_ || !context_ || !nativeW_ || !nativeH_) return -1;

        // The packed-stereo probe is intentionally asynchronous. A blocking Map() here
        // can stall the UI for high-resolution VR frames while the GPU finishes the copy.
        // We submit the tiny readback on one frame and inspect it on a later frame with
        // D3D11_MAP_FLAG_DO_NOT_WAIT. -2 means "not ready yet".
        const float aspect = static_cast<float>(nativeW_) / static_cast<float>(nativeH_);
        UINT probeW = 256u;
        UINT probeH = static_cast<UINT>(std::clamp<int>(static_cast<int>(std::lround(256.0f / std::max(0.01f, aspect))), 64, 256));
        if (aspect < 1.0f) {
            probeH = 256u;
            probeW = static_cast<UINT>(std::clamp<int>(static_cast<int>(std::lround(256.0f * aspect)), 64, 256));
        }
        probeW = std::max<UINT>(4u, probeW & ~1u);
        probeH = std::max<UINT>(4u, probeH & ~1u);

        if (stereoProbeW_ != probeW || stereoProbeH_ != probeH) {
            stereoProbeGpu_.Reset();
            stereoProbeStaging_.Reset();
            stereoProbePending_ = false;
            stereoProbeW_ = probeW;
            stereoProbeH_ = probeH;
        }

        if (!stereoProbeGpu_ || !stereoProbeStaging_) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = probeW;
            td.Height = probeH;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&td, nullptr, &stereoProbeGpu_))) return -1;

            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&sd, nullptr, &stereoProbeStaging_))) {
                stereoProbeGpu_.Reset();
                return -1;
            }
        }

        if (!stereoProbePending_) {
            MFVideoNormalizedRect src{0.f, 0.f, 1.f, 1.f};
            RECT dst{0, 0, static_cast<LONG>(probeW), static_cast<LONG>(probeH)};
            MFARGB border{0,0,0,255};
            if (FAILED(engine_->TransferVideoFrame(stereoProbeGpu_.Get(), &src, &dst, &border))) return -1;
            context_->CopyResource(stereoProbeStaging_.Get(), stereoProbeGpu_.Get());
            stereoProbePending_ = true;
            return -2;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT mapHr = context_->Map(stereoProbeStaging_.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (mapHr == DXGI_ERROR_WAS_STILL_DRAWING) return -2;
        if (FAILED(mapHr)) {
            stereoProbePending_ = false;
            return -1;
        }

        auto luma = [](const BYTE* p) -> double {
            return 0.0722*p[0] + 0.7152*p[1] + 0.2126*p[2]; // BGRA
        };
        struct Metric { double mad=1e9,corr=-1.0,contrast=0.0; };
        auto measureLR = [&]() -> Metric {
            const UINT halfW=probeW/2u;
            const int maxShift=std::clamp(static_cast<int>(halfW/10u),3,24);
            Metric best{};
            constexpr int sxCount=20,syCount=12;
            for(int shift=-maxShift;shift<=maxShift;++shift){
                double a=0,b=0,aa=0,bb=0,ab=0,mad=0; int count=0;
                for(int gy=0;gy<syCount;++gy){
                    const UINT y=std::min<UINT>(probeH-1u,static_cast<UINT>((gy+0.5)*probeH/syCount));
                    for(int gx=0;gx<sxCount;++gx){
                        const UINT x=std::min<UINT>(halfW-1u,static_cast<UINT>((gx+0.5)*halfW/sxCount));
                        const int bx=std::clamp(static_cast<int>(x+halfW)+shift,static_cast<int>(halfW),static_cast<int>(probeW)-1);
                        const BYTE* pa=static_cast<const BYTE*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch+static_cast<size_t>(x)*4u;
                        const BYTE* pb=static_cast<const BYTE*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch+static_cast<size_t>(bx)*4u;
                        const double av=luma(pa),bv=luma(pb); a+=av;b+=bv;aa+=av*av;bb+=bv*bv;ab+=av*bv;mad+=std::abs(av-bv);++count;
                    }
                }
                if(count<24) continue;
                const double ma=a/count,mb=b/count,va=std::max(0.0,aa/count-ma*ma),vb=std::max(0.0,bb/count-mb*mb);
                const double denom=std::sqrt(va*vb),corr=denom>1e-6?(ab/count-ma*mb)/denom:-1.0;
                const Metric m{mad/count,corr,std::sqrt(std::max(0.0,(va+vb)*0.5))};
                if(m.corr>best.corr || (std::abs(m.corr-best.corr)<0.015 && m.mad<best.mad)) best=m;
            }
            return best;
        };
        auto measureTB = [&]() -> Metric {
            const UINT halfH=probeH/2u;
            const int maxShift=std::clamp(static_cast<int>(probeW/20u),3,24);
            Metric best{};
            constexpr int sxCount=20,syCount=12;
            for(int shift=-maxShift;shift<=maxShift;++shift){
                double a=0,b=0,aa=0,bb=0,ab=0,mad=0; int count=0;
                for(int gy=0;gy<syCount;++gy){
                    const UINT y=std::min<UINT>(halfH-1u,static_cast<UINT>((gy+0.5)*halfH/syCount));
                    for(int gx=0;gx<sxCount;++gx){
                        const UINT x=std::min<UINT>(probeW-1u,static_cast<UINT>((gx+0.5)*probeW/sxCount));
                        const int bx=std::clamp(static_cast<int>(x)+shift,0,static_cast<int>(probeW)-1);
                        const BYTE* pa=static_cast<const BYTE*>(mapped.pData)+static_cast<size_t>(y)*mapped.RowPitch+static_cast<size_t>(x)*4u;
                        const BYTE* pb=static_cast<const BYTE*>(mapped.pData)+static_cast<size_t>(y+halfH)*mapped.RowPitch+static_cast<size_t>(bx)*4u;
                        const double av=luma(pa),bv=luma(pb); a+=av;b+=bv;aa+=av*av;bb+=bv*bv;ab+=av*bv;mad+=std::abs(av-bv);++count;
                    }
                }
                if(count<24) continue;
                const double ma=a/count,mb=b/count,va=std::max(0.0,aa/count-ma*ma),vb=std::max(0.0,bb/count-mb*mb);
                const double denom=std::sqrt(va*vb),corr=denom>1e-6?(ab/count-ma*mb)/denom:-1.0;
                const Metric m{mad/count,corr,std::sqrt(std::max(0.0,(va+vb)*0.5))};
                if(m.corr>best.corr || (std::abs(m.corr-best.corr)<0.015 && m.mad<best.mad)) best=m;
            }
            return best;
        };
        auto likely=[](const Metric& m){
            if(m.contrast<5.0) return false;
            return (m.corr>=0.72&&m.mad<=72.0)||(m.corr>=0.58&&m.mad<=42.0);
        };

        const Metric lr=measureLR();
        const Metric tb=measureTB();
        context_->Unmap(stereoProbeStaging_.Get(), 0);
        stereoProbePending_ = false;
        if(likely(lr) && (!likely(tb) || lr.corr>tb.corr+0.05)) return 1;
        if(likely(tb) && (!likely(lr) || tb.corr>lr.corr+0.05)) return 2;
        if(lr.contrast<7.0 && tb.contrast<7.0) return -1; // fade/black/flat frame: retry on a later frame
        return 0;
    }

    HRESULT CreateMediaEngine() {
        ComPtr<IMFMediaEngineClassFactory> factory;
        HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) return hr;
        notify_.Attach(new MediaEngineNotify(eventWindow_));

        ComPtr<IMFAttributes> attrs;
        hr = MFCreateAttributes(&attrs, 4);
        if (FAILED(hr)) return hr;
        attrs->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify_.Get());
        attrs->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
        attrs->SetUnknown(MF_MEDIA_ENGINE_DXGI_MANAGER, dxgiManager_.Get());

        hr = factory->CreateInstance(0, attrs.Get(), &engine_);
        if (FAILED(hr)) return hr;
        engine_->SetAutoPlay(FALSE);
        engine_->SetVolume(0.30);
        return S_OK;
    }

    HRESULT CreateBackbuffer() {
        ComPtr<ID3D11Texture2D> back;
        HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&back));
        if (FAILED(hr)) return hr;
        return device_->CreateRenderTargetView(back.Get(), nullptr, &renderTarget_);
    }

    HRESULT Compile(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
        ComPtr<ID3DBlob> errors;
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL1;
        HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, flags, 0, &blob, &errors);
        if (FAILED(hr) && errors) {
            std::string e(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
            MessageBoxA(eventWindow_, e.c_str(), "Shader compile error", MB_ICONERROR);
        }
        return hr;
    }

    HRESULT CreatePipeline() {
        static const char* hlsl = R"HLSL(
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
cbuffer C : register(b0) {
    float yaw;
    float pitch;
    float fov;
    float vrMode;
    float layout;
    float projection;
    float sourceAspect;
    float viewportAspect;
    float mirrorBack;
    float pad0;
    float pad1;
    float pad2;
};
struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };
struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
PSIn VSMain(VSIn i) { PSIn o; o.pos=float4(i.pos,0,1); o.uv=i.uv; return o; }
float4 PSMain(PSIn i) : SV_TARGET {
    if (vrMode < 0.5) {
        float2 p = i.uv * 2.0 - 1.0;
        float sx = 1.0, sy = 1.0;
        if (viewportAspect > sourceAspect) sx = sourceAspect / viewportAspect;
        else sy = viewportAspect / sourceAspect;
        float2 baseUv = float2(p.x / sx, p.y / sy) * 0.5 + 0.5;
        float zoom = clamp(pad0, 0.25, 8.0);
        float2 uv = float2(pad1, pad2) + (baseUv - 0.5) / zoom;
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return float4(0,0,0,layout);
        float4 sampled = tex0.Sample(samp0, uv);
        return float4(sampled.rgb, layout);
    }

    float2 ndc = i.uv * 2.0 - 1.0;
    float tanHalf = tan(fov * 0.5);
    float3 d = normalize(float3(ndc.x * viewportAspect * tanHalf, -ndc.y * tanHalf, 1.0));

    float cp = cos(pitch), sp = sin(pitch);
    d = float3(d.x, cp*d.y - sp*d.z, sp*d.y + cp*d.z);
    float cy = cos(yaw), syaw = sin(yaw);
    d = float3(cy*d.x + syaw*d.z, d.y, -syaw*d.x + cy*d.z);

    float lon = atan2(d.x, d.z);
    float lat = asin(clamp(d.y, -1.0, 1.0));
    float2 uv;
    if (projection > 1.5) {
        if (abs(lon) > 1.57079632679) return float4(0,0,0,layout);
        uv.x = lon / 3.14159265359 + 0.5;
    } else {
        if (mirrorBack > 0.5) {
            if (lon > 1.57079632679) lon = 3.14159265359 - lon;
            else if (lon < -1.57079632679) lon = -3.14159265359 - lon;
            uv.x = lon / 3.14159265359 + 0.5;
        } else {
            uv.x = frac(lon / 6.28318530718 + 0.5);
        }
    }
    uv.y = 0.5 - lat / 3.14159265359;

    float4 sampled = tex0.Sample(samp0, uv);
    return float4(sampled.rgb, layout);
}
)HLSL";
        ComPtr<ID3DBlob> vsBlob, psBlob;
        HRESULT hr = Compile(hlsl, "VSMain", "vs_4_0", vsBlob);
        if (FAILED(hr)) return hr;
        hr = Compile(hlsl, "PSMain", "ps_4_0", psBlob);
        if (FAILED(hr)) return hr;
        hr = device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs_);
        if (FAILED(hr)) return hr;
        hr = device_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps_);
        if (FAILED(hr)) return hr;

        D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}
        };
        hr = device_->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout_);
        if (FAILED(hr)) return hr;

        Vertex v[] = {
            {-1.f,-1.f,0.f,1.f}, {-1.f,1.f,0.f,0.f}, {1.f,1.f,1.f,0.f},
            {-1.f,-1.f,0.f,1.f}, {1.f,1.f,1.f,0.f}, {1.f,-1.f,1.f,1.f}
        };
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth = sizeof(v); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA init{}; init.pSysMem = v;
        hr = device_->CreateBuffer(&bd, &init, &vertexBuffer_);
        if (FAILED(hr)) return hr;

        D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = sizeof(ShaderConstants); cbd.Usage = D3D11_USAGE_DEFAULT; cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        hr = device_->CreateBuffer(&cbd, nullptr, &constantBuffer_);
        if (FAILED(hr)) return hr;

        D3D11_SAMPLER_DESC sd{};
        // VR projection is highly non-linear, especially near the poles.
        // 16x anisotropic filtering preserves considerably more source detail
        // than the old bilinear-only sampler.
        sd.Filter = D3D11_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = 16;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MinLOD = 0.0f;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        hr=device_->CreateSamplerState(&sd, &sampler_);
        if(FAILED(hr)) return hr;

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].BlendEnable=TRUE;
        blend.RenderTarget[0].SrcBlend=D3D11_BLEND_SRC_ALPHA;
        blend.RenderTarget[0].DestBlend=D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp=D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha=D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha=D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        return device_->CreateBlendState(&blend,&blendState_);
    }

    HRESULT EnsureVideoTexture() {
        if (!engine_) return E_FAIL;
        DWORD w=0,h=0;
        HRESULT hr = engine_->GetNativeVideoSize(&w,&h);
        if (FAILED(hr) || !w || !h) return hr;

        // A filename ending in "360" or "360 (number)" identifies the projection,
        // not necessarily the stereo packing.  Obvious 4:1 SBS and ~1:1 TB sources
        // can be resolved from aspect ratio.  A ~2:1 360 source is ambiguous because
        // it can be either a mono panorama or two square stereo eyes side-by-side;
        // defer that case to a tiny first-frame content probe in Render().
        if (vrInfo_.vr && !vrInfo_.layoutExplicit && !layoutDetectionDone_) {
            const float aspect = static_cast<float>(w) / static_cast<float>(h);
            layoutDetectionPending_ = false;
            if (vrInfo_.projection == 1) {
                if (aspect >= 3.20f) {
                    vrInfo_.layout = 1;
                    vrInfo_.projection = 2;
                    layoutDetectionDone_ = true;
                } else if (aspect <= 1.20f) {
                    vrInfo_.layout = 2;
                    vrInfo_.projection = 2;
                    layoutDetectionDone_ = true;
                } else {
                    vrInfo_.layout = 0;
                    layoutDetectionPending_ = true;
                }
            } else if (vrInfo_.projection == 2) {
                if (aspect >= 1.70f && aspect < 3.20f) vrInfo_.layout = 1;
                else if (aspect <= 0.70f) vrInfo_.layout = 2;
                else vrInfo_.layout = 0;
                if (vrInfo_.layout != 0) vrInfo_.projection = 2;
                layoutDetectionDone_ = true;
            }
        }

        UINT newEyeW = static_cast<UINT>(w);
        UINT newEyeH = static_cast<UINT>(h);
        if (vrInfo_.layout == 1) newEyeW = std::max<UINT>(1u, static_cast<UINT>(w) / 2u);
        else if (vrInfo_.layout == 2) newEyeH = std::max<UINT>(1u, static_cast<UINT>(h) / 2u);

        if (videoTexture_ && w == nativeW_ && h == nativeH_ && newEyeW == eyeW_ && newEyeH == eyeH_) return S_OK;
        nativeW_ = static_cast<UINT>(w);
        nativeH_ = static_cast<UINT>(h);
        eyeW_ = newEyeW;
        eyeH_ = newEyeH;
        videoSRV_.Reset(); videoTexture_.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = eyeW_; td.Height = eyeH_; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = device_->CreateTexture2D(&td, nullptr, &videoTexture_);
        if (FAILED(hr)) return hr;
        return device_->CreateShaderResourceView(videoTexture_.Get(), nullptr, &videoSRV_);
    }

    HWND eventWindow_{};
    HWND videoWindow_{};
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain> swapChain_;
    UINT swapChainW_ = 0, swapChainH_ = 0;
    ComPtr<ID3D11RenderTargetView> renderTarget_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11InputLayout> inputLayout_;
    ComPtr<ID3D11Buffer> vertexBuffer_;
    ComPtr<ID3D11Buffer> constantBuffer_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11BlendState> blendState_;
    ComPtr<ID3D11Texture2D> videoTexture_;
    ComPtr<ID3D11ShaderResourceView> videoSRV_;
    ComPtr<ID3D11Texture2D> transitionTexture_;
    ComPtr<ID3D11ShaderResourceView> transitionSRV_;
    SurfaceState transitionState_{};
    bool transitionWaitingForFrame_=false,transitionActive_=false;
    ULONGLONG transitionStart_=0;
    ComPtr<ID3D11Texture2D> stereoProbeGpu_;
    ComPtr<ID3D11Texture2D> stereoProbeStaging_;
    UINT stereoProbeW_ = 0, stereoProbeH_ = 0;
    bool stereoProbePending_ = false;
    ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    UINT dxgiResetToken_{};
    ComPtr<IMFMediaEngineNotify> notify_;
    ComPtr<IMFMediaEngine> engine_;
    std::wstring path_;
    VRInfo vrInfo_{};
    double pendingStartSeconds_ = -1.0;
    UINT nativeW_ = 0, nativeH_ = 0;
    UINT eyeW_ = 0, eyeH_ = 0;
    bool autoPlayWhenReady_ = false;
    bool layoutDetectionDone_ = false;
    bool layoutDetectionPending_ = false;
    int layoutDetectionAttempts_ = 0;
    int layoutMonoVotes_ = 0;
    int projectionOverride_ = 0; // 0=automatic, 1=force 360, 2=force front-only 180
    bool dragging_ = false;
    int lastX_ = 0, lastY_ = 0;
    float yaw_ = 0.f, pitch_ = 0.f, fovRadians_ = 65.f * PI_F / 180.f;
    float flatZoom_ = 1.0f, flatCenterU_ = 0.5f, flatCenterV_ = 0.5f;
    bool nativePixelSizing_ = false;
};

class App {
public:
    static constexpr int kDefaultLibraryCardWidth = 340;
    static constexpr int kMinLibraryCardWidth = 140;
    // Library posters and Timeline stills use the same 1920x1080 visual-master ceiling.
    // RAM decoding remains display-sized, so the larger disk master improves fullscreen/
    // zoom quality without forcing every visible card to occupy 1080p in memory.
    static constexpr int kVisualPreviewCacheWidth = 1920;
    static constexpr int kVisualPreviewCacheHeight = 1080;
    static constexpr int kVisualPreviewCacheVersion = 5;
    // Library posters use the same 1920x1080 master.
    static constexpr int kLibraryPreviewCacheWidth = 1920;
    static constexpr int kLibraryPreviewCacheHeight = 1080;
    static constexpr int kPreviewDecodeBucket = 128;
    // Moving hover frames are throughput-limited independently from still-cache quality.
    // This preserves native frame pacing on high-fps/portrait media instead of allowing
    // a large resized window to turn hover playback into a memory-bandwidth benchmark.
    static constexpr double kHoverPreviewPixelRateBudget = 55000000.0;
    static constexpr int kLibraryTitleHeight = 45;
    static constexpr int kLibraryGap = 16;
    static constexpr int kLibraryPad = 20;
    static constexpr int kLibraryScrollbarReserve = 18;

    static int QuantizedPreviewDecodeWidth(int displayWidth) {
        const int wanted=std::clamp(std::max(1,displayWidth),256,kVisualPreviewCacheWidth);
        const int bucketed=((wanted+kPreviewDecodeBucket-1)/kPreviewDecodeBucket)*kPreviewDecodeBucket;
        return std::min(kVisualPreviewCacheWidth,bucketed);
    }

    static int PreviewDecodeHeightForWidth(int width) {
        return std::min(kVisualPreviewCacheHeight,std::max(1,static_cast<int>(std::lround(static_cast<double>(width)*9.0/16.0))));
    }

    static int FourKReferenceDecodeWidthForAcross(int across) {
        // The layout/quality model was tuned on a 3840x2160 monitor. Keep those exact
        // reference tiers there: 4=1920, 5=1536, 6=1280, 7=1152.
        const int safeAcross=std::max(4,across);
        const int requested=static_cast<int>(std::ceil(
            static_cast<double>(kVisualPreviewCacheWidth)*4.0/static_cast<double>(safeAcross)));
        const int bucketed=((requested+kPreviewDecodeBucket-1)/kPreviewDecodeBucket)*kPreviewDecodeBucket;
        return std::clamp(bucketed,384,kVisualPreviewCacheWidth);
    }

    double CurrentMonitorQualityScale() const {
        // Scale RAM/display decode targets by the monitor's physical pixel dimensions,
        // using 3840x2160 as the exact reference. The persistent cache never grows beyond
        // 1920x1080; larger displays simply reach that existing master sooner.
        if(!hwnd_) return 1.0;
        HMONITOR mon=MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize=sizeof(mi);
        if(!mon || !GetMonitorInfoW(mon,&mi)) return 1.0;
        const int monitorW=std::max(1,static_cast<int>(mi.rcMonitor.right-mi.rcMonitor.left));
        const int monitorH=std::max(1,static_cast<int>(mi.rcMonitor.bottom-mi.rcMonitor.top));
        const double sx=static_cast<double>(monitorW)/3840.0;
        const double sy=static_cast<double>(monitorH)/2160.0;
        return std::clamp(std::min(sx,sy),0.25,2.0);
    }

    int WorkingDecodeWidthForAcross(int across) const {
        const int reference=FourKReferenceDecodeWidthForAcross(across);
        const double scale=CurrentMonitorQualityScale();
        const int requested=static_cast<int>(std::ceil(static_cast<double>(reference)*scale));
        const int bucketed=((std::max(1,requested)+kPreviewDecodeBucket-1)/kPreviewDecodeBucket)*kPreviewDecodeBucket;
        // Important: 1920 is both the working ceiling and the disk-master ceiling on every
        // monitor. 5K/8K displays never create or request a larger persistent cache.
        return std::clamp(bucketed,384,kVisualPreviewCacheWidth);
    }

    bool PreviewUsesFullHdWorkingBitmap(int displayWidth) const {
        (void)displayWidth;
        if(!hwnd_) return false;
        RECT rc{};
        if(!GetClientRect(hwnd_,&rc)) return false;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left));
        const int activeWidth=DetailsPreviewCardWidthForViewport(clientWidth);
        return PreviewAcrossForWidth(clientWidth,activeWidth)<=4;
    }

    int PreviewDecodeWidthForCurrentView(int displayWidth) const {
        if(!hwnd_) return QuantizedPreviewDecodeWidth(displayWidth);
        RECT rc{};
        if(!GetClientRect(hwnd_,&rc)) return QuantizedPreviewDecodeWidth(displayWidth);
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left));
        const int activeWidth=DetailsPreviewCardWidthForViewport(clientWidth);
        const int across=PreviewAcrossForWidth(clientWidth,activeWidth);
        return WorkingDecodeWidthForAcross(across);
    }

    bool LibraryUsesFullHdWorkingBitmap(int displayWidth) const {
        (void)displayWidth;
        if(!hwnd_) return false;
        RECT rc{};
        if(!GetClientRect(hwnd_,&rc)) return false;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left));
        return LibraryAcrossForWidth(clientWidth,libraryCardWidth_)<=4;
    }

    int QuantizedLibraryDecodeWidth(int displayWidth) const {
        if(!hwnd_) {
            const int requested=static_cast<int>(std::lround(std::max(1,displayWidth)*1.35));
            const int wanted=std::clamp(requested,384,kLibraryPreviewCacheWidth);
            const int bucketed=((wanted+kPreviewDecodeBucket-1)/kPreviewDecodeBucket)*kPreviewDecodeBucket;
            return std::min(kLibraryPreviewCacheWidth,bucketed);
        }
        RECT rc{};
        if(!GetClientRect(hwnd_,&rc)) return QuantizedPreviewDecodeWidth(displayWidth);
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left));
        const int across=LibraryAcrossForWidth(clientWidth,libraryCardWidth_);
        const int acrossTarget=WorkingDecodeWidthForAcross(across);

        // Do not decode a 1536/1920px working bitmap for a 300-400px card merely because
        // the app happens to be on a 4K monitor. Two source pixels per displayed pixel is
        // already enough headroom for the card renderer, while fullscreen/zoomed layouts
        // naturally reach the existing high-quality tiers. This cuts search/favorites
        // cache decode time and RAM substantially in the normal windowed layout.
        const int displayTarget=std::clamp(std::max(1,displayWidth)*2,384,kLibraryPreviewCacheWidth);
        const int displayBucket=((displayTarget+kPreviewDecodeBucket-1)/kPreviewDecodeBucket)*kPreviewDecodeBucket;
        return std::min(acrossTarget,std::min(kLibraryPreviewCacheWidth,displayBucket));
    }

    static int LibraryDecodeHeightForWidth(int width) {
        return std::min(kLibraryPreviewCacheHeight,std::max(1,static_cast<int>(std::lround(static_cast<double>(width)*9.0/16.0))));
    }

    static int DetailsHeroHeightForViewport(int clientWidth,int footerTop,bool isVideo) {
        if(!isVideo) return std::max(260,footerTop-150);
        const int mediaWidth=std::max(1,clientWidth-80);
        const int byAspect=std::max(1,static_cast<int>(std::lround(static_cast<double>(mediaWidth)*9.0/16.0)));
        const int byViewport=std::max(280,static_cast<int>(std::lround(static_cast<double>(std::max(1,footerTop))*0.62)));
        return std::clamp(std::min(byAspect,byViewport),280,1080);
    }

    static int FourAcrossLibraryMaxWidth(int clientWidth) {
        // Maximum zoom is always the largest card width that still fits four cards
        // horizontally in the current Library viewport.
        const int usable = std::max(1, clientWidth - kLibraryScrollbarReserve - kLibraryPad * 2 - kLibraryGap * 3);
        return std::max(kMinLibraryCardWidth, usable / 4);
    }

    static int FiveAcrossWindowedLibraryWidth(int clientWidth) {
        const int usable = std::max(1, clientWidth - kLibraryScrollbarReserve - kLibraryPad * 2 - kLibraryGap * 4);
        return std::max(kMinLibraryCardWidth, usable / 5);
    }

    static int SixAcrossLibraryWidth(int clientWidth) {
        const int usable = std::max(1, clientWidth - kLibraryScrollbarReserve - kLibraryPad * 2 - kLibraryGap * 5);
        return std::max(kMinLibraryCardWidth, usable / 6);
    }

    static int SevenAcrossLibraryWidth(int clientWidth) {
        const int usable = std::max(1, clientWidth - kLibraryScrollbarReserve - kLibraryPad * 2 - kLibraryGap * 6);
        return std::max(kMinLibraryCardWidth, usable / 7);
    }

    static int EightAcrossFullscreenLibraryWidth(int clientWidth) {
        const int usable = std::max(1, clientWidth - kLibraryScrollbarReserve - kLibraryPad * 2 - kLibraryGap * 7);
        return std::max(kMinLibraryCardWidth, usable / 8);
    }

    bool UseEightAcrossFullscreenLibrary(int clientWidth) const {
        (void)clientWidth;
        return false;
    }

    static int LibraryWidthForAcross(int clientWidth,int across) {
        const int safeAcross=std::max(1,across);
        const int usable = std::max(1, clientWidth - kLibraryScrollbarReserve - kLibraryPad * 2 - kLibraryGap * (safeAcross - 1));
        return std::max(kMinLibraryCardWidth, usable / safeAcross);
    }

    static int MaxLibraryAcrossForViewport(int clientWidth) {
        int across=1;
        while(across<64) {
            const int next=across+1;
            const int usable=clientWidth - kLibraryScrollbarReserve - kLibraryPad * 2 - kLibraryGap * (next - 1);
            if(usable<=0 || usable/next<kMinLibraryCardWidth) break;
            across=next;
        }
        return across;
    }

    int LibraryMinAcrossForCurrentMode() const {
        // Maximum zoom-in: four cards across in both windowed and fullscreen Library.
        return 4;
    }

    int LibraryMaxAcrossForCurrentMode() const {
        // Exact zoom-out limits requested for the Library.
        return fullscreen_ ? 7 : 6;
    }

    int LibraryDefaultAcrossForCurrentMode() const {
        return fullscreen_ ? 6 : 5;
    }

    int LibraryAcrossForWidth(int clientWidth,int width) const {
        const int minAcross=LibraryMinAcrossForCurrentMode();
        const int maxAcross=LibraryMaxAcrossForCurrentMode();
        int bestAcross=LibraryDefaultAcrossForCurrentMode();
        int bestDiff=std::numeric_limits<int>::max();
        for(int across=minAcross; across<=maxAcross; ++across) {
            const int candidate=LibraryWidthForAcross(clientWidth,across);
            const int diff=std::abs(candidate-width);
            if(diff<bestDiff) {
                bestDiff=diff;
                bestAcross=across;
            }
        }
        return std::clamp(bestAcross,minAcross,maxAcross);
    }

    int DefaultLibraryCardWidthForViewport(int clientWidth) const {
        // Windowed starts at five-across; fullscreen starts at six-across.
        return LibraryWidthForAcross(clientWidth,LibraryDefaultAcrossForCurrentMode());
    }

    void ApplyLibraryWidthForViewport(int clientWidth) {
        if (!libraryZoomOverridden_) {
            libraryCardWidth_ = DefaultLibraryCardWidthForViewport(clientWidth);
        } else {
            const int across=LibraryAcrossForWidth(clientWidth,libraryCardWidth_);
            libraryCardWidth_ = LibraryWidthForAcross(clientWidth,across);
        }
        libraryCardWidth_ = std::max(kMinLibraryCardWidth, libraryCardWidth_);
    }

    int LibraryWheelPixelsPerNotch() const {
        // Use exactly the same wheel distance in windowed and fullscreen Library views.
        // Fullscreen may use a different card layout, but scrolling itself is identical.
        return 120;
    }

    int DetailsWheelPixelsPerNotch(int clientWidth) const {
        const int cardW=DetailsPreviewCardWidthForViewport(clientWidth);
        const int imageH=std::max(79,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int rowStride=imageH+24+12;
        const int defaultImageH=std::max(79,static_cast<int>(std::lround(static_cast<double>(kDefaultPreviewCardWidth)*9.0/16.0)));
        const int defaultStride=defaultImageH+24+12;
        return std::max(72,static_cast<int>(std::lround(120.0*static_cast<double>(rowStride)/static_cast<double>(std::max(1,defaultStride)))));
    }

    static int ConsumeWheelPixels(short wheelDelta,int pixelsPerNotch,double& remainder) {
        remainder += static_cast<double>(wheelDelta)*static_cast<double>(pixelsPerNotch)/static_cast<double>(WHEEL_DELTA);
        const int pixels = remainder>=0.0 ? static_cast<int>(std::floor(remainder)) : static_cast<int>(std::ceil(remainder));
        remainder -= static_cast<double>(pixels);
        return pixels;
    }

    static int FourAcrossPreviewMaxWidth(int clientWidth) {
        // Same rule for Info secondary previews: maximum zoom still shows four
        // preview cards side-by-side in the current window.
        constexpr int sideMargins = 80;
        constexpr int previewGap = 12;
        constexpr int minPreviewWidth = 140;
        const int usable = std::max(1, clientWidth - sideMargins - previewGap * 3);
        return std::max(minPreviewWidth, usable / 4);
    }

    static int FiveAcrossPreviewWidth(int clientWidth) {
        constexpr int sideMargins = 80;
        constexpr int previewGap = 12;
        constexpr int minPreviewWidth = 140;
        const int usable = std::max(1, clientWidth - sideMargins - previewGap * 4);
        return std::max(minPreviewWidth, usable / 5);
    }

    static int TenAcrossFullscreenPreviewWidth(int clientWidth) {
        constexpr int sideMargins = 80;
        constexpr int previewGap = 12;
        constexpr int minPreviewWidth = 140;
        const int usable = std::max(1, clientWidth - sideMargins - previewGap * 9);
        return std::max(minPreviewWidth, usable / 10);
    }

    static int SevenAcrossWindowedPreviewWidth(int clientWidth) {
        constexpr int sideMargins = 80;
        constexpr int previewGap = 12;
        constexpr int minPreviewWidth = 140;
        const int usable = std::max(1, clientWidth - sideMargins - previewGap * 6);
        return std::max(minPreviewWidth, usable / 7);
    }
    static int PreviewWidthForAcross(int clientWidth,int across) {
        constexpr int sideMargins = 80;
        constexpr int previewGap = 12;
        constexpr int minPreviewWidth = 140;
        const int safeAcross=std::max(1,across);
        const int usable = std::max(1, clientWidth - sideMargins - previewGap * (safeAcross - 1));
        return std::max(minPreviewWidth, usable / safeAcross);
    }

    static int MaxPreviewAcrossForViewport(int clientWidth) {
        constexpr int sideMargins = 80;
        constexpr int previewGap = 12;
        constexpr int minPreviewWidth = 140;
        int across=1;
        while(across<64) {
            const int next=across+1;
            const int usable=clientWidth - sideMargins - previewGap * (next - 1);
            if(usable<=0 || usable/next<minPreviewWidth) break;
            across=next;
        }
        return across;
    }

    bool UseTenAcrossFullscreenPreviews(int clientWidth) const {
        (void)clientWidth;
        return false;
    }

    int PreviewMinAcrossForCurrentMode() const {
        // Maximum zoom-in: four timeline cards across in both modes.
        return 4;
    }

    int PreviewMaxAcrossForCurrentMode() const {
        // Both windowed and fullscreen Timeline may zoom out one step to six-across.
        return 6;
    }

    int PreviewDefaultAcrossForCurrentMode() const {
        // Both windowed and fullscreen Timeline start at five-across.
        return 5;
    }

    int PreviewAcrossForWidth(int clientWidth,int width) const {
        const int minAcross=PreviewMinAcrossForCurrentMode();
        const int maxAcross=PreviewMaxAcrossForCurrentMode();
        int bestAcross=PreviewDefaultAcrossForCurrentMode();
        int bestDiff=std::numeric_limits<int>::max();
        for(int across=minAcross; across<=maxAcross; ++across) {
            const int candidate=PreviewWidthForAcross(clientWidth,across);
            const int diff=std::abs(candidate-width);
            if(diff<bestDiff) {
                bestDiff=diff;
                bestAcross=across;
            }
        }
        return std::clamp(bestAcross,minAcross,maxAcross);
    }

    int DetailsPreviewCardWidthForViewport(int clientWidth) const {
        // Timeline starts at five-across in both modes and snaps only to the allowed
        // discrete layouts. There are no in-between pixel-width zoom states.
        if (!previewZoomOverridden_)
            return PreviewWidthForAcross(clientWidth,PreviewDefaultAcrossForCurrentMode());
        const int across=PreviewAcrossForWidth(clientWidth,previewCardWidth_);
        return PreviewWidthForAcross(clientWidth,across);
    }

    enum class Mode { Library, Details, Player };
    enum class Category { Videos, Images };
    enum class MediaHoverSurface { None, Library, Preview };

    struct PrefetchedPreviewSet {
        std::vector<PreviewFrame> frames;
        double duration = 0.0;
    };

    struct DetailPrefetchJob {
        uint64_t generation = 0;
        Category category = Category::Videos;
        size_t index = 0;
        std::wstring mediaPath;
        std::wstring bannerPath;
        std::wstring previewDir;
        bool isVideo = false;
        bool loadBanner = false;
        bool loadPreviews = false;
    };

    struct DetailPrefetchResult {
        uint64_t generation = 0;
        Category category = Category::Videos;
        size_t index = 0;
        std::wstring mediaPath;
        HBITMAP banner = nullptr;
        int bannerW = 0;
        int bannerH = 0;
        bool hasPreviewSet = false;
        PrefetchedPreviewSet previewSet;
    };

    struct PreviewBitmapDecodeJob {
        uint64_t generation = 0;
        int seconds = 0;
        double seekSeconds = 0.0;
        std::wstring path;
        std::wstring mediaPath;
        VRInfo vr{};
        int cachedLayout = -1;
        bool repairIfMissing = false;
        int width = 0;
        int height = 0;
    };

    struct PreviewBitmapDecodeResult {
        uint64_t generation = 0;
        int seconds = 0;
        std::wstring path;
        int width = 0;
        int height = 0;
        bool repairAttempted = false;
        bool repairSucceeded = false;
        HBITMAP bitmap = nullptr;
    };

    struct LibraryHoverPreviewFrame {
        HBITMAP bitmap = nullptr;
        int width = 0;
        int height = 0;
        double mediaSeconds = 0.0;
        ComPtr<ID2D1Bitmap> gpuBitmap;
        HBITMAP gpuBitmapSource = nullptr;
        uint64_t gpuGeneration = 0;
    };

    struct LibraryHoverPreviewResult {
        uint64_t generation = 0;
        MediaHoverSurface surface = MediaHoverSurface::None;
        size_t index = static_cast<size_t>(-1);
        std::wstring mediaPath;
        std::vector<LibraryHoverPreviewFrame> frames;
    };

    bool Initialize(HINSTANCE inst) {
        inst_ = inst;
        INITCOMMONCONTROLSEX ic{sizeof(ic), ICC_BAR_CLASSES}; InitCommonControlsEx(&ic);
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr)) return false;
        comInitialized_=true;
        hr = MFStartup(MF_VERSION);
        if (FAILED(hr)) return false;
        mfStarted_=true;

        Gdiplus::GdiplusStartupInput gdiplusInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusInput, nullptr) != Gdiplus::Ok) return false;
        LoadUiIcons();

        WNDCLASSW wc{};
        wc.hInstance = inst_;
        wc.lpszClassName = kVmpMainWindowClass;
        wc.lpfnWndProc = MainWndProc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
        wc.hbrBackground = CreateSolidBrush(RGB(13,15,20));
        RegisterClassW(&wc);

        WNDCLASSW vc{};
        vc.hInstance = inst_;
        vc.lpszClassName = L"VisualMediaPlayerVideo";
        vc.lpfnWndProc = VideoWndProc;
        vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        // The D3D swap chain owns every video pixel. A class background brush lets
        // USER32 paint black between WM_SIZE and Present(), which is visible as resize
        // flicker. Suppress background painting and redraw from the retained video SRV.
        vc.hbrBackground = nullptr;
        RegisterClassW(&vc);

        WNDCLASSW cc{};
        cc.hInstance = inst_;
        cc.lpszClassName = L"VisualMediaPlayerControls";
        cc.lpfnWndProc = ControlsWndProc;
        cc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cc.hbrBackground = CreateSolidBrush(RGB(16,18,24));
        RegisterClassW(&cc);

        WNDCLASSW ec{};
        ec.hInstance = inst_;
        ec.lpszClassName = L"VisualMediaPlayerEdgeArrow";
        ec.lpfnWndProc = EdgeArrowWndProc;
        ec.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        ec.hbrBackground = nullptr;
        RegisterClassW(&ec);

        // Standard window size is half of the monitor's fullscreen dimensions in
        // each axis. On a 3840x2160 display this is 1920x1080; other displays scale
        // proportionally instead of using a hard-coded QHD restore size.
        const int monitorW = std::max(1, GetSystemMetrics(SM_CXSCREEN));
        const int monitorH = std::max(1, GetSystemMetrics(SM_CYSCREEN));
        const int initialW = std::max(1, monitorW / 2);
        const int initialH = std::max(1, monitorH / 2);
        // Keep the geometric center of the window exactly on the geometric center
        // of the physical screen. Do not center in the work area, because a taskbar
        // would shift the window slightly away from the true screen center.
        const int initialX = (monitorW - initialW) / 2;
        const int initialY = (monitorH - initialH) / 2;
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"Visual MediaPlayer", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            initialX, initialY, initialW, initialH, nullptr, nullptr, inst_, this);
        if (!hwnd_) return false;
        // Same-user, same-executable named-pipe IPC replaces WM_COPYDATA. The pipe
        // server validates the connecting process before accepting file paths.
        instancePipeServer_.Start(kVmpIpcChannel, hwnd_, WM_APP_EXTERNAL_OPEN);
        HICON appIconBig = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR);
        HICON appIconSmall = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
        if (appIconBig) SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIconBig));
        if (appIconSmall) SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIconSmall));
        BOOL darkTitle = TRUE;
        DwmSetWindowAttribute(hwnd_, 20, &darkTitle, sizeof(darkTitle));
        ApplyMainWindowCornerPreference();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        StartLibraryThumbLoader();
        StartResolutionMetadataWorker();

#if !defined(VMP_PORTABLE_BUILD)
        // Installed builds register this exact EXE path as an "Open with Visual MediaPlayer" handler.
        // Portable builds intentionally leave HKCU\Software\Classes untouched.
        RegisterOpenWith();
#endif

        LoadSettings();
        SetTimer(hwnd_,kLibraryAccessRetryTimerId,1000,nullptr);
        if (!folder_.empty()) {
            if (IsLibraryRootAccessible()) Scan();
            else { libraryUnavailableLatched_=true; libraryAccessFailCount_=3; }
        }
        return true;
    }

    int Run() {
        MSG msg{};
        while (true) {
            bool dispatchedMessage = false;
            int playerMessagesProcessed = 0;
            const ULONGLONG playerPumpStart = GetTickCount64();
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) return static_cast<int>(msg.wParam);
                dispatchedMessage = true;

                // Media navigation shortcuts are application-level. Owned/player windows
                // can temporarily hold keyboard focus, so route these keys through the
                // main window instead of depending on the focused HWND.
                if (msg.message == WM_KEYDOWN && (mode_ == Mode::Library || mode_ == Mode::Details || mode_ == Mode::Player)) {
                    const WPARAM key = msg.wParam;
                    const bool routeSpace = (mode_ == Mode::Details || mode_ == Mode::Player) && key == VK_SPACE;
                    const bool routeMediaArrow = (mode_ == Mode::Details || mode_ == Mode::Player) && (key == VK_LEFT || key == VK_RIGHT);
                    if (routeSpace || routeMediaArrow) {
                        SendMessageW(hwnd_, WM_KEYDOWN, key, msg.lParam);
                    } else {
                        TranslateMessage(&msg);
                        DispatchMessageW(&msg);
                    }
                } else {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }

                // Do not let a flood of WM_MOUSEMOVE/WM_PAINT messages starve video
                // presentation. During playback, yield back to Render() after a small
                // bounded batch even if more UI messages are queued.
                if (mode_ == Mode::Player && (++playerMessagesProcessed >= 8 || GetTickCount64() - playerPumpStart >= 2)) break;
            }
            EnforceProcessMemoryBudget();
            if (mode_ == Mode::Player && player_) {
                // Playback remains the interactive priority, but Load Everything is allowed
                // to continue at below-normal thread priority after the short open-file
                // grace period. Do not continuously extend backgroundPauseUntil_ here.
                player_->Render();
                UpdateSeekUi();
                UpdatePlayerControlVisibility();
            } else if (!dispatchedMessage) {
                WaitMessage();
            }
        }
    }

    bool OpenExternalMedia(const std::wstring& rawPath) {
        if(rawPath.empty()) return false;
        return OpenExternalMediaBatch(std::vector<std::wstring>{rawPath});
    }

    bool OpenExternalMediaBatch(const std::vector<std::wstring>& rawPaths) {
        StopLibraryScanWorker();
        std::vector<fs::path> mediaPaths;
        std::set<std::wstring> seen;
        mediaPaths.reserve(rawPaths.size());
        for(const auto& rawPath:rawPaths){
            if(rawPath.empty()) continue;
            std::error_code ec;
            fs::path mediaPath=fs::path(rawPath);
            if(mediaPath.is_relative()) mediaPath=fs::absolute(mediaPath,ec);
            if(ec){ ec.clear(); mediaPath=fs::path(rawPath); }
            mediaPath=mediaPath.lexically_normal();
            if(!fs::exists(mediaPath,ec) || ec){ ec.clear(); continue; }
            if(!fs::is_regular_file(mediaPath,ec) || ec){ ec.clear(); continue; }
            const std::wstring ext=mediaPath.extension().wstring();
            if(!IsVideoExtension(ext) && !IsImageExtension(ext)) continue;
            const std::wstring key=ToLower(mediaPath.wstring());
            if(seen.insert(key).second) mediaPaths.push_back(std::move(mediaPath));
        }
        if(mediaPaths.empty()) return false;

        const std::wstring firstPath=mediaPaths.front().wstring();
        const bool firstIsVideo=IsVideoExtension(mediaPaths.front().extension().wstring());

        // A new Explorer/Open-With request replaces the previous external session in the
        // existing application window.  Do not create an alternate cache namespace: every
        // item keeps using BuildCachePath/BuildUiCachePath/BuildPreviewDirectory, exactly
        // the same paths it would use when discovered by a normal folder scan.
        KillTimer(hwnd_,kResumeDetailsWorkersTimerId);
        StopImageSlideshow();
        autoNext_=false;
        DestroyPlayerFooterTransition();
        if(player_){ player_->Pause(); player_->CloseSource(); player_->SetNativePixelSizing(false); }
        nativeVideoSizing_=false; nativeImageSizing_=false;
        nativeSizingRestoreRectValid_=false; nativeImageSizingRestoreRectValid_=false;
        if(videoHwnd_) ShowWindow(videoHwnd_,SW_HIDE);
        if(controlsHwnd_) ShowWindow(controlsHwnd_,SW_HIDE);
        if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_HIDE);
        if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_HIDE);
        playerControlsVisible_=false; controlsFading_=false; controlsAlpha_=0;
        controlsHideDeadline_=0;
        volumeFraction_=0.30;
        lastAudibleVolumeFraction_=0.30;

        StopPreviewWorker();
        ClearAllDetailInfoMemory();
        StopThumbnailWorker();
        ResetLibraryThumbLoadView();
        ResetResolutionMetadataWork();
        ClearThumbs(videos_); ClearThumbs(images_);
        videos_.clear(); images_.clear(); folders_.clear(); videoFolderIndices_.clear(); imageFolderIndices_.clear(); childFolderIndices_.clear();
        filteredIndices_.clear(); filterDirty_=true;
        ClearLoadFailureFilter();
        searchQuery_.clear(); searchVisible_=false; searchSelectAll_=false;
        detailsSearchNavigationActive_=false; detailsSearchNavigationIndices_.clear();
        folderViewStates_.clear();
        selected_=0; scrollY_=0; detailsScrollY_=0;
        ResetPreviewZoom(); ResetLibraryZoom(); ResetImageZoom();
        ClearMediaHoverImmediate();
        libraryReturnHighlightIndex_=static_cast<size_t>(-1); libraryReturnHighlightStart_=0; libraryReturnHighlightRect_=RECT{};

        externalMediaSession_=true;
        externalMediaPaths_.clear();
        externalMediaPaths_.reserve(mediaPaths.size());
        folder_=mediaPaths.front().parent_path().lexically_normal().wstring();
        currentFolder_=folder_;
        detailsOriginFolder_=folder_;
        libraryAccessFailCount_=0; libraryUnavailableLatched_=false; libraryAccessRetryNeedsRescan_=false;

        auto addItem=[this](const fs::path& path){
            const bool video=IsVideoExtension(path.extension().wstring());
            MediaItem item;
            item.path=path.lexically_normal().wstring();
            item.parentFolderKey=ToLower(path.parent_path().lexically_normal().wstring());
            item.title=path.stem().wstring();
            item.isVideo=video;
            if(video) item.vr=DetectVR(item.path);
            else item.title=StripLeadingImageResolutionPrefix(std::move(item.title));
            item.cachePath=BuildCachePath(item.path);
            item.uiCachePath=BuildUiCachePath(item.path);
            item.favorite=ReadFavoriteMetadata(item.path);
            item.searchText=ToLower(item.title+L"\n"+item.path);
            if(video){
                UINT w=0,h=0;
                if(ReadResolutionMetadata(item.uiCachePath,w,h)){
                    item.sourceWidth=w; item.sourceHeight=h; item.resolutionProbeAttempted=true;
                }
                videos_.push_back(std::move(item));
            }else images_.push_back(std::move(item));
        };
        for(const auto& path:mediaPaths){ externalMediaPaths_.push_back(path.wstring()); addItem(path); }

        // Keep the mini-library deterministic and consistent with a normal scanned library.
        auto mediaSorter=[](const MediaItem& a,const MediaItem& b){
            const MediaNameSortKey ka=BuildMediaNameSortKey(a.title);
            const MediaNameSortKey kb=BuildMediaNameSortKey(b.title);
            if(ka.primary!=kb.primary) return ka.primary<kb.primary;
            if(ka.group!=kb.group) return ka.group<kb.group;
            if(ka.secondary!=kb.secondary) return ka.secondary<kb.secondary;
            if(ka.hasNumber!=kb.hasNumber) return !ka.hasNumber;
            if(ka.hasNumber){ const int cmp=CompareSortNumbers(ka.number,kb.number); if(cmp!=0) return cmp<0; }
            if(ka.fallback!=kb.fallback) return ka.fallback<kb.fallback;
            return ToLower(a.path)<ToLower(b.path);
        };
        std::sort(videos_.begin(),videos_.end(),mediaSorter);
        std::sort(images_.begin(),images_.end(),mediaSorter);
        RebuildLibraryFolderIndexCaches();

        category_=firstIsVideo?Category::Videos:Category::Images;
        auto& active=CurrentItems();
        const std::wstring targetKey=ToLower(fs::path(firstPath).lexically_normal().wstring());
        selected_=0;
        for(size_t i=0;i<active.size();++i){
            if(ToLower(fs::path(active[i].path).lexically_normal().wstring())==targetKey){ selected_=i; break; }
        }

        mode_=Mode::Details;
        detailsScrollY_=0;
        filterDirty_=true;
        if(category_==Category::Videos){
            // Run the exact same Info initialization as an in-app selection before playback.
            // This immediately restores existing timeline files into RAM. If no timeline
            // exists yet, the external-session return path below generates it after playback.
            StartPreviewWorkerForSelected();
            QueueDetailPrefetchWindow();
            InvalidateRect(hwnd_,nullptr,TRUE);
            EnterPlayerAt(0.0);
        }else{
            ResetImageZoom();
            ClearLoadingState();
            InvalidateRect(hwnd_,nullptr,TRUE);
        }
        if(hwnd_){ ShowWindow(hwnd_,SW_RESTORE); SetForegroundWindow(hwnd_); }
        return true;
    }

    void QueueExternalMediaOpen(const std::vector<std::wstring>& paths) {
        if(paths.empty()) return;
        for(const auto& path:paths){
            if(path.empty()) continue;
            const std::wstring key=ToLower(fs::path(path).lexically_normal().wstring());
            bool exists=false;
            for(const auto& pending:pendingExternalMediaPaths_){
                if(ToLower(fs::path(pending).lexically_normal().wstring())==key){ exists=true; break; }
            }
            if(!exists) pendingExternalMediaPaths_.push_back(path);
        }
        if(pendingExternalMediaPaths_.empty()) return;
        // Explorer may start one process per selected file.  A short debounce merges those
        // handoffs into one mini-library while a later, ordinary single-file open replaces it.
        KillTimer(hwnd_,kExternalOpenBatchTimerId);
        SetTimer(hwnd_,kExternalOpenBatchTimerId,250,nullptr);
        ShowWindow(hwnd_,SW_RESTORE);
        SetForegroundWindow(hwnd_);
    }

    void ProcessPendingExternalMediaOpen() {
        KillTimer(hwnd_,kExternalOpenBatchTimerId);
        if(pendingExternalMediaPaths_.empty()) return;
        std::vector<std::wstring> batch;
        batch.swap(pendingExternalMediaPaths_);
        OpenExternalMediaBatch(batch);
    }

    ~App() {
        instancePipeServer_.Stop();
        DrainExternalOpenMessages();
        StopLibraryScanWorker();
        DestroyPlayerFooterTransition();
        StopResolutionMetadataWorker();
        StopLibraryThumbLoader();
        StopLibraryHoverPreviewWorker();
        StopPreviewWorker();
        StopPreviewBitmapDecodeWorker();
        StopDetailPrefetchWorker();
        StopFullLoadWorker();
        StopThumbnailWorker();
        ClearAllDetailInfoMemory();
        ClearThumbs(videos_);
        ClearThumbs(images_);
        for(auto& kv:fontCache_) if(kv.second) DeleteObject(kv.second);
        fontCache_.clear();
        DestroyControlsBackBuffer();
        DestroyBackBuffer();
        player_.reset();
        if (gdiplusToken_) Gdiplus::GdiplusShutdown(gdiplusToken_);
        if (mfStarted_) MFShutdown();
        if (comInitialized_) { ReleaseThreadWicFactory(); CoUninitialize(); }
    }

private:
    struct ThumbJob {
        std::wstring source;
        std::wstring output;
        std::wstring uiOutput;
        bool isVideo = true;
        VRInfo vr{};
    };

    struct LibraryThumbLoadJob {
        Category category = Category::Videos;
        size_t index = 0;
        std::wstring itemPath;
        std::wstring cachePath;
        bool isVideo = true;
        bool isVr = false;
        bool loadBitmap = true;
        bool allowGenerate = false; // only visible cards may touch the source to build a missing cache
        VRInfo vr{};
        int width = 640;
        int height = 360;
        uint64_t epoch = 0;
    };

    struct LibraryThumbLoadResult {
        Category category = Category::Videos;
        size_t index = 0;
        std::wstring itemPath;
        std::wstring cachePath;
        HBITMAP bitmap = nullptr;
        int width = 0;
        int height = 0;
        bool fromPrivateCache = false;
        bool privateDecodeFailed = false;
        bool bitmapRequest = false;
        uint64_t epoch = 0;
    };

    struct ResolutionMetadataJob {
        std::wstring itemPath;
        std::wstring uiCachePath;
        bool highPriority = false;
        uint64_t generation = 0;
    };

    struct ResolutionMetadataResult {
        std::wstring itemPath;
        std::wstring uiCachePath;
        UINT sourceWidth = 0;
        UINT sourceHeight = 0;
        bool attempted = false;
        uint64_t generation = 0;
    };

    struct FullLoadJob {
        std::wstring source;
        std::wstring cachePath;
        std::wstring uiCachePath;
        std::wstring previewDir;
        bool isVideo = true;
        VRInfo vr{};
    };

    struct FolderViewState {
        int scrollY = 0;
        std::wstring selectedPath;
    };

    struct LibraryScanCompletion {
        uint64_t generation = 0;
        bool cleanupOrphanCache = false;
        vmp::LibraryScanResult result;
    };

    struct AnimatedMediaHit {
        RECT hit{};
        RECT visual{};
        size_t id = static_cast<size_t>(-1);
    };

    static constexpr UINT WM_APP_THUMB_READY = WM_APP + 10;
    static constexpr UINT WM_APP_PREVIEW_READY = WM_APP + 11;
    static constexpr UINT WM_APP_CACHE_REPAIR = WM_APP + 12;
    static constexpr UINT WM_APP_LIBRARY_THUMB_LOADED = WM_APP + 13;
    static constexpr UINT WM_APP_FULL_LOAD_PROGRESS = WM_APP + 14;
    static constexpr UINT WM_APP_FULL_LOAD_DONE = WM_APP + 15;
    static constexpr UINT WM_APP_DETAIL_PREFETCH_READY = WM_APP + 16;
    static constexpr UINT WM_APP_RESOLUTION_METADATA_READY = WM_APP + 17;
    static constexpr UINT WM_APP_LIBRARY_HOVER_PREVIEW_READY = WM_APP + 18;
    static constexpr UINT WM_APP_PREVIEW_BITMAP_READY = WM_APP + 19;
    static constexpr UINT WM_APP_LIBRARY_SCAN_DONE = WM_APP + 20;
    static constexpr UINT WM_APP_EXTERNAL_OPEN = WM_APP + 21;
    static constexpr UINT_PTR kSlideshowTimerId = 41;
    static constexpr UINT_PTR kUiAnimationTimerId = 42;
    static constexpr UINT_PTR kResumeDetailsWorkersTimerId = 43;
    static constexpr UINT_PTR kLibraryAccessRetryTimerId = 44;
    static constexpr UINT_PTR kAppNoticeTimerId = 45;
    static constexpr UINT_PTR kLiveWindowMoveTimerId = 46;
    static constexpr UINT_PTR kExternalOpenBatchTimerId = 47;
    static constexpr UINT_PTR kPreviewZoomSettleTimerId = 48;
    static constexpr UINT_PTR kPreviewScrollSettleTimerId = 49;
    static constexpr UINT_PTR kHoverPreviewAudioDelayTimerId = 50;
    static constexpr UINT_PTR kLibraryScrollSettleTimerId = 51;
    static constexpr UINT_PTR kLibraryPrefetchPulseTimerId = 52;
    static constexpr UINT_PTR kLibraryThumbApplyTimerId = 53;
    static constexpr UINT kHoverPreviewAudioDelayMs = 500;
    static constexpr ULONGLONG kUiAnimationDurationMs = 160;
    static constexpr ULONGLONG kMediaHoverFadeInMs = 110;
    static constexpr ULONGLONG kLibraryHoverPreviewDelayMs = 20;
    static constexpr ULONGLONG kLibraryHoverPreviewFrameDurationMs = 110;
    static constexpr ULONGLONG kLibraryReturnHighlightDurationMs = 3000;
    static constexpr ULONGLONG kAppNoticePulseDurationMs = 5000;
    static constexpr ULONGLONG kPlayerFooterTransitionDurationMs = 240;
    static constexpr ULONGLONG kPlayerControlsFadeDurationMs = kPlayerFooterTransitionDurationMs;
    static constexpr ULONGLONG kFullLoadDonePopupDurationMs = 3000;
    static constexpr ULONGLONG kFullLoadFailedPopupDurationMs = 8000;
    static constexpr UINT kPreviewZoomSettleMs = 180;
    static constexpr UINT kPreviewScrollSettleMs = 140;
    static constexpr UINT kLibraryScrollSettleMs = 125;
    static constexpr UINT kLibraryPrefetchPulseMs = 24;
    static constexpr UINT kLibraryActivePrefetchPulseMs = 40;
    static constexpr UINT kLibraryThumbApplyDelayMs = 32;
    static constexpr UINT kLibraryActiveThumbApplyDelayMs = 48;
    static constexpr size_t kLibraryHotApplyBatchMax = 10u;
    static constexpr ULONGLONG kLibraryInteractionQuietMs = 70;
    static constexpr ULONGLONG kLibraryIdleTrimDelayMs = 1250;
    static constexpr ULONGLONG kLibraryIdleTrimRepeatMs = 2500;
    static constexpr BYTE kControlsVisibleAlpha = 218;
    static constexpr int kDefaultPreviewCardWidth = 220;

    static bool PathIsWithin(const std::wstring& childRaw, const std::wstring& rootRaw) {
        if (childRaw.empty() || rootRaw.empty()) return false;
        std::wstring child = ToLower(fs::path(childRaw).lexically_normal().wstring());
        std::wstring root = ToLower(fs::path(rootRaw).lexically_normal().wstring());
        while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
        if (child == root) return true;
        if (!root.empty() && root.back() != L'\\' && root.back() != L'/') root.push_back(L'\\');
        return child.size() >= root.size() && child.compare(0, root.size(), root) == 0;
    }

#if !defined(VMP_PORTABLE_BUILD)
    static void SetRegistryString(HKEY root, const std::wstring& subKey, const wchar_t* valueName, const std::wstring& value) {
        HKEY key{};
        if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
        RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
        RegCloseKey(key);
    }

    static void SetRegistryEmptyString(HKEY root, const std::wstring& subKey, const std::wstring& valueName) {
        HKEY key{};
        if (RegCreateKeyExW(root, subKey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
        const wchar_t empty[] = L"";
        RegSetValueExW(key, valueName.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(empty), sizeof(empty));
        RegCloseKey(key);
    }

    void RegisterOpenWith() const {
        wchar_t exeBuf[32768]{};
        const DWORD len = GetModuleFileNameW(nullptr, exeBuf, static_cast<DWORD>(std::size(exeBuf)));
        if (!len || len >= std::size(exeBuf)) return;
        const std::wstring exePath(exeBuf, len);
        const std::wstring appKey = L"Software\\Classes\\Applications\\VisualMediaPlayer.exe";
        SetRegistryString(HKEY_CURRENT_USER, appKey, L"FriendlyAppName", L"Visual MediaPlayer");
        SetRegistryString(HKEY_CURRENT_USER, appKey + L"\\shell\\open\\command", nullptr, L"\"" + exePath + L"\" \"%1\"");

        static const wchar_t* kSupported[] = {
            L".mp4", L".m4v", L".mkv", L".mk3d", L".webm", L".avi", L".divx", L".mov", L".qt", L".wmv", L".asf", L".mpg", L".mpeg", L".mpe", L".mpv", L".mpv2", L".m1v", L".m2v", L".m2p", L".ts", L".m2t", L".mts", L".m2ts", L".tp", L".trp", L".vob", L".vro", L".ogv", L".ogm", L".flv", L".f4v", L".f4p", L".3gp", L".3g2", L".3gp2", L".3gpp", L".rm", L".rmvb", L".rv", L".mxf", L".gxf", L".dv", L".dif", L".dvr-ms", L".wtv", L".mod", L".tod", L".amv", L".ivf", L".y4m", L".nut", L".nsv", L".roq", L".smk", L".bik", L".bk2", L".mjpeg", L".mjpg", L".mjp", L".h264", L".264", L".avc", L".h265", L".265", L".hevc", L".vp8", L".vp9", L".av1", L".r3d", L".braw", L".ari", L".cine", L".crm", L".insv", L".lrv", L".360", L".evo", L".mj2",
            L".jpg", L".jpeg", L".jpe", L".jfif", L".jif", L".jfi", L".png", L".apng", L".bmp", L".dib", L".gif", L".tif", L".tiff", L".webp", L".heic", L".heif", L".hif", L".avif", L".avifs", L".jxl", L".jp2", L".j2k", L".j2c", L".jpf", L".jpx", L".jpm", L".jxr", L".wdp", L".hdp", L".tga", L".targa", L".icb", L".vda", L".vst", L".dds", L".pcx", L".ico", L".cur", L".mng", L".psd", L".psb", L".exr", L".hdr", L".rgbe", L".pic", L".pfm", L".pnm", L".ppm", L".pgm", L".pbm", L".pam", L".qoi", L".sgi", L".rgb", L".rgba", L".bw", L".ras", L".sun", L".xbm", L".xpm", L".svg", L".svgz", L".dng", L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".srf", L".sr2", L".raf", L".orf", L".rw2", L".rwl", L".pef", L".x3f", L".3fr", L".fff", L".iiq", L".erf", L".mef", L".mos", L".mrw", L".kdc", L".dcr", L".raw", L".srw", L".bay", L".cap", L".eip", L".mdc", L".rwz"
        };
        const std::wstring supportedKey = appKey + L"\\SupportedTypes";
        for (const wchar_t* ext : kSupported) SetRegistryEmptyString(HKEY_CURRENT_USER, supportedKey, ext);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
#endif

    static LRESULT CALLBACK MainWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            app = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hwnd_ = h;
        }
        return app ? app->HandleMain(m,w,l) : DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK VideoWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            app = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(h,m,w,l);
        switch (m) {
        case WM_MOUSEACTIVATE:
            if(!app->IsAppForegroundForHover()){
                // Player interaction is immediate even when VMP is in the background:
                // activate the owner and let this same click reach the video surface.
                if(IsIconic(app->hwnd_)) ShowWindow(app->hwnd_,SW_RESTORE);
                SetForegroundWindow(app->hwnd_);
                BringWindowToTop(app->hwnd_);
            }
            return MA_ACTIVATE;
        case WM_LBUTTONDOWN:
            if(app->ConsumeForegroundActivationClickMessage(WM_LBUTTONDOWN)) return 0;
            if (!(app->player_ && !app->player_->VR().vr && app->player_->FlatZoomActive())) app->PlayerActivity(true);
            if (app->player_) app->player_->BeginDrag(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_MOUSEMOVE:
            app->PlayerActivity(false);
            if (app->player_) app->player_->Drag(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_LBUTTONUP:
            if(app->ConsumeForegroundActivationClickMessage(WM_LBUTTONUP)) return 0;
            if (app->player_) app->player_->EndDrag();
            return 0;
        case WM_CAPTURECHANGED:
            if (app->player_) app->player_->CancelDrag();
            return 0;
        case WM_MOUSEWHEEL:
            app->HandlePlayerWheel(w,l);
            return 0;
        case WM_KEYDOWN: SendMessageW(app->hwnd_, WM_KEYDOWN, w, l); return 0;
        case WM_ERASEBKGND:
            // Never expose a black USER32 erase frame while the swap chain is resizing.
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{}; BeginPaint(h,&ps);
            if(app->player_) app->player_->Render();
            EndPaint(h,&ps);
            return 0;
        }
        case WM_SIZE:
            if (app->player_) {
                // Resize the swap chain to the live child size and immediately repaint
                // from the retained decoded texture. The source video texture is not
                // destroyed by ResizeBuffers, so the last frame stays available.
                app->player_->Resize();
                app->player_->Render();
            }
            return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    static void ActivateMainFromOverlay(App* app) {
        if(!app || !app->hwnd_) return;
        // The layered player controls intentionally use WS_EX_NOACTIVATE so simply
        // clicking the popup does not activate it.  A real user click should still
        // bring the owning VMP window to the foreground before the control acts.
        if(IsIconic(app->hwnd_)) ShowWindow(app->hwnd_,SW_RESTORE);
        SetForegroundWindow(app->hwnd_);
        BringWindowToTop(app->hwnd_);
    }

    static LRESULT CALLBACK ControlsWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            app = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(h,m,w,l);
        switch (m) {
        case WM_MOUSEACTIVATE:
            if(!app->IsAppForegroundForHover()){
                // The popup itself remains non-activating, but the owner is brought
                // forward and the original click is preserved so the control acts now.
                ActivateMainFromOverlay(app);
            }
            return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: app->PaintControlsWindow(); return 0;
        case WM_MOUSEMOVE:
            {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                app->UpdateAnimatedHover(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
            }
            app->PlayerActivity(false);
            app->UpdateSeekHover(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            if (app->seekDragging_ || app->volumeDragging_) app->PlayerMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_MOUSELEAVE:
            app->ClearAnimatedHover(h);
            app->ClearSeekHover();
            return 0;
        case WM_LBUTTONDOWN:
            if(app->ConsumeForegroundActivationClickMessage(WM_LBUTTONDOWN)) return 0;
            ActivateMainFromOverlay(app);
            app->PlayerActivity(true);
            app->PlayerMouseDown(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_LBUTTONUP:
            if(app->ConsumeForegroundActivationClickMessage(WM_LBUTTONUP)) return 0;
            app->PlayerMouseUp(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            return 0;
        case WM_CAPTURECHANGED:
            app->CancelPlayerSliderDrag();
            return 0;
        case WM_MOUSEWHEEL:
            app->HandlePlayerWheel(w,l);
            return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK EdgeArrowWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        App* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            app = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (!app) return DefWindowProcW(h,m,w,l);
        switch (m) {
        case WM_MOUSEACTIVATE:
            if(!app->IsAppForegroundForHover()){
                // The popup itself remains non-activating, but the owner is brought
                // forward and the original click is preserved so the control acts now.
                ActivateMainFromOverlay(app);
            }
            return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: app->PaintEdgeArrowWindow(h); return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            app->UpdateAnimatedHover(h, GET_X_LPARAM(l), GET_Y_LPARAM(l));
            app->PlayerActivity(false);
            return 0;
        }
        case WM_MOUSELEAVE:
            app->ClearAnimatedHover(h);
            return 0;
        case WM_LBUTTONDOWN:
            if(app->ConsumeForegroundActivationClickMessage(WM_LBUTTONDOWN)) return 0;
            ActivateMainFromOverlay(app);
            app->PlayerActivity(true);
            return 0;
        case WM_LBUTTONUP:
            if(app->ConsumeForegroundActivationClickMessage(WM_LBUTTONUP)) return 0;
            ActivateMainFromOverlay(app);
            app->PlayerActivity(true);
            app->NavigatePlayerMedia(h == app->playerPrevHwnd_ ? -1 : 1);
            return 0;
        case WM_MOUSEWHEEL:
            app->HandlePlayerWheel(w,l);
            return 0;
        }
        return DefWindowProcW(h,m,w,l);
    }

    LRESULT HandleMain(UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_MOUSEACTIVATE:
            if(!IsAppForegroundForHover()){
                const int hitTest=static_cast<int>(LOWORD(l));
                // Native caption controls must always work on the first click, even when
                // VMP is in the background. The two-step activation rule applies only to
                // content surfaces such as Library cards and the video Timeline/Info view.
                if(hitTest==HTCLOSE || hitTest==HTMAXBUTTON || hitTest==HTMINBUTTON)
                    return MA_ACTIVATE;
                const UINT mouseMessage=HIWORD(l);
                if(ShouldEatForegroundActivationClick() &&
                   (mouseMessage==WM_LBUTTONDOWN || mouseMessage==WM_LBUTTONDBLCLK)){
                    // Library cards and the video Info/Timeline deliberately keep the
                    // activation-only first click. Image Details and the Player do not.
                    foregroundActivationClickPending_=true;
                    return MA_ACTIVATEANDEAT;
                }
            }
            return MA_ACTIVATE;
        case WM_CREATE: return 0;
        case WM_APP_EXTERNAL_OPEN: {
            std::unique_ptr<std::vector<std::wstring>> paths(
                reinterpret_cast<std::vector<std::wstring>*>(l));
            if(hwnd_){ ShowWindow(hwnd_,SW_RESTORE); SetForegroundWindow(hwnd_); }
            if(paths && !paths->empty()) QueueExternalMediaOpen(*paths);
            return 0;
        }
        case WM_APP_LIBRARY_SCAN_DONE: {
            std::unique_ptr<LibraryScanCompletion> completion(
                reinterpret_cast<LibraryScanCompletion*>(l));
            if(completion) ApplyLibraryScanCompletion(std::move(completion));
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_GETMINMAXINFO: {
            auto* mmi=reinterpret_cast<MINMAXINFO*>(l);
            if(mmi){
                SIZE minimum{};
                if(GetNativeMinimumWindowSize(minimum)){
                    mmi->ptMinTrackSize.x=std::max<LONG>(mmi->ptMinTrackSize.x,minimum.cx);
                    mmi->ptMinTrackSize.y=std::max<LONG>(mmi->ptMinTrackSize.y,minimum.cy);
                    // Native media can be larger than the monitor. Do not let the default
                    // system max-track metrics contradict the strict native minimum.
                    mmi->ptMaxTrackSize.x=std::max<LONG>(mmi->ptMaxTrackSize.x,mmi->ptMinTrackSize.x);
                    mmi->ptMaxTrackSize.y=std::max<LONG>(mmi->ptMaxTrackSize.y,mmi->ptMinTrackSize.y);
                }
            }
            return 0;
        }
        case WM_ACTIVATEAPP:
            if(!w){
                foregroundActivationClickPending_=false;
                ClearMediaHoverImmediate();
                CancelLibraryHoverPreviewRequest();
                ResetAnimatedHoverImmediate(hwnd_);
                ResetAnimatedHoverImmediate(controlsHwnd_);
                ResetAnimatedHoverImmediate(playerPrevHwnd_);
                ResetAnimatedHoverImmediate(playerNextHwnd_);
                if(hwnd_) InvalidateRect(hwnd_,nullptr,FALSE);
                if(controlsHwnd_) InvalidateRect(controlsHwnd_,nullptr,FALSE);
                if(playerPrevHwnd_) InvalidateRect(playerPrevHwnd_,nullptr,FALSE);
                if(playerNextHwnd_) InvalidateRect(playerNextHwnd_,nullptr,FALSE);
            }else if(mode_==Mode::Library){
                MarkLibraryBrowsingActivity();
                PrepareLibraryViewportFromPrivateCache();
                InvalidateRect(hwnd_,nullptr,FALSE);
            }
            return 0;
        case WM_ENTERSIZEMOVE:
            liveWindowMove_=true;
            if(mode_==Mode::Player){
                // A captured Info-footer snapshot has fixed pixel dimensions. If the user
                // begins resizing during its short opening cross-fade, discard the snapshot
                // rather than letting that old-size popup hang outside the new player bounds.
                DestroyPlayerFooterTransition();
                PlayerActivity(true);
            }
            SetTimer(hwnd_,kLiveWindowMoveTimerId,16,nullptr);
            return 0;
        case WM_EXITSIZEMOVE:
            liveWindowMove_=false;
            KillTimer(hwnd_,kLiveWindowMoveTimerId);
            if(mode_==Mode::Player && player_){
                // Commit the exact final child/overlay geometry and swap-chain size.
                // Resize() is same-size guarded, so this is also safe when the timer
                // already reached the final dimensions just before mouse-up.
                Layout();
                player_->Resize();
                player_->Render();
                UpdateSeekUi();
            }else if(mode_==Mode::Details && category_==Category::Images){
                InvalidateRect(hwnd_,nullptr,FALSE);
                UpdateWindow(hwnd_);
            }
            return 0;
        case WM_MOVE:
            if(mode_==Mode::Player){
                if(liveWindowMove_) RepositionPlayerOverlayWindows();
                else Layout();
                if(liveWindowMove_ && player_) player_->Render();
            }else if(liveWindowMove_ && mode_==Mode::Details && category_==Category::Images){
                InvalidateRect(hwnd_,nullptr,FALSE);
                UpdateWindow(hwnd_);
            }
            return 0;
        case WM_SIZE:
            ClearMediaHoverImmediate();
            if(w==SIZE_MINIMIZED){
                if(controlsHwnd_) ShowWindow(controlsHwnd_,SW_HIDE);
                if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_HIDE);
                if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_HIDE);
            }
            if (w != SIZE_MINIMIZED) {
                RECT zoomRc{}; GetClientRect(hwnd_, &zoomRc);
                const int clientW = std::max(1, static_cast<int>(zoomRc.right - zoomRc.left));
                if (mode_ == Mode::Library) {
                    // WM_SIZE arrives after the client rectangle has changed, so use the
                    // last painted/scrollbar range to determine whether the user was at
                    // the old bottom. Recompute against the new grid geometry immediately;
                    // carrying an out-of-range raw scroll offset is what could blank the grid.
                    const int oldMaxScroll = std::max(0, libraryLastKnownMaxScroll_);
                    const int oldScroll = std::clamp(scrollY_, 0, oldMaxScroll);
                    const bool wasAtBottom = oldMaxScroll > 0 && oldScroll >= oldMaxScroll - 2;
                    const double scrollFraction = oldMaxScroll > 0
                        ? static_cast<double>(oldScroll) / static_cast<double>(oldMaxScroll) : 0.0;
                    ApplyLibraryWidthForViewport(clientW);
                    const int newMaxScroll = LibraryMaxScroll(zoomRc);
                    scrollY_ = wasAtBottom ? newMaxScroll
                        : std::clamp(static_cast<int>(std::lround(scrollFraction * static_cast<double>(newMaxScroll))), 0, newMaxScroll);
                    UpdateLibraryScrollbarRects(zoomRc);
                } else if (mode_ == Mode::Details && category_ == Category::Videos) {
                    if (previewZoomOverridden_) {
                        const int across = PreviewAcrossForWidth(clientW, previewCardWidth_);
                        previewCardWidth_ = PreviewWidthForAcross(clientW, across);
                    }
                }
            }
            if(mode_==Mode::Player && liveWindowMove_){
                // Keep geometry and the D3D backbuffer matched throughout the drag.
                // VideoWndProc handles the child WM_SIZE synchronously, recreates only
                // the swap-chain RTV, and immediately presents the retained video frame.
                Layout();
                if(player_) player_->Render();
                InvalidateRect(hwnd_,nullptr,FALSE);
                return 0;
            }
            Layout(); InvalidateRect(hwnd_, nullptr, mode_==Mode::Library ? FALSE : TRUE); return 0;
        case WM_PAINT: Paint(); return 0;
        case WM_DPICHANGED: {
            ClearMediaHoverImmediate();
            RECT* suggested = reinterpret_cast<RECT*>(l);
            if (suggested) SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right-suggested->left, suggested->bottom-suggested->top, SWP_NOZORDER|SWP_NOACTIVATE);
            Layout(); InvalidateRect(hwnd_, nullptr, TRUE); return 0;
        }
        case WM_MOUSEMOVE:
            // Scrollbar capture is handled before hover/audio/preview tracking. A drag is
            // navigation, not a hover gesture; nothing unrelated is allowed to start work
            // from these high-frequency mouse messages.
            if (mode_ == Mode::Library && libraryScrollDragging_) {
                RECT rc{}; GetClientRect(hwnd_, &rc);
                UpdateLibraryScrollbarRects(rc);
                const int maxScroll = libraryLastKnownMaxScroll_;
                const int trackH = std::max(1, static_cast<int>(libraryScrollTrackRect_.bottom - libraryScrollTrackRect_.top));
                const int thumbH = std::max(1, static_cast<int>(libraryScrollThumbRect_.bottom - libraryScrollThumbRect_.top));
                const int travel = std::max(1, trackH - thumbH);
                const int wantedTop = std::clamp(GET_Y_LPARAM(l) - libraryScrollDragOffset_, static_cast<int>(libraryScrollTrackRect_.top), static_cast<int>(libraryScrollTrackRect_.bottom) - thumbH);
                const double fraction = static_cast<double>(wantedTop - libraryScrollTrackRect_.top) / static_cast<double>(travel);
                const int oldScroll = scrollY_;
                scrollY_ = std::clamp(static_cast<int>(fraction * maxScroll + 0.5), 0, maxScroll);
                UpdateLibraryScrollbarRects(rc);
                if (scrollY_ != oldScroll) {
                    // Scroll input is render-only. Do not decode, validate files, rebuild queues,
                    // trim RAM, or plan the working set from a mouse-move message.
                    NoteLibraryScrollInput();
                    ScheduleLibraryPrefetchPulse();
                    ScheduleLibraryScrollSettle();
                    InvalidateLibraryScrollWithFooter(oldScroll);
                }
                return 0;
            }
            {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
                TrackMouseEvent(&tme);
                UpdateAnimatedHover(hwnd_, GET_X_LPARAM(l), GET_Y_LPARAM(l));
                UpdateMediaHover(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            }
            if (mode_ == Mode::Details && category_ == Category::Images && imageZoomDragging_) {
                PanImageZoomByDelta(GET_X_LPARAM(l)-imageZoomLastPoint_.x, GET_Y_LPARAM(l)-imageZoomLastPoint_.y);
                imageZoomLastPoint_={GET_X_LPARAM(l),GET_Y_LPARAM(l)};
                return 0;
            }
            if (mode_ == Mode::Player) PlayerActivity(false);
            break;
        case WM_MOUSELEAVE:
            ClearAnimatedHover(hwnd_);
            ClearMediaHoverImmediate();
            return 0;
        case WM_MOUSEWHEEL:
            if (mode_ == Mode::Library) {
                const int wheelSteps = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
                if ((GET_KEYSTATE_WPARAM(w) & MK_CONTROL) != 0) {
                    libraryScrollWheelPixelRemainder_=0.0;
                    // Ctrl + wheel resizes the Library cards for this Library session only.
                    // Do not disturb hover/highlight state when the requested size is already
                    // at a limit or the wheel delta is too small to produce a step.
                    if (wheelSteps != 0) {
                        RECT rc{}; GetClientRect(hwnd_, &rc);
                        const int clientW = static_cast<int>(rc.right - rc.left);
                        const int defaultWidth = DefaultLibraryCardWidthForViewport(clientW);

                        // First Ctrl+wheel starts from the actual automatic layout:
                        // five-across windowed or six-across fullscreen. Zoom then snaps
                        // only to the exact allowed cards-across states.
                        if (!libraryZoomOverridden_) {
                            libraryCardWidth_ = defaultWidth;
                            libraryZoomOverridden_ = true;
                        }

                        const int oldWidth = libraryCardWidth_;
                        const int oldAcross = LibraryAcrossForWidth(clientW, oldWidth);
                        const int maxAcross = LibraryMaxAcrossForCurrentMode();
                        const int newAcross = std::clamp(oldAcross - wheelSteps, LibraryMinAcrossForCurrentMode(), maxAcross);
                        const int newWidth = LibraryWidthForAcross(clientW, newAcross);
                        if (newWidth != oldWidth) {
                            const int oldMaxScroll = LibraryMaxScroll(rc);
                            const int oldScroll = std::clamp(scrollY_, 0, oldMaxScroll);
                            const bool wasAtBottom = oldMaxScroll > 0 && oldScroll >= oldMaxScroll - 2;
                            const double scrollFraction = oldMaxScroll > 0
                                ? static_cast<double>(oldScroll) / static_cast<double>(oldMaxScroll) : 0.0;
                            libraryCardWidth_ = newWidth;
                            const int newMaxScroll = LibraryMaxScroll(rc);
                            scrollY_ = wasAtBottom ? newMaxScroll
                                : std::clamp(static_cast<int>(std::lround(scrollFraction * static_cast<double>(newMaxScroll))), 0, newMaxScroll);
                            UpdateLibraryScrollbarRects(rc);
                            NoteLibraryScrollInput();
                            ScheduleLibraryPrefetchPulse(1);
                            ScheduleLibraryScrollSettle();
                            InvalidateRect(hwnd_,nullptr,FALSE);
                        }
                    }
                } else {
                    const short wheelDelta=GET_WHEEL_DELTA_WPARAM(w);
                    if(wheelDelta!=0){
                        const int oldScroll=scrollY_;
                        const int scrollPixels=ConsumeWheelPixels(wheelDelta,LibraryWheelPixelsPerNotch(),libraryScrollWheelPixelRemainder_);
                        if(scrollPixels!=0){
                            scrollY_-=scrollPixels;
                            ClampScroll();
                            if(scrollY_!=oldScroll){
                                NoteLibraryScrollInput();
                                ScheduleLibraryPrefetchPulse();
                                ScheduleLibraryScrollSettle();
                                InvalidateLibraryScrollWithFooter(oldScroll);
                            }else{
                                // Do not carry movement that was consumed against a hard edge
                                // into the next gesture after the user reverses direction.
                                libraryScrollWheelPixelRemainder_=0.0;
                            }
                        }
                    }
                }
            } else if (mode_ == Mode::Details) {
                // Secondary-preview zoom is Ctrl + wheel only. Accumulate partial
                // high-resolution wheel/trackpad deltas instead of silently discarding them.
                const bool ctrlDown = ((GET_KEYSTATE_WPARAM(w) & MK_CONTROL) != 0) || ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
                const int wheelDelta = GET_WHEEL_DELTA_WPARAM(w);

                if (category_ == Category::Images && !nativeImageSizing_) {
                    // Match flat-video behavior: plain wheel zooms the image around the
                    // current mouse position. Native Size keeps free zoom disabled.
                    POINT zoomPoint{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
                    ScreenToClient(hwnd_,&zoomPoint);
                    if(PtInRect(&detailsMediaRect_,zoomPoint)){
                        const float oldScale=imageZoomScale_;
                        const float oldU=imageZoomCenterU_;
                        const float oldV=imageZoomCenterV_;
                        ZoomImageAtPoint(static_cast<short>(wheelDelta),zoomPoint);
                        if(imageZoomScale_!=oldScale || imageZoomCenterU_!=oldU || imageZoomCenterV_!=oldV)
                            DeferBackgroundWork();
                        return 0;
                    }
                }

                if (category_ == Category::Videos && ctrlDown) {
                    detailsScrollWheelPixelRemainder_=0.0;
                    previewWheelRemainder_ += wheelDelta;
                    const int wheelSteps = previewWheelRemainder_ / WHEEL_DELTA;
                    previewWheelRemainder_ -= wheelSteps * WHEEL_DELTA;
                    if (wheelSteps != 0) {
                        RECT rc{}; GetClientRect(hwnd_, &rc);
                        const int clientW = static_cast<int>(rc.right - rc.left);
                        // The first Ctrl+wheel gesture must start from the width that is
                        // actually on screen.  DetailsPreviewCardWidthForViewport() returns
                        // the automatic seven-across default while zoom is not overridden,
                        // in both windowed and fullscreen modes. Capture it before enabling
                        // the override so fullscreen never jumps to a stale smaller width.
                        if (!previewZoomOverridden_) {
                            previewCardWidth_ = DetailsPreviewCardWidthForViewport(clientW);
                            previewZoomOverridden_ = true;
                        }

                        const int oldWidth = previewCardWidth_;
                        const int oldAcross = PreviewAcrossForWidth(clientW, oldWidth);
                        const int maxAcross = PreviewMaxAcrossForCurrentMode();
                        const int minAcross = PreviewMinAcrossForCurrentMode();
                        const int newAcross = std::clamp(oldAcross - wheelSteps, minAcross, maxAcross);
                        const int newWidth = PreviewWidthForAcross(clientW, newAcross);
                        if (newWidth != oldWidth) {
                            BeginPreviewZoomGesture();
                            previewCardWidth_ = newWidth;
                            DeferBackgroundWork();
                            ClampDetailsScroll();
                            InvalidateRect(hwnd_, nullptr, FALSE);
                        }
                    }
                } else {
                    previewWheelRemainder_ = 0;
                    if(wheelDelta!=0){
                        RECT rc{}; GetClientRect(hwnd_,&rc);
                        const int oldScroll=detailsScrollY_;
                        const int scrollPixels=ConsumeWheelPixels(static_cast<short>(wheelDelta),DetailsWheelPixelsPerNotch(static_cast<int>(rc.right-rc.left)),detailsScrollWheelPixelRemainder_);
                        if(scrollPixels!=0){
                            if(category_==Category::Videos) BeginPreviewScrollGesture();
                            detailsScrollY_-=scrollPixels;
                            ClampDetailsScroll();
                            if(detailsScrollY_!=oldScroll){
                                DeferBackgroundWork();
                                InvalidateDetailsScrollOptimized(oldScroll);
                            }else{
                                detailsScrollWheelPixelRemainder_=0.0;
                            }
                        }
                    }
                }
            }
            return 0;
        case WM_LBUTTONDOWN:
            if(ConsumeForegroundActivationClickMessage(WM_LBUTTONDOWN)) return 0;
            if (mode_ == Mode::Details && category_ == Category::Images && !nativeImageSizing_ && imageZoomScale_ > 1.001f) {
                POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
                if(PtInRect(&detailsMediaRect_,p)){
                    imageZoomDragging_=true;
                    imageZoomLastPoint_=p;
                    SetCapture(hwnd_);
                    return 0;
                }
            }
            if (mode_ == Mode::Library) {
                DeferBackgroundWork(80);
                RECT rc{}; GetClientRect(hwnd_, &rc);
                UpdateLibraryScrollbarRects(rc);
                POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                if (!IsRectEmpty(&libraryScrollThumbRect_) && PtInRect(&libraryScrollThumbRect_, p)) {
                    ClearMediaHoverImmediate();
                    ClearAnimatedHover(hwnd_);
                    libraryScrollDragging_ = true;
                    libraryScrollDragOffset_ = p.y - libraryScrollThumbRect_.top;
                    SetCapture(hwnd_);
                    return 0;
                }
                if (!IsRectEmpty(&libraryScrollTrackRect_) && PtInRect(&libraryScrollTrackRect_, p)) {
                    const int maxScroll = LibraryMaxScroll(rc);
                    const int trackH = std::max(1, static_cast<int>(libraryScrollTrackRect_.bottom - libraryScrollTrackRect_.top));
                    const int thumbH = std::max(1, static_cast<int>(libraryScrollThumbRect_.bottom - libraryScrollThumbRect_.top));
                    const int travel = std::max(1, trackH - thumbH);
                    const int wantedTop = std::clamp(static_cast<int>(p.y) - thumbH / 2, static_cast<int>(libraryScrollTrackRect_.top), static_cast<int>(libraryScrollTrackRect_.bottom) - thumbH);
                    const double fraction = static_cast<double>(wantedTop - libraryScrollTrackRect_.top) / static_cast<double>(travel);
                    const int oldScroll=scrollY_;
                    scrollY_ = std::clamp(static_cast<int>(fraction * maxScroll + 0.5), 0, maxScroll);
                    UpdateLibraryScrollbarRects(rc);
                    if(scrollY_!=oldScroll){
                        NoteLibraryScrollInput();
                        ScheduleLibraryPrefetchPulse(1);
                        ScheduleLibraryScrollSettle();
                        InvalidateLibraryScrollWithFooter(oldScroll);
                    }
                    return 0;
                }
            }
            break;
        case WM_LBUTTONUP:
            if(ConsumeForegroundActivationClickMessage(WM_LBUTTONUP)) return 0;
            if(imageZoomDragging_){
                imageZoomDragging_=false;
                if(GetCapture()==hwnd_) ReleaseCapture();
                return 0;
            }
            if (libraryScrollDragging_) {
                libraryScrollDragging_ = false;
                if (GetCapture() == hwnd_) ReleaseCapture();
                // Releasing the thumb must also stay non-blocking. Queue a low-priority
                // catch-up pass; do not run cache planning or JPEG work inside WM_LBUTTONUP.
                if(mode_==Mode::Library){
                    NoteLibraryScrollInput();
                    ScheduleLibraryPrefetchPulse(1);
                    ScheduleLibraryScrollSettle();
                    ScheduleLibraryThumbApply();
                    InvalidateLibraryScrollableArea();
                }
                return 0;
            }
            if (mode_ != Mode::Player) { HandleClick(GET_X_LPARAM(l), GET_Y_LPARAM(l)); return 0; }
            break;
        case WM_CAPTURECHANGED:
            libraryScrollDragging_ = false;
            imageZoomDragging_ = false;
            break;
        case WM_APP_HOVER_AUDIO_EVENT: {
            const DWORD ev=static_cast<DWORD>(w);
            const uint64_t generation=static_cast<uint64_t>(l);
            if(generation==hoverPreviewAudioGeneration_ && hoverPreviewAudio_ &&
               hoverPreviewAudio_->Generation()==generation && HoverPreviewAudioContextStillValid()) {
                hoverPreviewAudio_->HandleMediaEvent(ev,libraryHoverPreviewCurrentMediaSeconds_,volumeFraction_);
            }
            return 0;
        }
        case WM_APP_MEDIA_EVENT: {
            const DWORD ev = static_cast<DWORD>(w);
            if (player_) player_->HandleMediaEvent(ev);
            if (ev == MF_MEDIA_ENGINE_EVENT_ENDED) HandlePlaybackEnded();
            return 0;
        }
        case WM_APP_MEDIA_ERROR: {
            if(mode_==Mode::Player && selected_<videos_.size()){
                const std::wstring failedPath=videos_[selected_].path;
                if(!PathExistsNoThrow(failedPath) || !IsLibraryRootAccessible()){
                    if(player_) player_->Pause();
                    NoteLibraryAccessFailure(true);
                    ArmLibraryAccessMonitor(3000);
                }else{
                    LeavePlayer();
                    ShowInAppNotice(L"This media is unsupported.",5000);
                }
            }
            return 0;
        }
        case WM_APP_PLAYER_READY:
            UpdateWindowTitle();
            if (player_ && player_->VR().vr) {
                // Native Size is a player-session preference. VR temporarily suspends
                // the flat/native presentation, but must not turn the preference off.
                SuspendNativeVideoSizingForVr();
                Layout();
                InvalidateControls();
            } else if (nativeVideoSizing_ && player_) {
                player_->SetNativePixelSizing(true);
                if (!fullscreen_) ApplyNativeVideoWindowSize();
                Layout();
                InvalidateControls();
            }
            return 0;
        case WM_APP_THUMB_READY:
            if (mode_ == Mode::Library) InvalidateLibraryScrollableArea();
            else InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_APP_LIBRARY_THUMB_LOADED:
            // Never let a burst of completed JPEG decodes interrupt a live scrollbar/wheel
            // gesture. Results remain owned by the loader queue and are installed once input
            // has been quiet briefly. This is the key separation between loading and scrolling.
            if(IsLibraryInteractionHot()){
                ScheduleLibraryThumbApply();
                return 0;
            }
            ApplyLibraryThumbLoadResults();
            return 0;
        case WM_APP_FULL_LOAD_PROGRESS:
            // w!=0 is the lightweight "starting this filename" notification used by the
            // progress popup. Completed jobs may have created a new VMP Library master,
            // so repaint the visible Library rows as well; otherwise a freshly generated
            // image banner can remain gray until the user scrolls or resizes the window.
            if(w==0 && mode_==Mode::Library) InvalidateLibraryScrollableArea();
            if (w==0 && mode_ == Mode::Details && category_ == Category::Videos &&
                selected_<videos_.size()) {
                RefreshPreviewFrames();
                ClampDetailsScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            InvalidateLoadingPopupArea();
            return 0;
        case WM_APP_FULL_LOAD_DONE:
            if(w!=0) {
                fullLoadFinishedAt_=GetTickCount64();
                StartUiAnimationTimer();
            } else {
                fullLoadFinishedAt_=0;
            }
            if(mode_==Mode::Library){
                // Give any cards that previously missed while the batch owned generation
                // an immediate chance to load the completed VMP cache from disk.
                for(auto& item:CurrentItems()) if(!item.thumb) item.thumbNextLoadAttempt=0;
                PrepareLibraryViewportFromPrivateCache();
                InvalidateLibraryScrollableArea();
            }
            if (mode_ == Mode::Details && category_ == Category::Videos) {
                if(selected_<videos_.size()) RefreshPreviewFrames();
                ClampDetailsScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            InvalidateLoadingPopupArea();
            return 0;
        case WM_APP_DETAIL_PREFETCH_READY:
            HandleDetailPrefetchResult(reinterpret_cast<DetailPrefetchResult*>(l));
            return 0;
        case WM_APP_RESOLUTION_METADATA_READY:
            HandleResolutionMetadataResult(reinterpret_cast<ResolutionMetadataResult*>(l));
            return 0;
        case WM_APP_LIBRARY_HOVER_PREVIEW_READY:
            HandleLibraryHoverPreviewResult(reinterpret_cast<LibraryHoverPreviewResult*>(l));
            return 0;
        case WM_APP_PREVIEW_BITMAP_READY:
            HandlePreviewBitmapDecodeResult(reinterpret_cast<PreviewBitmapDecodeResult*>(l));
            return 0;
        case WM_APP_PREVIEW_READY:
            if (mode_ == Mode::Details && category_ == Category::Videos) {
                if(selected_<videos_.size()) {
                    RefreshPreviewFrames();
                    if(PreviewCacheIsComplete()) QueueAllPreviewBitmapsForCurrentView();
                }
                ClampDetailsScroll();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_APP_SELECTED_WORK_DONE:
            if (mode_ == Mode::Details && category_ == Category::Videos) {
                ClearLoadingState();
                StartThumbnailWorker();
                QueueDetailPrefetchWindow();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_APP_CACHE_REPAIR:
            if (mode_ == Mode::Library) {
                // The visible-card loader will rebuild only the cache entry that is
                // actually needed. Never restart an all-library background walk.
                InvalidateLibraryScrollableArea();
            } else if (mode_ == Mode::Details && category_ == Category::Videos) {
                StartPreviewWorkerForSelected();
                QueueDetailPrefetchWindow();
            }
            return 0;
        case WM_CHAR:
            if (mode_ == Mode::Library && !(GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
                const wchar_t ch = static_cast<wchar_t>(w);
                if (ch == 8) {
                    if(loadFailureFilterActive_){
                        ClearLoadFailureFilter();
                        searchVisible_=false; searchSelectAll_=false; filteredIndices_.clear(); filterDirty_=true;
                        scrollY_=0; ClampScroll(); InvalidateRect(hwnd_,nullptr,FALSE);
                        return 0;
                    }
                    if (searchVisible_ && !searchQuery_.empty()) {
                        if (searchSelectAll_) searchQuery_.clear();
                        else searchQuery_.pop_back();
                        searchSelectAll_ = false;
                        filterDirty_ = true; scrollY_ = 0;
                        PrepareLibraryViewportFromPrivateCache(1);
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    return 0;
                }
                if (ch >= 32 && ch != 127) {
                    if(loadFailureFilterActive_){ ClearLoadFailureFilter(); filteredIndices_.clear(); filterDirty_=true; scrollY_=0; }
                    searchVisible_ = true;
                    if (searchSelectAll_) searchQuery_.clear();
                    searchSelectAll_ = false;
                    searchQuery_.push_back(ch); filterDirty_ = true; scrollY_ = 0;
                    // A filter changes which cards occupy the viewport. Retarget the loader
                    // immediately instead of waiting for an unrelated scroll/settle event;
                    // otherwise search results can sit gray even when their private cache
                    // files are already present on disk.
                    PrepareLibraryViewportFromPrivateCache(1);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return 0;
                }
            }
            break;
        case WM_TIMER:
            if (w == kSlideshowTimerId) {
                AdvanceImageSlideshow();
                return 0;
            }
            if (w == kUiAnimationTimerId) {
                TickUiAnimations();
                return 0;
            }
            if (w == kLibraryPrefetchPulseTimerId) {
                KillTimer(hwnd_,kLibraryPrefetchPulseTimerId);
                libraryPrefetchPulseArmed_=false;
                RunLibraryPrefetchPulse();
                return 0;
            }
            if (w == kLibraryThumbApplyTimerId) {
                KillTimer(hwnd_,kLibraryThumbApplyTimerId);
                libraryThumbApplyTimerArmed_=false;
                if(IsLibraryInteractionHot()) {
                    ApplyLibraryThumbLoadResults(kLibraryHotApplyBatchMax);
                    std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
                    if(!libraryThumbLoadResults_.empty()) ScheduleLibraryThumbApply(kLibraryActiveThumbApplyDelayMs);
                } else ApplyLibraryThumbLoadResults();
                return 0;
            }
            if (w == kLibraryScrollSettleTimerId) {
                KillTimer(hwnd_,kLibraryScrollSettleTimerId);
                // A settle timer may become due while the user is still dragging. Never
                // run the expensive working-set planner until the interaction is actually quiet.
                if(IsLibraryInteractionHot()) ScheduleLibraryScrollSettle();
                else SettleLibraryScrollWorkingSet();
                return 0;
            }
            if (w == kResumeDetailsWorkersTimerId) {
                KillTimer(hwnd_,kResumeDetailsWorkersTimerId);
                // Definitively release the video source after the reverse footer fade.
                // Do not immediately reopen the source just to continue background Info
                // generation; already-generated timeline/banner files are consumed lazily
                // and each cache file is closed as soon as it has been copied into RAM.
                if(player_) player_->CloseSource();
                if(mode_==Mode::Details && category_==Category::Videos){
                    if(selected_<videos_.size()){
                        RefreshPreviewFrames();
                        detailsDurationSeconds_.store(ReadCachedPreviewDuration(),std::memory_order_relaxed);
                        if(PreviewCacheIsComplete()) QueueAllPreviewBitmapsForCurrentView();
                        if(!PreviewCacheIsComplete()) {
                            // Entering Player cancels any in-progress selected-media generator.
                            // Once playback releases the source, resume any incomplete static
                            // Timeline, including VR stills. Animated VR previews remain disabled.
                            StartPreviewWorkerForSelected();
                        }
                    }else{
                        detailsDurationSeconds_.store(0.0,std::memory_order_relaxed);
                        // No valid selected item remains; restart the selected-media worker only to
                        // let its normal guard clear stale state after playback.
                        StartPreviewWorkerForSelected();
                    }
                    ClampDetailsScroll();
                    QueueDetailPrefetchWindow();
                    InvalidateRect(hwnd_,nullptr,FALSE);
                }
                return 0;
            }
            if (w == kLibraryAccessRetryTimerId) {
                CheckLibraryAccessHealth();
                return 0;
            }
            if (w == kExternalOpenBatchTimerId) {
                ProcessPendingExternalMediaOpen();
                return 0;
            }
            if (w == kLiveWindowMoveTimerId) {
                if(!liveWindowMove_){
                    KillTimer(hwnd_,kLiveWindowMoveTimerId);
                    return 0;
                }
                if(mode_==Mode::Player && player_){
                    // DefWindowProc runs a modal move/size loop while the title bar/border
                    // is being dragged, so the normal outer render loop cannot execute.
                    // WM_SIZE keeps the swap chain matched to the live child size; this
                    // timer keeps decoded frames presenting between individual size events.
                    player_->Render();
                    UpdateSeekUi();
                    UpdatePlayerControlVisibility();
                }else if(mode_==Mode::Details && category_==Category::Images){
                    // Keep slideshow/fade/image UI painting live during the same modal loop.
                    InvalidateRect(hwnd_,nullptr,FALSE);
                    UpdateWindow(hwnd_);
                }
                return 0;
            }
            if (w == kAppNoticeTimerId) {
                KillTimer(hwnd_,kAppNoticeTimerId);
                appNoticeText_.clear(); appNoticeUntil_=0; appNoticeStart_=0;
                InvalidateRect(hwnd_,nullptr,FALSE);
                return 0;
            }
            if (w == kHoverPreviewAudioDelayTimerId) {
                KillTimer(hwnd_,kHoverPreviewAudioDelayTimerId);
                StartHoverPreviewAudioIfStillValid();
                return 0;
            }
            if (w == kPreviewZoomSettleTimerId) {
                FinishPreviewZoomGesture();
                return 0;
            }
            if (w == kPreviewScrollSettleTimerId) {
                FinishPreviewScrollGesture();
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if ((GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
                if (w == L'F') {
                    if(mode_==Mode::Library){
                        size_t mediaIndex=static_cast<size_t>(-1);
                        if(HoveredLibraryMediaIndex(mediaIndex)){
                            auto& list=CurrentItems();
                            if(mediaIndex<list.size()) ToggleFavorite(list[mediaIndex]);
                        }
                        return 0;
                    }
                    if(mode_==Mode::Details){
                        auto& list=CurrentItems();
                        if(selected_<list.size()) ToggleFavorite(list[selected_]);
                        return 0;
                    }
                }
                if (mode_==Mode::Library && w == L'A' && searchVisible_ && !searchQuery_.empty()) {
                    searchSelectAll_ = true;
                    InvalidateRect(hwnd_, &searchBoxRect_, FALSE);
                    return 0;
                }
            }
            if (searchVisible_ && mode_ == Mode::Library && w == VK_ESCAPE) {
                // Close search/failure results without changing the folder. Force a fresh
                // local-folder filter next time Library is painted.
                ClearLoadFailureFilter();
                searchQuery_.clear();
                searchVisible_ = false;
                searchSelectAll_ = false;
                filteredIndices_.clear();
                filterDirty_ = true;
                scrollY_ = 0;
                if (mode_ == Mode::Library) { ClampScroll(); }
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (searchVisible_ && mode_ == Mode::Library && w == VK_RETURN) {
                const auto& filtered = FilteredIndices();
                if (!filtered.empty()) {
                    thumbStop_.store(true,std::memory_order_release); ClearLoadingStateIf(1);
                    detailsOriginFolder_ = currentFolder_;
                    detailsSearchNavigationActive_ = loadFailureFilterActive_ || (searchVisible_ && !searchQuery_.empty());
                    if(detailsSearchNavigationActive_) detailsSearchNavigationIndices_ = filtered;
                    else detailsSearchNavigationIndices_.clear();
                    selected_ = filtered.front();
                    const auto& list=CurrentItems();
                    if(selected_<list.size()) SaveCurrentFolderViewState(list[selected_].path);
                    ResetLibraryZoom();
                    if(category_==Category::Images) ResetImageZoom();
                    mode_ = Mode::Details; detailsScrollY_ = 0;
                    if(category_==Category::Videos) StartPreviewWorkerForSelected(); else ClearLoadingState();
                    QueueDetailPrefetchWindow();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                return 0;
            }
            if (w == VK_ESCAPE && mode_ == Mode::Player) {
                if(player_ && !player_->VR().vr && !nativeVideoSizing_ && player_->FlatZoomActive()){
                    player_->ResetFlatZoom();
                    if(videoHwnd_) InvalidateRect(videoHwnd_,nullptr,FALSE);
                    return 0;
                }
                // Escape navigates back to Info without changing the global fullscreen state.
                LeavePlayer();
                return 0;
            }
            if (w == VK_ESCAPE && mode_ == Mode::Details) {
                if(category_==Category::Images && !nativeImageSizing_ && ImageZoomActive()){
                    ResetImageZoom();
                    InvalidateRect(hwnd_,nullptr,FALSE);
                    return 0;
                }
                // Escape mirrors the visible Back button on the Info screen.
                ReturnFromDetailsToLibrary();
                return 0;
            }
            if (w == VK_ESCAPE && mode_ == Mode::Library && !IsAtLibraryRoot()) {
                // Escape mirrors the visible Back button inside a Library subfolder.
                StopImageSlideshow();
                searchQuery_.clear();
                searchVisible_ = false;
                        filteredIndices_.clear();
                filterDirty_ = true;
                SaveCurrentFolderViewState();
                fs::path parent = fs::path(currentFolder_).parent_path();
                currentFolder_ = parent.empty() ? folder_ : parent.lexically_normal().wstring();
                RestoreFolderViewState(currentFolder_);
                PrepareLibraryViewportFromPrivateCache();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (w == VK_ESCAPE && mode_ == Mode::Library && IsAtLibraryRoot() && fullscreen_) {
                // At the top Library there is no deeper UI state left for Escape to close,
                // so Escape may finally leave fullscreen. Search/subfolder/Info/Player
                // handling above still takes priority and preserves fullscreen.
                ToggleFullscreen();
                return 0;
            }
            if (w == VK_SPACE && category_ == Category::Images && mode_ == Mode::Details && selected_ < images_.size()) {
                if (slideshowActive_) StopImageSlideshow();
                else StartImageSlideshowFromSelected();
                return 0;
            }
            if (w == VK_SPACE && mode_ == Mode::Details && category_ == Category::Videos && selected_ < videos_.size()) {
                // Exactly the same action as the Info-screen "Play" button.
                EnterPlayerAt(0.0);
                return 0;
            }
            if (w == VK_LEFT && mode_ == Mode::Details) {
                if (CanNavigateDetailsMedia(-1)) NavigateDetailsMedia(-1);
                return 0;
            }
            if (w == VK_RIGHT && mode_ == Mode::Details) {
                if (CanNavigateDetailsMedia(1)) NavigateDetailsMedia(1);
                return 0;
            }
            if (w == VK_SPACE && mode_ == Mode::Player && player_) {
                PlayerActivity(true); player_->PlayPause(); InvalidateControls(); return 0;
            }
            if (w == VK_LEFT && mode_ == Mode::Player) {
                SkipPlaybackSeconds(-30.0);
                return 0;
            }
            if (w == VK_RIGHT && mode_ == Mode::Player) {
                SkipPlaybackSeconds(30.0);
                return 0;
            }
            if (w == VK_F11) {
                if(mode_==Mode::Player) PlayerActivity(true);
                ToggleFullscreen(); return 0;
            }
            break;
        case WM_DESTROY:
            instancePipeServer_.Stop();
            DrainExternalOpenMessages();
            StopLibraryScanWorker();
            KillTimer(hwnd_,kLibraryAccessRetryTimerId);
            KillTimer(hwnd_,kLiveWindowMoveTimerId);
            KillTimer(hwnd_,kExternalOpenBatchTimerId);
            KillTimer(hwnd_,kPreviewZoomSettleTimerId);
            KillTimer(hwnd_,kPreviewScrollSettleTimerId);
            KillTimer(hwnd_,kHoverPreviewAudioDelayTimerId);
            StopHoverPreviewAudio();
            StopImageSlideshow();
            ResetLibraryZoom();
            SaveSettings();
            StopPreviewWorker();
            StopPreviewBitmapDecodeWorker();
            StopDetailPrefetchWorker();
            StopFullLoadWorker();
            StopThumbnailWorker();
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(hwnd_,m,w,l);
    }

    void DestroyBackBuffer() {
        if(backDC_) {
            if(backOldBitmap_) SelectObject(backDC_,backOldBitmap_);
            if(backBitmap_) DeleteObject(backBitmap_);
            DeleteDC(backDC_);
        }
        backDC_=nullptr; backBitmap_=nullptr; backOldBitmap_=nullptr; backW_=backH_=0;
    }

    void EnsureBackBuffer(HDC reference, int w, int h) {
        if(backDC_ && backW_==w && backH_==h) return;
        DestroyBackBuffer();
        backDC_=CreateCompatibleDC(reference);
        backBitmap_=CreateCompatibleBitmap(reference,w,h);
        backOldBitmap_=SelectObject(backDC_,backBitmap_);
        backW_=w; backH_=h;
    }

    void DestroyControlsBackBuffer() {
        if(controlsBackDC_) {
            if(controlsBackOldBitmap_) SelectObject(controlsBackDC_,controlsBackOldBitmap_);
            if(controlsBackBitmap_) DeleteObject(controlsBackBitmap_);
            DeleteDC(controlsBackDC_);
        }
        controlsBackDC_=nullptr; controlsBackBitmap_=nullptr; controlsBackOldBitmap_=nullptr;
        controlsBackW_=controlsBackH_=0;
    }

    void EnsureControlsBackBuffer(HDC reference,int w,int h) {
        if(controlsBackDC_ && controlsBackW_==w && controlsBackH_==h) return;
        DestroyControlsBackBuffer();
        controlsBackDC_=CreateCompatibleDC(reference);
        controlsBackBitmap_=CreateCompatibleBitmap(reference,w,h);
        controlsBackOldBitmap_=SelectObject(controlsBackDC_,controlsBackBitmap_);
        controlsBackW_=w; controlsBackH_=h;
    }

    bool UseGpuLibraryRenderer(RECT /*rc*/) const {
        // Keep the accelerated renderer active during scrolling. The V5 separation is
        // achieved by removing cache/decode/queue/trim work from input and paint; switching
        // the whole Library to GDI during a drag would make a 4K full-frame repaint slower.
        return mode_==Mode::Library && hwnd_!=nullptr;
    }

    void ResetLibraryGpuRenderer() {
        libraryD2dCardBrush_.Reset();
        libraryD2dPlaceholderBrush_.Reset();
        libraryD2dUiBrush_.Reset();
        libraryD2dFavoriteIcon_.Reset();
        libraryD2dVrIcon_.Reset();
        libraryD2dResolution4k_.Reset();
        libraryD2dResolution5k_.Reset();
        libraryD2dResolution8k_.Reset();
        libraryD2dAutoNextSvg_.Reset();libraryD2dOpenFolderSvg_.Reset();libraryD2dReloadSvg_.Reset();libraryD2dLoadEverythingSvg_.Reset();
        libraryD2dExpandFullscreenSvg_.Reset();libraryD2dCollapseFullscreenSvg_.Reset();libraryD2dBackSvg_.Reset();libraryD2dImageSvg_.Reset();libraryD2dVideosSvg_.Reset();
        libraryD2dNativeSvg_.Reset();libraryD2dPreviousSvg_.Reset();libraryD2dNextSvg_.Reset();libraryD2dPlaySvg_.Reset();
        libraryD2dButtonBackgroundActive_.Reset();libraryD2dButtonBackgroundInactive_.Reset();
        libraryD2dTarget_.Reset();
        libraryD2dWidth_=libraryD2dHeight_=0;
        ++libraryD2dGeneration_;
        auto clear=[&](std::vector<MediaItem>& list){
            for(auto& item:list){
                item.libraryGpuThumb.Reset();
                item.libraryGpuThumbSource=nullptr;
                item.libraryGpuGeneration=0;
                item.detailsGpuThumb.Reset();
                item.detailsGpuThumbSource=nullptr;
                item.detailsGpuGeneration=0;
            }
        };
        clear(videos_); clear(images_);
        auto clearPreviewGpu=[](std::vector<PreviewFrame>& frames){
            for(auto& frame:frames){frame.gpuBitmap.Reset();frame.gpuBitmapSource=nullptr;frame.gpuGeneration=0;}
        };
        clearPreviewGpu(previewFrames_);
        for(auto& kv:prefetchedPreviewSets_) clearPreviewGpu(kv.second.frames);
        detailsGpuWorkingSetActive_=false;
    }

    bool EnsureLibraryGpuRenderer(RECT rc) {
        const int w=std::max(1,static_cast<int>(rc.right-rc.left));
        const int h=std::max(1,static_cast<int>(rc.bottom-rc.top));
        if(!libraryD2dFactory_){
            D2D1_FACTORY_OPTIONS options{};
            if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory),&options,reinterpret_cast<void**>(libraryD2dFactory_.GetAddressOf())))) return false;
        }
        if(!libraryWicFactory_){
            if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(libraryWicFactory_.GetAddressOf())))) return false;
        }
        if(!libraryDWriteFactory_){
            if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(libraryDWriteFactory_.ReleaseAndGetAddressOf())))) return false;
        }
        if(!libraryD2dTarget_){
            D2D1_RENDER_TARGET_PROPERTIES props{};
            props.type=D2D1_RENDER_TARGET_TYPE_HARDWARE;
            props.pixelFormat.format=DXGI_FORMAT_B8G8R8A8_UNORM;
            props.pixelFormat.alphaMode=D2D1_ALPHA_MODE_IGNORE;
            props.dpiX=96.0f; props.dpiY=96.0f;
            props.usage=D2D1_RENDER_TARGET_USAGE_NONE;
            props.minLevel=D2D1_FEATURE_LEVEL_DEFAULT;
            D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps{};
            hwndProps.hwnd=hwnd_;
            hwndProps.pixelSize=D2D1_SIZE_U{static_cast<UINT32>(w),static_cast<UINT32>(h)};
            hwndProps.presentOptions=D2D1_PRESENT_OPTIONS_NONE;
            if(FAILED(libraryD2dFactory_->CreateHwndRenderTarget(&props,&hwndProps,libraryD2dTarget_.GetAddressOf()))) return false;
            const D2D1_COLOR_F cardColor{31.0f/255.0f,35.0f/255.0f,46.0f/255.0f,1.0f};
            const D2D1_COLOR_F placeholderColor{43.0f/255.0f,48.0f/255.0f,61.0f/255.0f,1.0f};
            const D2D1_COLOR_F uiColor{1.0f,1.0f,1.0f,1.0f};
            if(FAILED(libraryD2dTarget_->CreateSolidColorBrush(&cardColor,nullptr,libraryD2dCardBrush_.GetAddressOf())) ||
               FAILED(libraryD2dTarget_->CreateSolidColorBrush(&placeholderColor,nullptr,libraryD2dPlaceholderBrush_.GetAddressOf())) ||
               FAILED(libraryD2dTarget_->CreateSolidColorBrush(&uiColor,nullptr,libraryD2dUiBrush_.GetAddressOf()))){
                ResetLibraryGpuRenderer(); return false;
            }
            libraryD2dWidth_=w; libraryD2dHeight_=h;
        }else if(libraryD2dWidth_!=w || libraryD2dHeight_!=h){
            const D2D1_SIZE_U newSize{static_cast<UINT32>(w),static_cast<UINT32>(h)};
            const HRESULT hr=libraryD2dTarget_->Resize(&newSize);
            if(FAILED(hr)){ ResetLibraryGpuRenderer(); return EnsureLibraryGpuRenderer(rc); }
            libraryD2dWidth_=w; libraryD2dHeight_=h;
        }
        return true;
    }

    ID2D1Bitmap* GetLibraryGpuThumb(MediaItem& item,HBITMAP source,int targetW,int targetH) {
        if(!source || !libraryD2dTarget_ || !libraryWicFactory_) return nullptr;
        targetW=std::max(1,targetW); targetH=std::max(1,targetH);
        if(item.libraryGpuThumb && item.libraryGpuThumbSource==source && item.libraryGpuGeneration==libraryD2dGeneration_){
            const D2D1_SIZE_U px=item.libraryGpuThumb->GetPixelSize();
            if(static_cast<int>(px.width)==targetW && static_cast<int>(px.height)==targetH)
                return item.libraryGpuThumb.Get();
        }

        item.libraryGpuThumb.Reset(); item.libraryGpuThumbSource=nullptr; item.libraryGpuGeneration=0;
        ComPtr<IWICBitmap> wicBitmap;
        if(FAILED(libraryWicFactory_->CreateBitmapFromHBITMAP(source,nullptr,WICBitmapIgnoreAlpha,wicBitmap.GetAddressOf()))) return nullptr;

        // CPU thumbnails remain at their existing high-resolution decode size. The
        // device-dependent presentation copy only needs one pixel per on-screen card
        // pixel; uploading the full 1080p master here was pure paint-thread overhead.
        ComPtr<IWICBitmapScaler> scaler;
        if(FAILED(libraryWicFactory_->CreateBitmapScaler(scaler.GetAddressOf()))) return nullptr;
        if(FAILED(scaler->Initialize(wicBitmap.Get(),static_cast<UINT>(targetW),static_cast<UINT>(targetH),
                                     WICBitmapInterpolationModeFant))) return nullptr;

        ComPtr<IWICFormatConverter> converter;
        if(FAILED(libraryWicFactory_->CreateFormatConverter(converter.GetAddressOf()))) return nullptr;
        if(FAILED(converter->Initialize(scaler.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom))) return nullptr;
        if(FAILED(libraryD2dTarget_->CreateBitmapFromWicBitmap(converter.Get(),nullptr,item.libraryGpuThumb.GetAddressOf()))) return nullptr;
        item.libraryGpuThumbSource=source;
        item.libraryGpuGeneration=libraryD2dGeneration_;
        return item.libraryGpuThumb.Get();
    }

    static D2D1_RECT_F D2DRect(RECT r) {
        return D2D1_RECT_F{static_cast<float>(r.left),static_cast<float>(r.top),static_cast<float>(r.right),static_cast<float>(r.bottom)};
    }

    ID2D1Bitmap* GetLibraryGpuUiBitmap(Gdiplus::Bitmap* source,ComPtr<ID2D1Bitmap>& cache) {
        if(!source || !libraryD2dTarget_) return nullptr;
        if(cache) return cache.Get();
        const UINT w=source->GetWidth(),h=source->GetHeight();
        if(w==0 || h==0) return nullptr;
        Gdiplus::Rect lockRect(0,0,static_cast<INT>(w),static_cast<INT>(h));
        Gdiplus::BitmapData data{};
        if(source->LockBits(&lockRect,Gdiplus::ImageLockModeRead,PixelFormat32bppPARGB,&data)!=Gdiplus::Ok) return nullptr;
        const UINT pitch=w*4u;
        std::vector<BYTE> pixels(static_cast<size_t>(pitch)*h);
        const BYTE* base=static_cast<const BYTE*>(data.Scan0);
        const INT stride=data.Stride;
        for(UINT y=0;y<h;++y){
            const BYTE* row=stride>=0 ? base+static_cast<size_t>(y)*static_cast<size_t>(stride)
                                      : base+static_cast<size_t>(h-1-y)*static_cast<size_t>(-stride);
            std::memcpy(pixels.data()+static_cast<size_t>(y)*pitch,row,pitch);
        }
        source->UnlockBits(&data);
        D2D1_BITMAP_PROPERTIES props{};
        props.pixelFormat=D2D1_PIXEL_FORMAT{DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED};
        props.dpiX=96.0f; props.dpiY=96.0f;
        if(FAILED(libraryD2dTarget_->CreateBitmap(D2D1_SIZE_U{w,h},pixels.data(),pitch,&props,cache.GetAddressOf()))) return nullptr;
        return cache.Get();
    }

    void DrawLibraryGpuBitmapCentered(ID2D1Bitmap* bitmap,RECT r,int insetX=0,int insetY=0) {
        if(!bitmap || !libraryD2dTarget_ || EmptyRectValue(r)) return;
        const auto size=bitmap->GetSize();
        const float availW=static_cast<float>(std::max<LONG>(1L,(r.right-r.left)-static_cast<LONG>(2*insetX)));
        const float availH=static_cast<float>(std::max<LONG>(1L,(r.bottom-r.top)-static_cast<LONG>(2*insetY)));
        const float scale=std::min(1.0f,std::min(availW/std::max(1.0f,size.width),availH/std::max(1.0f,size.height)));
        const float w=size.width*scale,h=size.height*scale;
        const float cx=(r.left+r.right)*0.5f,cy=(r.top+r.bottom)*0.5f;
        const D2D1_RECT_F dst{cx-w*0.5f,cy-h*0.5f,cx+w*0.5f,cy+h*0.5f};
        libraryD2dTarget_->DrawBitmap(bitmap,&dst,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,nullptr);
    }

    void DrawLibraryGpuBitmapStretched(ID2D1Bitmap* bitmap,RECT r) {
        if(!bitmap || !libraryD2dTarget_ || EmptyRectValue(r)) return;
        const D2D1_RECT_F dst=D2DRect(r);
        libraryD2dTarget_->DrawBitmap(bitmap,&dst,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,nullptr);
    }

    static D2D1_COLOR_F D2DColor(COLORREF c,float alpha=1.0f) {
        return D2D1_COLOR_F{GetRValue(c)/255.0f,GetGValue(c)/255.0f,GetBValue(c)/255.0f,std::clamp(alpha,0.0f,1.0f)};
    }

    void SetLibraryGpuBrush(COLORREF c,float alpha=1.0f) {
        if(libraryD2dUiBrush_) libraryD2dUiBrush_->SetColor(D2DColor(c,alpha));
    }

    void FillLibraryGpuRound(RECT r,COLORREF c,float radius) {
        if(!libraryD2dTarget_ || !libraryD2dUiBrush_ || EmptyRectValue(r)) return;
        SetLibraryGpuBrush(c);
        const D2D1_ROUNDED_RECT rr{D2DRect(r),radius,radius};
        libraryD2dTarget_->FillRoundedRectangle(&rr,libraryD2dUiBrush_.Get());
    }

    void FillLibraryGpuRect(RECT r,COLORREF c) {
        if(!libraryD2dTarget_ || !libraryD2dUiBrush_ || EmptyRectValue(r)) return;
        SetLibraryGpuBrush(c);
        const D2D1_RECT_F fr=D2DRect(r);
        libraryD2dTarget_->FillRectangle(&fr,libraryD2dUiBrush_.Get());
    }

    void FillLibraryGpuRectAlpha(RECT r,COLORREF c,float alpha) {
        if(!libraryD2dTarget_ || !libraryD2dUiBrush_ || EmptyRectValue(r)) return;
        SetLibraryGpuBrush(c,alpha);
        const D2D1_RECT_F fr=D2DRect(r);
        libraryD2dTarget_->FillRectangle(&fr,libraryD2dUiBrush_.Get());
    }

    IDWriteTextFormat* GetLibraryGpuTextFormat(int px,int weight) {
        if(!libraryDWriteFactory_) return nullptr;
        const uint64_t key=(static_cast<uint64_t>(static_cast<uint32_t>(px))<<32)|static_cast<uint32_t>(weight);
        auto it=libraryDWriteFormats_.find(key);
        if(it!=libraryDWriteFormats_.end()) return it->second.Get();
        ComPtr<IDWriteTextFormat> format;
        if(FAILED(libraryDWriteFactory_->CreateTextFormat(L"Segoe UI",nullptr,static_cast<DWRITE_FONT_WEIGHT>(weight),
            DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,static_cast<FLOAT>(std::max(1,px)),L"",format.GetAddressOf()))) return nullptr;
        auto inserted=libraryDWriteFormats_.emplace(key,std::move(format));
        return inserted.first->second.Get();
    }

    float MeasureLibraryGpuTextWidth(const std::wstring& text,int px,int weight=FW_NORMAL) {
        if(text.empty() || !libraryDWriteFactory_) return 0.0f;
        IDWriteTextFormat* format=GetLibraryGpuTextFormat(px,weight);if(!format)return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        if(FAILED(libraryDWriteFactory_->CreateTextLayout(text.c_str(),static_cast<UINT32>(text.size()),format,8192.0f,512.0f,layout.GetAddressOf()))) return 0.0f;
        DWRITE_TEXT_METRICS metrics{};if(FAILED(layout->GetMetrics(&metrics)))return 0.0f;
        return metrics.widthIncludingTrailingWhitespace;
    }

    void DrawLibraryGpuText(const std::wstring& text,RECT r,int px,int weight=FW_NORMAL,
                            COLORREF color=RGB(238,241,247),UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS) {
        if(text.empty() || !libraryD2dTarget_ || !libraryD2dUiBrush_) return;
        IDWriteTextFormat* format=GetLibraryGpuTextFormat(px,weight); if(!format) return;
        format->SetTextAlignment((flags&DT_CENTER)?DWRITE_TEXT_ALIGNMENT_CENTER:((flags&DT_RIGHT)?DWRITE_TEXT_ALIGNMENT_TRAILING:DWRITE_TEXT_ALIGNMENT_LEADING));
        format->SetParagraphAlignment((flags&DT_VCENTER)?DWRITE_PARAGRAPH_ALIGNMENT_CENTER:DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        format->SetWordWrapping((flags&DT_WORDBREAK)?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trimming{};
        ComPtr<IDWriteInlineObject> ellipsis;
        if(flags&DT_END_ELLIPSIS){
            trimming.granularity=DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            if(SUCCEEDED(libraryDWriteFactory_->CreateEllipsisTrimmingSign(format,ellipsis.GetAddressOf())))
                format->SetTrimming(&trimming,ellipsis.Get());
            else {trimming.granularity=DWRITE_TRIMMING_GRANULARITY_NONE;format->SetTrimming(&trimming,nullptr);}
        }else{
            trimming.granularity=DWRITE_TRIMMING_GRANULARITY_NONE;
            format->SetTrimming(&trimming,nullptr);
        }
        SetLibraryGpuBrush(color);
        const D2D1_RECT_F bounds=D2DRect(r);
        libraryD2dTarget_->PushAxisAlignedClip(bounds,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        libraryD2dTarget_->DrawText(text.c_str(),static_cast<UINT32>(text.size()),format,&bounds,libraryD2dUiBrush_.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
        libraryD2dTarget_->PopAxisAlignedClip();
    }

    void DrawLibraryGpuHoverBorder(RECT r,float amount,float radius) {
        amount=std::clamp(amount,0.0f,1.0f); if(amount<=0.001f || EmptyRectValue(r)) return;
        const COLORREF border=MixColor(RGB(74,81,96),RGB(238,242,250),amount);
        SetLibraryGpuBrush(border);
        D2D1_RECT_F fr=D2DRect(r); fr.left+=1.5f;fr.top+=1.5f;fr.right-=1.5f;fr.bottom-=1.5f;
        const D2D1_ROUNDED_RECT rr{fr,radius,radius};
        libraryD2dTarget_->DrawRoundedRectangle(&rr,libraryD2dUiBrush_.Get(),3.0f);
    }

    void DrawLibraryGpuButton(RECT r,const std::wstring& text,bool primary=false) {
        const float hover=ButtonHoverAmount(r);
        const COLORREF base=primary?RGB(188,200,224):RGB(111,124,145);
        const COLORREF over=primary?RGB(216,226,242):RGB(132,146,169);
        FillLibraryGpuRound(r,MixColor(base,over,hover),11.0f);
        DrawLibraryGpuText(text,r,14,FW_SEMIBOLD,primary?RGB(12,14,19):RGB(245,246,250),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }

    void DrawLibraryGpuTab(RECT r,const std::wstring& text,bool active) {
        const float hover=ButtonHoverAmount(r);
        const COLORREF base=active?RGB(188,200,224):RGB(111,124,145);
        const COLORREF over=active?RGB(216,226,242):RGB(132,146,169);
        FillLibraryGpuRound(r,MixColor(base,over,hover),11.0f);
        DrawLibraryGpuText(text,r,15,FW_BOLD,active?RGB(12,14,19):RGB(245,246,250),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }

    void DrawLibraryGpuLine(float x1,float y1,float x2,float y2,COLORREF color,float width=2.0f) {
        SetLibraryGpuBrush(color);
        libraryD2dTarget_->DrawLine(D2D1_POINT_2F{x1,y1},D2D1_POINT_2F{x2,y2},libraryD2dUiBrush_.Get(),width);
    }

    void DrawLibraryGpuIconButtonBase(RECT r,bool active=false) {
        const float hover=ButtonHoverAmount(r);
        const COLORREF base=active?RGB(188,200,224):RGB(111,124,145);
        const COLORREF over=active?RGB(216,226,242):RGB(132,146,169);
        FillLibraryGpuRound(r,MixColor(base,over,hover),11.0f);
    }

    void DrawLibraryGpuSvgIconButton(RECT r,Gdiplus::Bitmap* icon,ComPtr<ID2D1Bitmap>& cache,bool active=false,int inset=6) {
        const float hover=ButtonHoverAmount(r);
        const COLORREF base=active?RGB(188,200,224):RGB(111,124,145);
        const COLORREF over=active?RGB(216,226,242):RGB(132,146,169);
        FillLibraryGpuRound(r,MixColor(base,over,hover),11.0f);
        if(icon){
            const int drawInset=externalButtonAssets_.count(icon)?0:inset;
            if(auto* bmp=GetLibraryGpuUiBitmap(icon,cache)) DrawLibraryGpuBitmapCentered(bmp,r,drawInset,drawInset);
        }
    }

    void DrawLibraryGpuFullscreenButton(RECT r) {
        if(fullscreen_) DrawLibraryGpuSvgIconButton(r,collapseFullscreenSvgBitmap_.get(),libraryD2dCollapseFullscreenSvg_,true,6);
        else DrawLibraryGpuSvgIconButton(r,expandFullscreenSvgBitmap_.get(),libraryD2dExpandFullscreenSvg_,false,6);
    }

    void DrawLibraryGpuAutoAdvance(RECT r,bool active) {
        DrawLibraryGpuSvgIconButton(r,autoNextSvgBitmap_.get(),libraryD2dAutoNextSvg_,active,5);
    }

    void DrawLibraryGpuFolderIconButton(RECT r) {
        DrawLibraryGpuSvgIconButton(r,openFolderSvgBitmap_.get(),libraryD2dOpenFolderSvg_,false,5);
    }

    void DrawLibraryGpuRefreshIconButton(RECT r) {
        DrawLibraryGpuSvgIconButton(r,reloadSvgBitmap_.get(),libraryD2dReloadSvg_,false,5);
    }

    void DrawLibraryGpuDownloadIconButton(RECT r) {
        DrawLibraryGpuSvgIconButton(r,loadEverythingSvgBitmap_.get(),libraryD2dLoadEverythingSvg_,false,5);
    }

    void DrawLibraryGpuFavoriteBadge(RECT image) {
        RECT badge{image.left+8,image.top+5,image.left+42,image.top+42};
        if(auto* bmp=GetLibraryGpuUiBitmap(favoriteIconBitmap_.get(),libraryD2dFavoriteIcon_)){ DrawLibraryGpuBitmapCentered(bmp,badge); return; }
        DrawLibraryGpuText(L"♥",badge,27,FW_NORMAL,RGB(245,246,250),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }

    void DrawLibraryGpuVideoBadges(MediaItem& item,RECT card) {
        if(!item.isVideo) return;
        int badgeRight=card.right-8;
        if(item.vr.vr){
            RECT tag{badgeRight-28,card.top+8,badgeRight,card.top+36};
            if(auto* bmp=GetLibraryGpuUiBitmap(vrBadgeWhiteBitmap_.get(),libraryD2dVrIcon_)) DrawLibraryGpuBitmapCentered(bmp,tag);
            else {FillLibraryGpuRound(tag,RGB(16,19,25),8);DrawLibraryGpuText(L"VR",tag,11,FW_BOLD,RGB(220,225,235),DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
            badgeRight=tag.left-6;
        }
        const int klass=ResolutionBadgeClass(item);
        if(klass){
            RECT tag{badgeRight-28,card.top+8,badgeRight,card.top+36};
            Gdiplus::Bitmap* src=nullptr; ComPtr<ID2D1Bitmap>* cache=nullptr;
            if(klass==8){src=resolution8kBitmap_.get();cache=&libraryD2dResolution8k_;}
            else if(klass==5){src=resolution5kBitmap_.get();cache=&libraryD2dResolution5k_;}
            else if(klass==4){src=resolution4kBitmap_.get();cache=&libraryD2dResolution4k_;}
            if(src && cache){
                if(auto* bmp=GetLibraryGpuUiBitmap(src,*cache)) DrawLibraryGpuBitmapCentered(bmp,tag);
                else {FillLibraryGpuRound(tag,RGB(16,19,25),8);DrawLibraryGpuText(std::to_wstring(klass)+L"K",tag,10,FW_BOLD,RGB(230,234,242),DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
            }
        }
    }

    void DrawLibraryGpuFolderCard(const LibraryFolder& folder,RECT card) {
        FillLibraryGpuRound(card,RGB(31,35,46),12);
        const int imageH=std::max(113,static_cast<int>(std::lround(static_cast<double>(card.right-card.left)*9.0/16.0)));
        RECT image=card;image.bottom=image.top+imageH;FillLibraryGpuRect(image,RGB(37,42,54));
        const int iconW=118,iconH=82,cx=(image.left+image.right)/2,cy=(image.top+image.bottom)/2+4;
        RECT body{cx-iconW/2,cy-iconH/2+12,cx+iconW/2,cy+iconH/2};RECT tab{body.left+8,body.top-17,body.left+54,body.top+5};
        FillLibraryGpuRound(tab,RGB(210,170,73),7);FillLibraryGpuRound(body,RGB(225,184,82),10);
        RECT title{card.left+10,image.bottom+2,card.right-10,card.bottom-3};DrawLibraryGpuText(folder.name,title,14,FW_SEMIBOLD);
    }

    void PaintLibraryGpuScrollbar(RECT rc) {
        UpdateLibraryScrollbarRects(rc);
        if(IsRectEmpty(&libraryScrollTrackRect_)||IsRectEmpty(&libraryScrollThumbRect_)) return;
        FillLibraryGpuRound(libraryScrollTrackRect_,RGB(24,28,36),5);
        FillLibraryGpuRound(libraryScrollThumbRect_,libraryScrollDragging_?RGB(145,152,166):RGB(94,101,116),6);
    }

    void PaintLibraryGpuNavigator(RECT rc) {
        const int footerTop=std::max(0,static_cast<int>(rc.bottom)-64);
        libraryFooterRect_=RECT{0,footerTop,rc.right,rc.bottom};
        FillLibraryGpuRect(libraryFooterRect_,RGB(16,18,24));
        DrawLibraryGpuLine(0.0f,static_cast<float>(footerTop),static_cast<float>(rc.right),static_cast<float>(footerTop),RGB(42,47,60),1);
        const int buttonTop=footerTop+13,buttonBottom=rc.bottom-13;
        constexpr int iconW=48,iconGap=10,groupGap=24,rightMargin=20;
        const int iconButtonBottom=rc.bottom-10,iconButtonTop=iconButtonBottom-iconW;
        int navLeft=20;
        if(!IsAtLibraryRoot()){backRect_={20,iconButtonTop,20+iconW,iconButtonBottom};DrawLibraryGpuSvgIconButton(backRect_,backSvgBitmap_.get(),libraryD2dBackSvg_,false,5);navLeft=backRect_.right+10;}else backRect_=RECT{};
        categoryToggleRect_={navLeft,iconButtonTop,navLeft+iconW,iconButtonBottom};
        if(category_==Category::Videos) DrawLibraryGpuSvgIconButton(categoryToggleRect_,videosSvgBitmap_.get(),libraryD2dVideosSvg_,false,5);
        else DrawLibraryGpuSvgIconButton(categoryToggleRect_,imageSvgBitmap_.get(),libraryD2dImageSvg_,false,5);
        mediaCountRect_={categoryToggleRect_.right+10,buttonTop,categoryToggleRect_.right+110,buttonBottom};
        DrawLibraryGpuText(L"("+std::to_wstring(CurrentFolderMediaCount())+L")",mediaCountRect_,14,FW_SEMIBOLD,RGB(175,181,194),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
        int cursor=std::max(0,static_cast<int>(rc.right)-rightMargin);
        libraryFullRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};DrawLibraryGpuFullscreenButton(libraryFullRect_);cursor=libraryFullRect_.left-iconGap;
        const bool showSlideshow=(category_==Category::Images&&CurrentFolderMediaCount()>0);
        if(showSlideshow){slideshowRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};DrawLibraryGpuAutoAdvance(slideshowRect_,slideshowActive_);cursor=slideshowRect_.left-iconGap;}else slideshowRect_=RECT{};
        const bool showRootMaintenance=IsAtChosenLibraryRoot();const bool showChooseOnly=!showRootMaintenance&&(currentFolder_.empty()||externalMediaSession_);
        if(showRootMaintenance){cursor-=groupGap-iconGap;chooseRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};cursor=chooseRect_.left-iconGap;rescanRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};cursor=rescanRect_.left-iconGap;loadEverythingRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};DrawLibraryGpuFolderIconButton(chooseRect_);DrawLibraryGpuRefreshIconButton(rescanRect_);DrawLibraryGpuDownloadIconButton(loadEverythingRect_);}
        else if(showChooseOnly){rescanRect_=RECT{};loadEverythingRect_=RECT{};cursor-=groupGap-iconGap;chooseRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};DrawLibraryGpuFolderIconButton(chooseRect_);}
        else{chooseRect_=RECT{};rescanRect_=RECT{};loadEverythingRect_=RECT{};}
    }

    void PaintLibraryGpuSearch(RECT rc) {
        const int right=std::max(20,static_cast<int>(rc.right)-20),width=std::min(430,std::max(220,static_cast<int>(rc.right)/3));
        searchBoxRect_={right-width,12,right,52};FillLibraryGpuRound(searchBoxRect_,RGB(31,35,46),11);
        RECT text{searchBoxRect_.left+14,searchBoxRect_.top,searchBoxRect_.right-14,searchBoxRect_.bottom};
        if(searchSelectAll_&&!searchQuery_.empty()){const LONG selectedRight=std::min<LONG>(text.right,text.left+static_cast<LONG>(std::ceil(MeasureLibraryGpuTextWidth(searchQuery_,16,FW_SEMIBOLD)))+5);RECT selected{text.left-3,text.top+8,selectedRight,text.bottom-8};FillLibraryGpuRound(selected,RGB(72,82,105),5);}
        const std::wstring shown=loadFailureFilterActive_ ? (L"Load failures ("+std::to_wstring(loadFailureFilterTotal_)+L")") : (searchQuery_.empty()?L"Search...":searchQuery_);
        const bool muted=!loadFailureFilterActive_&&searchQuery_.empty();
        DrawLibraryGpuText(shown,text,16,FW_SEMIBOLD,muted?RGB(145,151,164):RGB(244,246,250));
    }

    std::wstring MiddleEllipsizeLibraryGpu(const std::wstring& text,float maxWidth,int px,int weight=FW_NORMAL) {
        if(text.empty() || maxWidth<=0.0f) return L"";
        if(MeasureLibraryGpuTextWidth(text,px,weight)<=maxWidth) return text;
        const std::wstring dots=L"...";
        if(MeasureLibraryGpuTextWidth(dots,px,weight)>maxWidth) return L"";
        size_t lo=0,hi=text.size();
        while(lo<hi){
            const size_t keep=(lo+hi+1)/2,left=(keep+1)/2,right=keep-left;
            const std::wstring candidate=text.substr(0,left)+dots+text.substr(text.size()-right);
            if(MeasureLibraryGpuTextWidth(candidate,px,weight)<=maxWidth) lo=keep; else hi=keep-1;
        }
        const size_t left=(lo+1)/2,right=lo-left;
        return text.substr(0,left)+dots+text.substr(text.size()-right);
    }

    void PaintLibraryGpuLoadingPopup(RECT rc) {
        std::wstring label,count,fileName;const bool fullRunning=fullLoadRunning_.load(std::memory_order_acquire);const ULONGLONG now=GetTickCount64();
        const ULONGLONG fullDoneDuration=fullLoadFailures_.load(std::memory_order_relaxed)>0?kFullLoadFailedPopupDurationMs:kFullLoadDonePopupDurationMs;
        const bool fullDoneVisible=!fullRunning&&fullLoadFinishedAt_!=0&&now-fullLoadFinishedAt_<fullDoneDuration;
        if(fullRunning){label=L"Loading everything";const int current=fullLoadCurrent_.load(std::memory_order_relaxed),total=fullLoadTotal_.load(std::memory_order_relaxed);count=total>0?std::to_wstring(std::min(current,total))+L" / "+std::to_wstring(total)+L" files":L"Working...";fileName=FullLoadCurrentFile();}
        else if(fullDoneVisible){const int failures=fullLoadFailures_.load(std::memory_order_relaxed);label=L"Load everything finished";count=failures==0?L"All files ready":std::to_wstring(failures)+L" failed  •  Click to view";}
        else{const int kind=loadingKind_.load(std::memory_order_acquire);if(kind==0)return;const int current=loadingCurrent_.load(std::memory_order_relaxed),total=loadingTotal_.load(std::memory_order_relaxed);if(kind==1)label=L"Loading library banners";else if(kind==2)label=L"Loading secondary images";else if(kind==3)label=L"Loading info banner";else return;count=total>0?std::to_wstring(std::min(current,total))+L" / "+std::to_wstring(total):L"Working...";}
        // Match the compact in-app notice (for example "This folder is unavailable.") exactly.
        const RECT box=FullLoadPopupRect(rc);
        if(fullRunning && !fileName.empty()){
            const std::wstring prefix=count+L"  •  ";
            const float available=static_cast<float>(std::max(0,static_cast<int>(box.right-box.left)-28));
            const float remaining=std::max(0.0f,available-MeasureLibraryGpuTextWidth(prefix,12,FW_NORMAL));
            count=prefix+MiddleEllipsizeLibraryGpu(fileName,remaining,12,FW_NORMAL);
        }
        FillLibraryGpuRound(box,RGB(25,29,38),11);RECT labelRect{box.left+14,box.top+3,box.right-14,box.top+27};DrawLibraryGpuText(label,labelRect,13,FW_SEMIBOLD);
        RECT countRect{box.left+14,box.top+25,box.right-14,box.bottom-3};
        const bool failureAction=fullDoneVisible&&fullLoadFailures_.load(std::memory_order_relaxed)>0;
        DrawLibraryGpuText(count,countRect,12,failureAction?FW_SEMIBOLD:FW_NORMAL,failureAction?RGB(226,230,238):RGB(165,172,185),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }

    void PaintLibraryGpuNotice(RECT rc) {
        const ULONGLONG now=GetTickCount64();if(appNoticeText_.empty()||now>=appNoticeUntil_)return;const RECT box=AppNoticeRect(rc);FillLibraryGpuRound(box,RGB(31,35,46),12);DrawLibraryGpuHoverBorder(box,AppNoticePulseAmount(now),12);RECT text{box.left+16,box.top+8,box.right-16,box.bottom-8};DrawLibraryGpuText(appNoticeText_,text,15,FW_SEMIBOLD,RGB(238,241,247),DT_CENTER|DT_VCENTER|DT_WORDBREAK);
    }

    void DrawLibraryGpuBitmapCover(ID2D1Bitmap* bitmap,RECT r) {
        if(!bitmap || !libraryD2dTarget_) return;
        const D2D1_SIZE_F size=bitmap->GetSize();
        if(size.width<=0.0f || size.height<=0.0f) return;
        const float dw=static_cast<float>(std::max<LONG>(1,r.right-r.left));
        const float dh=static_cast<float>(std::max<LONG>(1,r.bottom-r.top));
        const float scale=std::max(dw/size.width,dh/size.height);
        const float sw=dw/scale,sh=dh/scale;
        const float sx=(size.width-sw)*0.5f,sy=(size.height-sh)*0.5f;
        const D2D1_RECT_F src{sx,sy,sx+sw,sy+sh};
        const D2D1_RECT_F dest=D2DRect(r);
        libraryD2dTarget_->DrawBitmap(bitmap,&dest,1.0f,D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,&src);
    }

    bool PushLibraryGpuRoundedCardClip(RECT card,float radius,
                                       ComPtr<ID2D1RoundedRectangleGeometry>& geometry,
                                       ComPtr<ID2D1Layer>& layer) {
        if(!libraryD2dFactory_ || !libraryD2dTarget_ || EmptyRectValue(card)) return false;
        const D2D1_ROUNDED_RECT rounded{D2DRect(card),radius,radius};
        if(FAILED(libraryD2dFactory_->CreateRoundedRectangleGeometry(&rounded,geometry.GetAddressOf()))) return false;
        if(FAILED(libraryD2dTarget_->CreateLayer(nullptr,layer.GetAddressOf()))) return false;
        D2D1_LAYER_PARAMETERS params{};
        params.contentBounds=D2DRect(card);
        params.geometricMask=geometry.Get();
        params.maskAntialiasMode=D2D1_ANTIALIAS_MODE_PER_PRIMITIVE;
        params.maskTransform=D2D1_MATRIX_3X2_F{1.0f,0.0f,0.0f,1.0f,0.0f,0.0f};
        params.opacity=1.0f;
        params.opacityBrush=nullptr;
        params.layerOptions=D2D1_LAYER_OPTIONS_NONE;
        libraryD2dTarget_->PushLayer(&params,layer.Get());
        return true;
    }


    ID2D1Bitmap* CreateGpuBitmapFromHBitmap(HBITMAP source,ComPtr<ID2D1Bitmap>& cache,HBITMAP& cacheSource,uint64_t& cacheGeneration) {
        if(!source || !libraryD2dTarget_ || !libraryWicFactory_) return nullptr;
        if(cache && cacheSource==source && cacheGeneration==libraryD2dGeneration_) return cache.Get();
        cache.Reset(); cacheSource=nullptr; cacheGeneration=0;
        ComPtr<IWICBitmap> wicBitmap;
        if(FAILED(libraryWicFactory_->CreateBitmapFromHBITMAP(source,nullptr,WICBitmapIgnoreAlpha,wicBitmap.GetAddressOf()))) return nullptr;
        ComPtr<IWICFormatConverter> converter;
        if(FAILED(libraryWicFactory_->CreateFormatConverter(converter.GetAddressOf()))) return nullptr;
        if(FAILED(converter->Initialize(wicBitmap.Get(),GUID_WICPixelFormat32bppPBGRA,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom))) return nullptr;
        if(FAILED(libraryD2dTarget_->CreateBitmapFromWicBitmap(converter.Get(),nullptr,cache.GetAddressOf()))) return nullptr;
        cacheSource=source; cacheGeneration=libraryD2dGeneration_;
        return cache.Get();
    }

    ID2D1Bitmap* GetDetailsGpuBitmap(MediaItem& item,HBITMAP source) {
        ID2D1Bitmap* bitmap=CreateGpuBitmapFromHBitmap(source,item.detailsGpuThumb,item.detailsGpuThumbSource,item.detailsGpuGeneration);
        if(bitmap) detailsGpuWorkingSetActive_=true;
        return bitmap;
    }

    ID2D1Bitmap* GetPreviewGpuBitmap(PreviewFrame& frame,HBITMAP source) {
        ID2D1Bitmap* bitmap=CreateGpuBitmapFromHBitmap(source,frame.gpuBitmap,frame.gpuBitmapSource,frame.gpuGeneration);
        if(bitmap) detailsGpuWorkingSetActive_=true;
        return bitmap;
    }

    ID2D1Bitmap* GetLibraryHoverPreviewGpuBitmap(LibraryHoverPreviewFrame& frame,HBITMAP source) {
        ID2D1Bitmap* bitmap=CreateGpuBitmapFromHBitmap(source,frame.gpuBitmap,frame.gpuBitmapSource,frame.gpuGeneration);
        if(bitmap) detailsGpuWorkingSetActive_=true;
        return bitmap;
    }

    void DrawDetailsGpuBitmapContain(ID2D1Bitmap* bitmap,RECT r,float alpha=1.0f,bool noUpscale=false,bool fitNoUpscale=false,
                                     float zoom=1.0f,float centerU=0.5f,float centerV=0.5f) {
        if(!bitmap || !libraryD2dTarget_ || EmptyRectValue(r) || alpha<=0.0f) return;
        const D2D1_SIZE_F size=bitmap->GetSize(); if(size.width<=0.0f||size.height<=0.0f)return;
        const float rw=static_cast<float>(std::max<LONG>(1,r.right-r.left)),rh=static_cast<float>(std::max<LONG>(1,r.bottom-r.top));
        float scale=std::min(rw/size.width,rh/size.height);
        if(noUpscale) scale=1.0f;
        else if(fitNoUpscale) scale=std::min(1.0f,scale);
        else scale*=std::clamp(zoom,0.25f,8.0f);
        const float dw=std::max(1.0f,size.width*scale),dh=std::max(1.0f,size.height*scale);
        float dx=static_cast<float>(r.left)+(rw-dw)*0.5f,dy=static_cast<float>(r.top)+(rh-dh)*0.5f;
        if(!noUpscale && !fitNoUpscale && std::abs(zoom-1.0f)>0.001f){
            const float cx=static_cast<float>(r.left)+rw*0.5f,cy=static_cast<float>(r.top)+rh*0.5f;
            dx=cx-std::clamp(centerU,0.0f,1.0f)*dw; dy=cy-std::clamp(centerV,0.0f,1.0f)*dh;
        }
        const D2D1_RECT_F clip=D2DRect(r); libraryD2dTarget_->PushAxisAlignedClip(clip,D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const D2D1_RECT_F dest{dx,dy,dx+dw,dy+dh};
        libraryD2dTarget_->DrawBitmap(bitmap,&dest,std::clamp(alpha,0.0f,1.0f),D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,nullptr);
        libraryD2dTarget_->PopAxisAlignedClip();
    }

    void DrawDetailsGpuEdgeArrowButton(RECT r,bool next,bool enabled=true) {
        if(!enabled) return;
        Gdiplus::Bitmap* icon=next?nextSvgBitmap_.get():previousSvgBitmap_.get();
        ComPtr<ID2D1Bitmap>& cache=next?libraryD2dNextSvg_:libraryD2dPreviousSvg_;
        DrawLibraryGpuSvgIconButton(r,icon,cache,false,7);
    }

    void DrawDetailsGpuNativeSizeButton(RECT r) {
        DrawLibraryGpuSvgIconButton(r,nativeSvgBitmap_.get(),libraryD2dNativeSvg_,nativeImageSizing_,5);
    }

    void DrawDetailsGpuVideoBadges(MediaItem& item,RECT media) {
        if(!item.isVideo)return;
        int badgeRight=media.right-10; const int top=media.top+10,size=30;
        if(item.vr.vr){
            RECT tag{badgeRight-size,top,badgeRight,top+size};
            if(auto* bmp=GetLibraryGpuUiBitmap(vrBadgeWhiteBitmap_.get(),libraryD2dVrIcon_))DrawLibraryGpuBitmapCentered(bmp,tag);
            else{FillLibraryGpuRound(tag,RGB(16,19,25),8);DrawLibraryGpuText(L"VR",tag,11,FW_BOLD,RGB(220,225,235),DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
            badgeRight=tag.left-6;
        }
        const int klass=ResolutionBadgeClass(item); if(!klass)return;
        RECT tag{badgeRight-size,top,badgeRight,top+size}; Gdiplus::Bitmap* src=nullptr;ComPtr<ID2D1Bitmap>* cache=nullptr;
        if(klass==8){src=resolution8kBitmap_.get();cache=&libraryD2dResolution8k_;}
        else if(klass==5){src=resolution5kBitmap_.get();cache=&libraryD2dResolution5k_;}
        else if(klass==4){src=resolution4kBitmap_.get();cache=&libraryD2dResolution4k_;}
        if(src && cache){
            if(auto* bmp=GetLibraryGpuUiBitmap(src,*cache))DrawLibraryGpuBitmapCentered(bmp,tag);
            else{FillLibraryGpuRound(tag,RGB(16,19,25),8);DrawLibraryGpuText(std::to_wstring(klass)+L"K",tag,10,FW_BOLD,RGB(230,234,242),DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
        }
    }

    void ReleaseDetailsGpuWorkingSet() {
        if(!detailsGpuWorkingSetActive_) return;
        auto clearMedia=[](std::vector<MediaItem>& list){for(auto& item:list){item.detailsGpuThumb.Reset();item.detailsGpuThumbSource=nullptr;item.detailsGpuGeneration=0;}};
        clearMedia(videos_);clearMedia(images_);
        auto clearFrames=[](std::vector<PreviewFrame>& frames){for(auto& frame:frames){frame.gpuBitmap.Reset();frame.gpuBitmapSource=nullptr;frame.gpuGeneration=0;}};
        clearFrames(previewFrames_);for(auto& kv:prefetchedPreviewSets_)clearFrames(kv.second.frames);
        detailsGpuWorkingSetActive_=false;
    }

    bool PaintDetailsGpu(RECT rc) {
        if(!EnsureLibraryGpuRenderer(rc)) return false;
        previewHitRects_.clear();previewMediaHoverHits_.clear();previewZoomRect_=RECT{};detailsMediaRect_=RECT{};timelineReturnHighlightRect_=RECT{};
        auto& list=CurrentItems(); if(selected_>=list.size()) return false; MediaItem& item=list[selected_];
        ClampDetailsScroll();
        const int footerTop=std::max(0,static_cast<int>(rc.bottom)-64),contentOffset=detailsScrollY_;int y=18-contentOffset;
        std::set<int> visiblePreviewGpuSeconds;
        libraryD2dTarget_->BeginDraw(); const D2D1_COLOR_F clearColor=D2DColor(RGB(13,15,20));libraryD2dTarget_->Clear(&clearColor);
        RECT title{40,y,rc.right-40,y+42};DrawLibraryGpuText(item.title,title,30,FW_BOLD);y+=54;

        const int heroH=DetailsHeroHeightForViewport(static_cast<int>(rc.right-rc.left),footerTop,item.isVideo);RECT media{40,y,rc.right-40,y+heroH};
        if(!item.isVideo&&nativeImageSizing_)media=RECT{0,0,rc.right,rc.bottom};detailsMediaRect_=media;
        if(media.bottom>0&&media.top<footerTop){
            FillLibraryGpuRect(media,RGB(20,23,31));
            const int reqW=std::min(2560,std::max(1,static_cast<int>(media.right-media.left))),reqH=std::min(1440,std::max(1,static_cast<int>(media.bottom-media.top)));
            HBITMAP hbmp=nullptr;if(item.isVideo&&!PathExistsNoThrow(item.cachePath))hbmp=GetItemThumb(item,640,360);else hbmp=GetDetailsBanner(item);
            if(hbmp){
                ID2D1Bitmap* bmp=GetDetailsGpuBitmap(item,hbmp);
                if(!bmp){
                    const HRESULT fallbackHr=libraryD2dTarget_->EndDraw();
                    if(fallbackHr==D2DERR_RECREATE_TARGET) ResetLibraryGpuRenderer();
                    return false;
                }
                {
                    if(item.isVideo)DrawLibraryGpuBitmapCover(bmp,media);
                    else if(slideshowFadeActive_&&slideshowPreviousIndex_<images_.size()){
                        MediaItem& previousItem=images_[slideshowPreviousIndex_];HBITMAP prevH=nativeImageSizing_?GetDetailsBanner(previousItem):GetItemThumb(previousItem,reqW,reqH);
                        if(prevH){if(auto* prev=GetDetailsGpuBitmap(previousItem,prevH)){
                            if(nativeImageSizing_&&fullscreen_)DrawDetailsGpuBitmapContain(prev,media,1.0f,false,true);
                            else if(nativeImageSizing_)DrawDetailsGpuBitmapContain(prev,media,1.0f,true,false);
                            else DrawDetailsGpuBitmapContain(prev,media);
                        }}
                        const float progress=EaseUi(static_cast<float>(GetTickCount64()-slideshowFadeStart_)/static_cast<float>(kUiAnimationDurationMs));
                        if(nativeImageSizing_&&fullscreen_)DrawDetailsGpuBitmapContain(bmp,media,progress,false,true);
                        else if(nativeImageSizing_)DrawDetailsGpuBitmapContain(bmp,media,progress,true,false);
                        else DrawDetailsGpuBitmapContain(bmp,media,progress);
                    }else{
                        if(nativeImageSizing_&&fullscreen_)DrawDetailsGpuBitmapContain(bmp,media,1.0f,false,true);
                        else if(nativeImageSizing_)DrawDetailsGpuBitmapContain(bmp,media,1.0f,true,false);
                        else if(ImageZoomActive())DrawDetailsGpuBitmapContain(bmp,media,1.0f,false,false,imageZoomScale_,imageZoomCenterU_,imageZoomCenterV_);
                        else DrawDetailsGpuBitmapContain(bmp,media);
                    }
                    if(item.isVideo)DrawDetailsGpuVideoBadges(item,media);
                }
            }
            if(item.favorite)DrawLibraryGpuFavoriteBadge(media);
        }
        y+=heroH+22;

        if(item.isVideo){
            const int zoomTop=std::max(0,y);if(zoomTop<footerTop&&rc.right>80)previewZoomRect_=RECT{40,zoomTop,rc.right-40,footerTop};
            const int gap=12,cardW=DetailsPreviewCardWidthForViewport(static_cast<int>(rc.right-rc.left));
            const int imageH=std::max(79,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0))),labelH=24,cardH=imageH+labelH;
            const int availW=std::max(1,static_cast<int>(rc.right)-80),cols=std::max(1,(availW+gap)/(cardW+gap));
            if(previewFrames_.empty()){
                SetMediaHoverTarget(MediaHoverSurface::Preview,static_cast<size_t>(-1),RECT{},false);RECT note{40,y,rc.right-40,y+54};
                const bool complete=!previewDir_.empty()&&PreviewCacheIsComplete();DrawLibraryGpuText(complete?L"No secondary previews were available for this video.":L"Loading Timeline",note,14,FW_NORMAL,RGB(160,167,180));y+=64;
            }else{
                const int rows=static_cast<int>((previewFrames_.size()+static_cast<size_t>(cols)-1)/static_cast<size_t>(cols)),rowStride=cardH+gap;
                const int startRow=std::max(0,((0-y)/std::max(1,rowStride))-1),endRow=std::min(rows-1,((footerTop-y)/std::max(1,rowStride))+1);
                POINT cursor{};const bool cursorValid=GetCursorPos(&cursor)!=FALSE&&ScreenToClient(hwnd_,&cursor)!=FALSE;bool hoverFound=false;size_t hoverId=static_cast<size_t>(-1);RECT hoverRect{};
                if(cursorValid){RECT viewport{0,0,rc.right,footerTop};for(int row=startRow;row<=endRow&&!hoverFound;++row){for(int col=0;col<cols;++col){const size_t i=static_cast<size_t>(row)*cols+col;if(i>=previewFrames_.size())break;RECT card{40+col*(cardW+gap),y+row*(cardH+gap),40+col*(cardW+gap)+cardW,y+row*(cardH+gap)+cardH};if(card.bottom<0||card.top>footerTop)continue;RECT hit{};if(IntersectRect(&hit,&card,&viewport)&&PtInRect(&hit,cursor)){hoverFound=true;hoverId=i;hoverRect=card;break;}}}}
                SetMediaHoverTarget(MediaHoverSurface::Preview,hoverId,hoverRect,hoverFound);
                for(int row=startRow;row<=endRow;++row){for(int col=0;col<cols;++col){
                    const size_t i=static_cast<size_t>(row)*cols+col;if(i>=previewFrames_.size())break;
                    RECT card{40+col*(cardW+gap),y+row*(cardH+gap),40+col*(cardW+gap)+cardW,y+row*(cardH+gap)+cardH};
                    if(card.bottom<0||card.top>footerTop)continue;
                    RECT viewport{0,0,rc.right,footerTop},hit{};
                    if(IntersectRect(&hit,&card,&viewport)){previewHitRects_.push_back({hit,previewFrames_[i].seekSeconds});previewMediaHoverHits_.push_back({hit,card,i});}
                    if(card.right<=0||card.left>=rc.right)continue;
                    FillLibraryGpuRound(card,RGB(28,32,42),9);
                    RECT image=card;image.bottom=image.top+imageH;
                    ComPtr<ID2D1RoundedRectangleGeometry> clipGeometry;ComPtr<ID2D1Layer> clipLayer;
                    const bool clipped=PushLibraryGpuRoundedCardClip(card,9.0f,clipGeometry,clipLayer);
                    if(LibraryHoverPreviewFrame* hoverFrame=ActiveTimelineHoverPreviewFrame(i)){
                        if(hoverFrame->bitmap){if(auto* gpu=GetLibraryHoverPreviewGpuBitmap(*hoverFrame,hoverFrame->bitmap))DrawLibraryGpuBitmapCover(gpu,image);else FillLibraryGpuRect(image,RGB(43,48,61));}
                        else FillLibraryGpuRect(image,RGB(43,48,61));
                    }else{
                        HBITMAP ph=GetPreviewBitmap(previewFrames_[i],cardW,imageH);
                        if(ph){visiblePreviewGpuSeconds.insert(previewFrames_[i].seconds);if(auto* gpu=GetPreviewGpuBitmap(previewFrames_[i],ph))DrawLibraryGpuBitmapCover(gpu,image);else FillLibraryGpuRect(image,RGB(43,48,61));}
                        else FillLibraryGpuRect(image,RGB(43,48,61));
                    }
                    if(clipped)libraryD2dTarget_->PopLayer();
                    RECT label{card.left+8,image.bottom,card.right-8,card.bottom};
                    DrawLibraryGpuText(PreviewLabel(previewFrames_[i].seconds),label,11,FW_SEMIBOLD,RGB(200,206,218),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                    const float returnAmount=TimelineReturnHighlightAmount(i);
                    if(returnAmount>0.0f)timelineReturnHighlightRect_=card;
                    DrawLibraryGpuHoverBorder(card,std::max(MediaHoverAmount(MediaHoverSurface::Preview,i,card),returnAmount),9);
                }}
                y+=rows*(cardH+gap)+10;TrimPreviewMemory();
            }
        }
        for(auto& frame:previewFrames_){if(frame.gpuBitmap&&visiblePreviewGpuSeconds.find(frame.seconds)==visiblePreviewGpuSeconds.end()){frame.gpuBitmap.Reset();frame.gpuBitmapSource=nullptr;frame.gpuGeneration=0;}}
        MediaItem* slideshowPreviousItem=(!item.isVideo&&slideshowFadeActive_&&slideshowPreviousIndex_<images_.size())?&images_[slideshowPreviousIndex_]:nullptr;
        auto trimHeroGpu=[&](std::vector<MediaItem>& items){for(auto& candidate:items){const bool keep=(&candidate==&item)||(&candidate==slideshowPreviousItem);if(!keep){candidate.detailsGpuThumb.Reset();candidate.detailsGpuThumbSource=nullptr;candidate.detailsGpuGeneration=0;}}};
        trimHeroGpu(videos_);trimHeroGpu(images_);
        detailsContentBottom_=y+20+contentOffset;

        constexpr int edgeW=48,edgeH=76,edgePad=16;const int edgeCenterY=footerTop/2,edgeTop=std::max(8,edgeCenterY-edgeH/2);
        detailsPrevRect_={edgePad,edgeTop,edgePad+edgeW,edgeTop+edgeH};detailsNextRect_={std::max(edgePad,static_cast<int>(rc.right)-edgePad-edgeW),edgeTop,std::max(edgePad+edgeW,static_cast<int>(rc.right)-edgePad),edgeTop+edgeH};
        DrawDetailsGpuEdgeArrowButton(detailsPrevRect_,false,CanNavigateDetailsMedia(-1));DrawDetailsGpuEdgeArrowButton(detailsNextRect_,true,CanNavigateDetailsMedia(1));

        detailsFooterRect_=RECT{0,footerTop,rc.right,rc.bottom};FillLibraryGpuRectAlpha(detailsFooterRect_,RGB(16,19,25),238.0f/255.0f);DrawLibraryGpuLine(0.0f,static_cast<float>(footerTop),static_cast<float>(rc.right),static_cast<float>(footerTop),RGB(42,47,60),1);
        backRect_={20,rc.bottom-58,68,rc.bottom-10};DrawLibraryGpuSvgIconButton(backRect_,backSvgBitmap_.get(),libraryD2dBackSvg_,false,5);
        if(item.isVideo){imageDetailsSlideshowRect_=RECT{};const int gap=10;playRect_={backRect_.right+gap,backRect_.top,backRect_.right+gap+48,backRect_.bottom};DrawLibraryGpuSvgIconButton(playRect_,playSvgBitmap_.get(),libraryD2dPlaySvg_,false,6);}else playRect_=RECT{};
        constexpr int footerIconW=48,footerGap=10,footerRightMargin=20;const int footerRight=std::max(0,static_cast<int>(rc.right)-footerRightMargin),iconBottom=rc.bottom-10,iconTop=iconBottom-footerIconW;
        detailsFullRect_={footerRight-footerIconW,iconTop,footerRight,iconBottom};DrawLibraryGpuFullscreenButton(detailsFullRect_);imageDetailsNativeRect_=RECT{};
        if(!item.isVideo){imageDetailsNativeRect_={detailsFullRect_.left-footerGap-footerIconW,iconTop,detailsFullRect_.left-footerGap,iconBottom};DrawDetailsGpuNativeSizeButton(imageDetailsNativeRect_);imageDetailsSlideshowRect_={imageDetailsNativeRect_.left-footerGap-footerIconW,iconTop,imageDetailsNativeRect_.left-footerGap,iconBottom};DrawLibraryGpuAutoAdvance(imageDetailsSlideshowRect_,slideshowActive_);}
        std::wstring meta=item.isVideo?(item.vr.vr?(item.vr.projection==2?L"VR180":L"VR"):L"Video"):L"Image";
        if(item.isVideo&&item.sourceWidth&&item.sourceHeight){meta+=L"  •  ";meta+=std::to_wstring(item.sourceWidth);meta+=L"×";meta+=std::to_wstring(item.sourceHeight);}
        const LONG metaLeftLimit=item.isVideo?playRect_.right+20:backRect_.right+20,metaRightLimit=(item.isVideo?detailsFullRect_.left:imageDetailsSlideshowRect_.left)-20;
        const LONG metaLeft=std::max<LONG>(metaLeftLimit,rc.right/2-150),metaRight=std::max<LONG>(metaLeft+40,std::min<LONG>(metaRightLimit,rc.right/2+150));RECT metaTop{metaLeft,rc.bottom-62,metaRight,rc.bottom-43};
        DrawLibraryGpuText(meta,metaTop,13,FW_SEMIBOLD,RGB(165,172,185),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(item.isVideo){const double duration=detailsDurationSeconds_.load(std::memory_order_relaxed);RECT durationRect{metaTop.left,rc.bottom-43,metaTop.right,rc.bottom-8};DrawLibraryGpuText(duration>0.0?FormatTime(duration):L"--:--",durationRect,22,FW_SEMIBOLD,RGB(205,210,220),DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
        PaintLibraryGpuLoadingPopup(rc);PaintLibraryGpuNotice(rc);
        const HRESULT hr=libraryD2dTarget_->EndDraw();if(hr==D2DERR_RECREATE_TARGET){ResetLibraryGpuRenderer();return false;}return SUCCEEDED(hr);
    }

    static constexpr uint64_t kMiB=1024ull*1024ull;
    static constexpr uint64_t kGiB=1024ull*kMiB;

    // The RAM cache is made of HBITMAP objects, so byte limits alone are not enough.
    // Windows also imposes a per-process GDI-object quota (commonly ~10,000). Keep
    // generous headroom for Timeline/detail bitmaps, fonts, DCs and transient decoder
    // objects even on 32/64-GB machines where the byte budget is several gigabytes.
    static constexpr DWORD kGdiObjectSoftPressure=6000u;
    static constexpr DWORD kGdiObjectHardPressure=8000u;
    static constexpr size_t kLibraryWarmBitmapHandleCap=3500u;
    static constexpr int kLibraryProtectedRows=10;

    static DWORD ProcessGdiObjectCount() {
        return GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    }

    size_t LibraryBitmapHandleBudget() const {
        const DWORD gdi=ProcessGdiObjectCount();
        if(gdi>=kGdiObjectHardPressure) return 800u;
        if(gdi>=kGdiObjectSoftPressure) return 1800u;
        return kLibraryWarmBitmapHandleCap;
    }

    using ProcessMemoryPolicy = vmp::MemoryCachePolicy;

    ProcessMemoryPolicy CurrentProcessMemoryPolicy() const {
        // Installed RAM does not change while the app is running. The pure policy helper
        // rounds nominal capacities and intentionally caps all machines at the 32-GiB+ tier.
        static const ProcessMemoryPolicy policy=[](){
            MEMORYSTATUSEX state{};state.dwLength=sizeof(state);
            const uint64_t total=GlobalMemoryStatusEx(&state)
                ? static_cast<uint64_t>(state.ullTotalPhys)
                : 8ull*kGiB;
            return vmp::SelectMemoryCachePolicy(total);
        }();
        return policy;
    }

    // These are cache-pressure boundaries, not caps on active playback. Decoder surfaces,
    // current video textures and Media Foundation buffers are working memory and must be
    // allowed to grow when high-resolution/VR media genuinely requires it. The policy
    // below sheds reconstructable Library/preview state around the player instead.

    uint64_t ProcessMemoryBytes() const {
        PROCESS_MEMORY_COUNTERS_EX counters{};counters.cb=sizeof(counters);
        if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),sizeof(counters))) return 0;
        // Use the more conservative of private commit and resident working set. This keeps
        // the pressure policy aligned with both allocation growth and what users see as
        // process RAM in common task-manager views.
        return std::max<uint64_t>(static_cast<uint64_t>(counters.PrivateUsage),
                                  static_cast<uint64_t>(counters.WorkingSetSize));
    }

    bool LoadEverythingOwnsMemoryPressure() const {
        return fullLoadRunning_.load(std::memory_order_acquire) && mode_!=Mode::Player;
    }

    vmp::SystemMemoryPressure CurrentSystemMemoryPressure() const {
        MEMORYSTATUSEX state{};state.dwLength=sizeof(state);
        if(!GlobalMemoryStatusEx(&state)) return vmp::SystemMemoryPressure::Normal;
        return vmp::ClassifySystemMemoryPressure(CurrentProcessMemoryPolicy(),
                                                 static_cast<uint64_t>(state.ullAvailPhys),
                                                 static_cast<uint32_t>(state.dwMemoryLoad));
    }

    bool SystemMemoryCriticallyLow() const {
        // Only genuine machine-wide critical pressure is allowed to collapse the warm
        // thumbnail cache below its tier target. Ordinary idle time never counts.
        return CurrentSystemMemoryPressure()==vmp::SystemMemoryPressure::Critical;
    }

    uint64_t LibraryGpuBudgetBytes(uint64_t processBytes) const {
        const ProcessMemoryPolicy policy=CurrentProcessMemoryPolicy();
        if(LoadEverythingOwnsMemoryPressure() && !SystemMemoryCriticallyLow()) return 192ull*kMiB;
        if(processBytes>=policy.emergency) return 64ull*kMiB;
        if(processBytes>=policy.highPressure) return 96ull*kMiB;
        if(processBytes>=policy.normalProcess) return 128ull*kMiB;
        return 192ull*kMiB;
    }

    void MarkLibraryBrowsingActivity() {
        lastLibraryActivityTick_=GetTickCount64();
        libraryIdleCacheTrimmed_=false;
    }

    void MaybeTrimIdleLibraryCache(ULONGLONG now) {
        if(!hwnd_) return;
        const bool background=!IsAppForegroundForHover();
        if(mode_!=Mode::Library && !background) return;
        if(lastLibraryActivityTick_==0) lastLibraryActivityTick_=now;
        const bool idle=mode_==Mode::Library && !libraryScrollDragging_ &&
                        now-lastLibraryActivityTick_>=kLibraryIdleTrimDelayMs;
        if(!background && !idle) return;
        if(libraryIdleCacheTrimmed_ && now-lastLibraryIdleTrimTick_<kLibraryIdleTrimRepeatMs) return;

        // Idle time is not memory pressure. Drop reconstructable GPU copies, but keep the
        // CPU thumbnail cache warm. TrimThumbMemory() is hysteretic and therefore becomes
        // a no-op while the cache remains below its tier soft ceiling.
        TrimLibraryGpuTextures();
        TrimThumbMemory();
        lastLibraryIdleTrimTick_=now;
        libraryIdleCacheTrimmed_=true;
    }

    void EnforceProcessMemoryBudget() {
        const ULONGLONG now=GetTickCount64();
        if(now-lastMemoryPressureCheck_<250) return;
        lastMemoryPressureCheck_=now;
        const ProcessMemoryPolicy policy=CurrentProcessMemoryPolicy();
        const bool systemCritical=SystemMemoryCriticallyLow();
        const DWORD gdiObjects=ProcessGdiObjectCount();
        const bool gdiPressure=gdiObjects>=kGdiObjectSoftPressure;
        uint64_t processBytes=ProcessMemoryBytes();
        MaybeTrimIdleLibraryCache(now);
        // Quiet time is a convenient maintenance point, not a reason to collapse the cache.
        // Below the tier soft ceiling this call does nothing; above it, cold LRU thumbnails
        // are trimmed only back to the warm target (for example 10 -> ~8 GiB on 32-GiB+).
        if(mode_==Mode::Library && !IsLibraryInteractionHot()) TrimThumbMemory();
        processBytes=ProcessMemoryBytes();
        if(processBytes<policy.normalProcess && !systemCritical && !gdiPressure) return;

        // All reconstructable thumbnail pressure flows through the same LRU controller.
        // Explicit batch work no longer disables the 16-GB-class process safety boundary.
        TrimLibraryGpuTextures();
        TrimThumbMemory();
        TrimPreviewMemory();
        processBytes=ProcessMemoryBytes();
        if(processBytes>=policy.allocationGuard || systemCritical || gdiObjects>=kGdiObjectHardPressure){
            ClearPrefetchedPreviewSets();
            TrimDetailInfoToWindow(DetailWindowIndices());
            ResetResolutionMetadataWork();
            processBytes=ProcessMemoryBytes();
        }
        if(processBytes>=policy.panicRelease || gdiObjects>=kGdiObjectHardPressure){
            // At the hard boundary retain the actually visible Library cards, but release
            // reconstructable off-screen state aggressively. Player/decoder allocations are
            // not forcibly reset because they are not part of the thumbnail cache policy.
            auto purgeCold=[&](std::vector<MediaItem>& list){
                for(auto& item:list){
                    const bool visible=visibleLibraryGpuThumbPaths_.find(item.path)!=visibleLibraryGpuThumbPaths_.end();
                    const bool playbackWarm=playbackLibraryWarmPaths_.find(item.path)!=playbackLibraryWarmPaths_.end();
                    if(visible || playbackWarm) continue;
                    if(item.thumb){DeleteObject(item.thumb);item.thumb=nullptr;}
                    item.libraryGpuThumb.Reset();item.libraryGpuThumbSource=nullptr;item.libraryGpuGeneration=0;
                    item.thumbW=item.thumbH=0;item.thumbAttempted=false;item.thumbFromPrivateCache=false;
                    item.thumbLoadRequestEpoch=0;item.thumbNextLoadAttempt=0;
                }
            };
            purgeCold(videos_);purgeCold(images_);
            libraryWorkingSetThumbPaths_.clear();
            ClearPrefetchedPreviewSets();
            TrimDetailInfoToWindow(DetailWindowIndices());
            TrimLibraryGpuTextures();
            if(hwnd_) InvalidateRect(hwnd_,nullptr,FALSE);
        }
    }

    std::set<std::wstring> CaptureLibraryReturnWarmPaths(int marginRows=2) {
        std::set<std::wstring> warm;
        if(!hwnd_) return warm;
        RECT rc{};GetClientRect(hwnd_,&rc);
        const auto& filtered=FilteredIndices();
        const auto visibleFolders=VisibleFolderIndices();
        const size_t totalCards=visibleFolders.size()+filtered.size();
        if(totalCards==0) return warm;
        const auto& list=CurrentItems();
        const int pad=kLibraryPad,gap=kLibraryGap,cardW=libraryCardWidth_;
        const int imageH=std::max(113,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int rowStride=imageH+kLibraryTitleHeight+gap;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left)-kLibraryScrollbarReserve);
        const int cols=std::max(1,(clientWidth-pad*2+gap)/(cardW+gap));
        const int rows=static_cast<int>((totalCards+static_cast<size_t>(cols)-1)/static_cast<size_t>(cols));
        if(rows<=0) return warm;
        const int visibleBottom=std::max(pad,static_cast<int>(rc.bottom)-68);
        const int firstVisible=std::clamp(scrollY_/std::max(1,rowStride)-1,0,rows-1);
        const int lastVisible=std::clamp((scrollY_+std::max(0,visibleBottom-pad))/std::max(1,rowStride)+1,0,rows-1);
        const int firstRow=std::max(0,firstVisible-std::max(0,marginRows));
        const int lastRow=std::min(rows-1,lastVisible+std::max(0,marginRows));
        for(int row=firstRow;row<=lastRow;++row){
            const size_t rowFirst=static_cast<size_t>(row)*static_cast<size_t>(cols);
            const size_t rowLast=std::min(totalCards,rowFirst+static_cast<size_t>(cols));
            for(size_t displayIndex=rowFirst;displayIndex<rowLast;++displayIndex){
                if(displayIndex<visibleFolders.size()) continue;
                const size_t mediaDisplayIndex=displayIndex-visibleFolders.size();
                if(mediaDisplayIndex>=filtered.size()) continue;
                const size_t itemIndex=filtered[mediaDisplayIndex];
                if(itemIndex<list.size()) warm.insert(list[itemIndex].path);
            }
        }
        if(category_==Category::Videos && selected_<videos_.size()) warm.insert(videos_[selected_].path);
        return warm;
    }

    void TrimForPlayback() {
        // Preserve the exact Library viewport plus a small row margin as highest-priority
        // CPU state. Playback no longer collapses a healthy Library cache by itself; the
        // normal tier hysteresis or genuine memory pressure decides whether cold LRU data goes.
        playbackLibraryWarmPaths_=CaptureLibraryReturnWarmPaths(2);
        ResetLibraryThumbLoadView();
        protectedLibraryThumbPaths_=playbackLibraryWarmPaths_;
        visibleLibraryGpuThumbPaths_.clear();
        // GPU Library residency has no value while video covers the Library; it can be
        // reconstructed quickly from the protected CPU thumbnails on return.
        ResetLibraryGpuRenderer();
        ClearPrefetchedPreviewSets();
        TrimPreviewMemory();
        TrimThumbMemory();
    }

    void TrimLibraryGpuTextures() {
        const uint64_t processBytes=ProcessMemoryBytes();
        const uint64_t budget=LibraryGpuBudgetBytes(processBytes);
        struct Entry{MediaItem* item;ULONGLONG used;uint64_t bytes;bool visible;bool nearby;};
        std::vector<Entry> entries;uint64_t total=0;
        auto collect=[&](std::vector<MediaItem>& list){
            for(auto& item:list){
                if(!item.libraryGpuThumb) continue;
                const D2D1_SIZE_U gpuPx=item.libraryGpuThumb->GetPixelSize();
                const uint64_t bytes=static_cast<uint64_t>(std::max<UINT32>(1,gpuPx.width))*static_cast<uint64_t>(std::max<UINT32>(1,gpuPx.height))*4ull;
                const bool visible=visibleLibraryGpuThumbPaths_.find(item.path)!=visibleLibraryGpuThumbPaths_.end();
                const bool nearby=protectedLibraryThumbPaths_.find(item.path)!=protectedLibraryThumbPaths_.end();
                total+=bytes;entries.push_back({&item,item.thumbLastUsed,bytes,visible,nearby});
            }
        };
        collect(videos_);collect(images_);
        auto release=[&](Entry& e){if(!e.item->libraryGpuThumb)return;e.item->libraryGpuThumb.Reset();e.item->libraryGpuThumbSource=nullptr;e.item->libraryGpuGeneration=0;total=total>e.bytes?total-e.bytes:0;};
        // Distant GPU copies are never permanent cache entries. This is independent of
        // the byte budget and guarantees viewport/nearby residency semantics.
        for(auto& e:entries) if(!e.nearby) release(e);
        const ProcessMemoryPolicy policy=CurrentProcessMemoryPolicy();
        if(processBytes>=policy.emergency && !(LoadEverythingOwnsMemoryPressure() && !SystemMemoryCriticallyLow())){for(auto& e:entries) if(!e.visible) release(e);}
        if(total<=budget) return;
        std::sort(entries.begin(),entries.end(),[](const Entry&a,const Entry&b){if(a.visible!=b.visible)return !a.visible;return a.used<b.used;});
        for(auto& e:entries){if(total<=budget)break;if(e.visible)continue;release(e);}
    }

    bool PaintLibraryGpu(RECT rc) {
        if(!EnsureLibraryGpuRenderer(rc)) return false;
        ReleaseDetailsGpuWorkingSet();
        auto& mutableList=CurrentItems();const auto& filtered=FilteredIndices();const auto visibleFolders=VisibleFolderIndices();
        const size_t totalCards=visibleFolders.size()+filtered.size();
        libraryMediaHoverHits_.clear();libraryReturnHighlightRect_=RECT{};visibleLibraryGpuThumbPaths_.clear();

        // Paint is render-only. It must never touch disk, decode JPEGs, validate cache files,
        // rebuild the prefetch queue, or trim the multi-GB RAM working set. Those operations
        // are scheduled independently by the low-priority Library prefetch/settle timers.
        libraryD2dTarget_->BeginDraw();
        const D2D1_COLOR_F clearColor=D2DColor(RGB(13,15,20));libraryD2dTarget_->Clear(&clearColor);

        if(totalCards==0){
            std::wstring msg;
            if(libraryScanRunning_) msg=L"Scanning library...";
            else if(folder_.empty() || currentFolder_.empty()) msg=L"Choose a folder to load videos and images.";
            else if(loadFailureFilterActive_) msg=category_==Category::Videos?L"No failed videos.":L"No failed images.";
            else if(!searchQuery_.empty()) msg=L"No matching media.";
            else msg=category_==Category::Videos?L"No videos or subfolders here.":L"No images or subfolders here.";
            RECT mr{40,40,rc.right-40,128};DrawLibraryGpuText(msg,mr,25,FW_SEMIBOLD,RGB(180,185,197),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            PaintLibraryGpuNavigator(rc);if(searchVisible_)PaintLibraryGpuSearch(rc);PaintLibraryGpuLoadingPopup(rc);PaintLibraryGpuNotice(rc);
            const HRESULT hr=libraryD2dTarget_->EndDraw();if(hr==D2DERR_RECREATE_TARGET){ResetLibraryGpuRenderer();return false;}return SUCCEEDED(hr);
        }

        const int pad=kLibraryPad,gap=kLibraryGap,cardW=libraryCardWidth_;
        const int imageH=std::max(113,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0))),cardH=imageH+kLibraryTitleHeight,rowStride=cardH+gap;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left)-kLibraryScrollbarReserve);
        const int cols=std::max(1,(clientWidth-pad*2+gap)/(cardW+gap));
        const int rows=static_cast<int>((totalCards+static_cast<size_t>(cols)-1)/static_cast<size_t>(cols));
        const int startY=pad-scrollY_,visibleBottom=std::max(pad,static_cast<int>(rc.bottom)-68);
        const int firstVisibleRow=std::clamp(scrollY_/std::max(1,rowStride)-1,0,std::max(0,rows-1));
        const int lastVisibleRow=std::clamp((scrollY_+std::max(0,visibleBottom-pad))/std::max(1,rowStride)+1,0,std::max(0,rows-1));
        const size_t firstDisplay=static_cast<size_t>(firstVisibleRow)*static_cast<size_t>(cols),lastDisplay=std::min(totalCards,static_cast<size_t>(lastVisibleRow+1)*static_cast<size_t>(cols));

        POINT cursor{};const bool cursorValid=GetCursorPos(&cursor)!=FALSE&&ScreenToClient(hwnd_,&cursor)!=FALSE;bool hoverFound=false;size_t hoverId=static_cast<size_t>(-1);RECT hoverRect{};
        if(cursorValid){RECT viewport{0,pad,rc.right,visibleBottom};for(size_t displayIndex=firstDisplay;displayIndex<lastDisplay;++displayIndex){if(displayIndex<visibleFolders.size())continue;const int col=static_cast<int>(displayIndex)%cols,row=static_cast<int>(displayIndex)/cols;RECT card{pad+col*(cardW+gap),startY+row*rowStride,pad+col*(cardW+gap)+cardW,startY+row*rowStride+cardH};RECT hit{};if(!IntersectRect(&hit,&card,&viewport)||!PtInRect(&hit,cursor))continue;const size_t mdi=displayIndex-visibleFolders.size();if(mdi>=filtered.size())continue;hoverFound=true;hoverId=filtered[mdi];hoverRect=card;break;}}
        SetMediaHoverTarget(MediaHoverSurface::Library,hoverId,hoverRect,hoverFound);

        for(size_t displayIndex=firstDisplay;displayIndex<lastDisplay;++displayIndex){
            const int col=static_cast<int>(displayIndex)%cols,row=static_cast<int>(displayIndex)/cols;
            RECT card{pad+col*(cardW+gap),startY+row*rowStride,pad+col*(cardW+gap)+cardW,startY+row*rowStride+cardH};
            if(card.bottom<pad||card.top>visibleBottom)continue;
            if(displayIndex<visibleFolders.size()){DrawLibraryGpuFolderCard(folders_[visibleFolders[displayIndex]],card);continue;}
            const size_t mdi=displayIndex-visibleFolders.size();if(mdi>=filtered.size())continue;const size_t i=filtered[mdi];MediaItem& item=mutableList[i];
            protectedLibraryThumbPaths_.insert(item.path);visibleLibraryGpuThumbPaths_.insert(item.path);
            RECT viewport{0,pad,rc.right,visibleBottom},hit{};if(IntersectRect(&hit,&card,&viewport))libraryMediaHoverHits_.push_back({hit,card,i});
            FillLibraryGpuRound(card,RGB(31,35,46),12);RECT image=card;image.bottom=image.top+imageH;
            ComPtr<ID2D1RoundedRectangleGeometry> clipGeometry;ComPtr<ID2D1Layer> clipLayer;
            const bool clipped=PushLibraryGpuRoundedCardClip(card,12.0f,clipGeometry,clipLayer);
            if(LibraryHoverPreviewFrame* hoverFrame=ActiveLibraryHoverPreviewFrame(i)){
                if(hoverFrame->bitmap){
                    if(ID2D1Bitmap* gpu=GetLibraryHoverPreviewGpuBitmap(*hoverFrame,hoverFrame->bitmap)) DrawLibraryGpuBitmapCover(gpu,image);
                    else FillLibraryGpuRect(image,RGB(43,48,61));
                }else FillLibraryGpuRect(image,RGB(43,48,61));
            }else{
                HBITMAP thumb=GetResidentLibraryItemThumb(item);
                if(thumb){if(ID2D1Bitmap* gpu=GetLibraryGpuThumb(item,thumb,image.right-image.left,image.bottom-image.top))DrawLibraryGpuBitmapCover(gpu,image);else{FillLibraryGpuRect(image,RGB(43,48,61));}}
                else{FillLibraryGpuRect(image,RGB(43,48,61));}
            }
            if(clipped)libraryD2dTarget_->PopLayer();
            RECT title{card.left+10,image.bottom+2,card.right-10,card.bottom-3};DrawLibraryGpuText(item.title,title,14,FW_SEMIBOLD);
            if(item.favorite)DrawLibraryGpuFavoriteBadge(image);DrawLibraryGpuVideoBadges(item,card);
            const float returnAmount=LibraryReturnHighlightAmount(i);if(returnAmount>0.0f)libraryReturnHighlightRect_=card;
            DrawLibraryGpuHoverBorder(card,std::max(MediaHoverAmount(MediaHoverSurface::Library,i,card),returnAmount),12);
        }

        RECT topMask{0,0,rc.right,kLibraryPad};FillLibraryGpuRect(topMask,RGB(13,15,20));
        PaintLibraryGpuScrollbar(rc);PaintLibraryGpuNavigator(rc);if(searchVisible_)PaintLibraryGpuSearch(rc);PaintLibraryGpuLoadingPopup(rc);PaintLibraryGpuNotice(rc);
        const HRESULT drawHr=libraryD2dTarget_->EndDraw();if(drawHr==D2DERR_RECREATE_TARGET){ResetLibraryGpuRenderer();return false;}if(FAILED(drawHr))return false;
        // The normal Library viewport/prefetch set is live again, so return-specific
        // protection is no longer needed; normal LRU/prefetch behavior resumes here.
        playbackLibraryWarmPaths_.clear();
        return true;
    }

    void Paint() {
        paintOwner_ = hwnd_;
        // Preserve the true update region, not merely its bounding rectangle. Fullscreen
        // scrolling invalidates a few narrow strips; collapsing those into one 4K-sized
        // rectangle defeats the benefit and makes fullscreen feel heavier than windowed.
        HRGN updateRegion=CreateRectRgn(0,0,0,0);
        const int updateRegionType=updateRegion?GetUpdateRgn(hwnd_,updateRegion,FALSE):ERROR;
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd_, &ps);
        RECT rc{}; GetClientRect(hwnd_, &rc);
        const int w = std::max(1, static_cast<int>(rc.right-rc.left));
        const int h = std::max(1, static_cast<int>(rc.bottom-rc.top));
        if(mode_==Mode::Library && UseGpuLibraryRenderer(rc) && PaintLibraryGpu(rc)){
            EndPaint(hwnd_,&ps);
            if(updateRegion) DeleteObject(updateRegion);
            return;
        }
        if(mode_==Mode::Details && PaintDetailsGpu(rc)){
            EndPaint(hwnd_,&ps);
            if(updateRegion) DeleteObject(updateRegion);
            return;
        }
        EnsureBackBuffer(dc,w,h);

        RECT dirty = ps.rcPaint;
        if (IsRectEmpty(&dirty)) dirty = rc;
        const int savedDc = SaveDC(backDC_);
        if(updateRegion && updateRegionType!=ERROR && updateRegionType!=NULLREGION)
            SelectClipRgn(backDC_,updateRegion);
        else
            IntersectClipRect(backDC_, dirty.left, dirty.top, dirty.right, dirty.bottom);
        HBRUSH bg = CreateSolidBrush(RGB(13,15,20)); FillRect(backDC_,&rc,bg); DeleteObject(bg);
        SetBkMode(backDC_, TRANSPARENT);
        SetTextColor(backDC_, RGB(238,241,247));

        if (mode_ == Mode::Library) PaintLibrary(backDC_,rc);
        else if (mode_ == Mode::Details) PaintDetails(backDC_,rc);
        PaintLoadingPopup(backDC_, rc);
        PaintInAppNotice(backDC_, rc);
        RestoreDC(backDC_, savedDc);

        BitBlt(dc, dirty.left, dirty.top, dirty.right-dirty.left, dirty.bottom-dirty.top,
               backDC_, dirty.left, dirty.top, SRCCOPY);
        EndPaint(hwnd_,&ps);
        if(updateRegion) DeleteObject(updateRegion);
    }

    RECT AppNoticeRect(RECT rc) const {
        const bool compact=appNoticeText_.size()<=32;
        const int desiredW=compact?360:520;
        const int boxW=std::max(240,std::min(desiredW,std::max(240,static_cast<int>(rc.right)-40)));
        const int boxH=compact?54:64;
        const int right=std::max(20,static_cast<int>(rc.right)-18);
        const int top=(mode_==Mode::Library && searchVisible_)?68:18;
        return RECT{std::max(8,right-boxW),top,right,top+boxH};
    }

    static float BreathingHighlightAmountWithCadence(ULONGLONG elapsed, ULONGLONG duration, ULONGLONG cadenceDuration) {
        if(duration==0 || cadenceDuration==0 || elapsed>=duration) return 0.0f;
        // Keep the exact return-highlight cadence: four breaths every three seconds.
        // Longer-lived callers may extend the lifetime without slowing the breath rate.
        const float phaseT=static_cast<float>(elapsed)/static_cast<float>(cadenceDuration);
        const float breath=0.5f-0.5f*std::cos(8.0f*PI_F*phaseT);
        float envelope=1.0f;
        const ULONGLONG edgeMs=std::max<ULONGLONG>(1,static_cast<ULONGLONG>(std::llround(static_cast<double>(cadenceDuration)*0.06)));
        if(elapsed<edgeMs){
            const float x=std::clamp(static_cast<float>(elapsed)/static_cast<float>(edgeMs),0.0f,1.0f);
            envelope=x*x*(3.0f-2.0f*x);
        }else if(duration-elapsed<edgeMs){
            const float x=std::clamp(static_cast<float>(duration-elapsed)/static_cast<float>(edgeMs),0.0f,1.0f);
            envelope=x*x*(3.0f-2.0f*x);
        }
        return envelope*(0.28f+0.72f*breath);
    }

    static float BreathingHighlightAmount(ULONGLONG elapsed, ULONGLONG duration) {
        return BreathingHighlightAmountWithCadence(elapsed,duration,duration);
    }

    float AppNoticePulseAmount(ULONGLONG now) const {
        if(appNoticeStart_==0 || appNoticeUntil_<=appNoticeStart_ || now<appNoticeStart_ || now>=appNoticeUntil_) return 0.0f;
        const ULONGLONG elapsed=now-appNoticeStart_;
        if(elapsed>=kAppNoticePulseDurationMs) return 0.0f;
        // Same speed/intensity as the existing three-second return highlight, but
        // continue that exact cadence for the notice's full five-second lifetime.
        return BreathingHighlightAmountWithCadence(elapsed,kAppNoticePulseDurationMs,kLibraryReturnHighlightDurationMs);
    }

    void ShowInAppNotice(const std::wstring& text, ULONGLONG durationMs=5000) {
        appNoticeText_=text;
        appNoticeStart_=GetTickCount64();
        appNoticeUntil_=appNoticeStart_+durationMs;
        if(hwnd_){
            SetTimer(hwnd_,kAppNoticeTimerId,static_cast<UINT>(std::min<ULONGLONG>(durationMs,60000)),nullptr);
            StartUiAnimationTimer();
            InvalidateRect(hwnd_,nullptr,FALSE);
        }
    }

    void ClearInAppNotice() {
        appNoticeText_.clear(); appNoticeUntil_=0; appNoticeStart_=0;
        if(hwnd_) KillTimer(hwnd_,kAppNoticeTimerId);
    }

    void PaintInAppNotice(HDC dc, RECT rc) {
        const ULONGLONG now=GetTickCount64();
        if(appNoticeText_.empty() || now>=appNoticeUntil_) return;
        const RECT box=AppNoticeRect(rc);
        const float pulse=AppNoticePulseAmount(now);
        FillRound(dc,box,RGB(31,35,46),12);
        // Pulse for the entire notice lifetime with the exact same border renderer
        // used by the return-from-Info highlight.
        DrawMediaHoverBorder(dc,box,pulse,12);
        RECT text{box.left+16,box.top+8,box.right-16,box.bottom-8};
        DrawTextSimple(dc,appNoticeText_,text,15,FW_SEMIBOLD,RGB(238,241,247),DT_CENTER|DT_VCENTER|DT_WORDBREAK);
    }

    void SetLoadingState(int kind, int current, int total) {
        loadingCurrent_.store(std::max(0,current), std::memory_order_relaxed);
        loadingTotal_.store(std::max(0,total), std::memory_order_relaxed);
        loadingKind_.store(kind, std::memory_order_release);
        if(hwnd_) PostMessageW(hwnd_, WM_APP_THUMB_READY, 0, 0);
    }

    void ClearLoadingState() {
        loadingKind_.store(0, std::memory_order_release);
        loadingCurrent_.store(0, std::memory_order_relaxed);
        loadingTotal_.store(0, std::memory_order_relaxed);
        if(hwnd_) PostMessageW(hwnd_, WM_APP_THUMB_READY, 0, 0);
    }

    void ClearLoadingStateIf(int kind) {
        int expected=kind;
        if(loadingKind_.compare_exchange_strong(expected,0,std::memory_order_acq_rel)) {
            loadingCurrent_.store(0,std::memory_order_relaxed);
            loadingTotal_.store(0,std::memory_order_relaxed);
            if(hwnd_) PostMessageW(hwnd_, WM_APP_THUMB_READY, 0, 0);
        }
    }

    void PaintLoadingPopup(HDC dc, RECT rc) {
        if(mode_==Mode::Player) return;

        std::wstring label,count,fileName;
        const bool fullRunning=fullLoadRunning_.load(std::memory_order_acquire);
        const ULONGLONG now=GetTickCount64();
        const ULONGLONG fullDoneDuration=fullLoadFailures_.load(std::memory_order_relaxed)>0?kFullLoadFailedPopupDurationMs:kFullLoadDonePopupDurationMs;
        const bool fullDoneVisible=!fullRunning && fullLoadFinishedAt_!=0 && now-fullLoadFinishedAt_<fullDoneDuration;
        if(fullRunning) {
            label=L"Loading everything";
            const int current=fullLoadCurrent_.load(std::memory_order_relaxed);
            const int total=fullLoadTotal_.load(std::memory_order_relaxed);
            count=total>0?std::to_wstring(std::min(current,total))+L" / "+std::to_wstring(total)+L" files":L"Working...";
            fileName=FullLoadCurrentFile();
        } else if(fullDoneVisible) {
            const int failures=fullLoadFailures_.load(std::memory_order_relaxed);
            label=L"Load everything finished";
            if(failures==0) count=L"All files ready";
            else count=std::to_wstring(failures)+L" failed  •  Click to view";
        } else {
            const int kind=loadingKind_.load(std::memory_order_acquire);
            if(kind==0) return;
            const int current=loadingCurrent_.load(std::memory_order_relaxed);
            const int total=loadingTotal_.load(std::memory_order_relaxed);
            if(kind==1) label=L"Loading library banners";
            else if(kind==2) label=L"Loading secondary images";
            else if(kind==3) label=L"Loading info banner";
            else return;
            count=total>0?std::to_wstring(std::min(current,total))+L" / "+std::to_wstring(total):L"Working...";
        }

        // Match the compact in-app notice (for example "This folder is unavailable.") exactly.
        const RECT box=FullLoadPopupRect(rc);
        if(fullRunning && !fileName.empty()) {
            const std::wstring prefix=count+L"  •  ";
            const int available=std::max(0,static_cast<int>(box.right-box.left)-28);
            const int prefixWidth=MeasureHdcTextWidth(dc,prefix,12,FW_NORMAL);
            count=prefix+MiddleEllipsizeHdc(dc,fileName,std::max(0,available-prefixWidth),12,FW_NORMAL);
        }
        FillRound(dc,box,RGB(25,29,38),11);
        RECT labelRect{box.left+14,box.top+3,box.right-14,box.top+27};
        DrawTextSimple(dc,label,labelRect,13,FW_SEMIBOLD,RGB(238,241,247));
        RECT countRect{box.left+14,box.top+25,box.right-14,box.bottom-3};
        const bool failureAction=fullDoneVisible&&fullLoadFailures_.load(std::memory_order_relaxed)>0;
        DrawTextSimple(dc,count,countRect,12,failureAction?FW_SEMIBOLD:FW_NORMAL,failureAction?RGB(226,230,238):RGB(165,172,185),DT_LEFT|DT_VCENTER|DT_SINGLELINE);
    }

    void DeferBackgroundWork(ULONGLONG milliseconds = 90) {
        const ULONGLONG wanted = GetTickCount64() + milliseconds;
        ULONGLONG current = backgroundPauseUntil_.load(std::memory_order_relaxed);
        while (current < wanted && !backgroundPauseUntil_.compare_exchange_weak(current, wanted, std::memory_order_release, std::memory_order_relaxed)) {}
    }

    void BeginForegroundGenerationPriority() {
        foregroundGenerationPriorityCount_.fetch_add(1,std::memory_order_acq_rel);
        // If Load Everything is in a long Timeline pass, ask it to finish the current
        // frame/atomic operation and yield. The selected-media worker then owns decoder
        // priority until it has produced the foreground cache it needs.
        fullLoadYieldCurrent_.store(true,std::memory_order_release);
    }

    void EndForegroundGenerationPriority() {
        const int previous=foregroundGenerationPriorityCount_.fetch_sub(1,std::memory_order_acq_rel);
        if(previous<=1) foregroundGenerationPriorityCount_.store(0,std::memory_order_release);
    }

    bool ForegroundGenerationPriorityActive() const {
        return foregroundGenerationPriorityCount_.load(std::memory_order_acquire)>0;
    }

    bool WaitForBackgroundPermit(const std::atomic<bool>& stop, bool heavyBatch = false) const {
        while (!stop.load(std::memory_order_acquire)) {
            // Selected Info/Timeline generation always outranks the batch. Existing visible
            // thumbnail decode workers are separate and are not parked by this gate.
            if(heavyBatch && ForegroundGenerationPriorityActive()){ Sleep(10); continue; }
            // Ordinary background producers respect the process allocation guard. Load
            // Everything is different: one VR/8K decode may legitimately exceed it, so
            // that batch waits only for genuine machine-wide memory exhaustion.
            if(heavyBatch) {
                if(SystemMemoryCriticallyLow()){ Sleep(100); continue; }
            } else if(ProcessMemoryBytes()>=CurrentProcessMemoryPolicy().allocationGuard){ Sleep(20); continue; }
            const ULONGLONG until = backgroundPauseUntil_.load(std::memory_order_acquire);
            const ULONGLONG now = GetTickCount64();
            if (until <= now) return true;
            const ULONGLONG remaining = until - now;
            Sleep(static_cast<DWORD>(std::min<ULONGLONG>(20, remaining)));
        }
        return false;
    }


    void PaintControlsWindow() {
        if (!controlsHwnd_ || !playerControlsVisible_) return;
        paintOwner_ = controlsHwnd_;
        HRGN updateRegion=CreateRectRgn(0,0,0,0);
        const int updateRegionType=updateRegion?GetUpdateRgn(controlsHwnd_,updateRegion,FALSE):ERROR;
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(controlsHwnd_, &ps);
        RECT rc{}; GetClientRect(controlsHwnd_, &rc);
        const int w = std::max(1, static_cast<int>(rc.right - rc.left));
        const int h = std::max(1, static_cast<int>(rc.bottom - rc.top));
        const bool bufferRecreated=!controlsBackDC_ || controlsBackW_!=w || controlsBackH_!=h;
        EnsureControlsBackBuffer(dc,w,h);

        RECT dirty=ps.rcPaint;
        if(IsRectEmpty(&dirty)) dirty=rc;
        const int savedDc=SaveDC(controlsBackDC_);
        if(bufferRecreated)
            IntersectClipRect(controlsBackDC_,rc.left,rc.top,rc.right,rc.bottom);
        else if(updateRegion && updateRegionType!=ERROR && updateRegionType!=NULLREGION)
            SelectClipRgn(controlsBackDC_,updateRegion);
        else
            IntersectClipRect(controlsBackDC_,dirty.left,dirty.top,dirty.right,dirty.bottom);
        HBRUSH bg = CreateSolidBrush(RGB(13,15,20)); FillRect(controlsBackDC_,&rc,bg); DeleteObject(bg);
        SetBkMode(controlsBackDC_, TRANSPARENT);
        PaintPlayerControls(controlsBackDC_, rc);
        RestoreDC(controlsBackDC_,savedDc);
        BitBlt(dc,dirty.left,dirty.top,dirty.right-dirty.left,dirty.bottom-dirty.top,
               controlsBackDC_,dirty.left,dirty.top,SRCCOPY);
        EndPaint(controlsHwnd_,&ps);
        if(updateRegion) DeleteObject(updateRegion);
    }

    void PaintEdgeArrowWindow(HWND h) {
        if (!h || !playerControlsVisible_) return;
        paintOwner_ = h;
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(h, &ps);
        RECT rc{}; GetClientRect(h, &rc);
        const int w=std::max<LONG>(1,rc.right-rc.left),hgt=std::max<LONG>(1,rc.bottom-rc.top);
        HDC mem=CreateCompatibleDC(dc);
        HBITMAP buffer=mem?CreateCompatibleBitmap(dc,w,hgt):nullptr;
        HGDIOBJ old=buffer?SelectObject(mem,buffer):nullptr;
        HDC target=(mem&&buffer)?mem:dc;
        HBRUSH bg=CreateSolidBrush(RGB(13,15,20)); FillRect(target,&rc,bg); DeleteObject(bg);
        const bool next = h == playerNextHwnd_;
        DrawEdgeArrowButton(target, rc, next, CanNavigatePlayerMedia(next ? 1 : -1));
        if(mem&&buffer) BitBlt(dc,0,0,w,hgt,mem,0,0,SRCCOPY);
        if(old) SelectObject(mem,old);
        if(buffer) DeleteObject(buffer);
        if(mem) DeleteDC(mem);
        EndPaint(h, &ps);
    }

    HFONT GetFont(int px, int weight=FW_NORMAL) {
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(px)) << 32) | static_cast<uint32_t>(weight);
        const auto it = fontCache_.find(key);
        if (it != fontCache_.end()) return it->second;
        HFONT f = CreateFontW(-px,0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        fontCache_[key] = f;
        return f;
    }

    int MeasureHdcTextWidth(HDC dc,const std::wstring& text,int size,int weight=FW_NORMAL) {
        if(text.empty()) return 0;
        HFONT f=GetFont(size,weight);HGDIOBJ old=SelectObject(dc,f);SIZE sz{};
        const BOOL ok=GetTextExtentPoint32W(dc,text.c_str(),static_cast<int>(text.size()),&sz);SelectObject(dc,old);
        return ok?sz.cx:0;
    }

    std::wstring MiddleEllipsizeHdc(HDC dc,const std::wstring& text,int maxWidth,int size,int weight=FW_NORMAL) {
        if(text.empty() || maxWidth<=0) return L"";
        if(MeasureHdcTextWidth(dc,text,size,weight)<=maxWidth) return text;
        const std::wstring dots=L"...";
        if(MeasureHdcTextWidth(dc,dots,size,weight)>maxWidth) return L"";
        size_t lo=0,hi=text.size();
        while(lo<hi){
            const size_t keep=(lo+hi+1)/2,left=(keep+1)/2,right=keep-left;
            const std::wstring candidate=text.substr(0,left)+dots+text.substr(text.size()-right);
            if(MeasureHdcTextWidth(dc,candidate,size,weight)<=maxWidth) lo=keep; else hi=keep-1;
        }
        const size_t left=(lo+1)/2,right=lo-left;
        return text.substr(0,left)+dots+text.substr(text.size()-right);
    }

    void DrawTextSimple(HDC dc, const std::wstring& s, RECT r, int size, int weight=FW_NORMAL, COLORREF color=RGB(238,241,247), UINT fmt=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS) {
        HFONT f=GetFont(size,weight); HGDIOBJ old=SelectObject(dc,f); SetTextColor(dc,color); DrawTextW(dc,s.c_str(),-1,&r,fmt | DT_NOPREFIX); SelectObject(dc,old);
    }

    void FillRound(HDC dc, RECT r, COLORREF color, int radius=10) {
        HRGN region = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
        HBRUSH b = CreateSolidBrush(color); FillRgn(dc, region, b); DeleteObject(b); DeleteObject(region);
    }

    static bool SameRect(const RECT& a, const RECT& b) {
        return a.left==b.left && a.top==b.top && a.right==b.right && a.bottom==b.bottom;
    }

    static bool EmptyRectValue(const RECT& r) {
        return r.right <= r.left || r.bottom <= r.top;
    }

    static float EaseUi(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        const float inv = 1.0f - t;
        return 1.0f - inv*inv*inv;
    }

    static COLORREF MixColor(COLORREF a, COLORREF b, float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        const auto mix = [t](BYTE x, BYTE y) -> BYTE {
            return static_cast<BYTE>(std::clamp<int>(static_cast<int>(std::lround(x + (y-x)*t)), 0, 255));
        };
        return RGB(mix(GetRValue(a),GetRValue(b)), mix(GetGValue(a),GetGValue(b)), mix(GetBValue(a),GetBValue(b)));
    }

    void DrawMediaHoverBorder(HDC dc, RECT r, float amount, int radius) {
        amount=std::clamp(amount,0.0f,1.0f);
        if(amount<=0.001f || EmptyRectValue(r)) return;

        // Media hover/focus never alters the thumbnail itself.  Animate one common
        // border treatment for Library cards, secondary timeline cards and the
        // brief return-to-Library focus marker.
        const COLORREF border=MixColor(RGB(74,81,96),RGB(238,242,250),amount);
        HRGN round=CreateRoundRectRgn(r.left,r.top,r.right+1,r.bottom+1,radius,radius);
        if(!round) return;
        HBRUSH brush=CreateSolidBrush(border);
        if(brush){
            FrameRgn(dc,round,brush,3,3);
            DeleteObject(brush);
        }
        DeleteObject(round);
    }

    float LibraryReturnHighlightAmount(size_t mediaIndex) const {
        if(libraryReturnHighlightStart_==0 || libraryReturnHighlightCategory_!=category_ || libraryReturnHighlightIndex_!=mediaIndex) return 0.0f;
        const ULONGLONG elapsed=GetTickCount64()-libraryReturnHighlightStart_;
        if(elapsed>=kLibraryReturnHighlightDurationMs) return 0.0f;

        return BreathingHighlightAmount(elapsed,kLibraryReturnHighlightDurationMs);
    }

    float TimelineReturnHighlightAmount(size_t previewIndex) const {
        if(timelineReturnHighlightStart_==0 || mode_!=Mode::Details || category_!=Category::Videos ||
           timelineReturnHighlightMediaIndex_!=selected_ || timelineReturnHighlightIndex_!=previewIndex) return 0.0f;
        const ULONGLONG elapsed=GetTickCount64()-timelineReturnHighlightStart_;
        if(elapsed>=kLibraryReturnHighlightDurationMs) return 0.0f;
        return BreathingHighlightAmount(elapsed,kLibraryReturnHighlightDurationMs);
    }

    bool IsAppForegroundForHover() const {
        if(!hwnd_) return false;
        HWND foreground=GetForegroundWindow();
        if(!foreground) return false;
        if(foreground==hwnd_) return true;
        if(GetAncestor(foreground,GA_ROOT)==hwnd_) return true;
        return GetAncestor(foreground,GA_ROOTOWNER)==hwnd_;
    }

    bool ShouldEatForegroundActivationClick() const {
        // Preserve the deliberate two-step activation only in browsing surfaces, where
        // an accidental first click could select/open media. Once media is actually
        // being viewed, the first click should both activate VMP and operate the UI.
        return mode_==Mode::Library || (mode_==Mode::Details && category_==Category::Videos);
    }

    bool ConsumeForegroundActivationClickMessage(UINT message) {
        if(!foregroundActivationClickPending_) return false;
        switch(message){
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            return true;
        case WM_LBUTTONUP:
            foregroundActivationClickPending_=false;
            return true;
        default:
            return false;
        }
    }

    float ButtonHoverAmount(RECT r) const {
        if(!IsAppForegroundForHover()) return 0.0f;
        const ULONGLONG now = GetTickCount64();
        float t = 1.0f;
        if (hoverTransitionStart_ != 0)
            t = EaseUi(static_cast<float>(now-hoverTransitionStart_) / static_cast<float>(kUiAnimationDurationMs));
        if (paintOwner_ == hoverOwner_ && SameRect(r, hoverRect_)) return t;
        if (paintOwner_ == hoverPreviousOwner_ && SameRect(r, hoverPreviousRect_)) return 1.0f-t;
        return 0.0f;
    }

    void DrawButton(HDC dc, RECT r, const wchar_t* text, bool primary=false) {
        const float hover=ButtonHoverAmount(r);
        const COLORREF base=primary ? RGB(188,200,224) : RGB(111,124,145);
        const COLORREF over=primary ? RGB(216,226,242) : RGB(132,146,169);
        FillRound(dc, r, MixColor(base,over,hover), 11);
        DrawTextSimple(dc,text,r,14,FW_SEMIBOLD,primary?RGB(12,14,19):RGB(245,246,250),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }

    void DrawEdgeArrowButton(HDC dc, RECT r, bool next, bool enabled=true) {
        if(!enabled) return;
        Gdiplus::Bitmap* icon=next?nextSvgBitmap_.get():previousSvgBitmap_.get();
        DrawSvgIconButton(dc,r,icon,false,7);
    }

    void DrawTab(HDC dc, RECT r, const wchar_t* text, bool active) {
        const float hover=ButtonHoverAmount(r);
        const COLORREF base=active ? RGB(188,200,224) : RGB(111,124,145);
        const COLORREF over=active ? RGB(216,226,242) : RGB(132,146,169);
        FillRound(dc, r, MixColor(base,over,hover), 11);
        DrawTextSimple(dc, text, r, 15, FW_BOLD, active ? RGB(12,14,19) : RGB(245,246,250), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }


    std::unique_ptr<Gdiplus::Bitmap> LoadPngBitmapFromResource(int resId) {
        HRSRC hrsrc = FindResourceW(inst_, MAKEINTRESOURCEW(resId), L"PNG");
        if (!hrsrc) return {};
        const DWORD size = SizeofResource(inst_, hrsrc);
        if (!size) return {};
        HGLOBAL hres = LoadResource(inst_, hrsrc);
        if (!hres) return {};
        const void* src = LockResource(hres);
        if (!src) return {};

        HGLOBAL hmem = GlobalAlloc(GMEM_MOVEABLE, size);
        if (!hmem) return {};
        void* dst = GlobalLock(hmem);
        if (!dst) { GlobalFree(hmem); return {}; }
        std::memcpy(dst, src, size);
        GlobalUnlock(hmem);

        IStream* stream = nullptr;
        if (FAILED(CreateStreamOnHGlobal(hmem, TRUE, &stream))) {
            GlobalFree(hmem);
            return {};
        }

        std::unique_ptr<Gdiplus::Bitmap> copy;
        {
            std::unique_ptr<Gdiplus::Bitmap> decoded(Gdiplus::Bitmap::FromStream(stream, FALSE));
            if (decoded && decoded->GetLastStatus() == Gdiplus::Ok && decoded->GetWidth() > 0 && decoded->GetHeight() > 0) {
                copy = std::make_unique<Gdiplus::Bitmap>(decoded->GetWidth(), decoded->GetHeight(), PixelFormat32bppARGB);
                if (copy && copy->GetLastStatus() == Gdiplus::Ok) {
                    Gdiplus::Graphics g(copy.get());
                    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
                    g.DrawImage(decoded.get(), 0, 0, decoded->GetWidth(), decoded->GetHeight());
                } else {
                    copy.reset();
                }
            }
        }
        stream->Release();
        return copy;
    }

    bool EnsureSvgRasterContext() {
        if (svgD2dContext_) return true;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        const D3D_FEATURE_LEVEL legacyLevels[] = {
            D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL createdLevel{};
        ComPtr<ID3D11DeviceContext> immediate;
        auto createDevice=[&](D3D_DRIVER_TYPE type)->HRESULT{
            svgD3dDevice_.Reset();immediate.Reset();
            HRESULT result=D3D11CreateDevice(nullptr,type,nullptr,flags,levels,static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,svgD3dDevice_.GetAddressOf(),&createdLevel,immediate.GetAddressOf());
            if(result==E_INVALIDARG){
                svgD3dDevice_.Reset();immediate.Reset();
                result=D3D11CreateDevice(nullptr,type,nullptr,flags,legacyLevels,static_cast<UINT>(std::size(legacyLevels)),
                    D3D11_SDK_VERSION,svgD3dDevice_.GetAddressOf(),&createdLevel,immediate.GetAddressOf());
            }
            return result;
        };
        HRESULT hr=createDevice(D3D_DRIVER_TYPE_HARDWARE);
        if(FAILED(hr)) hr=createDevice(D3D_DRIVER_TYPE_WARP);
        if (FAILED(hr) || !svgD3dDevice_) return false;

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(svgD3dDevice_.As(&dxgiDevice))) return false;

        D2D1_FACTORY_OPTIONS options{};
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
            __uuidof(ID2D1Factory1),&options,
            reinterpret_cast<void**>(svgD2dFactory_.GetAddressOf())))) return false;
        if (FAILED(svgD2dFactory_->CreateDevice(dxgiDevice.Get(),svgD2dDevice_.GetAddressOf()))) return false;

        ComPtr<ID2D1DeviceContext> baseContext;
        if (FAILED(svgD2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,baseContext.GetAddressOf()))) return false;
        if (FAILED(baseContext.As(&svgD2dContext_))) return false;

        return true;
    }

    std::unique_ptr<Gdiplus::Bitmap> RasterizeSvgBytes(const char* source,size_t sourceSize,UINT rasterSize=512,bool normalizeViewport=false) {
        if(!source || sourceSize==0 || !EnsureSvgRasterContext() || rasterSize==0) return {};
        std::string svg(source,source+sourceSize);
        size_t svgOffset=svg.find("<svg");
        if(svgOffset==std::string::npos) return {};
        if(svgOffset>0) svg.erase(0,svgOffset);

        // Direct2D uses the SVG root width/height as an intrinsic viewport. For replacement
        // artwork that also has a viewBox, strip only those two root attributes in memory so
        // the viewBox scales to the requested raster size. The file on disk is never changed.
        const size_t tagEnd=svg.find('>');
        if(normalizeViewport && tagEnd!=std::string::npos){
            std::string root=svg.substr(0,tagEnd+1);
            if(root.find("viewBox")!=std::string::npos || root.find("viewbox")!=std::string::npos){
                static const std::regex widthAttr(R"(\s+width\s*=\s*([\"'])[^\"']*\1)",std::regex_constants::icase);
                static const std::regex heightAttr(R"(\s+height\s*=\s*([\"'])[^\"']*\1)",std::regex_constants::icase);
                root=std::regex_replace(root,widthAttr,"");
                root=std::regex_replace(root,heightAttr,"");
                svg.replace(0,tagEnd+1,root);
            }
        }

        HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,svg.size());
        if(!memory) return {};
        void* destination=GlobalLock(memory);
        if(!destination){GlobalFree(memory);return {};}
        std::memcpy(destination,svg.data(),svg.size());
        GlobalUnlock(memory);
        ComPtr<IStream> stream;
        if(FAILED(CreateStreamOnHGlobal(memory,TRUE,stream.GetAddressOf()))){GlobalFree(memory);return {};}

        const D2D1_SIZE_F viewport{static_cast<FLOAT>(rasterSize),static_cast<FLOAT>(rasterSize)};
        ComPtr<ID2D1SvgDocument> document;
        if(FAILED(svgD2dContext_->CreateSvgDocument(stream.Get(),viewport,document.GetAddressOf())) || !document) return {};

        const D2D1_SIZE_U size{rasterSize,rasterSize};
        D2D1_BITMAP_PROPERTIES1 targetProps{};
        targetProps.pixelFormat.format=DXGI_FORMAT_B8G8R8A8_UNORM;
        targetProps.pixelFormat.alphaMode=D2D1_ALPHA_MODE_PREMULTIPLIED;
        targetProps.dpiX=96.0f;targetProps.dpiY=96.0f;
        targetProps.bitmapOptions=D2D1_BITMAP_OPTIONS_TARGET;
        ComPtr<ID2D1Bitmap1> target;
        if(FAILED(svgD2dContext_->CreateBitmap(size,nullptr,0,&targetProps,target.GetAddressOf()))) return {};

        svgD2dContext_->SetTarget(target.Get());
        svgD2dContext_->BeginDraw();
        const D2D1_COLOR_F transparent{0.0f,0.0f,0.0f,0.0f};
        svgD2dContext_->Clear(&transparent);
        svgD2dContext_->DrawSvgDocument(document.Get());
        const HRESULT drawHr=svgD2dContext_->EndDraw();
        svgD2dContext_->SetTarget(nullptr);
        if(FAILED(drawHr)) return {};

        D2D1_BITMAP_PROPERTIES1 readProps=targetProps;
        readProps.bitmapOptions=D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        ComPtr<ID2D1Bitmap1> readback;
        if(FAILED(svgD2dContext_->CreateBitmap(size,nullptr,0,&readProps,readback.GetAddressOf()))) return {};
        if(FAILED(readback->CopyFromBitmap(nullptr,target.Get(),nullptr))) return {};
        D2D1_MAPPED_RECT mapped{};
        if(FAILED(readback->Map(D2D1_MAP_OPTIONS_READ,&mapped)) || !mapped.bits) return {};

        auto bitmap=std::make_unique<Gdiplus::Bitmap>(rasterSize,rasterSize,PixelFormat32bppPARGB);
        if(!bitmap || bitmap->GetLastStatus()!=Gdiplus::Ok){readback->Unmap();return {};}
        Gdiplus::Rect outRect(0,0,static_cast<INT>(rasterSize),static_cast<INT>(rasterSize));
        Gdiplus::BitmapData out{};
        if(bitmap->LockBits(&outRect,Gdiplus::ImageLockModeWrite,PixelFormat32bppPARGB,&out)!=Gdiplus::Ok){readback->Unmap();return {};}
        const size_t rowBytes=static_cast<size_t>(rasterSize)*4u;
        for(UINT y=0;y<rasterSize;++y){
            const BYTE* srcRow=mapped.bits+static_cast<size_t>(y)*mapped.pitch;
            BYTE* dstRow=reinterpret_cast<BYTE*>(out.Scan0)+static_cast<INT_PTR>(y)*static_cast<INT_PTR>(out.Stride);
            std::memcpy(dstRow,srcRow,rowBytes);
        }
        bitmap->UnlockBits(&out);
        readback->Unmap();
        return bitmap;
    }

    std::unique_ptr<Gdiplus::Bitmap> LoadSvgBitmapFromResource(int resId,UINT rasterSize=512) {
        HRSRC hrsrc=FindResourceW(inst_,MAKEINTRESOURCEW(resId),RT_RCDATA);
        if(!hrsrc) return {};
        const DWORD resourceSize=SizeofResource(inst_,hrsrc);
        if(!resourceSize) return {};
        HGLOBAL loaded=LoadResource(inst_,hrsrc);
        if(!loaded) return {};
        const void* source=LockResource(loaded);
        if(!source) return {};
        return RasterizeSvgBytes(reinterpret_cast<const char*>(source),resourceSize,rasterSize,false);
    }

    std::unique_ptr<Gdiplus::Bitmap> LoadSvgBitmapFromFile(const fs::path& path,UINT rasterSize=512) {
        std::ifstream input(path,std::ios::binary);
        if(!input) return {};
        std::string bytes((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
        if(bytes.empty()) return {};
        return RasterizeSvgBytes(bytes.data(),bytes.size(),rasterSize,true);
    }

    std::unique_ptr<Gdiplus::Bitmap> LoadRasterBitmapFromFile(const fs::path& path) {
        std::unique_ptr<Gdiplus::Bitmap> decoded(Gdiplus::Bitmap::FromFile(path.c_str(),FALSE));
        if(!decoded || decoded->GetLastStatus()!=Gdiplus::Ok || decoded->GetWidth()==0 || decoded->GetHeight()==0) return {};
        auto copy=std::make_unique<Gdiplus::Bitmap>(decoded->GetWidth(),decoded->GetHeight(),PixelFormat32bppARGB);
        if(!copy || copy->GetLastStatus()!=Gdiplus::Ok) return {};
        Gdiplus::Graphics g(copy.get());
        g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        g.DrawImage(decoded.get(),0,0,decoded->GetWidth(),decoded->GetHeight());
        return copy;
    }

    fs::path ButtonAssetsDirectory() const {
        wchar_t exeBuf[32768]{};
        const DWORD len=GetModuleFileNameW(nullptr,exeBuf,static_cast<DWORD>(std::size(exeBuf)));
        fs::path exeDir=(len && len<std::size(exeBuf))?fs::path(std::wstring(exeBuf,len)).parent_path():fs::current_path();
        const fs::path direct=exeDir/L"ButtonAssets";
        std::error_code ec;
        if(fs::is_directory(direct,ec)) return direct;
        // Visual Studio / msbuild output lives in Source\\x64\\Release. This makes the same
        // user-editable ButtonAssets folder work when launching that build directly.
        const fs::path projectRelative=(exeDir/L".."/L".."/L".."/L"ButtonAssets").lexically_normal();
        ec.clear();
        if(fs::is_directory(projectRelative,ec)) return projectRelative;
        return direct;
    }

    fs::path FindButtonAssetFile(const wchar_t* slot) const {
        if(!slot || !*slot) return {};
        const fs::path dir=ButtonAssetsDirectory();
        // PNG intentionally wins over SVG, so dropping Play.png beside the shipped Play.svg
        // immediately overrides it without requiring the user to delete or rename the SVG.
        static const wchar_t* extensions[]={L".png",L".svg",L".jpg",L".jpeg",L".bmp",L".gif"};
        std::error_code ec;
        for(const wchar_t* ext:extensions){
            fs::path candidate=dir/(std::wstring(slot)+ext);
            ec.clear();
            if(fs::is_regular_file(candidate,ec)) return candidate;
        }
        return {};
    }

    std::unique_ptr<Gdiplus::Bitmap> LoadButtonAsset(const wchar_t* slot,int fallbackSvgResource,UINT rasterSize=512) {
        const fs::path file=FindButtonAssetFile(slot);
        if(!file.empty()){
            std::wstring ext=ToLower(file.extension().wstring());
            std::unique_ptr<Gdiplus::Bitmap> loaded;
            if(ext==L".svg") loaded=LoadSvgBitmapFromFile(file,rasterSize);
            else loaded=LoadRasterBitmapFromFile(file);
            if(loaded){
                externalButtonAssets_.insert(loaded.get());
                return loaded;
            }
        }
        return fallbackSvgResource?LoadSvgBitmapFromResource(fallbackSvgResource,rasterSize):std::unique_ptr<Gdiplus::Bitmap>{};
    }

    void LoadUiIcons() {
        externalButtonAssets_.clear();
        // ButtonAssets is the user-editable visual layer. Replace any named file in that
        // folder and restart the app; no resource edit and no rebuild are required.
        buttonBackgroundActiveBitmap_=LoadButtonAsset(L"Background_Active",0);
        buttonBackgroundInactiveBitmap_=LoadButtonAsset(L"Background_Inactive",0);
        autoNextSvgBitmap_=LoadButtonAsset(L"AutoNext",IDR_AUTONEXT_SVG);
        openFolderSvgBitmap_=LoadButtonAsset(L"OpenFolder",IDR_OPEN_FOLDER_SVG);
        reloadSvgBitmap_=LoadButtonAsset(L"Refresh",IDR_RELOAD_SVG);
        loadEverythingSvgBitmap_=LoadButtonAsset(L"LoadEverything",IDR_LOAD_EVERYTHING_SVG);
        expandFullscreenSvgBitmap_=LoadButtonAsset(L"FullscreenEnter",IDR_EXPAND_FULLSCREEN_SVG);
        collapseFullscreenSvgBitmap_=LoadButtonAsset(L"FullscreenExit",IDR_COLLAPSE_FULLSCREEN_SVG);
        backSvgBitmap_=LoadButtonAsset(L"Back",IDR_BACK_SVG);
        imageSvgBitmap_=LoadButtonAsset(L"Image",IDR_IMAGE_SVG);
        videosSvgBitmap_=LoadButtonAsset(L"Video",IDR_VIDEOS_SVG);
        nativeSvgBitmap_=LoadButtonAsset(L"Native",IDR_NATIVE_SVG);
        previousSvgBitmap_=LoadButtonAsset(L"Previous",IDR_PREVIOUS_SVG);
        nextSvgBitmap_=LoadButtonAsset(L"Next",IDR_NEXT_SVG);
        rewindSvgBitmap_=LoadButtonAsset(L"Rewind",0);
        forwardSvgBitmap_=LoadButtonAsset(L"Forward",0);
        volumeSvgBitmap_=LoadButtonAsset(L"Volume",IDR_VOLUME_SVG);
        volumeMuteSvgBitmap_=LoadButtonAsset(L"VolumeMute",IDR_VOLUME_MUTE_SVG);
        skip30SvgBitmap_=LoadSvgBitmapFromResource(IDR_SKIP30_SVG);
        pauseSvgBitmap_=LoadButtonAsset(L"Pause",IDR_PAUSE_SVG);
        playSvgBitmap_=LoadButtonAsset(L"Play",IDR_PLAY_SVG);
        vrProjectionSvgBitmap_=LoadButtonAsset(L"VRProjection",IDR_VR_PROJECTION_SVG);

        resolution4kBitmap_=LoadPngBitmapFromResource(IDR_RESOLUTION_4K_PNG);
        resolution5kBitmap_=LoadPngBitmapFromResource(IDR_RESOLUTION_5K_PNG);
        resolution8kBitmap_=LoadPngBitmapFromResource(IDR_RESOLUTION_8K_PNG);
        vrBadgeBitmap_=LoadPngBitmapFromResource(IDR_VR_PNG);
        vrBadgeWhiteBitmap_=LoadPngBitmapFromResource(IDR_VR_PNG);
        if(vrBadgeWhiteBitmap_) ForceBitmapWhitePreserveAlpha(vrBadgeWhiteBitmap_.get());
        favoriteIconBitmap_=LoadPngBitmapFromResource(IDR_FAVORITE_PNG);
    }

    void ForceBitmapWhitePreserveAlpha(Gdiplus::Bitmap* bmp) {
        if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) return;
        Gdiplus::Rect rect(0, 0, static_cast<INT>(bmp->GetWidth()), static_cast<INT>(bmp->GetHeight()));
        Gdiplus::BitmapData data{};
        if (bmp->LockBits(&rect, Gdiplus::ImageLockModeRead | Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &data) != Gdiplus::Ok) return;
        for (UINT y = 0; y < data.Height; ++y) {
            auto* row = reinterpret_cast<BYTE*>(data.Scan0) + static_cast<INT_PTR>(y) * static_cast<INT_PTR>(data.Stride);
            for (UINT x = 0; x < data.Width; ++x) {
                BYTE* px = row + static_cast<size_t>(x) * 4u;
                const BYTE a = px[3];
                if (a == 0) continue;
                px[0] = 255;
                px[1] = 255;
                px[2] = 255;
                px[3] = a;
            }
        }
        bmp->UnlockBits(&data);
    }

    void DrawBitmapCentered(HDC dc, RECT r, Gdiplus::Bitmap* bmp, int insetX = 8, int insetY = 8, bool rotate180 = false) {
        if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) return;
        const float availW = static_cast<float>(std::max(1L, (r.right - r.left) - insetX * 2));
        const float availH = static_cast<float>(std::max(1L, (r.bottom - r.top) - insetY * 2));
        const float bw = static_cast<float>(bmp->GetWidth());
        const float bh = static_cast<float>(bmp->GetHeight());
        if (bw <= 0.0f || bh <= 0.0f) return;
        const float scale = std::min(availW / bw, availH / bh);
        const float dw = std::max(1.0f, bw * scale);
        const float dh = std::max(1.0f, bh * scale);
        const float dx = static_cast<float>(r.left + insetX) + (availW - dw) * 0.5f;
        const float dy = static_cast<float>(r.top + insetY) + (availH - dh) * 0.5f;
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        if(rotate180){
            const float cx=(r.left+r.right)*0.5f,cy=(r.top+r.bottom)*0.5f;
            Gdiplus::Matrix rotation;
            rotation.RotateAt(180.0f,Gdiplus::PointF(cx,cy));
            g.SetTransform(&rotation);
        }
        g.DrawImage(bmp, Gdiplus::RectF(dx, dy, dw, dh));
    }

    void DrawBitmapStretched(HDC dc,RECT r,Gdiplus::Bitmap* bmp) {
        if(!bmp || bmp->GetLastStatus()!=Gdiplus::Ok || EmptyRectValue(r)) return;
        Gdiplus::Graphics g(dc);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        g.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
        g.DrawImage(bmp,Gdiplus::RectF(static_cast<Gdiplus::REAL>(r.left),static_cast<Gdiplus::REAL>(r.top),
            static_cast<Gdiplus::REAL>(r.right-r.left),static_cast<Gdiplus::REAL>(r.bottom-r.top)));
    }

    void DrawSvgIconButton(HDC dc,RECT r,Gdiplus::Bitmap* icon,bool active=false,int inset=6,bool rotate180=false,bool honorInsetForExternal=false,int iconOffsetX=0,int iconOffsetY=0) {
        const float hover=ButtonHoverAmount(r);
        const COLORREF base=active?RGB(188,200,224):RGB(111,124,145);
        const COLORREF over=active?RGB(216,226,242):RGB(132,146,169);
        FillRound(dc,r,MixColor(base,over,hover),11);
        if(icon){
            const int drawInset=(honorInsetForExternal || !externalButtonAssets_.count(icon))?inset:0;
            RECT iconRect=r;
            if(iconOffsetX || iconOffsetY) OffsetRect(&iconRect,iconOffsetX,iconOffsetY);
            DrawBitmapCentered(dc,iconRect,icon,drawInset,drawInset,rotate180);
        }
    }



    void DrawFavoriteBadge(HDC dc, RECT imageRect) {
        if(!favoriteIconBitmap_) return;
        RECT badge{imageRect.left+8,imageRect.top+8,imageRect.left+42,imageRect.top+42};
        DrawBitmapCentered(dc,badge,favoriteIconBitmap_.get(),0,0);
    }


    void DrawFolderIconButton(HDC dc, RECT r) {
        DrawSvgIconButton(dc,r,openFolderSvgBitmap_.get(),false,5);
    }

    void DrawRefreshIconButton(HDC dc, RECT r) {
        DrawSvgIconButton(dc,r,reloadSvgBitmap_.get(),false,5);
    }

    void DrawDownloadIconButton(HDC dc, RECT r) {
        DrawSvgIconButton(dc,r,loadEverythingSvgBitmap_.get(),false,5);
    }

    bool NativeVideoSizingAvailable() const {
        return mode_==Mode::Player && player_ && !player_->VR().vr;
    }

    bool NativeImageSizingAvailable() const {
        return mode_==Mode::Details && category_==Category::Images && selected_<images_.size();
    }

    void DrawNativeSizeButton(HDC dc, RECT r) {
        const bool active=nativeVideoSizing_ || nativeImageSizing_;
        DrawSvgIconButton(dc,r,nativeSvgBitmap_.get(),active,5);
    }

    void DrawFullscreenButton(HDC dc, RECT r) {
        DrawSvgIconButton(dc,r,fullscreen_?collapseFullscreenSvgBitmap_.get():expandFullscreenSvgBitmap_.get(),fullscreen_,6);
    }

    void DrawVrProjectionToggle(HDC dc, RECT r) {
        if (!player_ || !player_->VR().vr) return;
        const bool full360 = player_->IsVr360Enabled();
        DrawSvgIconButton(dc,r,vrProjectionSvgBitmap_.get(),full360,6,false,true,0,0);
    }

    void DrawAutoAdvanceIcon(HDC dc, RECT r, bool active) {
        DrawSvgIconButton(dc,r,autoNextSvgBitmap_.get(),active,5);
    }

    void DrawAutoNextIcon(HDC dc, RECT r) {
        DrawAutoAdvanceIcon(dc,r,autoNext_);
    }

    void DrawPlayPauseIcon(HDC dc, RECT r) {
        Gdiplus::Bitmap* icon=(player_ && player_->IsPaused())?playSvgBitmap_.get():pauseSvgBitmap_.get();
        DrawSvgIconButton(dc,r,icon,false,6);
    }

    void DrawSkip30Icon(HDC dc, RECT r, bool forward) {
        Gdiplus::Bitmap* replacement=forward?forwardSvgBitmap_.get():rewindSvgBitmap_.get();
        if(replacement) DrawSvgIconButton(dc,r,replacement,false,5,false);
        else DrawSvgIconButton(dc,r,skip30SvgBitmap_.get(),false,5,!forward);
    }

    std::wstring FormatTime(double seconds) {
        if (!(seconds >= 0.0) || !std::isfinite(seconds)) seconds = 0.0;
        const long long total = static_cast<long long>(seconds + 0.5);
        const long long h = total / 3600, m = (total % 3600) / 60, sec = total % 60;
        wchar_t buf[64]{};
        if (h > 0) swprintf_s(buf, L"%lld:%02lld:%02lld", h, m, sec);
        else swprintf_s(buf, L"%lld:%02lld", m, sec);
        return buf;
    }

    void PaintPlayerControls(HDC dc, RECT rc) {
        if (!player_ || !playerControlsVisible_) return;
        HBRUSH bg=CreateSolidBrush(RGB(16,18,24)); FillRect(dc,&rc,bg); DeleteObject(bg);
        HPEN topLine=CreatePen(PS_SOLID,1,RGB(52,57,69)); HGDIOBJ oldPen=SelectObject(dc,topLine);
        MoveToEx(dc,0,0,nullptr); LineTo(dc,rc.right,0); SelectObject(dc,oldPen); DeleteObject(topLine);

        const int cy=(seekRect_.top+seekRect_.bottom)/2;
        RECT base{seekRect_.left,cy-2,seekRect_.right,cy+2}; FillRound(dc,base,RGB(72,78,92),4);
        const double frac=std::clamp(seekFraction_,0.0,1.0);
        RECT progress=base; progress.right=progress.left+static_cast<LONG>((progress.right-progress.left)*frac);
        if(progress.right>progress.left) FillRound(dc,progress,RGB(235,238,245),4);
        const int knobX=seekRect_.left+static_cast<int>((seekRect_.right-seekRect_.left)*frac);
        HBRUSH kb=CreateSolidBrush(RGB(250,251,253)); HGDIOBJ oldBrush=SelectObject(dc,kb);
        Ellipse(dc,knobX-6,cy-6,knobX+7,cy+7); SelectObject(dc,oldBrush); DeleteObject(kb);

        // Seek-drag preview: show the exact target time only while the timeline is held.
        if (seekHoverVisible_ && player_->Duration() > 0.0) {
            const int seekWidth = std::max(1, static_cast<int>(seekRect_.right - seekRect_.left));
            const int hoverX = std::clamp(seekHoverX_, static_cast<int>(seekRect_.left), static_cast<int>(seekRect_.right));
            const double hoverFraction = static_cast<double>(hoverX - seekRect_.left) / static_cast<double>(seekWidth);
            const std::wstring hoverTime = FormatTime(player_->Duration() * std::clamp(hoverFraction, 0.0, 1.0));

            constexpr int tipW = 74;
            constexpr int tipH = 28;
            int tipLeft = hoverX - tipW / 2;
            tipLeft = std::clamp(tipLeft, static_cast<int>(rc.left + 8), static_cast<int>(rc.right - tipW - 8));
            RECT tip{tipLeft, seekRect_.top - tipH - 5, tipLeft + tipW, seekRect_.top - 5};
            FillRound(dc, tip, RGB(38,43,55), 9);
            DrawTextSimple(dc, hoverTime, tip, 12, FW_SEMIBOLD, RGB(245,246,250), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }

        DrawSvgIconButton(dc,playerBackRect_,backSvgBitmap_.get(),false,5);
        DrawVrProjectionToggle(dc,playerVrToggleRect_);
        DrawSkip30Icon(dc,playerSkipBackRect_,false);
        DrawPlayPauseIcon(dc,playerPlayRect_);
        DrawSkip30Icon(dc,playerSkipForwardRect_,true);
        DrawAutoNextIcon(dc,playerAutoNextRect_);
        if(NativeVideoSizingAvailable()) DrawNativeSizeButton(dc,playerNativeSizeRect_);
        DrawFullscreenButton(dc,playerFullRect_);

        const std::wstring time=FormatTime(player_->CurrentTime())+L" / "+FormatTime(player_->Duration());
        DrawTextSimple(dc,time,playerTimeRect_,13,FW_NORMAL,RGB(190,195,206),DT_CENTER|DT_VCENTER|DT_SINGLELINE);

        if(!IsRectEmpty(&volumeLabelRect_)) {
            const bool muted = volumeFraction_ <= 0.001;
            DrawSvgIconButton(dc,volumeLabelRect_,muted?volumeMuteSvgBitmap_.get():volumeSvgBitmap_.get(),muted,5);
        }
        const int vy=(volumeRect_.top+volumeRect_.bottom)/2;
        RECT vb{volumeRect_.left,vy-2,volumeRect_.right,vy+2}; FillRound(dc,vb,RGB(72,78,92),4);
        RECT vf=vb; vf.right=vf.left+static_cast<LONG>((vf.right-vf.left)*std::clamp(volumeFraction_,0.0,1.0));
        if(vf.right>vf.left) FillRound(dc,vf,RGB(235,238,245),4);
        const int vx=volumeRect_.left+static_cast<int>((volumeRect_.right-volumeRect_.left)*std::clamp(volumeFraction_,0.0,1.0));
        HBRUSH vbk=CreateSolidBrush(RGB(250,251,253)); oldBrush=SelectObject(dc,vbk);
        Ellipse(dc,vx-5,vy-5,vx+6,vy+6); SelectObject(dc,oldBrush); DeleteObject(vbk);

        // Volume-drag preview: show percentage only while the slider is held.
        if (volumeDragging_) {
            const int percent = std::clamp(static_cast<int>(std::lround(volumeFraction_ * 100.0)), 0, 100);
            wchar_t percentText[16]{};
            swprintf_s(percentText, L"%d%%", percent);

            constexpr int tipW = 58;
            constexpr int tipH = 28;
            int tipLeft = vx - tipW / 2;
            tipLeft = std::clamp(tipLeft, static_cast<int>(rc.left + 8), static_cast<int>(rc.right - tipW - 8));
            RECT tip{tipLeft, volumeRect_.top - tipH - 7, tipLeft + tipW, volumeRect_.top - 7};
            FillRound(dc, tip, RGB(38,43,55), 9);
            DrawTextSimple(dc, percentText, tip, 12, FW_SEMIBOLD, RGB(245,246,250), DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
    }

    static bool IsImageExtension(const std::wstring& extRaw) {
        return vmp::IsImageExtension(extRaw);
    }

    static uint64_t Fnv1a64(const std::wstring& s) {
        return vmp::Fnv1a64(s);
    }

    fs::path CacheRootForSource(const std::wstring& source) const {
        return fs::path(vmp::CacheRootForSource(source));
    }

    std::wstring BuildCachePath(const std::wstring& source) const {
        return vmp::BuildThumbCachePath(source);
    }

    std::wstring BuildUiCachePath(const std::wstring& source) const {
        return vmp::BuildUiThumbCachePath(source);
    }

    std::wstring BuildFavoriteMetadataPath(const std::wstring& source) const {
        return vmp::BuildFavoriteMetadataPath(source);
    }

    bool ReadFavoriteMetadata(const std::wstring& source) const {
        return vmp::ReadFavoriteMetadata(source);
    }

    bool WriteFavoriteMetadata(MediaItem& item, bool favorite) {
        const fs::path target=fs::path(BuildFavoriteMetadataPath(item.path));
        if(favorite){
            std::error_code ec;
            fs::create_directories(target.parent_path(),ec);
            if(ec) return false;
            const fs::path tmp=target.wstring()+L".tmp";
            {
                std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
                if(!out) return false;
                out<<"VMPFAV1\n";
                if(!out){ std::error_code rm; fs::remove(tmp,rm); return false; }
            }
            DeleteFileW(target.c_str());
            if(!MoveFileExW(tmp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
                DeleteFileW(tmp.c_str());
                return false;
            }
            HideCacheRootIfCreated(item.path);
            item.favorite=true;
        }else{
            DeleteFileW(target.c_str());
            item.favorite=false;
        }
        filterDirty_=true;
        return true;
    }

    void ToggleFavorite(MediaItem& item) {
        if(!WriteFavoriteMetadata(item,!item.favorite)) return;
        if(mode_==Mode::Library) ClampScroll();
        InvalidateRect(hwnd_,nullptr,FALSE);
    }


    static std::wstring BannerTimestampPath(const std::wstring& uiCachePath) {
        return uiCachePath + L".time";
    }

    static bool ReadBannerTimestamp(const std::wstring& uiCachePath, double& seconds) {
        seconds = -1.0;
        std::ifstream in(fs::path(BannerTimestampPath(uiCachePath)), std::ios::binary);
        double value = -1.0;
        if (!(in >> value) || !std::isfinite(value) || value < 0.0) return false;
        seconds = value;
        return true;
    }

    static void WriteBannerTimestamp(const std::wstring& uiCachePath, double seconds) {
        if (!std::isfinite(seconds) || seconds < 0.0) return;
        const std::wstring timePath = BannerTimestampPath(uiCachePath);
        std::ofstream out(fs::path(timePath), std::ios::binary | std::ios::trunc);
        if (out) out << seconds;
    }

    static std::wstring ResolutionMetadataPath(const std::wstring& uiCachePath) {
        return uiCachePath + L".resolution";
    }

    static bool ReadResolutionMetadata(const std::wstring& uiCachePath, UINT& width, UINT& height) {
        width=height=0;
        std::ifstream in(fs::path(ResolutionMetadataPath(uiCachePath)),std::ios::binary);
        std::string tag;
        unsigned long long w=0,h=0;
        if(!(in>>tag>>w>>h) || tag!="VMPRES1" || w==0 || h==0 || w>65535ull || h>65535ull) return false;
        width=static_cast<UINT>(w); height=static_cast<UINT>(h);
        return true;
    }

    static bool WriteResolutionMetadata(const std::wstring& uiCachePath, UINT width, UINT height) {
        if(uiCachePath.empty() || !width || !height) return false;
        const fs::path target=fs::path(ResolutionMetadataPath(uiCachePath));
        std::error_code ec; fs::create_directories(target.parent_path(),ec); if(ec) return false;
        const fs::path tmp=target.wstring()+L".tmp";
        {
            std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
            if(!out) return false;
            out<<"VMPRES1 "<<width<<" "<<height<<"\n";
            if(!out) { std::error_code rm; fs::remove(tmp,rm); return false; }
        }
        DeleteFileW(target.c_str());
        if(!MoveFileExW(tmp.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)){
            DeleteFileW(tmp.c_str()); return false;
        }
        return true;
    }

    static bool CacheFileLooksHealthy(const std::wstring& path, uintmax_t minimumBytes = 256) {
        if (path.empty()) return false;
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec) return false;
        const uintmax_t size = fs::file_size(path, ec);
        return !ec && size >= minimumBytes;
    }

    static void RemoveGeneratedCacheFile(const std::wstring& path) {
        if (path.empty()) return;
        DeleteFileW(path.c_str());
    }

    static bool CommitGeneratedCacheFile(const std::wstring& temporary, const std::wstring& target) {
        // Generated JPEGs are reconstructable. Keep atomic same-volume replacement, but
        // do not force every individual thumbnail through MOVEFILE_WRITE_THROUGH: Windows
        // can coalesce the buffered writes while Load Everything is producing hundreds of
        // Timeline frames. Cache metadata/favorites keep their stricter durability path.
        if(MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING)) return true;
        DeleteFileW(temporary.c_str());
        return false;
    }

    std::wstring BuildPreviewDirectory(const std::wstring& source) const {
        std::wstring sig=L"details-previews-v5-stereo-lock|"+source;
        std::error_code ec;
        auto sz=fs::file_size(source,ec); if(!ec) sig+=L"|"+std::to_wstring(sz);
        ec.clear(); auto ft=fs::last_write_time(source,ec); if(!ec) sig+=L"|"+std::to_wstring(ft.time_since_epoch().count());
        const uint64_t hash=Fnv1a64(sig);
        wchar_t name[40]{}; swprintf_s(name,L"%016llx",static_cast<unsigned long long>(hash));
        return (CacheRootForSource(source)/L"previews"/name).wstring();
    }

    void HideCacheRootIfCreated(const std::wstring& source) const {
        if (source.empty()) return;
        const fs::path root=CacheRootForSource(source);
        std::error_code ec;
        if (!fs::exists(root,ec)) return;
        const DWORD attrs=GetFileAttributesW(root.c_str());
        if (attrs==INVALID_FILE_ATTRIBUTES) return;
        if ((attrs&FILE_ATTRIBUTE_HIDDEN)==0) SetFileAttributesW(root.c_str(),attrs|FILE_ATTRIBUTE_HIDDEN);
    }

    static std::wstring CacheCleanupPathKey(const fs::path& path) {
        return ToLower(path.lexically_normal().wstring());
    }

    static bool IsHexCacheHash(const std::wstring& text) {
        if (text.size()!=16) return false;
        for (wchar_t c:text) {
            if (!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')||(c>=L'A'&&c<=L'F'))) return false;
        }
        return true;
    }

    static bool IsManagedThumbCacheFilename(const std::wstring& rawName) {
        const std::wstring name=ToLower(rawName);
        if (name.size()<20 || !IsHexCacheHash(name.substr(0,16))) return false;
        const std::wstring suffix=name.substr(16);
        // Only names created by Visual MediaPlayer are eligible for orphan cleanup.
        // Unknown files are deliberately left alone even inside our cache directory.
        return suffix==L".jpg" || suffix==L".jpg.time" ||
               suffix==L".ui.jpg" || suffix==L".ui.jpg.time" || suffix==L".ui.jpg.resolution" || suffix==L".ui.jpg.resolution.tmp" ||
               suffix==L".jpg.tmp.jpg" || suffix==L".jpg.tmp.jpg.time" ||
               suffix==L".ui.jpg.tmp.jpg" || suffix==L".ui.jpg.tmp.jpg.time" ||
               suffix==L".jpg.tmp.jpg.tmp.jpg" || suffix==L".ui.jpg.tmp.jpg.tmp.jpg";
    }

    static bool IsManagedPreviewCacheFilename(const std::wstring& rawName) {
        const std::wstring name=ToLower(rawName);
        if (name==L"complete.txt" || name==L"layout.txt") return true;
        size_t i=0;
        while (i<name.size() && name[i]>=L'0' && name[i]<=L'9') ++i;
        if (i<6) return false;
        const std::wstring suffix=name.substr(i);
        return suffix==L".jpg" || suffix==L".jpg.tmp";
    }

    static void RemoveManagedPreviewDirectory(const std::wstring& dir) {
        if(dir.empty()) return;
        const fs::path root(dir);
        if(!IsRealDirectoryWithoutReparsePoint(root)) return;
        std::error_code ec;
        fs::directory_iterator it(root,fs::directory_options::skip_permission_denied,ec),end;
        if(ec) return;
        for(;it!=end;it.increment(ec)){
            if(ec) return;
            std::error_code entryEc;
            if(!it->is_regular_file(entryEc) || entryEc) continue;
            if(!IsManagedPreviewCacheFilename(it->path().filename().wstring())) continue;
            std::error_code removeEc; fs::remove(it->path(),removeEc);
        }
        ec.clear(); fs::remove(root,ec); // succeeds only when nothing unknown remains
    }

    static bool IsRealDirectoryWithoutReparsePoint(const fs::path& path) {
        const DWORD attrs=GetFileAttributesW(path.c_str());
        return attrs!=INVALID_FILE_ATTRIBUTES &&
               (attrs&FILE_ATTRIBUTE_DIRECTORY)!=0 &&
               (attrs&FILE_ATTRIBUTE_REPARSE_POINT)==0;
    }

    void CleanupOrphanMediaCache(const std::vector<fs::path>& discoveredCacheRoots) {
        // Deletion-only and fail-closed: never create a cache during cleanup, never
        // traverse reparse points, and never delete unknown files.
        if (folder_.empty() || discoveredCacheRoots.empty()) return;
        std::error_code rootEc;
        if (!fs::is_directory(folder_,rootEc) || rootEc) return;

        std::set<std::wstring> expectedThumbFiles;
        std::set<std::wstring> expectedPreviewDirs;
        auto remember=[&](const MediaItem& item) {
            if (item.isVideo && !item.cachePath.empty()) {
                expectedThumbFiles.insert(CacheCleanupPathKey(fs::path(item.cachePath)));
                expectedThumbFiles.insert(CacheCleanupPathKey(fs::path(BannerTimestampPath(item.cachePath))));
            }
            if (!item.uiCachePath.empty()) {
                expectedThumbFiles.insert(CacheCleanupPathKey(fs::path(item.uiCachePath)));
                if(item.isVideo) {
                    expectedThumbFiles.insert(CacheCleanupPathKey(fs::path(BannerTimestampPath(item.uiCachePath))));
                    expectedThumbFiles.insert(CacheCleanupPathKey(fs::path(ResolutionMetadataPath(item.uiCachePath))));
                }
            }
            if (item.isVideo) expectedPreviewDirs.insert(CacheCleanupPathKey(fs::path(BuildPreviewDirectory(item.path))));
        };
        for (const auto& item:videos_) remember(item);
        for (const auto& item:images_) remember(item);

        for (const fs::path& rawRoot:discoveredCacheRoots) {
            const fs::path cacheRoot=rawRoot.lexically_normal();
            if (ToLower(cacheRoot.filename().wstring())!=L".visualmediaplayer-cache") continue;
            if (!PathIsWithin(cacheRoot.wstring(),folder_)) continue;
            if (!IsRealDirectoryWithoutReparsePoint(cacheRoot)) continue;

            const fs::path thumbs=cacheRoot/L"thumbs";
            if (IsRealDirectoryWithoutReparsePoint(thumbs)) {
                std::vector<fs::path> orphanThumbs;
                bool safe=true;
                std::error_code ec;
                fs::directory_iterator it(thumbs,fs::directory_options::skip_permission_denied,ec),end;
                if (ec) safe=false;
                for (; safe && it!=end; it.increment(ec)) {
                    if (ec) { safe=false; break; }
                    std::error_code entryEc;
                    if (!it->is_regular_file(entryEc) || entryEc) { if(entryEc) safe=false; continue; }
                    if (!IsManagedThumbCacheFilename(it->path().filename().wstring())) continue;
                    if (expectedThumbFiles.find(CacheCleanupPathKey(it->path()))==expectedThumbFiles.end()) orphanThumbs.push_back(it->path());
                }
                if (safe) for (const auto& path:orphanThumbs) { std::error_code removeEc; fs::remove(path,removeEc); }
            }

            const fs::path previews=cacheRoot/L"previews";
            if (IsRealDirectoryWithoutReparsePoint(previews)) {
                std::vector<fs::path> orphanDirs;
                bool safe=true;
                std::error_code ec;
                fs::directory_iterator it(previews,fs::directory_options::skip_permission_denied,ec),end;
                if(ec) safe=false;
                for(;safe && it!=end;it.increment(ec)) {
                    if(ec){safe=false;break;}
                    std::error_code entryEc;
                    if(!it->is_directory(entryEc)||entryEc){if(entryEc)safe=false;continue;}
                    if(!IsHexCacheHash(it->path().filename().wstring())) continue;
                    if(!IsRealDirectoryWithoutReparsePoint(it->path())) continue;
                    if(expectedPreviewDirs.find(CacheCleanupPathKey(it->path()))==expectedPreviewDirs.end()) orphanDirs.push_back(it->path());
                }
                if(safe) {
                    for(const auto& dir:orphanDirs) {
                        std::vector<fs::path> generated;
                        bool dirSafe=true;
                        std::error_code dirEc;
                        fs::directory_iterator pit(dir,fs::directory_options::skip_permission_denied,dirEc),pend;
                        if(dirEc) dirSafe=false;
                        for(;dirSafe && pit!=pend;pit.increment(dirEc)) {
                            if(dirEc){dirSafe=false;break;}
                            std::error_code entryEc;
                            if(!pit->is_regular_file(entryEc)||entryEc){if(entryEc)dirSafe=false;continue;}
                            if(IsManagedPreviewCacheFilename(pit->path().filename().wstring())) generated.push_back(pit->path());
                        }
                        if(!dirSafe) continue;
                        for(const auto& path:generated){std::error_code removeEc;fs::remove(path,removeEc);}
                        std::error_code removeDirEc; fs::remove(dir,removeDirEc); // only succeeds if truly empty
                    }
                }
            }

            std::error_code ec;
            if(IsRealDirectoryWithoutReparsePoint(thumbs)) fs::remove(thumbs,ec);
            ec.clear();
            if(IsRealDirectoryWithoutReparsePoint(previews)) fs::remove(previews,ec);
            ec.clear();
            fs::remove(cacheRoot,ec); // only if empty
        }
    }

    static int ResolvePreviewLayout(VRInfo vr, UINT w, UINT h) {
        if (!vr.vr || vr.layoutExplicit) return vr.layout;
        if (!w || !h) return 0;
        const float aspect=static_cast<float>(w)/static_cast<float>(h);
        if (vr.projection==1) {
            if (aspect>=3.20f) return 1;
            if (aspect<=1.20f) return 2;
            return 0;
        }
        if (vr.projection==2) {
            if (aspect>=1.70f && aspect<3.20f) return 1;
            if (aspect<=0.70f) return 2;
        }
        return 0;
    }

    struct StereoMetric {
        double mad = 1e9;
        double corr = -1.0;
        double contrast = 0.0;
    };

    static double PixelLuma(const Gdiplus::Color& c) {
        return 0.2126 * c.GetR() + 0.7152 * c.GetG() + 0.0722 * c.GetB();
    }

    static StereoMetric MeasureStereoLR(Gdiplus::Bitmap& bitmap) {
        const UINT w=bitmap.GetWidth(), h=bitmap.GetHeight();
        if (w<16 || h<8) return {};
        const UINT half=w/2u;
        const int maxShift=std::clamp(static_cast<int>(half/12u),4,64);
        StereoMetric best{};
        constexpr int samplesX=20, samplesY=12;

        for(int shift=-maxShift;shift<=maxShift;++shift){
            double sumA=0.0,sumB=0.0,sumAA=0.0,sumBB=0.0,sumAB=0.0,mad=0.0;
            int count=0;
            for(int gy=0;gy<samplesY;++gy){
                const UINT y=std::min<UINT>(h-1u,static_cast<UINT>((gy+0.5)*h/samplesY));
                for(int gx=0;gx<samplesX;++gx){
                    const UINT x=std::min<UINT>(half-1u,static_cast<UINT>((gx+0.5)*half/samplesX));
                    const int bx=std::clamp(static_cast<int>(x+half)+shift,static_cast<int>(half),static_cast<int>(w)-1);
                    Gdiplus::Color a,b;
                    if(bitmap.GetPixel(static_cast<INT>(x),static_cast<INT>(y),&a)!=Gdiplus::Ok) continue;
                    if(bitmap.GetPixel(bx,static_cast<INT>(y),&b)!=Gdiplus::Ok) continue;
                    const double av=PixelLuma(a),bv=PixelLuma(b);
                    sumA+=av;sumB+=bv;sumAA+=av*av;sumBB+=bv*bv;sumAB+=av*bv;mad+=std::abs(av-bv);++count;
                }
            }
            if(count<24) continue;
            const double meanA=sumA/count,meanB=sumB/count;
            const double varA=std::max(0.0,sumAA/count-meanA*meanA);
            const double varB=std::max(0.0,sumBB/count-meanB*meanB);
            const double denom=std::sqrt(varA*varB);
            const double corr=denom>1e-6?(sumAB/count-meanA*meanB)/denom:-1.0;
            const double contrast=std::sqrt(std::max(0.0,(varA+varB)*0.5));
            const double currentMad=mad/count;
            if(corr>best.corr || (std::abs(corr-best.corr)<0.015 && currentMad<best.mad))
                best={currentMad,corr,contrast};
        }
        return best;
    }

    static StereoMetric MeasureStereoTB(Gdiplus::Bitmap& bitmap) {
        const UINT w=bitmap.GetWidth(), h=bitmap.GetHeight();
        if (w<8 || h<16) return {};
        const UINT half=h/2u;
        const int maxShift=std::clamp(static_cast<int>(w/24u),4,64);
        StereoMetric best{};
        constexpr int samplesX=20, samplesY=12;

        for(int shift=-maxShift;shift<=maxShift;++shift){
            double sumA=0.0,sumB=0.0,sumAA=0.0,sumBB=0.0,sumAB=0.0,mad=0.0;
            int count=0;
            for(int gy=0;gy<samplesY;++gy){
                const UINT y=std::min<UINT>(half-1u,static_cast<UINT>((gy+0.5)*half/samplesY));
                for(int gx=0;gx<samplesX;++gx){
                    const UINT x=std::min<UINT>(w-1u,static_cast<UINT>((gx+0.5)*w/samplesX));
                    const int bx=std::clamp(static_cast<int>(x)+shift,0,static_cast<int>(w)-1);
                    Gdiplus::Color a,b;
                    if(bitmap.GetPixel(static_cast<INT>(x),static_cast<INT>(y),&a)!=Gdiplus::Ok) continue;
                    if(bitmap.GetPixel(bx,static_cast<INT>(y+half),&b)!=Gdiplus::Ok) continue;
                    const double av=PixelLuma(a),bv=PixelLuma(b);
                    sumA+=av;sumB+=bv;sumAA+=av*av;sumBB+=bv*bv;sumAB+=av*bv;mad+=std::abs(av-bv);++count;
                }
            }
            if(count<24) continue;
            const double meanA=sumA/count,meanB=sumB/count;
            const double varA=std::max(0.0,sumAA/count-meanA*meanA);
            const double varB=std::max(0.0,sumBB/count-meanB*meanB);
            const double denom=std::sqrt(varA*varB);
            const double corr=denom>1e-6?(sumAB/count-meanA*meanB)/denom:-1.0;
            const double contrast=std::sqrt(std::max(0.0,(varA+varB)*0.5));
            const double currentMad=mad/count;
            if(corr>best.corr || (std::abs(corr-best.corr)<0.015 && currentMad<best.mad))
                best={currentMad,corr,contrast};
        }
        return best;
    }

    static bool LikelyStereo(const StereoMetric& m) {
        // Allow realistic stereo parallax. A confident left/right or top/bottom match
        // should be preferred over a single-frame mono guess.
        if (m.contrast < 5.0) return false;
        return (m.corr >= 0.72 && m.mad <= 72.0) ||
               (m.corr >= 0.58 && m.mad <= 42.0);
    }

    static int ResolveStillLayout(VRInfo vr, Gdiplus::Bitmap& bitmap, int initialLayout) {
        if (!vr.vr) return 0;
        if (vr.layoutExplicit) return vr.layout;
        if (initialLayout==1 || initialLayout==2) return initialLayout;

        const double aspect=static_cast<double>(bitmap.GetWidth())/std::max<UINT>(1u,bitmap.GetHeight());
        // The main ambiguous case is a ~2:1 VR frame: mono 360 and two square SBS eyes
        // share the same dimensions. Compare the halves with a small parallax shift allowance.
        if (aspect >= 1.30) return LikelyStereo(MeasureStereoLR(bitmap)) ? 1 : 0;
        if (aspect <= 0.82) return LikelyStereo(MeasureStereoTB(bitmap)) ? 2 : 0;
        return 0;
    }

    struct ThreadWicFactoryState {
        ComPtr<IWICImagingFactory> factory;
    };

    static ThreadWicFactoryState& CurrentThreadWicFactoryState() {
        thread_local ThreadWicFactoryState state;
        return state;
    }

    static IWICImagingFactory* ThreadWicFactory() {
        // Reuse one WIC factory per COM-initialized worker/UI thread. Recreating the
        // factory for every JPEG decode/encode added measurable overhead during large
        // Timeline loads and fast scrolling. The holder is explicitly reset before
        // CoUninitialize() so its COM Release never occurs after COM has shut down.
        auto& state=CurrentThreadWicFactoryState();
        if(!state.factory){
            if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(state.factory.GetAddressOf())))) return nullptr;
        }
        return state.factory.Get();
    }

    static void ReleaseThreadWicFactory() {
        CurrentThreadWicFactoryState().factory.Reset();
    }

    static bool EncodeWicJpeg(IWICImagingFactory* factory, IWICBitmapSource* source,
                              const std::wstring& outputPath, UINT width, UINT height,
                              ULONG quality) {
        if(!factory || !source || outputPath.empty() || !width || !height) return false;
        ComPtr<IWICBitmapEncoder> encoder;
        if(FAILED(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,encoder.GetAddressOf())) || !encoder) return false;
        ComPtr<IStream> stream;
        if(FAILED(SHCreateStreamOnFileEx(outputPath.c_str(),STGM_CREATE|STGM_WRITE|STGM_SHARE_DENY_WRITE,
                                         FILE_ATTRIBUTE_NORMAL,TRUE,nullptr,stream.GetAddressOf())) || !stream) return false;
        if(FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))) return false;

        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if(FAILED(encoder->CreateNewFrame(frame.GetAddressOf(),options.GetAddressOf())) || !frame) return false;
        if(options){
            PROPBAG2 bag{};
            bag.pstrName=const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value; VariantInit(&value);
            value.vt=VT_R4;
            value.fltVal=std::clamp(static_cast<float>(quality)/100.0f,0.0f,1.0f);
            options->Write(1,&bag,&value);
            VariantClear(&value);
        }
        if(FAILED(frame->Initialize(options.Get()))) return false;
        if(FAILED(frame->SetSize(width,height))) return false;
        WICPixelFormatGUID pixelFormat=GUID_WICPixelFormat24bppBGR;
        if(FAILED(frame->SetPixelFormat(&pixelFormat))) return false;

        ComPtr<IWICFormatConverter> converter;
        IWICBitmapSource* writeSource=source;
        WICPixelFormatGUID sourceFormat{};
        if(FAILED(source->GetPixelFormat(&sourceFormat)) || sourceFormat!=pixelFormat){
            if(FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) || !converter) return false;
            if(FAILED(converter->Initialize(source,pixelFormat,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom))) return false;
            writeSource=converter.Get();
        }
        if(FAILED(frame->WriteSource(writeSource,nullptr))) return false;
        if(FAILED(frame->Commit())) return false;
        if(FAILED(encoder->Commit())) return false;
        return true;
    }

    static double RawPixelLuma(const BYTE* scan0, LONG pitch, UINT x, UINT y) {
        const BYTE* row=scan0+static_cast<LONG_PTR>(y)*static_cast<LONG_PTR>(pitch);
        const BYTE* px=row+static_cast<size_t>(x)*4u; // MFVideoFormat_RGB32 = B,G,R,X
        return 0.2126*px[2]+0.7152*px[1]+0.0722*px[0];
    }

    static StereoMetric MeasureStereoLRRaw(const BYTE* scan0,LONG pitch,UINT w,UINT h) {
        if(!scan0 || w<16 || h<8) return {};
        const UINT half=w/2u;
        const int maxShift=std::clamp(static_cast<int>(half/12u),4,64);
        StereoMetric best{};
        constexpr int samplesX=20,samplesY=12;
        for(int shift=-maxShift;shift<=maxShift;++shift){
            double sumA=0.0,sumB=0.0,sumAA=0.0,sumBB=0.0,sumAB=0.0,mad=0.0;
            int count=0;
            for(int gy=0;gy<samplesY;++gy){
                const UINT y=std::min<UINT>(h-1u,static_cast<UINT>((gy+0.5)*h/samplesY));
                for(int gx=0;gx<samplesX;++gx){
                    const UINT x=std::min<UINT>(half-1u,static_cast<UINT>((gx+0.5)*half/samplesX));
                    const int bx=std::clamp(static_cast<int>(x+half)+shift,static_cast<int>(half),static_cast<int>(w)-1);
                    const double av=RawPixelLuma(scan0,pitch,x,y),bv=RawPixelLuma(scan0,pitch,static_cast<UINT>(bx),y);
                    sumA+=av;sumB+=bv;sumAA+=av*av;sumBB+=bv*bv;sumAB+=av*bv;mad+=std::abs(av-bv);++count;
                }
            }
            if(count<24) continue;
            const double meanA=sumA/count,meanB=sumB/count;
            const double varA=std::max(0.0,sumAA/count-meanA*meanA);
            const double varB=std::max(0.0,sumBB/count-meanB*meanB);
            const double denom=std::sqrt(varA*varB);
            const double corr=denom>1e-6?(sumAB/count-meanA*meanB)/denom:-1.0;
            const double contrast=std::sqrt(std::max(0.0,(varA+varB)*0.5));
            const double currentMad=mad/count;
            if(corr>best.corr || (std::abs(corr-best.corr)<0.015 && currentMad<best.mad)) best={currentMad,corr,contrast};
        }
        return best;
    }

    static StereoMetric MeasureStereoTBRaw(const BYTE* scan0,LONG pitch,UINT w,UINT h) {
        if(!scan0 || w<8 || h<16) return {};
        const UINT half=h/2u;
        const int maxShift=std::clamp(static_cast<int>(w/24u),4,64);
        StereoMetric best{};
        constexpr int samplesX=20,samplesY=12;
        for(int shift=-maxShift;shift<=maxShift;++shift){
            double sumA=0.0,sumB=0.0,sumAA=0.0,sumBB=0.0,sumAB=0.0,mad=0.0;
            int count=0;
            for(int gy=0;gy<samplesY;++gy){
                const UINT y=std::min<UINT>(half-1u,static_cast<UINT>((gy+0.5)*half/samplesY));
                for(int gx=0;gx<samplesX;++gx){
                    const UINT x=std::min<UINT>(w-1u,static_cast<UINT>((gx+0.5)*w/samplesX));
                    const int by=std::clamp(static_cast<int>(y+half)+shift,static_cast<int>(half),static_cast<int>(h)-1);
                    const double av=RawPixelLuma(scan0,pitch,x,y),bv=RawPixelLuma(scan0,pitch,x,static_cast<UINT>(by));
                    sumA+=av;sumB+=bv;sumAA+=av*av;sumBB+=bv*bv;sumAB+=av*bv;mad+=std::abs(av-bv);++count;
                }
            }
            if(count<24) continue;
            const double meanA=sumA/count,meanB=sumB/count;
            const double varA=std::max(0.0,sumAA/count-meanA*meanA);
            const double varB=std::max(0.0,sumBB/count-meanB*meanB);
            const double denom=std::sqrt(varA*varB);
            const double corr=denom>1e-6?(sumAB/count-meanA*meanB)/denom:-1.0;
            const double contrast=std::sqrt(std::max(0.0,(varA+varB)*0.5));
            const double currentMad=mad/count;
            if(corr>best.corr || (std::abs(corr-best.corr)<0.015 && currentMad<best.mad)) best={currentMad,corr,contrast};
        }
        return best;
    }

    static int ResolveStillLayoutRaw(VRInfo vr,const BYTE* scan0,LONG pitch,UINT width,UINT height,int initialLayout) {
        if(!vr.vr) return 0;
        if(vr.layoutExplicit) return vr.layout;
        if(initialLayout==1 || initialLayout==2) return initialLayout;
        const double aspect=static_cast<double>(width)/std::max<UINT>(1u,height);
        if(aspect>=1.30) return LikelyStereo(MeasureStereoLRRaw(scan0,pitch,width,height))?1:0;
        if(aspect<=0.82) return LikelyStereo(MeasureStereoTBRaw(scan0,pitch,width,height))?2:0;
        return 0;
    }

    static bool SaveVideoSampleJpegWic(IMFSample* sample,UINT width,UINT height,LONG defaultStride,
                                       VRInfo vr,int& layoutState,const std::wstring& output,
                                       int outW,int outH,ULONG quality,bool cover) {
        if(!sample || !width || !height || outW<=0 || outH<=0) return false;
        IWICImagingFactory* factory=ThreadWicFactory();
        if(!factory) return false;
        ComPtr<IMFMediaBuffer> buffer;
        if(FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return false;

        BYTE* scan0=nullptr; LONG pitch=defaultStride; bool locked2D=false;
        ComPtr<IMF2DBuffer> buffer2D; BYTE* raw=nullptr; DWORD maxLen=0,currentLen=0;
        if(SUCCEEDED(buffer.As(&buffer2D)) && buffer2D && SUCCEEDED(buffer2D->Lock2D(&scan0,&pitch))){
            locked2D=true;
        }else{
            if(FAILED(buffer->Lock(&raw,&maxLen,&currentLen)) || !raw) return false;
            if(!pitch) pitch=static_cast<LONG>(width*4u);
            scan0=raw;
            if(pitch<0) scan0=raw+static_cast<size_t>(height-1u)*static_cast<size_t>(-pitch);
        }

        bool ok=false;
        do{
            if(layoutState<0) layoutState=ResolveStillLayoutRaw(vr,scan0,pitch,width,height,0);
            const int layout=std::max(0,layoutState);
            UINT sx=0,sy=0,sw=width,sh=height;
            if(layout==1 && width>=2) sw=width/2u;
            else if(layout==2 && height>=2) sh=height/2u;

            if(width>UINT_MAX/4u) break;
            const UINT tightStride=width*4u;
            std::vector<BYTE> packed;
            const BYTE* bitmapData=scan0;
            UINT bitmapStride=0,bitmapBytes=0;
            if(pitch>0 && static_cast<UINT>(pitch)>=tightStride){
                bitmapStride=static_cast<UINT>(pitch);
                if(height>UINT_MAX/bitmapStride) break;
                bitmapBytes=bitmapStride*height;
            }else{
                if(height>UINT_MAX/tightStride) break;
                bitmapStride=tightStride; bitmapBytes=bitmapStride*height;
                packed.resize(bitmapBytes);
                for(UINT y=0;y<height;++y){
                    const BYTE* srcRow=scan0+static_cast<LONG_PTR>(y)*static_cast<LONG_PTR>(pitch);
                    std::memcpy(packed.data()+static_cast<size_t>(y)*bitmapStride,srcRow,tightStride);
                }
                bitmapData=packed.data();
            }

            ComPtr<IWICBitmap> sourceBitmap;
            if(FAILED(factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat32bppBGR,
                                                      bitmapStride,bitmapBytes,const_cast<BYTE*>(bitmapData),
                                                      sourceBitmap.GetAddressOf())) || !sourceBitmap) break;

            WICRect crop{static_cast<INT>(sx),static_cast<INT>(sy),static_cast<INT>(sw),static_cast<INT>(sh)};
            if(cover){
                const double scale=std::max(static_cast<double>(outW)/std::max<UINT>(1u,sw),
                                            static_cast<double>(outH)/std::max<UINT>(1u,sh));
                const UINT cropW=std::max<UINT>(1u,std::min<UINT>(sw,static_cast<UINT>(outW/scale+0.5)));
                const UINT cropH=std::max<UINT>(1u,std::min<UINT>(sh,static_cast<UINT>(outH/scale+0.5)));
                crop.X=static_cast<INT>(sx+(sw-cropW)/2u); crop.Y=static_cast<INT>(sy+(sh-cropH)/2u);
                crop.Width=static_cast<INT>(cropW); crop.Height=static_cast<INT>(cropH);
            }

            IWICBitmapSource* source=sourceBitmap.Get();
            ComPtr<IWICBitmapClipper> clipper;
            if(crop.X!=0 || crop.Y!=0 || static_cast<UINT>(crop.Width)!=width || static_cast<UINT>(crop.Height)!=height){
                if(FAILED(factory->CreateBitmapClipper(clipper.GetAddressOf())) || !clipper ||
                   FAILED(clipper->Initialize(sourceBitmap.Get(),&crop))) break;
                source=clipper.Get();
            }
            const UINT sourceW=static_cast<UINT>(crop.Width),sourceH=static_cast<UINT>(crop.Height);

            if(cover){
                ComPtr<IWICBitmapScaler> scaler;
                if(sourceW!=static_cast<UINT>(outW) || sourceH!=static_cast<UINT>(outH)){
                    if(FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) || !scaler ||
                       FAILED(scaler->Initialize(source,static_cast<UINT>(outW),static_cast<UINT>(outH),WICBitmapInterpolationModeFant))) break;
                    source=scaler.Get();
                }
                ok=EncodeWicJpeg(factory,source,output,static_cast<UINT>(outW),static_cast<UINT>(outH),quality);
                break;
            }

            const double fit=std::min(static_cast<double>(outW)/std::max<UINT>(1u,sourceW),
                                      static_cast<double>(outH)/std::max<UINT>(1u,sourceH));
            const UINT dw=std::max<UINT>(1u,std::min<UINT>(static_cast<UINT>(outW),static_cast<UINT>(std::lround(sourceW*fit))));
            const UINT dh=std::max<UINT>(1u,std::min<UINT>(static_cast<UINT>(outH),static_cast<UINT>(std::lround(sourceH*fit))));
            ComPtr<IWICBitmapScaler> scaler;
            IWICBitmapSource* fitted=source;
            if(dw!=sourceW || dh!=sourceH){
                if(FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) || !scaler ||
                   FAILED(scaler->Initialize(source,dw,dh,WICBitmapInterpolationModeFant))) break;
                fitted=scaler.Get();
            }
            if(dw==static_cast<UINT>(outW) && dh==static_cast<UINT>(outH)){
                ok=EncodeWicJpeg(factory,fitted,output,static_cast<UINT>(outW),static_cast<UINT>(outH),quality);
                break;
            }

            ComPtr<IWICFormatConverter> converter;
            if(FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) || !converter ||
               FAILED(converter->Initialize(fitted,GUID_WICPixelFormat32bppBGR,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom))) break;
            if(dw>UINT_MAX/4u) break;
            const UINT scaledStride=dw*4u;
            if(dh>UINT_MAX/scaledStride) break;
            std::vector<BYTE> scaledPixels(static_cast<size_t>(scaledStride)*dh);
            if(FAILED(converter->CopyPixels(nullptr,scaledStride,static_cast<UINT>(scaledPixels.size()),scaledPixels.data()))) break;

            ComPtr<IWICBitmap> canvas;
            if(FAILED(factory->CreateBitmap(static_cast<UINT>(outW),static_cast<UINT>(outH),GUID_WICPixelFormat32bppBGR,
                                            WICBitmapCacheOnLoad,canvas.GetAddressOf())) || !canvas) break;
            WICRect lockRect{0,0,outW,outH};
            ComPtr<IWICBitmapLock> lock;
            if(FAILED(canvas->Lock(&lockRect,WICBitmapLockWrite,lock.GetAddressOf())) || !lock) break;
            UINT canvasStride=0,canvasBytes=0; BYTE* canvasData=nullptr;
            if(FAILED(lock->GetStride(&canvasStride)) || FAILED(lock->GetDataPointer(&canvasBytes,&canvasData)) || !canvasData) break;
            for(int y=0;y<outH;++y){
                BYTE* row=canvasData+static_cast<size_t>(y)*canvasStride;
                for(int x=0;x<outW;++x){row[x*4+0]=24;row[x*4+1]=18;row[x*4+2]=16;row[x*4+3]=0;}
            }
            const UINT dx=(static_cast<UINT>(outW)-dw)/2u,dy=(static_cast<UINT>(outH)-dh)/2u;
            for(UINT y=0;y<dh;++y){
                std::memcpy(canvasData+static_cast<size_t>(dy+y)*canvasStride+static_cast<size_t>(dx)*4u,
                            scaledPixels.data()+static_cast<size_t>(y)*scaledStride,scaledStride);
            }
            lock.Reset();
            ok=EncodeWicJpeg(factory,canvas.Get(),output,static_cast<UINT>(outW),static_cast<UINT>(outH),quality);
        }while(false);

        if(locked2D) buffer2D->Unlock2D(); else buffer->Unlock();
        return ok;
    }

    static bool SaveVideoSampleJpegGdiFallback(IMFSample* sample, UINT width, UINT height, LONG defaultStride,
                                                VRInfo vr, int& layoutState, const std::wstring& output,
                                                int outW, int outH, ULONG quality, bool cover) {
        if (!sample || !width || !height || outW<=0 || outH<=0) return false;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return false;

        BYTE* scan0=nullptr;
        LONG pitch=defaultStride;
        bool locked2D=false;
        ComPtr<IMF2DBuffer> buffer2D;
        BYTE* raw=nullptr; DWORD maxLen=0,currentLen=0;
        if (SUCCEEDED(buffer.As(&buffer2D)) && buffer2D && SUCCEEDED(buffer2D->Lock2D(&scan0,&pitch))) {
            locked2D=true;
        } else {
            if (FAILED(buffer->Lock(&raw,&maxLen,&currentLen)) || !raw) return false;
            if (!pitch) pitch=static_cast<LONG>(width*4u);
            scan0=raw;
            if (pitch<0) scan0=raw+static_cast<size_t>(height-1u)*static_cast<size_t>(-pitch);
        }

        bool ok=false;
        {
            Gdiplus::Bitmap frame(static_cast<INT>(width),static_cast<INT>(height),pitch,PixelFormat32bppRGB,scan0);
            if (frame.GetLastStatus()==Gdiplus::Ok) {
                if (layoutState < 0) layoutState=ResolveStillLayout(vr,frame,0);
                const int layout=std::max(0,layoutState);
                UINT sx=0,sy=0,sw=width,sh=height;
                if (layout==1 && width>=2) sw=width/2u;
                else if (layout==2 && height>=2) sh=height/2u;

                Gdiplus::Bitmap out(outW,outH,PixelFormat24bppRGB);
                Gdiplus::Graphics g(&out);
                g.Clear(Gdiplus::Color(255,16,18,24));
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

                if (cover) {
                    const double scale=std::max(static_cast<double>(outW)/std::max<UINT>(1u,sw),static_cast<double>(outH)/std::max<UINT>(1u,sh));
                    const UINT cropW=std::max<UINT>(1u,std::min<UINT>(sw,static_cast<UINT>(outW/scale+0.5)));
                    const UINT cropH=std::max<UINT>(1u,std::min<UINT>(sh,static_cast<UINT>(outH/scale+0.5)));
                    const UINT cropX=sx+(sw-cropW)/2u,cropY=sy+(sh-cropH)/2u;
                    g.DrawImage(&frame,Gdiplus::Rect(0,0,outW,outH),static_cast<INT>(cropX),static_cast<INT>(cropY),static_cast<INT>(cropW),static_cast<INT>(cropH),Gdiplus::UnitPixel);
                } else {
                    const double scale=std::min(static_cast<double>(outW)/std::max<UINT>(1u,sw),static_cast<double>(outH)/std::max<UINT>(1u,sh));
                    const int dw=std::max(1,static_cast<int>(sw*scale));
                    const int dh=std::max(1,static_cast<int>(sh*scale));
                    const int dx=(outW-dw)/2,dy=(outH-dh)/2;
                    g.DrawImage(&frame,Gdiplus::Rect(dx,dy,dw,dh),static_cast<INT>(sx),static_cast<INT>(sy),static_cast<INT>(sw),static_cast<INT>(sh),Gdiplus::UnitPixel);
                }
                ok=SaveJpeg(out,output,quality);
            }
        }

        if (locked2D) buffer2D->Unlock2D(); else buffer->Unlock();
        return ok;
    }

    static bool SaveVideoSampleJpeg(IMFSample* sample, UINT width, UINT height, LONG defaultStride,
                                    VRInfo vr, int& layoutState, const std::wstring& output,
                                    int outW, int outH, ULONG quality, bool cover) {
        // WIC is the normal video-still crop/scale/JPEG path. GDI+ remains a compatibility
        // fallback only for an unusual WIC failure; Direct2D still owns final GPU display.
        if(SaveVideoSampleJpegWic(sample,width,height,defaultStride,vr,layoutState,output,outW,outH,quality,cover)) return true;
        return SaveVideoSampleJpegGdiFallback(sample,width,height,defaultStride,vr,layoutState,output,outW,outH,quality,cover);
    }

    static bool SavePreviewSample(IMFSample* sample, UINT width, UINT height, LONG defaultStride,
                                  VRInfo vr, int& layoutState, const std::wstring& output) {
        // Timeline stills use the same 16:9 center-cover presentation as Library posters.
        return SaveVideoSampleJpeg(sample,width,height,defaultStride,vr,layoutState,output,
                                   kVisualPreviewCacheWidth,kVisualPreviewCacheHeight,92,true);
    }

    static int DetectVideoSampleLayout(IMFSample* sample, UINT width, UINT height, LONG defaultStride, VRInfo vr) {
        // -1 means the frame is too dark/flat to make a useful decision. Keeping that
        // separate from a real mono vote prevents an intro/fade frame from poisoning
        // the layout choice for the entire video.
        if (!sample || !width || !height || !vr.vr) return 0;
        if (vr.layoutExplicit) return vr.layout;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return -1;

        BYTE* scan0=nullptr; LONG pitch=defaultStride; bool locked2D=false;
        ComPtr<IMF2DBuffer> buffer2D; BYTE* raw=nullptr; DWORD maxLen=0,currentLen=0;
        if (SUCCEEDED(buffer.As(&buffer2D)) && buffer2D && SUCCEEDED(buffer2D->Lock2D(&scan0,&pitch))) {
            locked2D=true;
        } else {
            if (FAILED(buffer->Lock(&raw,&maxLen,&currentLen)) || !raw) return -1;
            if (!pitch) pitch=static_cast<LONG>(width*4u);
            scan0=raw;
            if (pitch<0) scan0=raw+static_cast<size_t>(height-1u)*static_cast<size_t>(-pitch);
        }

        int result=-1;
        const double aspect=static_cast<double>(width)/std::max<UINT>(1u,height);
        if (aspect>=1.30) {
            const StereoMetric m=MeasureStereoLRRaw(scan0,pitch,width,height);
            if (m.contrast>=7.0) result=LikelyStereo(m)?1:0;
        } else if (aspect<=0.82) {
            const StereoMetric m=MeasureStereoTBRaw(scan0,pitch,width,height);
            if (m.contrast>=7.0) result=LikelyStereo(m)?2:0;
        } else {
            result=0;
        }
        if (locked2D) buffer2D->Unlock2D(); else buffer->Unlock();
        return result;
    }

    static int ReadCachedPreviewLayout(const std::wstring& previewDir) {
        std::ifstream in(fs::path(previewDir)/L"layout.txt",std::ios::binary);
        int value=-1; if(in) in>>value;
        return (value>=0&&value<=2)?value:-1;
    }

    static void WriteCachedPreviewLayout(const std::wstring& previewDir, int layout) {
        if(layout<0||layout>2) return;
        std::error_code ec; fs::create_directories(previewDir,ec); if(ec) return;
        std::ofstream out(fs::path(previewDir)/L"layout.txt",std::ios::binary|std::ios::trunc);
        if(out) out<<layout;
    }

    static std::vector<int> BuildPreviewCaptureSeconds(double duration) {
        std::vector<int> captureSeconds;
        if (!(duration > 0.0) || !std::isfinite(duration)) return captureSeconds;

        // Keep explicit first/end anchors, but do not show a cadence frame that is
        // effectively a duplicate of the end. The end label follows the same rounded
        // duration shown by the player while generation still seeks just before EOS.
        const int lastSec=std::max(0,static_cast<int>(std::llround(duration)));
        auto add=[&](int sec){
            sec=std::clamp(sec,0,lastSec);
            if(std::find(captureSeconds.begin(),captureSeconds.end(),sec)==captureSeconds.end()) captureSeconds.push_back(sec);
        };
        add(0);
        if(duration>=1.2){
            if (duration<61.0) {
                for (double f : {0.25,0.50,0.75}) add(static_cast<int>(std::round(duration*f)));
            } else {
                const int lastMinute=static_cast<int>(std::floor(std::max(0.0,duration-0.5)/60.0));
                for (int minute=1;minute<=lastMinute;++minute) add(minute*60);
            }
        }

        std::sort(captureSeconds.begin(),captureSeconds.end());
        // A 39:03 video should not show both 39:00 and 39:03. Scale the minimum
        // useful end gap with duration, but cap it so long videos never lose a frame
        // more than 12 seconds from the end. For a 6-second clip this is only 1 sec.
        const int minEndGap=std::clamp(static_cast<int>(std::llround(duration*0.05)),1,12);
        while(captureSeconds.size()>1 && captureSeconds.back()>0 &&
              lastSec-captureSeconds.back()<=minEndGap){
            captureSeconds.pop_back();
        }
        // If the only cadence point of a short ~1-minute clip was near the end,
        // retain useful context by inserting a midpoint rather than leaving just
        // start/end.
        if(duration>=4.0 && captureSeconds.size()==1 && lastSec>1) add(lastSec/2);
        add(lastSec);
        std::sort(captureSeconds.begin(),captureSeconds.end());
        return captureSeconds;
    }

    static double PreviewSeekSecondsForLabel(double duration,int labelSeconds) {
        if(!(duration>0.0) || !std::isfinite(duration)) return std::max(0.0,static_cast<double>(labelSeconds));
        const int lastSec=std::max(0,static_cast<int>(std::llround(duration)));
        if(labelSeconds==lastSec) return duration>0.05?std::max(0.0,duration-0.05):0.0;
        return std::clamp(static_cast<double>(labelSeconds),0.0,std::max(0.0,duration-0.05));
    }

    static bool FileHasData(const fs::path& path) {
        std::error_code ec;
        return fs::is_regular_file(path,ec) && !ec && fs::file_size(path,ec)>=256 && !ec;
    }

    static bool CachedImageHasExactSize(const std::wstring& path,int width,int height) {
        if(!CacheFileLooksHealthy(path,512)) return false;
        // WIC is the normal validation path too; do not instantiate a GDI+ decoder just
        // to check dimensions on every cached Timeline/Library master.
        if(IWICImagingFactory* factory=ThreadWicFactory()){
            ComPtr<IWICBitmapDecoder> decoder;
            if(SUCCEEDED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand,decoder.GetAddressOf())) && decoder){
                ComPtr<IWICBitmapFrameDecode> frame;
                UINT w=0,h=0;
                if(SUCCEEDED(decoder->GetFrame(0,frame.GetAddressOf())) && frame &&
                   SUCCEEDED(frame->GetSize(&w,&h)) && static_cast<int>(w)==width && static_cast<int>(h)==height) return true;
            }
        }
        // Compatibility fallback for an unusual codec/provider failure.
        Gdiplus::Image image(path.c_str(),FALSE);
        return image.GetLastStatus()==Gdiplus::Ok &&
               static_cast<int>(image.GetWidth())==width && static_cast<int>(image.GetHeight())==height;
    }

    static bool VisualPreviewMasterHealthy(const std::wstring& path) {
        return CachedImageHasExactSize(path,kVisualPreviewCacheWidth,kVisualPreviewCacheHeight);
    }

    static bool LibraryPreviewMasterHealthy(const std::wstring& path) {
        return CachedImageHasExactSize(path,kLibraryPreviewCacheWidth,kLibraryPreviewCacheHeight);
    }

    static bool GenerateVideoPreviewsMF(const std::wstring& source, const std::wstring& previewDir, VRInfo vr, std::atomic<bool>& stop, HWND notifyHwnd, std::atomic<double>* durationOut = nullptr, std::atomic<int>* progressCurrent = nullptr, std::atomic<int>* progressTotal = nullptr, const std::atomic<ULONGLONG>* pauseUntil = nullptr, UINT* nativeWidthOut = nullptr, UINT* nativeHeightOut = nullptr, const std::atomic<bool>* yieldRequested = nullptr) {
        // Static Timeline stills are generated for VR too. Animated hover previews stay
        // disabled elsewhere. VR stills use the same selected-eye + 16:9 center-cover
        // presentation as the Library and Info banner.
        ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs,2))) return false;
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE);
        attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS,FALSE);

        auto waitForPermit=[&]()->bool{
            while(!stop.load(std::memory_order_acquire)){
                if(yieldRequested && yieldRequested->load(std::memory_order_acquire)) return false;
                if(!pauseUntil) return true;
                const ULONGLONG until=pauseUntil->load(std::memory_order_acquire);
                const ULONGLONG now=GetTickCount64();
                if(until<=now) return true;
                Sleep(static_cast<DWORD>(std::min<ULONGLONG>(20,until-now)));
            }
            return false;
        };
        if(!waitForPermit()) return false;

        ComPtr<IMFSourceReader> reader;
        HRESULT hr=MFCreateSourceReaderFromURL(source.c_str(),attrs.Get(),&reader);
        if (FAILED(hr)||!reader) return false;
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),TRUE);

        ComPtr<IMFMediaType> nativeType;
        hr=reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&nativeType);
        if (FAILED(hr)||!nativeType) return false;
        UINT nativeW=0,nativeH=0;
        if (FAILED(MFGetAttributeSize(nativeType.Get(),MF_MT_FRAME_SIZE,&nativeW,&nativeH))||!nativeW||!nativeH) return false;
        if(nativeWidthOut) *nativeWidthOut=nativeW;
        if(nativeHeightOut) *nativeHeightOut=nativeH;
        const int resolvedLayout=ResolvePreviewLayout(vr,nativeW,nativeH);
        int layoutState=resolvedLayout;
        if(vr.vr&&!vr.layoutExplicit&&resolvedLayout==0){
            layoutState=ReadCachedPreviewLayout(previewDir);
        }

        // Decode enough *per-eye* source pixels to make a sharp 1920x1080 cover crop.
        // For SBS/TB VR, sizing the packed frame as if it were one flat picture halves
        // the useful eye resolution before the crop. Size against the selected eye first,
        // then request the corresponding packed dimensions from Media Foundation.
        UINT sizingEyeW=nativeW,sizingEyeH=nativeH;
        int decodeLayout=resolvedLayout;
        if(vr.vr && !vr.layoutExplicit && decodeLayout==0){
            const double aspect=static_cast<double>(nativeW)/std::max<UINT>(1u,nativeH);
            if(aspect>=1.30) decodeLayout=1;
            else if(aspect<=0.82) decodeLayout=2;
        }
        if(decodeLayout==1 && sizingEyeW>=2) sizingEyeW=std::max<UINT>(1u,sizingEyeW/2u);
        else if(decodeLayout==2 && sizingEyeH>=2) sizingEyeH=std::max<UINT>(1u,sizingEyeH/2u);
        const double scale=std::min(1.0,std::max(static_cast<double>(kVisualPreviewCacheWidth)/static_cast<double>(std::max<UINT>(1u,sizingEyeW)),
                                                 static_cast<double>(kVisualPreviewCacheHeight)/static_cast<double>(std::max<UINT>(1u,sizingEyeH))));
        UINT outW=std::max<UINT>(2u,static_cast<UINT>(nativeW*scale));
        UINT outH=std::max<UINT>(2u,static_cast<UINT>(nativeH*scale));
        outW=(outW+1u)&~1u; outH=(outH+1u)&~1u;

        ComPtr<IMFMediaType> outType;
        if (FAILED(MFCreateMediaType(&outType))) return false;
        outType->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
        MFSetAttributeSize(outType.Get(),MF_MT_FRAME_SIZE,outW,outH);
        MFSetAttributeRatio(outType.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
        hr=reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,outType.Get());
        if (FAILED(hr)) return false;

        ComPtr<IMFMediaType> actualType;
        reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),&actualType);
        UINT actualW=outW,actualH=outH;
        LONG stride=static_cast<LONG>(actualW*4u);
        if (actualType) {
            MFGetAttributeSize(actualType.Get(),MF_MT_FRAME_SIZE,&actualW,&actualH);
            UINT32 strideValue=0;
            if (SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE,&strideValue))) stride=static_cast<LONG>(strideValue);
        }

        PROPVARIANT durationVar; PropVariantInit(&durationVar);
        LONGLONG duration100ns=0;
        if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&durationVar))) {
            if (durationVar.vt==VT_UI8) duration100ns=static_cast<LONGLONG>(durationVar.uhVal.QuadPart);
            else if (durationVar.vt==VT_I8) duration100ns=durationVar.hVal.QuadPart;
        }
        PropVariantClear(&durationVar);
        if (duration100ns<=0) return false;
        const double duration=static_cast<double>(duration100ns)/10000000.0;
        if (durationOut) durationOut->store(duration, std::memory_order_relaxed);
        if (notifyHwnd) PostMessageW(notifyHwnd,WM_APP_PREVIEW_READY,0,0);

        const std::vector<int> captureSeconds=BuildPreviewCaptureSeconds(duration);
        if (captureSeconds.empty()) return false;

        // Version 5 changes the adaptive end-anchor spacing/label semantics. An
        // older short-video 75% frame can share the same second-based filename as the
        // new true end anchor, so refresh only the two anchors while reusing all other
        // healthy 1920x1080 stills from the previous cache version.
        bool markerIsCurrent=false;
        {
            std::ifstream marker(fs::path(previewDir)/L"complete.txt",std::ios::binary);
            double markerDuration=0.0; int markerVersion=0,markerW=0,markerH=0;
            if(marker && (marker>>markerDuration>>markerVersion>>markerW>>markerH))
                markerIsCurrent=markerVersion==kVisualPreviewCacheVersion && markerW==kVisualPreviewCacheWidth && markerH==kVisualPreviewCacheHeight;
        }
        if(!markerIsCurrent){
            const int firstAnchor=captureSeconds.front(),lastAnchor=captureSeconds.back();
            wchar_t anchorName[32]{}; swprintf_s(anchorName,L"%06d.jpg",firstAnchor);
            RemoveGeneratedCacheFile((fs::path(previewDir)/anchorName).wstring());
            if(lastAnchor!=firstAnchor){ swprintf_s(anchorName,L"%06d.jpg",lastAnchor); RemoveGeneratedCacheFile((fs::path(previewDir)/anchorName).wstring()); }
        }
        int alreadyComplete=0;
        for(int sec:captureSeconds){
            wchar_t progressName[32]{}; swprintf_s(progressName,L"%06d.jpg",sec);
            if(VisualPreviewMasterHealthy((fs::path(previewDir)/progressName).wstring())) ++alreadyComplete;
        }
        if(progressTotal) progressTotal->store(static_cast<int>(captureSeconds.size()),std::memory_order_relaxed);
        if(progressCurrent) progressCurrent->store(alreadyComplete,std::memory_order_relaxed);

        struct PendingSample { int sec=0; ComPtr<IMFSample> sample; };
        std::vector<PendingSample> pending;
        int layoutVotes[3]{0,0,0};
        bool any=false,allComplete=true;
        ULONGLONG lastPreviewNotify=0;
        int previewNotifyBudget=0;
        auto postPreviewProgress=[&](bool force){
            if(!notifyHwnd) return;
            ++previewNotifyBudget;
            const ULONGLONG now=GetTickCount64();
            if(force || previewNotifyBudget>=8 || now-lastPreviewNotify>=250){
                PostMessageW(notifyHwnd,WM_APP_PREVIEW_READY,0,0);
                lastPreviewNotify=now;
                previewNotifyBudget=0;
            }
        };

        auto saveOne=[&](int sec, IMFSample* sample)->bool{
            wchar_t name[32]{}; swprintf_s(name,L"%06d.jpg",sec);
            const std::wstring output=(fs::path(previewDir)/name).wstring();
            if(VisualPreviewMasterHealthy(output)){ any=true; return true; }
            if(FileHasData(output)) RemoveGeneratedCacheFile(output);
            std::error_code dirEc; fs::create_directories(previewDir,dirEc);
            if(dirEc){ allComplete=false; return false; }
            int finalLayout=std::max(0,layoutState);
            const std::wstring tmp=output+L".tmp"; DeleteFileW(tmp.c_str());
            if(!SavePreviewSample(sample,actualW,actualH,stride,vr,finalLayout,tmp)){
                DeleteFileW(tmp.c_str()); allComplete=false; return false;
            }
            if(!CommitGeneratedCacheFile(tmp,output)){
                allComplete=false; return false;
            }
            any=true;
            if(progressCurrent) progressCurrent->fetch_add(1,std::memory_order_relaxed);
            postPreviewProgress(false);
            return true;
        };

        auto finalizePendingLayout=[&](){
            if(layoutState<0){
                if(layoutVotes[1]>=2) layoutState=1;
                else if(layoutVotes[2]>=2) layoutState=2;
                // If only one useful frame was visible and it confidently identified a
                // stereo orientation, prefer that over otherwise inconclusive dark frames.
                else if(layoutVotes[1]>0 && layoutVotes[2]==0 && layoutVotes[1]>=layoutVotes[0]) layoutState=1;
                else if(layoutVotes[2]>0 && layoutVotes[1]==0 && layoutVotes[2]>=layoutVotes[0]) layoutState=2;
                else if(layoutVotes[1]>layoutVotes[0] && layoutVotes[1]>layoutVotes[2]) layoutState=1;
                else if(layoutVotes[2]>layoutVotes[0] && layoutVotes[2]>layoutVotes[1]) layoutState=2;
                else layoutState=0;
                WriteCachedPreviewLayout(previewDir,layoutState);
            }
            for(auto& p:pending){
                if(stop.load()){ allComplete=false; break; }
                saveOne(p.sec,p.sample.Get());
            }
            pending.clear();
        };

        for (int sec : captureSeconds) {
            if (!waitForPermit() || stop.load()) { allComplete=false; break; }
            wchar_t name[32]{}; swprintf_s(name,L"%06d.jpg",sec);
            const std::wstring output=(fs::path(previewDir)/name).wstring();
            if (VisualPreviewMasterHealthy(output)) { any=true; continue; }
            if(FileHasData(output)) RemoveGeneratedCacheFile(output);

            // The filename/label stays second-based for deterministic cache paths, while
            // the represented seek timestamp is exact. The end card therefore never seeks
            // past EOS even when the displayed duration rounds up to the next second.
            const double captureAt=PreviewSeekSecondsForLabel(duration,sec);
            const LONGLONG captureAt100ns=static_cast<LONGLONG>(captureAt*10000000.0);
            PROPVARIANT pos; PropVariantInit(&pos); pos.vt=VT_I8; pos.hVal.QuadPart=captureAt100ns;
            hr=reader->SetCurrentPosition(GUID_NULL,pos); PropVariantClear(&pos);
            if (FAILED(hr)) { allComplete=false; continue; }

            ComPtr<IMFSample> chosen;
            for (int attempts=0;attempts<240 && !stop.load();++attempts) {
                if((attempts&7)==0 && !waitForPermit()) break;
                DWORD streamIndex=0,flags=0; LONGLONG timestamp=0; ComPtr<IMFSample> sample;
                hr=reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&streamIndex,&flags,&timestamp,&sample);
                if (FAILED(hr)||(flags&MF_SOURCE_READERF_ENDOFSTREAM)) break;
                if (!sample) continue;
                chosen=sample;
                if (timestamp>=captureAt100ns) break;
            }
            if (!chosen||stop.load()) { allComplete=false; continue; }

            if(layoutState<0){
                const int candidate=DetectVideoSampleLayout(chosen.Get(),actualW,actualH,stride,vr);
                if(candidate>=0 && candidate<=2) ++layoutVotes[candidate];
                PendingSample pendingItem; pendingItem.sec=sec; pendingItem.sample=chosen;
                pending.push_back(std::move(pendingItem));
                // Two agreeing stereo detections are enough. Otherwise inspect up to
                // three representative frames; inconclusive dark frames do not vote mono.
                if(layoutVotes[1]>=2 || layoutVotes[2]>=2 || pending.size()>=5) finalizePendingLayout();
            } else {
                saveOne(sec,chosen.Get());
            }
        }

        if(!pending.empty()&&!stop.load()) finalizePendingLayout();
        postPreviewProgress(true);

        const fs::path markerPath=fs::path(previewDir)/L"complete.txt";
        if (!stop.load() && any && allComplete) {
            std::ofstream marker(markerPath,std::ios::binary|std::ios::trunc);
            if (marker) marker<<duration<<" "<<kVisualPreviewCacheVersion<<" "
                              <<kVisualPreviewCacheWidth<<" "<<kVisualPreviewCacheHeight;

            // Remove numeric JPEGs that belonged to an older cadence/version. This is
            // intentionally done only after the new Timeline is complete, so migration
            // can reuse healthy frames without risking a half-empty cache on failure.
            std::set<int> expectedSeconds(captureSeconds.begin(),captureSeconds.end());
            std::error_code cleanupEc;
            for(const auto& entry:fs::directory_iterator(previewDir,cleanupEc)){
                if(cleanupEc) break;
                if(!entry.is_regular_file(cleanupEc) || ToLower(entry.path().extension().wstring())!=L".jpg") continue;
                const std::wstring stem=entry.path().stem().wstring();
                wchar_t* end=nullptr; const long sec=wcstol(stem.c_str(),&end,10);
                if(!end || *end!=L'\0' || sec<0) continue;
                if(expectedSeconds.find(static_cast<int>(sec))==expectedSeconds.end()) RemoveGeneratedCacheFile(entry.path().wstring());
            }
        } else {
            std::error_code markerEc; fs::remove(markerPath,markerEc);
        }
        return any;
    }

    static void DeletePreviewFrameBitmaps(std::vector<PreviewFrame>& frames) {
        for(auto& p:frames){ if(p.bitmap) DeleteObject(p.bitmap); p.bitmap=nullptr; p.gpuBitmap.Reset(); p.gpuBitmapSource=nullptr; p.gpuGeneration=0; }
    }

    static void DeleteLibraryHoverPreviewBitmaps(std::vector<LibraryHoverPreviewFrame>& frames) {
        for(auto& frame:frames){ if(frame.bitmap) DeleteObject(frame.bitmap); frame.bitmap=nullptr; frame.width=frame.height=0; frame.gpuBitmap.Reset(); frame.gpuBitmapSource=nullptr; frame.gpuGeneration=0; }
        frames.clear();
    }

    void ClearPreviewBitmaps() {
        CancelPreviewBitmapDecodeJobs();
        previewAsyncDecodePreferred_=false;
        previewZoomGestureActive_=false;
        if(hwnd_) KillTimer(hwnd_,kPreviewZoomSettleTimerId);
        DeletePreviewFrameBitmaps(previewFrames_);
        previewFrames_.clear();
        previewMediaPath_.clear();
    }

    void ClearPrefetchedPreviewSets() {
        for(auto& kv:prefetchedPreviewSets_) DeletePreviewFrameBitmaps(kv.second.frames);
        prefetchedPreviewSets_.clear();
    }

    void FreeAllDetailBanners() {
        auto freeList=[](std::vector<MediaItem>& list){
            for(auto& item:list){
                if(item.detailThumb) DeleteObject(item.detailThumb);
                item.detailThumb=nullptr; item.detailThumbW=0; item.detailThumbH=0;
                item.detailsGpuThumb.Reset(); item.detailsGpuThumbSource=nullptr; item.detailsGpuGeneration=0;
            }
        };
        freeList(videos_); freeList(images_);
    }

    void CancelDetailPrefetchJobs() {
        detailPrefetchGeneration_.fetch_add(1,std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(detailPrefetchMutex_);
            detailPrefetchJobs_.clear();
        }
        detailPrefetchCv_.notify_all();
    }

    void ClearAllDetailInfoMemory() {
        CancelDetailPrefetchJobs();
        ClearPreviewBitmaps();
        ClearPrefetchedPreviewSets();
        FreeAllDetailBanners();
        previewDir_.clear();
        detailsDurationSeconds_.store(0.0,std::memory_order_relaxed);
    }

    PrefetchedPreviewSet LoadPrefetchedPreviewSet(const std::wstring& dir) {
        PrefetchedPreviewSet out;
        out.duration=ReadCachedPreviewDurationFromDir(dir);
        if(!(out.duration>0.0)) return out;
        const auto expected=BuildPreviewCaptureSeconds(out.duration);
        out.frames.reserve(expected.size());
        size_t decoded=0;
        for(int sec:expected){
            wchar_t name[32]{}; swprintf_s(name,L"%06d.jpg",sec);
            PreviewFrame frame; frame.seconds=sec; frame.seekSeconds=PreviewSeekSecondsForLabel(out.duration,sec); frame.path=(fs::path(dir)/name).wstring();
            if(decoded<24 && FileHasData(frame.path)){
                const int decodeW=QuantizedPreviewDecodeWidth(kDefaultPreviewCardWidth);
                frame.bitmap=LoadScaledBitmap(frame.path,decodeW,PreviewDecodeHeightForWidth(decodeW));
                if(frame.bitmap){frame.lastUsed=GetTickCount64();++decoded;}
            }
            out.frames.push_back(std::move(frame));
        }
        return out;
    }

    void ParkActivePreviewSet() {
        if(previewMediaPath_.empty() || previewFrames_.empty()) return;
        auto it=prefetchedPreviewSets_.find(previewMediaPath_);
        if(it!=prefetchedPreviewSets_.end()){
            DeletePreviewFrameBitmaps(it->second.frames);
            prefetchedPreviewSets_.erase(it);
        }
        PrefetchedPreviewSet set;
        set.frames=std::move(previewFrames_);
        set.duration=detailsDurationSeconds_.load(std::memory_order_relaxed);
        prefetchedPreviewSets_.emplace(previewMediaPath_,std::move(set));
        previewFrames_.clear();
        previewMediaPath_.clear();
    }

    bool RestorePrefetchedPreviewSet(const std::wstring& mediaPath) {
        auto it=prefetchedPreviewSets_.find(mediaPath);
        if(it==prefetchedPreviewSets_.end()) return false;
        previewFrames_=std::move(it->second.frames);
        detailsDurationSeconds_.store(it->second.duration,std::memory_order_relaxed);
        prefetchedPreviewSets_.erase(it);
        previewMediaPath_=mediaPath;
        return true;
    }

    bool HoverPreviewAudioContextStillValid() const {
        if(hoverPreviewAudioSurface_==MediaHoverSurface::Library) {
            return mode_==Mode::Library && category_==Category::Videos &&
                   hoverPreviewAudioItemId_<videos_.size() &&
                   mediaHoverSurface_==MediaHoverSurface::Library && mediaHoverId_==hoverPreviewAudioItemId_ &&
                   videos_[hoverPreviewAudioItemId_].path==hoverPreviewAudioPath_ && !videos_[hoverPreviewAudioItemId_].vr.vr &&
                   libraryHoverPreviewSurface_==MediaHoverSurface::Library &&
                   libraryHoverPreviewItemId_==hoverPreviewAudioItemId_ && !libraryHoverPreviewFrames_.empty();
        }
        if(hoverPreviewAudioSurface_==MediaHoverSurface::Preview) {
            return mode_==Mode::Details && category_==Category::Videos && selected_<videos_.size() &&
                   hoverPreviewAudioItemId_<previewFrames_.size() &&
                   mediaHoverSurface_==MediaHoverSurface::Preview && mediaHoverId_==hoverPreviewAudioItemId_ &&
                   videos_[selected_].path==hoverPreviewAudioPath_ && !videos_[selected_].vr.vr &&
                   libraryHoverPreviewSurface_==MediaHoverSurface::Preview &&
                   libraryHoverPreviewItemId_==hoverPreviewAudioItemId_ && !libraryHoverPreviewFrames_.empty();
        }
        return false;
    }

    void StopHoverPreviewAudio() {
        if(hwnd_) KillTimer(hwnd_,kHoverPreviewAudioDelayTimerId);
        ++hoverPreviewAudioGeneration_;
        if(hoverPreviewAudio_) hoverPreviewAudio_->Stop();
        hoverPreviewAudio_.reset();
        hoverPreviewAudioSurface_=MediaHoverSurface::None;
        hoverPreviewAudioItemId_=static_cast<size_t>(-1);
        hoverPreviewAudioPath_.clear();
        hoverPreviewAudioDelayArmed_=false;
    }

    void ScheduleHoverPreviewAudio(MediaHoverSurface surface,size_t itemId,const std::wstring& path) {
        StopHoverPreviewAudio();
        if(!hwnd_ || path.empty()) return;
        hoverPreviewAudioSurface_=surface;
        hoverPreviewAudioItemId_=itemId;
        hoverPreviewAudioPath_=path;
        hoverPreviewAudioDelayArmed_=true;
        SetTimer(hwnd_,kHoverPreviewAudioDelayTimerId,kHoverPreviewAudioDelayMs,nullptr);
    }

    void StartHoverPreviewAudioIfStillValid() {
        hoverPreviewAudioDelayArmed_=false;
        if(!HoverPreviewAudioContextStillValid()) { StopHoverPreviewAudio(); return; }
        const uint64_t generation=hoverPreviewAudioGeneration_;
        auto audio=std::make_unique<HoverPreviewAudioPlayer>();
        if(FAILED(audio->Open(hwnd_,hoverPreviewAudioPath_,generation))) return;
        hoverPreviewAudio_=std::move(audio);
    }

    void ClearLibraryHoverPreviewDisplay() {
        StopHoverPreviewAudio();
        libraryHoverPreviewCurrentMediaSeconds_=0.0;
        const RECT oldRect=libraryHoverPreviewRect_;
        DeleteLibraryHoverPreviewBitmaps(libraryHoverPreviewFrames_);
        libraryHoverPreviewSurface_=MediaHoverSurface::None;
        libraryHoverPreviewItemId_=static_cast<size_t>(-1);
        libraryHoverPreviewFrameIndex_=0;
        libraryHoverPreviewFrameTick_=0;
        libraryHoverPreviewRect_=RECT{};
        if(hwnd_ && !EmptyRectValue(oldRect)) InvalidateAnimatedRegion(hwnd_,oldRect);
    }

    void CancelLibraryHoverPreviewRequest() {
        {
            std::lock_guard<std::mutex> lock(libraryHoverPreviewRequestMutex_);
            libraryHoverPreviewRequestedSurface_=MediaHoverSurface::None;
            libraryHoverPreviewRequestedId_=static_cast<size_t>(-1);
            libraryHoverPreviewRequestedPath_.clear();
            libraryHoverPreviewRequestedVr_={};
            libraryHoverPreviewRequestedStartSeconds_=-1.0;
            libraryHoverPreviewRequestedWidth_=libraryHoverPreviewRequestedHeight_=0;
        }
        libraryHoverPreviewGeneration_.fetch_add(1,std::memory_order_acq_rel);
        libraryHoverPreviewLoadingSurface_=MediaHoverSurface::None;
        libraryHoverPreviewLoadingId_=static_cast<size_t>(-1);
        libraryHoverPreviewWorkerRunning_.store(false,std::memory_order_release);
        libraryHoverPreviewCv_.notify_one();
        ClearLibraryHoverPreviewDisplay();
    }

    void StopLibraryHoverPreviewWorker() {
        libraryHoverPreviewStop_.store(true,std::memory_order_release);
        libraryHoverPreviewGeneration_.fetch_add(1,std::memory_order_acq_rel);
        libraryHoverPreviewCv_.notify_one();
        if(libraryHoverPreviewThread_.joinable()) libraryHoverPreviewThread_.join();
        libraryHoverPreviewWorkerRunning_.store(false,std::memory_order_release);
        libraryHoverPreviewLoadingSurface_=MediaHoverSurface::None;
        libraryHoverPreviewLoadingId_=static_cast<size_t>(-1);
        libraryHoverPreviewPendingSurface_=MediaHoverSurface::None;
        libraryHoverPreviewPendingId_=static_cast<size_t>(-1);
        libraryHoverPreviewPendingStart_=0;
        {
            std::lock_guard<std::mutex> lock(libraryHoverPreviewRequestMutex_);
            libraryHoverPreviewRequestedSurface_=MediaHoverSurface::None;
            libraryHoverPreviewRequestedId_=static_cast<size_t>(-1);
            libraryHoverPreviewRequestedPath_.clear();
            libraryHoverPreviewRequestedVr_={};
            libraryHoverPreviewRequestedStartSeconds_=-1.0;
            libraryHoverPreviewRequestedWidth_=libraryHoverPreviewRequestedHeight_=0;
        }
        ClearLibraryHoverPreviewDisplay();
    }

    void EnsureLibraryHoverPreviewWorker() {
        if(libraryHoverPreviewThread_.joinable()) return;
        libraryHoverPreviewStop_.store(false,std::memory_order_release);
        libraryHoverPreviewThread_=std::thread([this]() {
            const HRESULT coHr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            // Animated hover preview is direct user interaction. Keep the worker at
            // normal priority; background cache generators are explicitly parked while
            // hover playback is active, so this thread should not inherit batch priority.
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_NORMAL);
            uint64_t seenGeneration=0;

            while(!libraryHoverPreviewStop_.load(std::memory_order_acquire)){
                uint64_t generation=0;
                MediaHoverSurface surface=MediaHoverSurface::None;
                size_t itemId=static_cast<size_t>(-1);
                std::wstring path;
                VRInfo vr{};
                double startSeconds=-1.0;
                int targetW=0,targetH=0;
                {
                    std::unique_lock<std::mutex> lock(libraryHoverPreviewRequestMutex_);
                    libraryHoverPreviewCv_.wait(lock,[&]{
                        return libraryHoverPreviewStop_.load(std::memory_order_acquire) ||
                               libraryHoverPreviewGeneration_.load(std::memory_order_acquire)!=seenGeneration;
                    });
                    if(libraryHoverPreviewStop_.load(std::memory_order_acquire)) break;
                    generation=libraryHoverPreviewGeneration_.load(std::memory_order_acquire);
                    seenGeneration=generation;
                    surface=libraryHoverPreviewRequestedSurface_;
                    itemId=libraryHoverPreviewRequestedId_;
                    path=libraryHoverPreviewRequestedPath_;
                    vr=libraryHoverPreviewRequestedVr_;
                    startSeconds=libraryHoverPreviewRequestedStartSeconds_;
                    targetW=libraryHoverPreviewRequestedWidth_;
                    targetH=libraryHoverPreviewRequestedHeight_;
                }

                if(surface==MediaHoverSurface::None || itemId==static_cast<size_t>(-1) || path.empty()) continue;

                libraryHoverPreviewWorkerRunning_.store(true,std::memory_order_release);
                const bool ok=StreamLibraryHoverPreviewFramesMF(path,vr,startSeconds,targetW,targetH,libraryHoverPreviewStop_,libraryHoverPreviewGeneration_,
                                                               generation,hwnd_,surface,itemId,libraryHoverPreviewFrameMessagePending_);
                libraryHoverPreviewWorkerRunning_.store(false,std::memory_order_release);

                if(!ok && !libraryHoverPreviewStop_.load(std::memory_order_acquire) &&
                   libraryHoverPreviewGeneration_.load(std::memory_order_acquire)==generation){
                    // Send one empty result so the UI stops retrying a source that cannot
                    // be previewed. Do not block the worker behind an older queued frame.
                    for(int attempt=0;attempt<40 && !libraryHoverPreviewStop_.load(std::memory_order_acquire);++attempt){
                        if(libraryHoverPreviewGeneration_.load(std::memory_order_acquire)!=generation) break;
                        bool expected=false;
                        if(libraryHoverPreviewFrameMessagePending_.compare_exchange_strong(expected,true,std::memory_order_acq_rel)){
                            auto* result=new LibraryHoverPreviewResult();
                            result->generation=generation;
                            result->surface=surface;
                            result->index=itemId;
                            result->mediaPath=path;
                            if(!hwnd_ || !PostMessageW(hwnd_,WM_APP_LIBRARY_HOVER_PREVIEW_READY,0,reinterpret_cast<LPARAM>(result))){
                                libraryHoverPreviewFrameMessagePending_.store(false,std::memory_order_release);
                                delete result;
                            }
                            break;
                        }
                        Sleep(5);
                    }
                }
            }

            if(SUCCEEDED(coHr)) { ReleaseThreadWicFactory(); CoUninitialize(); }
        });
    }

    void StartHoverPreviewWorker(MediaHoverSurface surface,size_t itemId) {
        if(category_!=Category::Videos) return;
        const MediaItem* item=nullptr;
        double startSeconds=-1.0;
        if(surface==MediaHoverSurface::Library){
            if(itemId>=videos_.size()) return;
            item=&videos_[itemId];
            // GenerateGridThumb records the exact decoded timestamp used for the Library image.
            ReadBannerTimestamp(item->uiCachePath,startSeconds);
        }else if(surface==MediaHoverSurface::Preview){
            if(mode_!=Mode::Details || selected_>=videos_.size() || itemId>=previewFrames_.size()) return;
            item=&videos_[selected_];
            // Secondary images are generated by seeking to this timeline timestamp and
            // selecting the first decoded frame at/after it. Repeating the same seek here
            // makes the still timeline image transition directly into moving playback.
            startSeconds=previewFrames_[itemId].seekSeconds;
        }else return;
        if(item->vr.vr){
            CancelLibraryHoverPreviewRequest();
            return;
        }

        // Give the interactive decoder a short uncontested startup window, then let the
        // below-normal Load Everything worker continue beside the normal-priority hover decoder.
        DeferBackgroundWork(250);
        EnsureLibraryHoverPreviewWorker();
        ClearLibraryHoverPreviewDisplay();
        libraryHoverPreviewFailedSurface_=MediaHoverSurface::None;
        libraryHoverPreviewFailedId_=static_cast<size_t>(-1);
        libraryHoverPreviewLoadingSurface_=surface;
        libraryHoverPreviewLoadingId_=itemId;
        int displayW=libraryCardWidth_;
        if(surface==MediaHoverSurface::Preview && hwnd_){
            RECT rc{}; GetClientRect(hwnd_,&rc);
            displayW=DetailsPreviewCardWidthForViewport(std::max(1,static_cast<int>(rc.right-rc.left)));
        }
        const int targetW=(surface==MediaHoverSurface::Preview)
            ? PreviewDecodeWidthForCurrentView(displayW)
            : QuantizedLibraryDecodeWidth(displayW);
        const int targetH=(surface==MediaHoverSurface::Preview)
            ? PreviewDecodeHeightForWidth(targetW)
            : LibraryDecodeHeightForWidth(targetW);
        {
            std::lock_guard<std::mutex> lock(libraryHoverPreviewRequestMutex_);
            libraryHoverPreviewRequestedSurface_=surface;
            libraryHoverPreviewRequestedId_=itemId;
            libraryHoverPreviewRequestedPath_=item->path;
            libraryHoverPreviewRequestedVr_=item->vr;
            libraryHoverPreviewRequestedStartSeconds_=startSeconds;
            libraryHoverPreviewRequestedWidth_=targetW;
            libraryHoverPreviewRequestedHeight_=targetH;
            libraryHoverPreviewGeneration_.fetch_add(1,std::memory_order_acq_rel);
        }
        libraryHoverPreviewCv_.notify_one();
    }

    void StartLibraryHoverPreviewWorker(size_t itemId) { StartHoverPreviewWorker(MediaHoverSurface::Library,itemId); }
    void StartTimelineHoverPreviewWorker(size_t itemId) { StartHoverPreviewWorker(MediaHoverSurface::Preview,itemId); }

    LibraryHoverPreviewFrame* ActiveLibraryHoverPreviewFrame(size_t itemId) {
        if(libraryHoverPreviewSurface_!=MediaHoverSurface::Library || itemId!=libraryHoverPreviewItemId_ || libraryHoverPreviewFrames_.empty()) return nullptr;
        return &libraryHoverPreviewFrames_.front();
    }

    LibraryHoverPreviewFrame* ActiveTimelineHoverPreviewFrame(size_t itemId) {
        if(previewZoomGestureActive_) return nullptr;
        if(libraryHoverPreviewSurface_!=MediaHoverSurface::Preview || itemId!=libraryHoverPreviewItemId_ || libraryHoverPreviewFrames_.empty()) return nullptr;
        return &libraryHoverPreviewFrames_.front();
    }

    void HandleLibraryHoverPreviewResult(LibraryHoverPreviewResult* result) {
        libraryHoverPreviewFrameMessagePending_.store(false,std::memory_order_release);
        if(!result) return;
        std::unique_ptr<LibraryHoverPreviewResult> holder(result);
        const uint64_t currentGeneration=libraryHoverPreviewGeneration_.load(std::memory_order_acquire);
        if(result->generation!=currentGeneration){
            DeleteLibraryHoverPreviewBitmaps(result->frames);
            return;
        }

        bool stillHovered=false;
        if(result->surface==MediaHoverSurface::Library){
            stillHovered=mode_==Mode::Library && category_==Category::Videos &&
                         result->index<videos_.size() &&
                         mediaHoverSurface_==MediaHoverSurface::Library && mediaHoverId_==result->index &&
                         videos_[result->index].path==result->mediaPath;
        }else if(result->surface==MediaHoverSurface::Preview){
            stillHovered=mode_==Mode::Details && category_==Category::Videos && selected_<videos_.size() &&
                         result->index<previewFrames_.size() &&
                         mediaHoverSurface_==MediaHoverSurface::Preview && mediaHoverId_==result->index &&
                         videos_[selected_].path==result->mediaPath;
        }
        if(!stillHovered){
            DeleteLibraryHoverPreviewBitmaps(result->frames);
            return;
        }

        // The hover decoder already runs at normal priority; do not continuously park
        // Load Everything while animated preview playback is active.

        if(result->frames.empty()){
            libraryHoverPreviewFailedSurface_=result->surface;
            libraryHoverPreviewFailedId_=result->index;
            libraryHoverPreviewLoadingSurface_=MediaHoverSurface::None;
            libraryHoverPreviewLoadingId_=static_cast<size_t>(-1);
            ClearLibraryHoverPreviewDisplay();
            return;
        }

        const bool startingNewHover = libraryHoverPreviewSurface_!=result->surface || libraryHoverPreviewItemId_!=result->index;
        const RECT oldRect=libraryHoverPreviewRect_;
        DeleteLibraryHoverPreviewBitmaps(libraryHoverPreviewFrames_);
        libraryHoverPreviewFrames_.swap(result->frames);
        libraryHoverPreviewSurface_=result->surface;
        libraryHoverPreviewItemId_=result->index;
        if(!libraryHoverPreviewFrames_.empty()) libraryHoverPreviewCurrentMediaSeconds_=libraryHoverPreviewFrames_.front().mediaSeconds;
        if(startingNewHover) ScheduleHoverPreviewAudio(result->surface,result->index,result->mediaPath);
        libraryHoverPreviewFrameIndex_=0;
        libraryHoverPreviewFrameTick_=GetTickCount64();
        libraryHoverPreviewRect_=mediaHoverRect_;
        libraryHoverPreviewFailedSurface_=MediaHoverSurface::None;
        libraryHoverPreviewFailedId_=static_cast<size_t>(-1);
        if(hwnd_){
            if(!EmptyRectValue(oldRect) && !SameRect(oldRect,libraryHoverPreviewRect_)) InvalidateAnimatedRegion(hwnd_,oldRect);
            if(!EmptyRectValue(libraryHoverPreviewRect_)) InvalidateAnimatedRegion(hwnd_,libraryHoverPreviewRect_);
        }
    }

    void UpdateLibraryHoverPreview(ULONGLONG now, bool& active) {
        MediaHoverSurface hoverSurface=MediaHoverSurface::None;
        size_t hoveredId=static_cast<size_t>(-1);
        bool validContext=false;

        if(!IsAppForegroundForHover()){
            if(libraryHoverPreviewLoadingSurface_!=MediaHoverSurface::None || !libraryHoverPreviewFrames_.empty())
                CancelLibraryHoverPreviewRequest();
            libraryHoverPreviewPendingSurface_=MediaHoverSurface::None;
            libraryHoverPreviewPendingId_=static_cast<size_t>(-1);
            libraryHoverPreviewPendingStart_=0;
            return;
        }

        // Ctrl+wheel timeline zoom gets exclusive priority over moving-preview work.
        // Static frames stay visible and are GPU-scaled; animation resumes only after
        // the zoom-settle timer fires.
        if((previewZoomGestureActive_ || previewScrollGestureActive_) && mode_==Mode::Details){
            if(libraryHoverPreviewLoadingSurface_==MediaHoverSurface::Preview ||
               libraryHoverPreviewSurface_==MediaHoverSurface::Preview)
                CancelLibraryHoverPreviewRequest();
            libraryHoverPreviewPendingSurface_=MediaHoverSurface::None;
            libraryHoverPreviewPendingId_=static_cast<size_t>(-1);
            libraryHoverPreviewPendingStart_=0;
            return;
        }

        if(mode_==Mode::Library && category_==Category::Videos &&
           mediaHoverSurface_==MediaHoverSurface::Library && mediaHoverId_<videos_.size() &&
           !EmptyRectValue(mediaHoverRect_)){
            validContext=true; hoverSurface=MediaHoverSurface::Library; hoveredId=mediaHoverId_;
        }else if(mode_==Mode::Details && category_==Category::Videos && selected_<videos_.size() &&
                 mediaHoverSurface_==MediaHoverSurface::Preview &&
                 mediaHoverId_<previewFrames_.size() && !EmptyRectValue(mediaHoverRect_)){
            validContext=true; hoverSurface=MediaHoverSurface::Preview; hoveredId=mediaHoverId_;
        }

        if(!validContext){
            if(libraryHoverPreviewLoadingSurface_!=MediaHoverSurface::None || !libraryHoverPreviewFrames_.empty())
                CancelLibraryHoverPreviewRequest();
            libraryHoverPreviewPendingSurface_=MediaHoverSurface::None;
            libraryHoverPreviewPendingId_=static_cast<size_t>(-1);
            libraryHoverPreviewPendingStart_=0;
            return;
        }

        // Hover animation is latency-sensitive, but the hover decoder itself runs at
        // normal priority while Load Everything runs below normal. Avoid extending the
        // global pause on every animation tick so the batch can keep making progress.

        if(libraryHoverPreviewSurface_==hoverSurface && libraryHoverPreviewItemId_==hoveredId)
            libraryHoverPreviewRect_=mediaHoverRect_;

        if(hoverSurface!=libraryHoverPreviewPendingSurface_ || hoveredId!=libraryHoverPreviewPendingId_){
            if(libraryHoverPreviewLoadingSurface_!=MediaHoverSurface::None &&
               (libraryHoverPreviewLoadingSurface_!=hoverSurface || libraryHoverPreviewLoadingId_!=hoveredId))
                CancelLibraryHoverPreviewRequest();
            libraryHoverPreviewPendingSurface_=hoverSurface;
            libraryHoverPreviewPendingId_=hoveredId;
            libraryHoverPreviewPendingStart_=now;
            libraryHoverPreviewFailedSurface_=MediaHoverSurface::None;
            libraryHoverPreviewFailedId_=static_cast<size_t>(-1);
        }

        if((libraryHoverPreviewLoadingSurface_==hoverSurface && libraryHoverPreviewLoadingId_==hoveredId) ||
           (libraryHoverPreviewSurface_==hoverSurface && libraryHoverPreviewItemId_==hoveredId)) return;
        if(libraryHoverPreviewFailedSurface_==hoverSurface && libraryHoverPreviewFailedId_==hoveredId) return;

        active=true; // Keep the 16 ms timer alive only for the very short hover debounce.
        if(now>=libraryHoverPreviewPendingStart_ && now-libraryHoverPreviewPendingStart_>=kLibraryHoverPreviewDelayMs){
            if(hoverSurface==MediaHoverSurface::Library) StartLibraryHoverPreviewWorker(hoveredId);
            else StartTimelineHoverPreviewWorker(hoveredId);
        }
    }

    void RefreshPreviewFrames() {
        if (previewDir_.empty()) return;
        // The frame vector may be rebuilt as generated files arrive. Any display-size
        // decode result targeting the previous vector generation must not be installed.
        CancelPreviewBitmapDecodeJobs();
        std::map<int,HBITMAP> old;
        std::map<int,ULONGLONG> oldUsed;
        std::map<int,int> oldFailures;
        std::map<int,ULONGLONG> oldNextAttempt;
        std::map<int,ComPtr<ID2D1Bitmap>> oldGpu;
        std::map<int,HBITMAP> oldGpuSource;
        std::map<int,uint64_t> oldGpuGeneration;
        for (auto& p : previewFrames_) {
            if (p.bitmap) old[p.seconds]=p.bitmap;
            if (p.gpuBitmap) oldGpu[p.seconds]=std::move(p.gpuBitmap);
            if (p.gpuBitmapSource) oldGpuSource[p.seconds]=p.gpuBitmapSource;
            if (p.gpuGeneration) oldGpuGeneration[p.seconds]=p.gpuGeneration;
            oldUsed[p.seconds]=p.lastUsed;
            oldFailures[p.seconds]=p.loadFailures;
            oldNextAttempt[p.seconds]=p.nextLoadAttempt;
        }
        previewFrames_.clear();

        // A valid complete.txt marker is authoritative. Do not stat thousands of JPEGs
        // every time Details opens; construct their deterministic paths directly. If a
        // cached preview repeatedly fails to decode, GetPreviewBitmap invalidates that entry
        // and the marker, so the next pass repairs only what is actually broken.
        const double cachedDuration=ReadCachedPreviewDuration();
        if(cachedDuration>0.0){
            const auto expected=BuildPreviewCaptureSeconds(cachedDuration);
            previewFrames_.reserve(expected.size());
            for(int sec:expected){
                wchar_t name[32]{}; swprintf_s(name,L"%06d.jpg",sec);
                PreviewFrame f; f.seconds=sec; f.seekSeconds=PreviewSeekSecondsForLabel(cachedDuration,sec); f.path=(fs::path(previewDir_)/name).wstring();
                auto it=old.find(f.seconds); if(it!=old.end()){f.bitmap=it->second;old.erase(it);}
                auto git=oldGpu.find(f.seconds);if(git!=oldGpu.end()){f.gpuBitmap=std::move(git->second);oldGpu.erase(git);}
                auto gsit=oldGpuSource.find(f.seconds);if(gsit!=oldGpuSource.end())f.gpuBitmapSource=gsit->second;
                auto ggit=oldGpuGeneration.find(f.seconds);if(ggit!=oldGpuGeneration.end())f.gpuGeneration=ggit->second;
                auto uit=oldUsed.find(f.seconds); if(uit!=oldUsed.end()) f.lastUsed=uit->second;
                auto fit=oldFailures.find(f.seconds); if(fit!=oldFailures.end()) f.loadFailures=fit->second;
                auto nit=oldNextAttempt.find(f.seconds); if(nit!=oldNextAttempt.end()) f.nextLoadAttempt=nit->second;
                previewFrames_.push_back(std::move(f));
            }
        } else {
            // Incomplete generation has no authoritative marker yet, so enumerate only
            // the partial files that already exist and show those while generation runs.
            std::error_code ec;
            if (fs::exists(previewDir_,ec)) {
                for (const auto& e : fs::directory_iterator(previewDir_,ec)) {
                    if (ec) break;
                    if (!e.is_regular_file(ec)||ToLower(e.path().extension().wstring())!=L".jpg") continue;
                    const std::wstring stem=e.path().stem().wstring();
                    wchar_t* end=nullptr; const long sec=wcstol(stem.c_str(),&end,10);
                    if (!end||*end!=L'\0'||sec<0) continue;
                    PreviewFrame f; f.seconds=static_cast<int>(sec); f.seekSeconds=static_cast<double>(sec); f.path=e.path().wstring();
                    auto it=old.find(f.seconds); if(it!=old.end()){f.bitmap=it->second;old.erase(it);}
                auto git=oldGpu.find(f.seconds);if(git!=oldGpu.end()){f.gpuBitmap=std::move(git->second);oldGpu.erase(git);}
                auto gsit=oldGpuSource.find(f.seconds);if(gsit!=oldGpuSource.end())f.gpuBitmapSource=gsit->second;
                auto ggit=oldGpuGeneration.find(f.seconds);if(ggit!=oldGpuGeneration.end())f.gpuGeneration=ggit->second;
                    auto uit=oldUsed.find(f.seconds); if(uit!=oldUsed.end()) f.lastUsed=uit->second;
                    auto fit=oldFailures.find(f.seconds); if(fit!=oldFailures.end()) f.loadFailures=fit->second;
                    auto nit=oldNextAttempt.find(f.seconds); if(nit!=oldNextAttempt.end()) f.nextLoadAttempt=nit->second;
                    previewFrames_.push_back(std::move(f));
                }
            }
            std::sort(previewFrames_.begin(),previewFrames_.end(),[](const PreviewFrame&a,const PreviewFrame&b){return a.seconds<b.seconds;});
        }
        for (auto& kv:old) if(kv.second) DeleteObject(kv.second);
        TryApplyPendingTimelineReturnFocus();
    }

    static double ReadCachedPreviewDurationFromDir(const std::wstring& dir) {
        if (dir.empty()) return 0.0;
        std::ifstream in(fs::path(dir) / L"complete.txt", std::ios::binary);
        double value=0.0; int version=0,width=0,height=0;
        if(in) in>>value>>version>>width>>height;
        if(version!=kVisualPreviewCacheVersion || width!=kVisualPreviewCacheWidth || height!=kVisualPreviewCacheHeight) return 0.0;
        return (value>0.0 && std::isfinite(value)) ? value : 0.0;
    }

    static bool PreviewCacheIsCompleteForDir(const std::wstring& dir) {
        const double duration=ReadCachedPreviewDurationFromDir(dir);
        return duration>0.0 && !BuildPreviewCaptureSeconds(duration).empty();
    }

    static bool PreviewCacheFilesCompleteForDir(const std::wstring& dir) {
        const double duration=ReadCachedPreviewDurationFromDir(dir);
        if(!(duration>0.0)) return false;
        const auto expected=BuildPreviewCaptureSeconds(duration);
        if(expected.empty()) return false;
        for(int sec:expected){
            wchar_t name[32]{}; swprintf_s(name,L"%06d.jpg",sec);
            if(!VisualPreviewMasterHealthy((fs::path(dir)/name).wstring())) return false;
        }
        return true;
    }

    double ReadCachedPreviewDuration() const {
        return ReadCachedPreviewDurationFromDir(previewDir_);
    }

    static double ProbeVideoDurationMF(const std::wstring& path) {
        ComPtr<IMFSourceReader> reader;
        if(FAILED(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader)) || !reader) return 0.0;
        PROPVARIANT durationVar; PropVariantInit(&durationVar);
        LONGLONG duration100ns=0;
        if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&durationVar))){
            if(durationVar.vt==VT_UI8) duration100ns=static_cast<LONGLONG>(durationVar.uhVal.QuadPart);
            else if(durationVar.vt==VT_I8) duration100ns=durationVar.hVal.QuadPart;
        }
        PropVariantClear(&durationVar);
        return duration100ns>0 ? static_cast<double>(duration100ns)/10000000.0 : 0.0;
    }

    bool PreviewCacheIsComplete() const {
        // Normal Details opening trusts complete.txt for speed. Individual bad files
        // are still repaired lazily; explicit Load everything performs a full check.
        return PreviewCacheIsCompleteForDir(previewDir_);
    }

    void StopPreviewWorker() {
        previewStop_=true;
        if (previewThread_.joinable()) previewThread_.join();
        previewStop_=false;
        const int kind=loadingKind_.load(std::memory_order_acquire);
        if(kind==1 || kind==2 || kind==3) ClearLoadingState();
    }

    void StartPreviewWorkerForSelected() {
        StopPreviewWorker();
        // Selected media has priority, but entering Info must not block the UI waiting
        // for the low-priority library generator to join. Signal it to stop; it is
        // reaped later when library generation is restarted.
        thumbStop_.store(true,std::memory_order_release);
        ClearLoadingStateIf(1);
        if(category_!=Category::Videos||selected_>=videos_.size()) {
            ClearPreviewBitmaps();
            previewDir_.clear();
            detailsDurationSeconds_.store(0.0,std::memory_order_relaxed);
            ClearLoadingState();
            return;
        }
        // V15 can retain several GB of warm Library thumbnails on high-RAM systems.
        // Before allocating an Info hero and Timeline, enforce both the byte budget and
        // the independent Windows GDI-handle budget. This is normally a no-op, but it
        // prevents a large browsing session from exhausting HBITMAP handles exactly when
        // the Timeline starts allocating its own decoded stills.
        TrimThumbMemory();
        TrimPreviewMemory();

        MediaItem& selectedItem=videos_[selected_];
        if(!selectedItem.resolutionProbeAttempted && !selectedItem.resolutionMetadataQueued){
            selectedItem.resolutionMetadataQueued=true;
            QueueResolutionMetadata(selectedItem.path,selectedItem.uiCachePath,true);
        }
        const MediaItem item=selectedItem;
        if(!previewMediaPath_.empty() && previewMediaPath_!=item.path) ParkActivePreviewSet();
        previewDir_=BuildPreviewDirectory(item.path);
        const bool restored=(previewMediaPath_==item.path) || RestorePrefetchedPreviewSet(item.path);
        if(!restored){
            DeletePreviewFrameBitmaps(previewFrames_);
            previewFrames_.clear();
            previewMediaPath_=item.path;
            detailsDurationSeconds_.store(0.0,std::memory_order_relaxed);
        }
        RefreshPreviewFrames();
        detailsDurationSeconds_.store(ReadCachedPreviewDuration(), std::memory_order_relaxed);
        const bool previewsComplete=PreviewCacheIsComplete();
        // A completed Timeline is loaded as one background working set. Queue every card
        // now rather than waiting for lower rows to be discovered by scrolling. The UI
        // stays asynchronous; visible paint requests can still jump ahead in the queue.
        if(previewsComplete) QueueAllPreviewBitmapsForCurrentView();
        if(!previewsComplete){ std::error_code markerEc; fs::remove(fs::path(previewDir_)/L"complete.txt",markerEc); }
        double libraryBannerTime=-1.0, infoBannerTime=-1.0;
        const bool libraryTimeReady=ReadBannerTimestamp(item.uiCachePath,libraryBannerTime);
        const bool libraryBannerComplete=LibraryPreviewMasterHealthy(item.uiCachePath) && libraryTimeReady;
        bool bannerComplete=CacheFileLooksHealthy(item.cachePath,1024) && ReadBannerTimestamp(item.cachePath,infoBannerTime);
        if(!bannerComplete || !libraryTimeReady || std::abs(infoBannerTime-libraryBannerTime)>0.001){
            // The Info banner is tied to the Library frame timestamp, not to the Library
            // JPEG's pixel dimensions. A visual-cache resolution migration can therefore
            // keep an already-valid native Info banner instead of decoding it again.
            if(CacheFileLooksHealthy(item.cachePath,1)) RemoveGeneratedCacheFile(item.cachePath);
            RemoveGeneratedCacheFile(BannerTimestampPath(item.cachePath));
            bannerComplete=false;
        }
        if(previewsComplete && bannerComplete && libraryBannerComplete){ ClearLoadingState(); StartThumbnailWorker(); return; }
        previewStop_=false;
        previewThread_=std::thread([this,item,dir=previewDir_]() {
            const HRESULT coHr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_NORMAL);
            BeginForegroundGenerationPriority();

            // Duration is metadata, not timeline-cache state. Read it before waiting for
            // a generation claim so an open Info panel can show the real time even while
            // Load Everything owns the decoder/cache-generation slot for this video.
            if(detailsDurationSeconds_.load(std::memory_order_relaxed)<=0.0 && !previewStop_.load(std::memory_order_acquire)){
                const double duration=ProbeVideoDurationMF(item.path);
                if(duration>0.0 && !previewStop_.load(std::memory_order_acquire)){
                    detailsDurationSeconds_.store(duration,std::memory_order_relaxed);
                    if(hwnd_) PostMessageW(hwnd_,WM_APP_PREVIEW_READY,0,0);
                }
            }

            const bool claimed=WaitForGenerationClaim(item.path,previewStop_);
            if(claimed && !previewStop_.load(std::memory_order_acquire)) {
                // Re-evaluate after the generation claim. Load everything may have filled
                // this cache while the selected-media worker was waiting.
                double libraryTime=-1.0,infoTime=-1.0;
                bool libraryTimeReady=ReadBannerTimestamp(item.uiCachePath,libraryTime);
                bool libraryReady=LibraryPreviewMasterHealthy(item.uiCachePath) && libraryTimeReady;
                bool infoReady=CacheFileLooksHealthy(item.cachePath,1024) && ReadBannerTimestamp(item.cachePath,infoTime);
                bool previewsReady=PreviewCacheIsCompleteForDir(dir);
                if(!infoReady || !libraryTimeReady || std::abs(infoTime-libraryTime)>0.001) {
                    if(CacheFileLooksHealthy(item.cachePath,1)) RemoveGeneratedCacheFile(item.cachePath);
                    RemoveGeneratedCacheFile(BannerTimestampPath(item.cachePath));
                    infoReady=false;
                }

                // Strict selected-media loading order:
                // 1) Library banner, 2) native Info banner, 3) secondary timeline.
                if(!previewStop_.load(std::memory_order_acquire) && !libraryReady) {
                    SetLoadingState(1,0,1);
                    ThumbJob grid{item.path,item.cachePath,item.uiCachePath,true,item.vr};
                    if(GenerateGridThumb(grid,&previewStop_,&backgroundPauseUntil_)) {
                        HideCacheRootIfCreated(item.path);
                        loadingCurrent_.store(1,std::memory_order_relaxed);
                        libraryReady=LibraryPreviewMasterHealthy(item.uiCachePath) && ReadBannerTimestamp(item.uiCachePath,libraryTime);
                        if(libraryReady && infoReady && (!ReadBannerTimestamp(item.cachePath,infoTime) || std::abs(infoTime-libraryTime)>0.001)){
                            if(CacheFileLooksHealthy(item.cachePath,1)) RemoveGeneratedCacheFile(item.cachePath);
                            RemoveGeneratedCacheFile(BannerTimestampPath(item.cachePath));
                            infoReady=false;
                        }
                        if(hwnd_) PostMessageW(hwnd_,WM_APP_THUMB_READY,0,0);
                    }
                }

                if(!previewStop_.load(std::memory_order_acquire) && libraryReady && !infoReady) {
                    SetLoadingState(3,0,1);
                    ThumbJob high{item.path,item.cachePath,item.uiCachePath,true,item.vr};
                    if(GenerateVideoCache(high,&previewStop_,&backgroundPauseUntil_)) {
                        HideCacheRootIfCreated(item.path);
                        loadingCurrent_.store(1,std::memory_order_relaxed);
                        if(hwnd_) PostMessageW(hwnd_,WM_APP_THUMB_READY,0,0);
                    }
                }

                if(!previewStop_.load(std::memory_order_acquire) && !previewsReady) {
                    SetLoadingState(2,0,0);
                    if(GenerateVideoPreviewsMF(item.path,dir,item.vr,previewStop_,hwnd_,&detailsDurationSeconds_,&loadingCurrent_,&loadingTotal_,&backgroundPauseUntil_))
                        HideCacheRootIfCreated(item.path);
                }
            }
            if(claimed) ReleaseGenerationClaim(item.path);

            if(!previewStop_.load(std::memory_order_acquire)) {
                ClearLoadingState();
                if(hwnd_) PostMessageW(hwnd_,WM_APP_SELECTED_WORK_DONE,0,0);
            }
            if(hwnd_&&!previewStop_.load()) PostMessageW(hwnd_,WM_APP_PREVIEW_READY,0,0);
            EndForegroundGenerationPriority();
            if(SUCCEEDED(coHr)) { ReleaseThreadWicFactory(); CoUninitialize(); }
        });
    }

    void CancelPreviewBitmapDecodeJobs() {
        previewBitmapDecodeGeneration_.fetch_add(1,std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(previewBitmapDecodeMutex_);
            previewBitmapDecodeJobs_.clear();
        }
        for(auto& frame:previewFrames_){
            frame.pendingDecodeW=0;
            frame.pendingDecodeGeneration=0;
        }
        previewBitmapDecodeCv_.notify_all();
    }

    void EnsurePreviewBitmapDecodeWorker() {
        if(previewBitmapDecodeThread_.joinable()) return;
        previewBitmapDecodeStop_.store(false,std::memory_order_release);
        previewBitmapDecodeThread_=std::thread([this](){
            const HRESULT coHr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_NORMAL);
            while(!previewBitmapDecodeStop_.load(std::memory_order_acquire)){
                PreviewBitmapDecodeJob job;
                {
                    std::unique_lock<std::mutex> lock(previewBitmapDecodeMutex_);
                    previewBitmapDecodeCv_.wait(lock,[this]{
                        return previewBitmapDecodeStop_.load(std::memory_order_acquire) || !previewBitmapDecodeJobs_.empty();
                    });
                    if(previewBitmapDecodeStop_.load(std::memory_order_acquire)) break;
                    job=std::move(previewBitmapDecodeJobs_.front());
                    previewBitmapDecodeJobs_.pop_front();
                }
                if(job.generation!=previewBitmapDecodeGeneration_.load(std::memory_order_acquire)) continue;

                HBITMAP bitmap=nullptr;
                bool repairAttempted=false,repairSucceeded=false;
                bool masterHealthy=VisualPreviewMasterHealthy(job.path);
                if(masterHealthy) bitmap=LoadScaledBitmap(job.path,job.width,job.height);

                // A complete Timeline marker can outlive one missing/corrupt JPEG. Repair
                // exactly that timestamp on the existing low-priority decode worker rather
                // than invalidating/rebuilding the whole Timeline. Healthy neighboring
                // 1920 masters remain untouched.
                if(!bitmap && job.repairIfMissing && !job.mediaPath.empty() &&
                   !previewBitmapDecodeStop_.load(std::memory_order_acquire) &&
                   job.generation==previewBitmapDecodeGeneration_.load(std::memory_order_acquire)){
                    const bool claimed=TryClaimGeneration(job.mediaPath);
                    if(claimed){
                        repairAttempted=true;
                        VRInfo repairVr=job.vr;
                        if(repairVr.vr && job.cachedLayout>=0 && job.cachedLayout<=2){
                            repairVr.layout=job.cachedLayout;
                            repairVr.layoutExplicit=true;
                        }
                        const std::wstring tmp=job.path+L".repair.tmp.jpg";
                        DeleteFileW(tmp.c_str());
                        if(FileHasData(job.path)) RemoveGeneratedCacheFile(job.path);
                        if(GenerateVideoStillMF(job.mediaPath,tmp,repairVr,
                                                kVisualPreviewCacheWidth,kVisualPreviewCacheHeight,92,
                                                0.0,job.seekSeconds,nullptr,&previewBitmapDecodeStop_,&backgroundPauseUntil_) &&
                           !previewBitmapDecodeStop_.load(std::memory_order_acquire) &&
                           CommitGeneratedCacheFile(tmp,job.path)){
                            repairSucceeded=VisualPreviewMasterHealthy(job.path);
                            if(repairSucceeded){
                                HideCacheRootIfCreated(job.mediaPath);
                                bitmap=LoadScaledBitmap(job.path,job.width,job.height);
                            }
                        }
                        DeleteFileW(tmp.c_str());
                        ReleaseGenerationClaim(job.mediaPath);
                    }
                }

                if(previewBitmapDecodeStop_.load(std::memory_order_acquire) ||
                   job.generation!=previewBitmapDecodeGeneration_.load(std::memory_order_acquire)){
                    if(bitmap) DeleteObject(bitmap);
                    continue;
                }

                auto* result=new PreviewBitmapDecodeResult();
                result->generation=job.generation;
                result->seconds=job.seconds;
                result->path=job.path;
                result->width=job.width;
                result->height=job.height;
                result->repairAttempted=repairAttempted;
                result->repairSucceeded=repairSucceeded;
                result->bitmap=bitmap;
                if(!hwnd_ || !PostMessageW(hwnd_,WM_APP_PREVIEW_BITMAP_READY,0,reinterpret_cast<LPARAM>(result))){
                    if(result->bitmap) DeleteObject(result->bitmap);
                    delete result;
                }
            }
            if(SUCCEEDED(coHr)) { ReleaseThreadWicFactory(); CoUninitialize(); }
        });
    }

    void StopPreviewBitmapDecodeWorker() {
        previewBitmapDecodeStop_.store(true,std::memory_order_release);
        previewBitmapDecodeGeneration_.fetch_add(1,std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(previewBitmapDecodeMutex_);
            previewBitmapDecodeJobs_.clear();
        }
        previewBitmapDecodeCv_.notify_all();
        if(previewBitmapDecodeThread_.joinable()) previewBitmapDecodeThread_.join();
        for(auto& frame:previewFrames_){
            frame.pendingDecodeW=0;
            frame.pendingDecodeGeneration=0;
        }
    }

    void QueuePreviewBitmapUpgrade(PreviewFrame& frame,int decodeW,int decodeH,bool highPriority=false) {
        if(previewZoomGestureActive_ || frame.path.empty()) return;
        if(ProcessGdiObjectCount()>=kGdiObjectHardPressure){
            TrimThumbMemory();
            TrimPreviewMemory();
            if(ProcessGdiObjectCount()>=kGdiObjectHardPressure) return;
        }
        const ULONGLONG now=GetTickCount64();
        if(frame.nextLoadAttempt>now) return;
        if(frame.bitmap){
            BITMAP current{};
            if(!GetObjectW(frame.bitmap,sizeof(current),&current)) return;
            if(current.bmWidth>=decodeW && current.bmHeight>=decodeH) return;
        }

        const uint64_t generation=previewBitmapDecodeGeneration_.load(std::memory_order_acquire);
        if(frame.pendingDecodeGeneration==generation && frame.pendingDecodeW>=decodeW) return;
        EnsurePreviewBitmapDecodeWorker();

        PreviewBitmapDecodeJob job;
        job.generation=generation;
        job.seconds=frame.seconds;
        job.seekSeconds=frame.seekSeconds;
        job.path=frame.path;
        job.width=decodeW;
        job.height=decodeH;
        if(mode_==Mode::Details && category_==Category::Videos && selected_<videos_.size() &&
           PathEquals(previewMediaPath_,videos_[selected_].path)){
            job.mediaPath=videos_[selected_].path;
            job.vr=videos_[selected_].vr;
            job.cachedLayout=ReadCachedPreviewLayout(previewDir_);
            job.repairIfMissing=true;
        }
        {
            std::lock_guard<std::mutex> lock(previewBitmapDecodeMutex_);
            // Coalesce an older queued request for the same timeline position. A decode
            // already in progress is harmless; generation checks prevent stale gestures
            // from being installed after the user starts zooming again.
            previewBitmapDecodeJobs_.erase(
                std::remove_if(previewBitmapDecodeJobs_.begin(),previewBitmapDecodeJobs_.end(),[&](const PreviewBitmapDecodeJob& queued){
                    return queued.generation==generation && queued.seconds==job.seconds && queued.path==job.path;
                }),
                previewBitmapDecodeJobs_.end());
            if(highPriority) previewBitmapDecodeJobs_.push_front(job);
            else previewBitmapDecodeJobs_.push_back(job);
        }
        frame.pendingDecodeW=decodeW;
        frame.pendingDecodeGeneration=generation;
        previewBitmapDecodeCv_.notify_one();
    }

    void QueueAllPreviewBitmapsForCurrentView() {
        if(!hwnd_ || mode_!=Mode::Details || category_!=Category::Videos || previewFrames_.empty()) return;
        RECT rc{};
        if(!GetClientRect(hwnd_,&rc)) return;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left));
        const int cardW=DetailsPreviewCardWidthForViewport(clientWidth);
        const int imageH=std::max(79,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int requiredW=std::max(cardW,static_cast<int>(std::ceil(static_cast<double>(imageH)*16.0/9.0)));
        const int decodeW=PreviewDecodeWidthForCurrentView(requiredW);
        const int decodeH=PreviewDecodeHeightForWidth(decodeW);

        // Once a complete cache is known, all of its RAM-decode jobs are submitted in one
        // pass.  They remain background WIC work, so opening Details is not blocked.
        previewAsyncDecodePreferred_=true;
        for(auto& frame:previewFrames_) QueuePreviewBitmapUpgrade(frame,decodeW,decodeH,false);
    }

    void HandlePreviewBitmapDecodeResult(PreviewBitmapDecodeResult* result) {
        if(!result) return;
        std::unique_ptr<PreviewBitmapDecodeResult> holder(result);
        const auto discard=[&](){ if(result->bitmap){DeleteObject(result->bitmap);result->bitmap=nullptr;} };
        if(result->generation!=previewBitmapDecodeGeneration_.load(std::memory_order_acquire) ||
           mode_!=Mode::Details || category_!=Category::Videos || previewZoomGestureActive_){
            discard();
            return;
        }

        PreviewFrame* target=nullptr;
        size_t targetIndex=static_cast<size_t>(-1);
        for(size_t i=0;i<previewFrames_.size();++i){
            auto& frame=previewFrames_[i];
            if(frame.seconds==result->seconds && frame.path==result->path){ target=&frame; targetIndex=i; break; }
        }
        if(!target){ discard(); return; }

        if(target->pendingDecodeGeneration==result->generation && target->pendingDecodeW<=result->width){
            target->pendingDecodeW=0;
            target->pendingDecodeGeneration=0;
        }
        if(!result->bitmap){
            // If another generator currently owns this media, simply retry later; do not
            // punish the cache marker. When this worker actually attempted a one-frame
            // repair, count failures and fall back to the full repair path only after
            // repeated codec/source failures.
            if(!result->repairAttempted){
                target->nextLoadAttempt=GetTickCount64()+500;
                return;
            }
            ++target->loadFailures;
            target->nextLoadAttempt=GetTickCount64()+1000;
            if(target->loadFailures>=3){
                if(!previewDir_.empty()) RemoveGeneratedCacheFile((fs::path(previewDir_)/L"complete.txt").wstring());
                target->loadFailures=0;
                target->nextLoadAttempt=GetTickCount64()+1200;
                if(hwnd_) PostMessageW(hwnd_,WM_APP_CACHE_REPAIR,0,0);
            }
            return;
        }

        BITMAP incoming{}; GetObjectW(result->bitmap,sizeof(incoming),&incoming);
        if(target->bitmap){
            BITMAP current{}; GetObjectW(target->bitmap,sizeof(current),&current);
            if(current.bmWidth>=incoming.bmWidth && current.bmHeight>=incoming.bmHeight){
                discard();
                return;
            }
        }

        HBITMAP old=target->bitmap;
        target->bitmap=result->bitmap;
        result->bitmap=nullptr;
        target->lastUsed=GetTickCount64();
        target->loadFailures=0;
        target->nextLoadAttempt=0;
        target->gpuBitmap.Reset();
        target->gpuBitmapSource=nullptr;
        target->gpuGeneration=0;
        if(old) DeleteObject(old);
        if(hwnd_){
            bool invalidated=false;
            for(const auto& hit:previewMediaHoverHits_){
                if(hit.id==targetIndex){ InvalidateAnimatedRegion(hwnd_,hit.visual); invalidated=true; break; }
            }
            // Off-screen frames need no immediate repaint; the sharper bitmap is already
            // resident when scrolling brings that card back into view.
            if(!invalidated && targetIndex==static_cast<size_t>(-1)) InvalidateRect(hwnd_,nullptr,FALSE);
        }
    }

    void BeginPreviewScrollGesture() {
        previewScrollGestureActive_=true;
        // Once the user starts scrolling, never decode a newly exposed 1920-master
        // thumbnail synchronously from Paint(). Existing resident bitmaps stay visible;
        // missing/undersized cards are promoted to the interactive WIC worker instead.
        previewAsyncDecodePreferred_=true;
        if(libraryHoverPreviewLoadingSurface_==MediaHoverSurface::Preview ||
           libraryHoverPreviewSurface_==MediaHoverSurface::Preview ||
           libraryHoverPreviewPendingSurface_==MediaHoverSurface::Preview)
            CancelLibraryHoverPreviewRequest();
        libraryHoverPreviewPendingSurface_=MediaHoverSurface::None;
        libraryHoverPreviewPendingId_=static_cast<size_t>(-1);
        libraryHoverPreviewPendingStart_=0;
        if(hwnd_){
            KillTimer(hwnd_,kPreviewScrollSettleTimerId);
            SetTimer(hwnd_,kPreviewScrollSettleTimerId,kPreviewScrollSettleMs,nullptr);
        }
    }

    void FinishPreviewScrollGesture() {
        if(hwnd_) KillTimer(hwnd_,kPreviewScrollSettleTimerId);
        if(!previewScrollGestureActive_) return;
        previewScrollGestureActive_=false;
        // Keep async decode preferred after the first scroll. This prevents future scroll
        // positions from ever turning disk/WIC decode into a UI-thread stall. Quality is
        // unchanged: the exact same display bucket is installed as soon as it is ready.
        if(hwnd_) InvalidateRect(hwnd_,nullptr,FALSE);
        if(mediaHoverSurface_==MediaHoverSurface::Preview) StartUiAnimationTimer();
    }

    void BeginPreviewZoomGesture() {
        previewZoomGestureActive_=true;
        CancelPreviewBitmapDecodeJobs();
        if(libraryHoverPreviewLoadingSurface_==MediaHoverSurface::Preview ||
           libraryHoverPreviewSurface_==MediaHoverSurface::Preview ||
           libraryHoverPreviewPendingSurface_==MediaHoverSurface::Preview)
            CancelLibraryHoverPreviewRequest();
        libraryHoverPreviewPendingSurface_=MediaHoverSurface::None;
        libraryHoverPreviewPendingId_=static_cast<size_t>(-1);
        libraryHoverPreviewPendingStart_=0;
        if(hwnd_){
            KillTimer(hwnd_,kPreviewZoomSettleTimerId);
            SetTimer(hwnd_,kPreviewZoomSettleTimerId,kPreviewZoomSettleMs,nullptr);
        }
    }

    void FinishPreviewZoomGesture() {
        if(hwnd_) KillTimer(hwnd_,kPreviewZoomSettleTimerId);
        if(!previewZoomGestureActive_) return;
        previewZoomGestureActive_=false;
        previewAsyncDecodePreferred_=true;
        // Submit the whole Timeline at the new quality tier immediately. Existing bitmaps
        // remain visible until their sharper replacements arrive, so zoom stays smooth.
        QueueAllPreviewBitmapsForCurrentView();
        if(hwnd_) InvalidateRect(hwnd_,nullptr,FALSE);
        if(mediaHoverSurface_==MediaHoverSurface::Preview) StartUiAnimationTimer();
    }

    HBITMAP GetPreviewBitmap(PreviewFrame& frame,int displayW,int displayH) {
        const ULONGLONG now=GetTickCount64();
        frame.lastUsed=now;
        const int requiredW=std::max(std::max(1,displayW),static_cast<int>(std::ceil(static_cast<double>(std::max(1,displayH))*16.0/9.0)));
        const int decodeW=PreviewDecodeWidthForCurrentView(requiredW);
        const int decodeH=PreviewDecodeHeightForWidth(decodeW);
        if(frame.bitmap){
            BITMAP bm{}; GetObjectW(frame.bitmap,sizeof(bm),&bm);
            // Keep a larger resident bitmap when the window shrinks.  If zoom/window
            // growth needs more pixels, never destroy/redecode from inside Paint(): the
            // current bitmap remains visible and Direct2D scales it until the background
            // decode worker supplies a sharper replacement.
            if(bm.bmWidth>=decodeW && bm.bmHeight>=decodeH) return frame.bitmap;
            if(!previewZoomGestureActive_) QueuePreviewBitmapUpgrade(frame,decodeW,decodeH,true);
            return frame.bitmap;
        }
        // Active Ctrl+wheel zoom is deliberately I/O-free. A newly exposed card can
        // briefly show its neutral placeholder; once the gesture settles the normal
        // paint pass loads/queues it without compromising gesture responsiveness.
        if(previewZoomGestureActive_) return nullptr;
        if(previewAsyncDecodePreferred_){
            QueuePreviewBitmapUpgrade(frame,decodeW,decodeH,true);
            return nullptr;
        }
        if(frame.nextLoadAttempt>now) return nullptr;

        if(ProcessGdiObjectCount()>=kGdiObjectHardPressure){
            TrimThumbMemory();
            TrimPreviewMemory();
            if(ProcessGdiObjectCount()>=kGdiObjectHardPressure) return nullptr;
        }

        if(VisualPreviewMasterHealthy(frame.path)){
            frame.bitmap=LoadScaledBitmap(frame.path,decodeW,decodeH);
            if(frame.bitmap){
                frame.loadFailures=0;
                frame.nextLoadAttempt=0;
                return frame.bitmap;
            }
        }

        // The complete marker says this deterministic frame should exist, but the file
        // is missing/corrupt or failed to decode. Queue an immediate asynchronous repair
        // of this timestamp only. Do not delete complete.txt or rebuild healthy neighbors.
        QueuePreviewBitmapUpgrade(frame,decodeW,decodeH,true);
        frame.nextLoadAttempt=now+500;
        return nullptr;
    }

    void TrimPreviewMemory() {
        // Do not make the currently displayed Info timeline disappear merely because
        // Load Everything is decoding another large file in the background, unless the
        // process is genuinely close to exhausting its finite GDI-object quota.
        const DWORD gdiObjects=ProcessGdiObjectCount();
        if(LoadEverythingOwnsMemoryPressure() && !SystemMemoryCriticallyLow() && gdiObjects<kGdiObjectSoftPressure) return;
        const uint64_t processBytes=ProcessMemoryBytes();
        const ProcessMemoryPolicy policy=CurrentProcessMemoryPolicy();
        if(processBytes<policy.normalProcess && gdiObjects<kGdiObjectSoftPressure) return;
        // Parked timelines are pure cache and are the first detail allocations to go.
        ClearPrefetchedPreviewSets();
        if(processBytes<policy.highPressure && gdiObjects<kGdiObjectSoftPressure) return;
        std::vector<PreviewFrame*> loaded;for(auto& frame:previewFrames_)if(frame.bitmap)loaded.push_back(&frame);
        std::sort(loaded.begin(),loaded.end(),[](const PreviewFrame*a,const PreviewFrame*b){return a->lastUsed<b->lastUsed;});
        const size_t keep=gdiObjects>=kGdiObjectHardPressure?4u:(gdiObjects>=kGdiObjectSoftPressure?12u:(processBytes>=policy.emergency?8u:24u));
        while(loaded.size()>keep){PreviewFrame* frame=loaded.front();loaded.erase(loaded.begin());if(frame->bitmap){DeleteObject(frame->bitmap);frame->bitmap=nullptr;}frame->gpuBitmap.Reset();frame->gpuBitmapSource=nullptr;frame->gpuGeneration=0;}
        if(processBytes>=policy.emergency || gdiObjects>=kGdiObjectHardPressure) TrimDetailInfoToWindow(DetailWindowIndices());
    }

    std::wstring PreviewLabel(int seconds) {
        return FormatTime(static_cast<double>(seconds));
    }

    static int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
        UINT num=0,size=0; Gdiplus::GetImageEncodersSize(&num,&size); if(!size) return -1;
        std::vector<BYTE> data(size); auto* codecs=reinterpret_cast<Gdiplus::ImageCodecInfo*>(data.data());
        if(Gdiplus::GetImageEncoders(num,size,codecs)!=Gdiplus::Ok) return -1;
        for(UINT i=0;i<num;++i){ if(wcscmp(codecs[i].MimeType,format)==0){ *clsid=codecs[i].Clsid; return static_cast<int>(i); } }
        return -1;
    }

    static bool SaveJpeg(Gdiplus::Image& image, const std::wstring& path, ULONG quality=94) {
        CLSID clsid{}; if(GetEncoderClsid(L"image/jpeg",&clsid)<0) return false;
        Gdiplus::EncoderParameters params{}; params.Count=1; params.Parameter[0].Guid=Gdiplus::EncoderQuality;
        params.Parameter[0].Type=Gdiplus::EncoderParameterValueTypeLong; params.Parameter[0].NumberOfValues=1; params.Parameter[0].Value=&quality;
        return image.Save(path.c_str(),&clsid,&params)==Gdiplus::Ok;
    }

    // Still-image Library masters use the same reusable WIC factory/encoder helpers
    // as video-derived stills. GDI+ is a compatibility fallback only.
    static bool GenerateImagePreviewMasterWic(const std::wstring& sourcePath,
                                              const std::wstring& outputPath,
                                              int outW, int outH, ULONG quality) {
        if(sourcePath.empty() || outputPath.empty() || outW<=0 || outH<=0) return false;
        std::error_code dirEc;
        fs::create_directories(fs::path(outputPath).parent_path(),dirEc);
        if(dirEc) return false;

        IWICImagingFactory* factory=ThreadWicFactory();
        if(!factory) return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if(FAILED(factory->CreateDecoderFromFilename(sourcePath.c_str(),nullptr,GENERIC_READ,
                                                     WICDecodeMetadataCacheOnDemand,decoder.GetAddressOf())) || !decoder) return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if(FAILED(decoder->GetFrame(0,frame.GetAddressOf())) || !frame) return false;

        UINT sw=0,sh=0;
        if(FAILED(frame->GetSize(&sw,&sh)) || !sw || !sh) return false;

        const double scale=std::max(static_cast<double>(outW)/static_cast<double>(sw),
                                    static_cast<double>(outH)/static_cast<double>(sh));
        const UINT cropW=std::max<UINT>(1u,std::min<UINT>(sw,static_cast<UINT>(std::lround(static_cast<double>(outW)/scale))));
        const UINT cropH=std::max<UINT>(1u,std::min<UINT>(sh,static_cast<UINT>(std::lround(static_cast<double>(outH)/scale))));
        WICRect crop{};
        crop.X=static_cast<INT>((sw-cropW)/2u);
        crop.Y=static_cast<INT>((sh-cropH)/2u);
        crop.Width=static_cast<INT>(cropW);
        crop.Height=static_cast<INT>(cropH);

        IWICBitmapSource* source=frame.Get();
        ComPtr<IWICBitmapClipper> clipper;
        if(cropW!=sw || cropH!=sh){
            if(FAILED(factory->CreateBitmapClipper(clipper.GetAddressOf())) || !clipper ||
               FAILED(clipper->Initialize(frame.Get(),&crop))) return false;
            source=clipper.Get();
        }

        ComPtr<IWICBitmapScaler> scaler;
        if(FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) || !scaler ||
           FAILED(scaler->Initialize(source,static_cast<UINT>(outW),static_cast<UINT>(outH),WICBitmapInterpolationModeFant))) return false;

        DeleteFileW(outputPath.c_str());
        return EncodeWicJpeg(factory,scaler.Get(),outputPath,static_cast<UINT>(outW),static_cast<UINT>(outH),quality);
    }

    static HBITMAP CreateVideoSampleBitmap(IMFSample* sample, UINT width, UINT height, LONG defaultStride,
                                          VRInfo vr, int& layoutState, int outW, int outH, bool cover,
                                          int* actualOutW = nullptr, int* actualOutH = nullptr) {
        if (!sample || !width || !height || outW<=0 || outH<=0) return nullptr;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return nullptr;

        BYTE* scan0=nullptr;
        LONG pitch=defaultStride;
        bool locked2D=false;
        ComPtr<IMF2DBuffer> buffer2D;
        BYTE* raw=nullptr; DWORD maxLen=0,currentLen=0;
        if (SUCCEEDED(buffer.As(&buffer2D)) && buffer2D && SUCCEEDED(buffer2D->Lock2D(&scan0,&pitch))) {
            locked2D=true;
        } else {
            if (FAILED(buffer->Lock(&raw,&maxLen,&currentLen)) || !raw) return nullptr;
            if (!pitch) pitch=static_cast<LONG>(width*4u);
            scan0=raw;
            if (pitch<0) scan0=raw+static_cast<size_t>(height-1u)*static_cast<size_t>(-pitch);
        }

        HBITMAP hbmp=nullptr;
        {
            Gdiplus::Bitmap frame(static_cast<INT>(width),static_cast<INT>(height),pitch,PixelFormat32bppRGB,scan0);
            if (frame.GetLastStatus()==Gdiplus::Ok) {
                if (layoutState < 0) layoutState=ResolveStillLayout(vr,frame,0);
                const int layout=std::max(0,layoutState);
                UINT sx=0,sy=0,sw=width,sh=height;
                if (layout==1 && width>=2) sw=width/2u;
                else if (layout==2 && height>=2) sh=height/2u;

                Gdiplus::Bitmap out(outW,outH,PixelFormat32bppARGB);
                Gdiplus::Graphics g(&out);
                g.Clear(Gdiplus::Color(255,16,18,24));
                g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);

                if (cover) {
                    const double scale=std::max(static_cast<double>(outW)/std::max<UINT>(1u,sw),static_cast<double>(outH)/std::max<UINT>(1u,sh));
                    const UINT cropW=std::max<UINT>(1u,std::min<UINT>(sw,static_cast<UINT>(outW/scale+0.5)));
                    const UINT cropH=std::max<UINT>(1u,std::min<UINT>(sh,static_cast<UINT>(outH/scale+0.5)));
                    const UINT cropX=sx+(sw-cropW)/2u,cropY=sy+(sh-cropH)/2u;
                    g.DrawImage(&frame,Gdiplus::Rect(0,0,outW,outH),static_cast<INT>(cropX),static_cast<INT>(cropY),static_cast<INT>(cropW),static_cast<INT>(cropH),Gdiplus::UnitPixel);
                } else {
                    const double scale=std::min(static_cast<double>(outW)/std::max<UINT>(1u,sw),static_cast<double>(outH)/std::max<UINT>(1u,sh));
                    const int dw=std::max(1,static_cast<int>(sw*scale));
                    const int dh=std::max(1,static_cast<int>(sh*scale));
                    const int dx=(outW-dw)/2,dy=(outH-dh)/2;
                    g.DrawImage(&frame,Gdiplus::Rect(dx,dy,dw,dh),static_cast<INT>(sx),static_cast<INT>(sy),static_cast<INT>(sw),static_cast<INT>(sh),Gdiplus::UnitPixel);
                }

                if (actualOutW) *actualOutW=outW;
                if (actualOutH) *actualOutH=outH;
                if (out.GetHBITMAP(Gdiplus::Color(255,16,18,24), &hbmp) != Gdiplus::Ok) hbmp=nullptr;
            }
        }

        if (locked2D) buffer2D->Unlock2D(); else buffer->Unlock();
        return hbmp;
    }

    static HBITMAP CreateVideoSampleHBitmapDirect(IMFSample* sample, UINT width, UINT height, LONG defaultStride) {
        if(!sample || !width || !height) return nullptr;
        ComPtr<IMFMediaBuffer> buffer;
        if(FAILED(sample->ConvertToContiguousBuffer(&buffer)) || !buffer) return nullptr;

        BYTE* scan0=nullptr;
        LONG pitch=defaultStride;
        bool locked2D=false;
        ComPtr<IMF2DBuffer> buffer2D;
        BYTE* raw=nullptr; DWORD maxLen=0,currentLen=0;
        if(SUCCEEDED(buffer.As(&buffer2D)) && buffer2D && SUCCEEDED(buffer2D->Lock2D(&scan0,&pitch))){
            locked2D=true;
        }else{
            if(FAILED(buffer->Lock(&raw,&maxLen,&currentLen)) || !raw) return nullptr;
            if(!pitch) pitch=static_cast<LONG>(width*4u);
            scan0=raw;
            if(pitch<0) scan0=raw+static_cast<size_t>(height-1u)*static_cast<size_t>(-pitch);
        }

        HBITMAP bitmap=nullptr;
        {
            Gdiplus::Bitmap frame(static_cast<INT>(width),static_cast<INT>(height),pitch,PixelFormat32bppRGB,scan0);
            if(frame.GetLastStatus()==Gdiplus::Ok &&
               frame.GetHBITMAP(Gdiplus::Color(255,16,18,24),&bitmap)!=Gdiplus::Ok) bitmap=nullptr;
        }
        if(locked2D) buffer2D->Unlock2D(); else buffer->Unlock();
        return bitmap;
    }

    static bool StreamLibraryHoverPreviewFramesMF(const std::wstring& source,
                                                   VRInfo vr,
                                                   double requestedStartSeconds,
                                                   int targetWidth,
                                                   int targetHeight,
                                                   std::atomic<bool>& stop,
                                                   const std::atomic<uint64_t>& generationState,
                                                   uint64_t generation,
                                                   HWND notifyHwnd,
                                                   MediaHoverSurface surface,
                                                   size_t itemId,
                                                   std::atomic<bool>& messagePending) {
        if(source.empty() || !notifyHwnd || vr.vr) return false;
        auto cancelled=[&](){
            return stop.load(std::memory_order_acquire) ||
                   generationState.load(std::memory_order_acquire)!=generation;
        };
        if(cancelled()) return true;

        ComPtr<IMFAttributes> attrs;
        if(FAILED(MFCreateAttributes(&attrs,3))) return false;
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE);
        attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS,FALSE);
        attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,TRUE);

        ComPtr<IMFSourceReader> reader;
        if(FAILED(MFCreateSourceReaderFromURL(source.c_str(),attrs.Get(),&reader)) || !reader) return false;
        if(cancelled()) return true;
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),TRUE);

        ComPtr<IMFMediaType> nativeType;
        if(FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&nativeType)) || !nativeType) return false;
        UINT nativeW=0,nativeH=0;
        if(FAILED(MFGetAttributeSize(nativeType.Get(),MF_MT_FRAME_SIZE,&nativeW,&nativeH)) || !nativeW || !nativeH) return false;

        // Animated previews are flat-video only. Decode only the bucket needed by the
        // current card size, so a small window does not pay a 720p-per-frame cost while
        // a large/zoomed window still gets enough pixels to stay sharp.
        const int safeTargetW=std::clamp(targetWidth,256,kVisualPreviewCacheWidth);
        const int safeTargetH=std::clamp(targetHeight,144,kVisualPreviewCacheHeight);
        double scale=std::min(1.0,std::max(static_cast<double>(safeTargetW)/static_cast<double>(nativeW),
                                          static_cast<double>(safeTargetH)/static_cast<double>(nativeH)));

        // Use the source frame rate to protect UI smoothness. The requested display
        // bucket remains the quality target, but unusually tall/wide or high-fps media
        // is bounded by a pixel-throughput budget before RGB32 frames reach the UI.
        UINT32 fpsNum=0,fpsDen=0;
        double fps=30.0;
        if(SUCCEEDED(MFGetAttributeRatio(nativeType.Get(),MF_MT_FRAME_RATE,&fpsNum,&fpsDen)) && fpsNum && fpsDen)
            fps=std::clamp(static_cast<double>(fpsNum)/static_cast<double>(fpsDen),1.0,240.0);
        const double proposedPixels=static_cast<double>(nativeW)*static_cast<double>(nativeH)*scale*scale;
        const double maxFramePixels=kHoverPreviewPixelRateBudget/std::max(1.0,fps);
        if(proposedPixels>maxFramePixels && maxFramePixels>0.0)
            scale*=std::sqrt(maxFramePixels/proposedPixels);

        UINT outW=std::max<UINT>(2u,static_cast<UINT>(nativeW*scale));
        UINT outH=std::max<UINT>(2u,static_cast<UINT>(nativeH*scale));
        outW=(outW+1u)&~1u; outH=(outH+1u)&~1u;

        ComPtr<IMFMediaType> outType;
        if(FAILED(MFCreateMediaType(&outType))) return false;
        outType->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
        MFSetAttributeSize(outType.Get(),MF_MT_FRAME_SIZE,outW,outH);
        MFSetAttributeRatio(outType.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
        if(FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,outType.Get()))) return false;

        ComPtr<IMFMediaType> actualType;
        reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),&actualType);
        UINT actualW=outW,actualH=outH;
        LONG stride=static_cast<LONG>(actualW*4u);
        if(actualType){
            MFGetAttributeSize(actualType.Get(),MF_MT_FRAME_SIZE,&actualW,&actualH);
            UINT32 strideValue=0;
            if(SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE,&strideValue))) stride=static_cast<LONG>(strideValue);
        }

        PROPVARIANT durationVar; PropVariantInit(&durationVar);
        LONGLONG duration100ns=0;
        if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&durationVar))){
            if(durationVar.vt==VT_UI8) duration100ns=static_cast<LONGLONG>(durationVar.uhVal.QuadPart);
            else if(durationVar.vt==VT_I8) duration100ns=durationVar.hVal.QuadPart;
        }
        PropVariantClear(&durationVar);
        if(duration100ns<=0) return false;

        const double duration=static_cast<double>(duration100ns)/10000000.0;
        const double safeEnd=std::max(0.0,duration-0.08);
        // Prefer the exact decoded timestamp that produced the Library thumbnail. This
        // makes hover playback continue naturally from the frame the user was already
        // looking at. Older/missing cache metadata falls back to the previous safe start.
        double segmentStart=-1.0;
        if(std::isfinite(requestedStartSeconds) && requestedStartSeconds>=0.0)
            segmentStart=std::clamp(requestedStartSeconds,0.0,safeEnd);
        if(segmentStart<0.0){
            if(duration<=24.0) segmentStart=duration>2.0?std::min(0.35,duration*0.04):0.0;
            else segmentStart=std::clamp(duration*0.12,0.0,std::max(0.0,safeEnd-0.25));
        }
        const double segmentEnd=safeEnd;
        if(segmentEnd<=segmentStart) return false;
        const LONGLONG segmentStart100ns=static_cast<LONGLONG>(segmentStart*10000000.0);
        const LONGLONG segmentEnd100ns=static_cast<LONGLONG>(segmentEnd*10000000.0);

        PROPVARIANT startPos; PropVariantInit(&startPos); startPos.vt=VT_I8; startPos.hVal.QuadPart=segmentStart100ns;
        const HRESULT startSeekHr=reader->SetCurrentPosition(GUID_NULL,startPos);
        PropVariantClear(&startPos);
        if(FAILED(startSeekHr)) return false;

        bool postedAny=false;
        LONGLONG timingBase=-1;
        ULONGLONG wallBase=GetTickCount64();

        while(!cancelled()){
            DWORD streamIndex=0,flags=0;
            LONGLONG timestamp=0;
            ComPtr<IMFSample> sample;
            const HRESULT hr=reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,
                                                &streamIndex,&flags,&timestamp,&sample);
            if(cancelled()) break;
            if(FAILED(hr)) return postedAny;

            if((flags&MF_SOURCE_READERF_ENDOFSTREAM) || timestamp>=segmentEnd100ns){
                // Leave the most recently delivered frame visible at the real end of
                // the video. A new hover starts a fresh preview; the current hover never loops.
                break;
            }
            if(!sample || timestamp<segmentStart100ns) continue;

            if(timingBase<0){
                timingBase=timestamp;
                wallBase=GetTickCount64();
            }

            // Pace output using the media timestamps instead of a fixed slideshow timer.
            // This preserves native 24/25/30/50/60/etc. fps. If the UI falls behind,
            // stale frames are dropped rather than queued, protecting input responsiveness.
            const ULONGLONG targetMs=static_cast<ULONGLONG>(std::max<LONGLONG>(0,timestamp-timingBase)/10000LL);
            while(!cancelled()){
                const ULONGLONG elapsed=GetTickCount64()-wallBase;
                if(elapsed>=targetMs) break;
                Sleep(static_cast<DWORD>(std::min<ULONGLONG>(4,targetMs-elapsed)));
            }
            if(cancelled()) break;
            const ULONGLONG elapsed=GetTickCount64()-wallBase;
            if(elapsed>targetMs+120) continue;

            bool expected=false;
            if(!messagePending.compare_exchange_strong(expected,true,std::memory_order_acq_rel)) continue;

            LibraryHoverPreviewFrame frame;
            frame.bitmap=CreateVideoSampleHBitmapDirect(sample.Get(),actualW,actualH,stride);
            frame.width=static_cast<int>(actualW);
            frame.height=static_cast<int>(actualH);
            frame.mediaSeconds=static_cast<double>(timestamp)/10000000.0;
            if(!frame.bitmap){
                messagePending.store(false,std::memory_order_release);
                continue;
            }

            auto* result=new LibraryHoverPreviewResult();
            result->generation=generation;
            result->surface=surface;
            result->index=itemId;
            result->mediaPath=source;
            result->frames.push_back(std::move(frame));
            if(!PostMessageW(notifyHwnd,WM_APP_LIBRARY_HOVER_PREVIEW_READY,0,reinterpret_cast<LPARAM>(result))){
                DeleteLibraryHoverPreviewBitmaps(result->frames);
                delete result;
                messagePending.store(false,std::memory_order_release);
                return postedAny;
            }
            postedAny=true;
        }
        return true;
    }

    static bool GenerateVideoStillMF(const std::wstring& source, const std::wstring& output, VRInfo vr,
                                     int targetW, int targetH, ULONG quality, double thumbFraction = 0.25,
                                     double exactSeconds = -1.0, double* capturedSecondsOut = nullptr,
                                     const std::atomic<bool>* cancel = nullptr,
                                     const std::atomic<ULONGLONG>* pauseUntil = nullptr) {
        auto waitForPermit=[&]()->bool{
            while(!(cancel && cancel->load(std::memory_order_acquire))){
                if(!pauseUntil) return true;
                const ULONGLONG until=pauseUntil->load(std::memory_order_acquire);
                const ULONGLONG now=GetTickCount64();
                if(until<=now) return true;
                Sleep(static_cast<DWORD>(std::min<ULONGLONG>(20,until-now)));
            }
            return false;
        };
        if(!waitForPermit()) return false;
        ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs,2))) return false;
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE);
        attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS,FALSE);

        ComPtr<IMFSourceReader> reader;
        if (FAILED(MFCreateSourceReaderFromURL(source.c_str(),attrs.Get(),&reader)) || !reader) return false;
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),TRUE);

        ComPtr<IMFMediaType> nativeType;
        if (FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&nativeType)) || !nativeType) return false;
        UINT nativeW=0,nativeH=0;
        if (FAILED(MFGetAttributeSize(nativeType.Get(),MF_MT_FRAME_SIZE,&nativeW,&nativeH)) || !nativeW || !nativeH) return false;

        const int resolvedLayout=ResolvePreviewLayout(vr,nativeW,nativeH);
        int layoutState=(vr.vr && !vr.layoutExplicit && resolvedLayout==0) ? -1 : resolvedLayout;
        const bool nativeOutput = targetW<=0 || targetH<=0;

        UINT effectiveW=nativeW,effectiveH=nativeH;
        if(!nativeOutput){
            // Size the decoder against the eye that will actually be shown. For ambiguous
            // VR geometry, use a stereo-safe sizing assumption only for decode quality;
            // the later multi-frame detector still decides the real saved layout.
            int decodeLayout=resolvedLayout;
            if(vr.vr && !vr.layoutExplicit && decodeLayout==0){
                const double aspect=static_cast<double>(nativeW)/std::max<UINT>(1u,nativeH);
                if(aspect>=1.30) decodeLayout=1;
                else if(aspect<=0.82) decodeLayout=2;
            }
            if(decodeLayout==1 && effectiveW>=2) effectiveW/=2u;
            else if(decodeLayout==2 && effectiveH>=2) effectiveH/=2u;
        }
        const double scale=nativeOutput ? 1.0 :
            std::min(1.0,std::max(static_cast<double>(targetW)/std::max<UINT>(1u,effectiveW),
                                  static_cast<double>(targetH)/std::max<UINT>(1u,effectiveH)));
        UINT outW=std::max<UINT>(2u,static_cast<UINT>(nativeW*scale));
        UINT outH=std::max<UINT>(2u,static_cast<UINT>(nativeH*scale));
        outW=(outW+1u)&~1u; outH=(outH+1u)&~1u;

        ComPtr<IMFMediaType> outType;
        if (FAILED(MFCreateMediaType(&outType))) return false;
        outType->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
        MFSetAttributeSize(outType.Get(),MF_MT_FRAME_SIZE,outW,outH);
        MFSetAttributeRatio(outType.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
        if (FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,outType.Get()))) return false;

        ComPtr<IMFMediaType> actualType;
        reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),&actualType);
        UINT actualW=outW,actualH=outH; LONG stride=static_cast<LONG>(actualW*4u);
        if(actualType){
            MFGetAttributeSize(actualType.Get(),MF_MT_FRAME_SIZE,&actualW,&actualH);
            UINT32 strideValue=0; if(SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE,&strideValue))) stride=static_cast<LONG>(strideValue);
        }

        PROPVARIANT durationVar; PropVariantInit(&durationVar); LONGLONG duration100ns=0;
        if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&durationVar))){
            if(durationVar.vt==VT_UI8) duration100ns=static_cast<LONGLONG>(durationVar.uhVal.QuadPart);
            else if(durationVar.vt==VT_I8) duration100ns=durationVar.hVal.QuadPart;
        }
        PropVariantClear(&durationVar);
        const double duration=duration100ns>0?static_cast<double>(duration100ns)/10000000.0:0.0;
        const double safeFraction=std::clamp(thumbFraction,0.0,0.95);
        const double requestedSeconds=(std::isfinite(exactSeconds) && exactSeconds>=0.0) ? exactSeconds : duration*safeFraction;
        const double seekSeconds=duration>0.2?std::clamp(requestedSeconds,0.0,std::max(0.0,duration-0.1)):0.0;

        auto readAt=[&](double seconds, double* actualSeconds)->ComPtr<IMFSample>{
            ComPtr<IMFSample> chosenSample;
            PROPVARIANT pos; PropVariantInit(&pos); pos.vt=VT_I8;
            pos.hVal.QuadPart=static_cast<LONGLONG>(std::max(0.0,seconds)*10000000.0);
            const HRESULT seekHr=reader->SetCurrentPosition(GUID_NULL,pos); PropVariantClear(&pos);
            if(FAILED(seekHr)) return chosenSample;
            const LONGLONG target=static_cast<LONGLONG>(std::max(0.0,seconds)*10000000.0);
            for(int attempts=0;attempts<240;++attempts){
                if((attempts&7)==0 && !waitForPermit()) break;
                if(cancel && cancel->load(std::memory_order_acquire)) break;
                DWORD streamIndex=0,flags=0; LONGLONG timestamp=0; ComPtr<IMFSample> sample;
                const HRESULT readHr=reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&streamIndex,&flags,&timestamp,&sample);
                if(FAILED(readHr)||(flags&MF_SOURCE_READERF_ENDOFSTREAM)) break;
                if(!sample) continue;
                chosenSample=sample;
                if(actualSeconds) *actualSeconds=static_cast<double>(timestamp)/10000000.0;
                if(timestamp>=target) break;
            }
            return chosenSample;
        };

        double capturedSeconds=seekSeconds;
        ComPtr<IMFSample> chosen=readAt(seekSeconds,&capturedSeconds);
        if(!chosen || (cancel && cancel->load(std::memory_order_acquire))) return false;
        if(capturedSecondsOut) *capturedSecondsOut=capturedSeconds;

        // A banner/grid frame must use the same stable stereo decision as the secondary
        // previews. For ambiguous VR, inspect representative frames instead of trusting
        // a single dark or unusual frame and then lock one layout for the saved image.
        if(layoutState<0){
            int votes[3]{0,0,0};
            auto vote=[&](IMFSample* sample){
                const int candidate=DetectVideoSampleLayout(sample,actualW,actualH,stride,vr);
                if(candidate>=0 && candidate<=2) ++votes[candidate];
            };
            vote(chosen.Get());
            if(duration>0.4){
                for(double fraction : {0.15,0.40,0.60,0.82}){
                    if(!waitForPermit()) break;
                    if(votes[1]>=2 || votes[2]>=2) break;
                    const double probeSeconds=std::clamp(duration*fraction,0.0,std::max(0.0,duration-0.1));
                    ComPtr<IMFSample> probe=readAt(probeSeconds,nullptr);
                    if(probe) vote(probe.Get());
                }
            }
            if(votes[1]>=2) layoutState=1;
            else if(votes[2]>=2) layoutState=2;
            else if(votes[1]>0 && votes[2]==0 && votes[1]>=votes[0]) layoutState=1;
            else if(votes[2]>0 && votes[1]==0 && votes[2]>=votes[0]) layoutState=2;
            else if(votes[1]>votes[0] && votes[1]>votes[2]) layoutState=1;
            else if(votes[2]>votes[0] && votes[2]>votes[1]) layoutState=2;
            else layoutState=0;
        }

        if(!waitForPermit() || (cancel && cancel->load(std::memory_order_acquire))) return false;
        std::error_code dirEc; fs::create_directories(fs::path(output).parent_path(),dirEc);
        if(dirEc) return false;
        int saveW=targetW, saveH=targetH; bool cover=true;
        if(nativeOutput){
            saveW=static_cast<int>(actualW); saveH=static_cast<int>(actualH); cover=false;
            if(layoutState==1) saveW=std::max(1,saveW/2);
            else if(layoutState==2) saveH=std::max(1,saveH/2);
        }
        return SaveVideoSampleJpeg(chosen.Get(),actualW,actualH,stride,vr,layoutState,output,saveW,saveH,quality,cover);
    }

    static bool GenerateVideoVisualCachesMF(const ThumbJob& job,
                                                const std::atomic<bool>* cancel = nullptr,
                                                const std::atomic<ULONGLONG>* pauseUntil = nullptr,
                                                UINT* nativeWidthOut = nullptr,
                                                UINT* nativeHeightOut = nullptr) {
        // Fast Load Everything path: when both the Library poster and native Info banner
        // are missing, capture the 10% frame once at native decoder resolution and save
        // both derivatives from the same IMFSample. This removes a complete second
        // Media Foundation open + seek without changing either output's quality.
        auto waitForPermit=[&]()->bool{
            while(!(cancel && cancel->load(std::memory_order_acquire))){
                if(!pauseUntil) return true;
                const ULONGLONG until=pauseUntil->load(std::memory_order_acquire);
                const ULONGLONG now=GetTickCount64();
                if(until<=now) return true;
                Sleep(static_cast<DWORD>(std::min<ULONGLONG>(20,until-now)));
            }
            return false;
        };
        if(!waitForPermit()) return false;

        ComPtr<IMFAttributes> attrs;
        if(FAILED(MFCreateAttributes(&attrs,2))) return false;
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING,TRUE);
        attrs->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS,FALSE);

        ComPtr<IMFSourceReader> reader;
        if(FAILED(MFCreateSourceReaderFromURL(job.source.c_str(),attrs.Get(),&reader)) || !reader) return false;
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS),FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),TRUE);

        ComPtr<IMFMediaType> nativeType;
        if(FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&nativeType)) || !nativeType) return false;
        UINT nativeW=0,nativeH=0;
        if(FAILED(MFGetAttributeSize(nativeType.Get(),MF_MT_FRAME_SIZE,&nativeW,&nativeH)) || !nativeW || !nativeH) return false;
        if(nativeWidthOut) *nativeWidthOut=nativeW;
        if(nativeHeightOut) *nativeHeightOut=nativeH;

        ComPtr<IMFMediaType> outType;
        if(FAILED(MFCreateMediaType(&outType))) return false;
        outType->SetGUID(MF_MT_MAJOR_TYPE,MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE,MFVideoFormat_RGB32);
        MFSetAttributeSize(outType.Get(),MF_MT_FRAME_SIZE,nativeW,nativeH);
        MFSetAttributeRatio(outType.Get(),MF_MT_PIXEL_ASPECT_RATIO,1,1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE,MFVideoInterlace_Progressive);
        if(FAILED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),nullptr,outType.Get()))) return false;

        ComPtr<IMFMediaType> actualType;
        reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),&actualType);
        UINT actualW=nativeW,actualH=nativeH;
        LONG stride=static_cast<LONG>(actualW*4u);
        if(actualType){
            MFGetAttributeSize(actualType.Get(),MF_MT_FRAME_SIZE,&actualW,&actualH);
            UINT32 strideValue=0;
            if(SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE,&strideValue))) stride=static_cast<LONG>(strideValue);
        }

        PROPVARIANT durationVar; PropVariantInit(&durationVar);
        LONGLONG duration100ns=0;
        if(SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),MF_PD_DURATION,&durationVar))){
            if(durationVar.vt==VT_UI8) duration100ns=static_cast<LONGLONG>(durationVar.uhVal.QuadPart);
            else if(durationVar.vt==VT_I8) duration100ns=durationVar.hVal.QuadPart;
        }
        PropVariantClear(&durationVar);
        const double duration=duration100ns>0?static_cast<double>(duration100ns)/10000000.0:0.0;
        const double requestedSeconds=duration*0.10;
        const double seekSeconds=duration>0.2?std::clamp(requestedSeconds,0.0,std::max(0.0,duration-0.1)):0.0;

        auto readAt=[&](double seconds,double* actualSeconds)->ComPtr<IMFSample>{
            ComPtr<IMFSample> chosenSample;
            PROPVARIANT pos; PropVariantInit(&pos); pos.vt=VT_I8;
            pos.hVal.QuadPart=static_cast<LONGLONG>(std::max(0.0,seconds)*10000000.0);
            const HRESULT seekHr=reader->SetCurrentPosition(GUID_NULL,pos); PropVariantClear(&pos);
            if(FAILED(seekHr)) return chosenSample;
            const LONGLONG target=static_cast<LONGLONG>(std::max(0.0,seconds)*10000000.0);
            for(int attempts=0;attempts<240;++attempts){
                if((attempts&7)==0 && !waitForPermit()) break;
                if(cancel && cancel->load(std::memory_order_acquire)) break;
                DWORD streamIndex=0,flags=0; LONGLONG timestamp=0; ComPtr<IMFSample> sample;
                const HRESULT readHr=reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&streamIndex,&flags,&timestamp,&sample);
                if(FAILED(readHr)||(flags&MF_SOURCE_READERF_ENDOFSTREAM)) break;
                if(!sample) continue;
                chosenSample=sample;
                if(actualSeconds) *actualSeconds=static_cast<double>(timestamp)/10000000.0;
                if(timestamp>=target) break;
            }
            return chosenSample;
        };

        double capturedSeconds=seekSeconds;
        ComPtr<IMFSample> chosen=readAt(seekSeconds,&capturedSeconds);
        if(!chosen || (cancel && cancel->load(std::memory_order_acquire))) return false;

        const int resolvedLayout=ResolvePreviewLayout(job.vr,nativeW,nativeH);
        int layoutState=(job.vr.vr && !job.vr.layoutExplicit && resolvedLayout==0) ? -1 : resolvedLayout;
        if(layoutState<0){
            int votes[3]{0,0,0};
            auto vote=[&](IMFSample* sample){
                const int candidate=DetectVideoSampleLayout(sample,actualW,actualH,stride,job.vr);
                if(candidate>=0 && candidate<=2) ++votes[candidate];
            };
            vote(chosen.Get());
            if(duration>0.4){
                for(double fraction : {0.15,0.40,0.60,0.82}){
                    if(!waitForPermit()) break;
                    if(votes[1]>=2 || votes[2]>=2) break;
                    const double probeSeconds=std::clamp(duration*fraction,0.0,std::max(0.0,duration-0.1));
                    ComPtr<IMFSample> probe=readAt(probeSeconds,nullptr);
                    if(probe) vote(probe.Get());
                }
            }
            if(votes[1]>=2) layoutState=1;
            else if(votes[2]>=2) layoutState=2;
            else if(votes[1]>0 && votes[2]==0 && votes[1]>=votes[0]) layoutState=1;
            else if(votes[2]>0 && votes[1]==0 && votes[2]>=votes[0]) layoutState=2;
            else if(votes[1]>votes[0] && votes[1]>votes[2]) layoutState=1;
            else if(votes[2]>votes[0] && votes[2]>votes[1]) layoutState=2;
            else layoutState=0;
        }

        if(!waitForPermit() || (cancel && cancel->load(std::memory_order_acquire))) return false;
        std::error_code dirEc;
        fs::create_directories(fs::path(job.uiOutput).parent_path(),dirEc);
        if(dirEc) return false;

        const std::wstring libraryTmp=job.uiOutput+L".tmp.jpg";
        const std::wstring infoTmp=job.output+L".tmp.jpg";
        DeleteFileW(libraryTmp.c_str());
        DeleteFileW(infoTmp.c_str());

        if(!SaveVideoSampleJpeg(chosen.Get(),actualW,actualH,stride,job.vr,layoutState,libraryTmp,
                                kLibraryPreviewCacheWidth,kLibraryPreviewCacheHeight,92,true)){
            DeleteFileW(libraryTmp.c_str());
            return false;
        }

        int infoW=static_cast<int>(actualW),infoH=static_cast<int>(actualH);
        if(layoutState==1) infoW=std::max(1,infoW/2);
        else if(layoutState==2) infoH=std::max(1,infoH/2);
        if(!SaveVideoSampleJpeg(chosen.Get(),actualW,actualH,stride,job.vr,layoutState,infoTmp,
                                infoW,infoH,98,false)){
            DeleteFileW(libraryTmp.c_str());
            DeleteFileW(infoTmp.c_str());
            return false;
        }

        if(cancel && cancel->load(std::memory_order_acquire)){
            DeleteFileW(libraryTmp.c_str());
            DeleteFileW(infoTmp.c_str());
            return false;
        }
        if(!CommitGeneratedCacheFile(libraryTmp,job.uiOutput)){
            DeleteFileW(infoTmp.c_str());
            return false;
        }
        WriteBannerTimestamp(job.uiOutput,capturedSeconds);
        if(!CommitGeneratedCacheFile(infoTmp,job.output)) return false;
        WriteBannerTimestamp(job.output,capturedSeconds);

        double libraryTime=-1.0,infoTime=-1.0;
        return LibraryPreviewMasterHealthy(job.uiOutput) &&
               CacheFileLooksHealthy(job.output,1024) &&
               ReadBannerTimestamp(job.uiOutput,libraryTime) &&
               ReadBannerTimestamp(job.output,infoTime) &&
               std::abs(libraryTime-infoTime)<=0.001;
    }

    static bool GenerateVideoCache(const ThumbJob& job, const std::atomic<bool>* cancel = nullptr, const std::atomic<ULONGLONG>* pauseUntil = nullptr) {
        double existingLinkedTime=-1.0;
        if(CacheFileLooksHealthy(job.output,1024) && ReadBannerTimestamp(job.output,existingLinkedTime)) return true;
        if(PathExistsNoThrow(job.output)) RemoveGeneratedCacheFile(job.output);
        RemoveGeneratedCacheFile(BannerTimestampPath(job.output));
        if(cancel && cancel->load(std::memory_order_acquire)) return false;
        const std::wstring tmp=job.output+L".tmp.jpg"; DeleteFileW(tmp.c_str());
        double exactSeconds=-1.0, capturedSeconds=-1.0;
        ReadBannerTimestamp(job.uiOutput,exactSeconds);
        if(!GenerateVideoStillMF(job.source,tmp,job.vr,0,0,98,0.10,exactSeconds,&capturedSeconds,cancel,pauseUntil)){ DeleteFileW(tmp.c_str()); return false; }
        if(cancel && cancel->load(std::memory_order_acquire)){ DeleteFileW(tmp.c_str()); return false; }
        if(!CommitGeneratedCacheFile(tmp,job.output)) return false;
        const double linkedTime=(std::isfinite(exactSeconds) && exactSeconds>=0.0) ? exactSeconds : capturedSeconds;
        WriteBannerTimestamp(job.output,linkedTime);
        double verifyTime=-1.0;
        return CacheFileLooksHealthy(job.output,1024) && ReadBannerTimestamp(job.output,verifyTime);
    }


    static bool GenerateGridThumb(const ThumbJob& job, const std::atomic<bool>* cancel = nullptr, const std::atomic<ULONGLONG>* pauseUntil = nullptr) {
        if (job.uiOutput.empty()) return true;
        double existingTime=-1.0;
        const bool imageHealthy=LibraryPreviewMasterHealthy(job.uiOutput);
        const bool timeHealthy=!job.isVideo || ReadBannerTimestamp(job.uiOutput,existingTime);
        if(imageHealthy && timeHealthy) return true;
        if(PathExistsNoThrow(job.uiOutput)) RemoveGeneratedCacheFile(job.uiOutput);
        if(job.isVideo) RemoveGeneratedCacheFile(BannerTimestampPath(job.uiOutput));
        if(cancel && cancel->load(std::memory_order_acquire)) return false;

        const std::wstring tmp=job.uiOutput+L".tmp.jpg"; DeleteFileW(tmp.c_str());
        bool ok=false;
        double capturedSeconds=-1.0;
        if (job.isVideo) {
            // Library banners use the frame at 10%. Record the exact decoded timestamp so
            // the native Info banner can request the same frame rather than seeking again
            // from a percentage and potentially landing on a neighboring keyframe.
            ok=GenerateVideoStillMF(job.source,tmp,job.vr,kLibraryPreviewCacheWidth,kLibraryPreviewCacheHeight,92,0.10,-1.0,&capturedSeconds,cancel,pauseUntil);
        } else {
            // Normal still-image path: WIC decode/crop/scale/encode. Rendering of the
            // resulting display bucket is Direct2D/GPU. GDI+ is a compatibility fallback
            // only for a source that the installed WIC codecs cannot service.
            ok=GenerateImagePreviewMasterWic(job.source,tmp,kLibraryPreviewCacheWidth,kLibraryPreviewCacheHeight,92);
            if(!ok && !(cancel && cancel->load(std::memory_order_acquire))){
                Gdiplus::Image src(job.source.c_str());
                if (src.GetLastStatus()==Gdiplus::Ok&&src.GetWidth()&&src.GetHeight()) {
                    const UINT sw=src.GetWidth(),sh=src.GetHeight();
                    Gdiplus::Bitmap out(kLibraryPreviewCacheWidth,kLibraryPreviewCacheHeight,PixelFormat24bppRGB); Gdiplus::Graphics g(&out);
                    g.Clear(Gdiplus::Color(255,16,18,24)); g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                    const double scale=std::max(static_cast<double>(kLibraryPreviewCacheWidth)/static_cast<double>(sw),
                                                static_cast<double>(kLibraryPreviewCacheHeight)/static_cast<double>(sh));
                    const UINT cropW=std::max<UINT>(1u,std::min<UINT>(sw,static_cast<UINT>(kLibraryPreviewCacheWidth/scale+0.5)));
                    const UINT cropH=std::max<UINT>(1u,std::min<UINT>(sh,static_cast<UINT>(kLibraryPreviewCacheHeight/scale+0.5)));
                    const UINT cropX=(sw-cropW)/2u,cropY=(sh-cropH)/2u;
                    g.DrawImage(&src,Gdiplus::Rect(0,0,kLibraryPreviewCacheWidth,kLibraryPreviewCacheHeight),
                                static_cast<INT>(cropX),static_cast<INT>(cropY),static_cast<INT>(cropW),static_cast<INT>(cropH),Gdiplus::UnitPixel);
                    std::error_code dirEc; fs::create_directories(fs::path(job.uiOutput).parent_path(),dirEc);
                    if (!dirEc) ok=SaveJpeg(out,tmp,92);
                }
            }
        }
        if (!ok || (cancel && cancel->load(std::memory_order_acquire))){DeleteFileW(tmp.c_str());return false;}
        if(!CommitGeneratedCacheFile(tmp,job.uiOutput)) return false;
        if(job.isVideo) WriteBannerTimestamp(job.uiOutput,capturedSeconds);
        return LibraryPreviewMasterHealthy(job.uiOutput);
    }

    void StartThumbnailWorker() {
        // Do not walk the entire media drive in the background. Missing Library
        // thumbnails are generated lazily by the viewport loader only when a card is
        // actually visible. This leaves removable/VeraCrypt libraries idle once the
        // current on-screen data has been copied into RAM.
        thumbWorkerRunning_.store(false,std::memory_order_release);
        thumbRepairRequested_.store(false,std::memory_order_release);
        ClearLoadingStateIf(1);
    }

    void StopThumbnailWorker() {
        thumbStop_.store(true,std::memory_order_release);
        if(thumbThread_.joinable()) thumbThread_.join();
        thumbWorkerRunning_.store(false,std::memory_order_release);
        thumbRepairRequested_.store(false,std::memory_order_release);
        ClearLoadingStateIf(1);
    }

    static std::wstring GenerationClaimKey(const std::wstring& source) {
        return ToLower(fs::path(source).lexically_normal().wstring());
    }

    bool TryClaimGeneration(const std::wstring& source) {
        const std::wstring key=GenerationClaimKey(source);
        if(key.empty()) return false;
        std::lock_guard<std::mutex> lock(generationClaimMutex_);
        return generationClaims_.insert(key).second;
    }

    void ReleaseGenerationClaim(const std::wstring& source) {
        const std::wstring key=GenerationClaimKey(source);
        std::lock_guard<std::mutex> lock(generationClaimMutex_);
        generationClaims_.erase(key);
    }

    bool WaitForGenerationClaim(const std::wstring& source,const std::atomic<bool>& stop) {
        while(!stop.load(std::memory_order_acquire)) {
            if(TryClaimGeneration(source)) return true;
            Sleep(30);
        }
        return false;
    }

    void SetFullLoadCurrentFile(const std::wstring& source) {
        std::lock_guard<std::mutex> lock(fullLoadStatusMutex_);
        fullLoadCurrentFile_=source.empty()?L"":fs::path(source).filename().wstring();
    }

    std::wstring FullLoadCurrentFile() {
        std::lock_guard<std::mutex> lock(fullLoadStatusMutex_);
        return fullLoadCurrentFile_;
    }

    void ClearFullLoadFailedPaths() {
        std::lock_guard<std::mutex> lock(fullLoadStatusMutex_);
        fullLoadFailedPaths_.clear();
    }

    void RecordFullLoadFailedPath(const std::wstring& source) {
        if(source.empty()) return;
        std::lock_guard<std::mutex> lock(fullLoadStatusMutex_);
        fullLoadFailedPaths_.push_back(fs::path(source).lexically_normal().wstring());
    }

    std::vector<std::wstring> FullLoadFailedPaths() {
        std::lock_guard<std::mutex> lock(fullLoadStatusMutex_);
        return fullLoadFailedPaths_;
    }

    ULONGLONG FullLoadDonePopupDuration() const {
        return fullLoadFailures_.load(std::memory_order_relaxed)>0?kFullLoadFailedPopupDurationMs:kFullLoadDonePopupDurationMs;
    }

    bool FullLoadFailurePopupVisible() const {
        if(fullLoadRunning_.load(std::memory_order_acquire) || fullLoadFinishedAt_==0) return false;
        if(fullLoadFailures_.load(std::memory_order_relaxed)<=0) return false;
        return GetTickCount64()-fullLoadFinishedAt_<FullLoadDonePopupDuration();
    }

    RECT FullLoadPopupRect(RECT rc) const {
        const int width=std::max(240,std::min(360,std::max(240,static_cast<int>(rc.right)-40)));
        const int height=54;
        const int right=std::max(20,static_cast<int>(rc.right)-18);
        const int top=(mode_==Mode::Library&&searchVisible_)?68:18;
        return RECT{std::max(8,right-width),top,right,top+height};
    }

    RECT FullLoadFailureActionRect(RECT rc) const {
        const RECT box=FullLoadPopupRect(rc);
        return RECT{box.left+10,box.top+23,box.right-10,box.bottom-1};
    }

    void ClearLoadFailureFilter() {
        loadFailureFilterActive_=false;
        loadFailureFilterPaths_.clear();
        loadFailureFilterTotal_=0;
    }

    void ShowFullLoadFailureResults() {
        const auto failed=FullLoadFailedPaths();
        if(failed.empty()) return;

        if(mode_==Mode::Details) ReturnFromDetailsToLibrary();
        if(mode_!=Mode::Library) return;

        loadFailureFilterPaths_.clear();
        for(const auto& path:failed)
            loadFailureFilterPaths_.insert(ToLower(fs::path(path).lexically_normal().wstring()));
        loadFailureFilterTotal_=static_cast<int>(loadFailureFilterPaths_.size());
        if(loadFailureFilterTotal_<=0){ ClearLoadFailureFilter(); return; }

        loadFailureFilterActive_=true;
        searchQuery_.clear();
        searchVisible_=true;
        searchSelectAll_=false;
        currentFolder_=folder_;
        detailsSearchNavigationActive_=false;
        detailsSearchNavigationIndices_.clear();

        auto categoryFailureCount=[&](Category wanted){
            const auto& items=wanted==Category::Videos?videos_:images_;
            size_t count=0;
            for(const auto& item:items){
                const std::wstring key=ToLower(fs::path(item.path).lexically_normal().wstring());
                if(loadFailureFilterPaths_.find(key)!=loadFailureFilterPaths_.end()) ++count;
            }
            return count;
        };
        if(categoryFailureCount(category_)==0)
            category_=categoryFailureCount(Category::Videos)>0?Category::Videos:Category::Images;

        selected_=0;
        scrollY_=0;
        filteredIndices_.clear();
        filterDirty_=true;
        fullLoadFinishedAt_=0;
        ResetLibraryThumbLoadView();
        ClampScroll();
        PrepareLibraryViewportFromPrivateCache();
        StartThumbnailWorker();
        InvalidateRect(hwnd_,nullptr,FALSE);
    }

    void InvalidateLoadingPopupArea() {
        if(!hwnd_) return;
        RECT rc{}; GetClientRect(hwnd_,&rc);
        RECT dirty{std::max<LONG>(0,rc.right-430),0,rc.right,std::min<LONG>(140,rc.bottom)};
        InvalidateRect(hwnd_,&dirty,FALSE);
    }

    void StopFullLoadWorker() {
        fullLoadStop_.store(true,std::memory_order_release);
        fullLoadYieldCurrent_.store(true,std::memory_order_release);
        if(fullLoadThread_.joinable()) fullLoadThread_.join();
        fullLoadRunning_.store(false,std::memory_order_release);
        fullLoadStop_.store(false,std::memory_order_release);
        fullLoadYieldCurrent_.store(false,std::memory_order_release);
        fullLoadCurrent_.store(0,std::memory_order_relaxed);
        fullLoadTotal_.store(0,std::memory_order_relaxed);
        fullLoadFailures_.store(0,std::memory_order_relaxed);
        fullLoadFinishedAt_=0;
        ClearFullLoadFailedPaths();
        SetFullLoadCurrentFile(L"");
        InvalidateLoadingPopupArea();
    }

    void StartFullLoadEverything() {
        if(fullLoadRunning_.load(std::memory_order_acquire)) {
            MessageBoxW(hwnd_,L"Load everything is already running in the background.",L"Visual MediaPlayer",MB_OK|MB_ICONINFORMATION);
            return;
        }
        if(videos_.empty() && images_.empty()) return;
        const int answer=MessageBoxW(hwnd_,
            L"Generate all library banners, info banners and Timeline images for every media file in the current library?\n\n"
            L"VR videos generate static Timeline stills using the same selected-eye style as their Library/Info images; animated VR hover previews remain disabled. You can keep using Visual MediaPlayer while this runs.",
            L"Load everything",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2);
        if(answer!=IDYES) return;

        // Reap a previously completed worker before starting a new run.
        if(fullLoadThread_.joinable()) fullLoadThread_.join();
        StopThumbnailWorker();

        std::vector<FullLoadJob> jobs;
        jobs.reserve(videos_.size()+images_.size());
        for(const auto& item:videos_) jobs.push_back({item.path,item.cachePath,item.uiCachePath,BuildPreviewDirectory(item.path),true,item.vr});
        for(const auto& item:images_) jobs.push_back({item.path,item.cachePath,item.uiCachePath,L"",false,item.vr});
        if(jobs.empty()) return;

        // Work where the user is looking first, then continue in the same natural
        // alphabetical filename order used by the Library. A visible Library viewport
        // may contain several cards; an open Info panel contributes its selected item.
        std::set<std::wstring> priorityPaths;
        if(mode_==Mode::Library){
            const std::set<std::wstring> visibleNow=!visibleLibraryGpuThumbPaths_.empty()?visibleLibraryGpuThumbPaths_:CaptureLibraryReturnWarmPaths(0);
            for(const auto& path:visibleNow)
                priorityPaths.insert(ToLower(fs::path(path).lexically_normal().wstring()));
        }else if(mode_==Mode::Details){
            const auto& current=CurrentItems();
            if(selected_<current.size()) priorityPaths.insert(ToLower(fs::path(current[selected_].path).lexically_normal().wstring()));
        }
        const bool preferredVideoCategory=(category_==Category::Videos);
        auto jobSortKey=[](const FullLoadJob& job){
            return BuildMediaNameSortKey(fs::path(job.source).stem().wstring());
        };
        auto keyLess=[](const MediaNameSortKey& a,const MediaNameSortKey& b){
            if(a.primary!=b.primary) return a.primary<b.primary;
            if(a.group!=b.group) return a.group<b.group;
            if(a.secondary!=b.secondary) return a.secondary<b.secondary;
            if(a.hasNumber!=b.hasNumber) return !a.hasNumber;
            if(a.hasNumber){ const int cmp=CompareSortNumbers(a.number,b.number); if(cmp!=0) return cmp<0; }
            return a.fallback<b.fallback;
        };
        std::stable_sort(jobs.begin(),jobs.end(),[&](const FullLoadJob& a,const FullLoadJob& b){
            const std::wstring aPath=ToLower(fs::path(a.source).lexically_normal().wstring());
            const std::wstring bPath=ToLower(fs::path(b.source).lexically_normal().wstring());
            const bool aPriority=priorityPaths.find(aPath)!=priorityPaths.end();
            const bool bPriority=priorityPaths.find(bPath)!=priorityPaths.end();
            const int aRank=aPriority?0:(a.isVideo==preferredVideoCategory?1:2);
            const int bRank=bPriority?0:(b.isVideo==preferredVideoCategory?1:2);
            if(aRank!=bRank) return aRank<bRank;
            const MediaNameSortKey ka=jobSortKey(a),kb=jobSortKey(b);
            if(keyLess(ka,kb)) return true;
            if(keyLess(kb,ka)) return false;
            return aPath<bPath;
        });

        fullLoadStop_.store(false,std::memory_order_release);
        fullLoadYieldCurrent_.store(false,std::memory_order_release);
        fullLoadCurrent_.store(0,std::memory_order_relaxed);
        fullLoadTotal_.store(static_cast<int>(jobs.size()),std::memory_order_relaxed);
        fullLoadFailures_.store(0,std::memory_order_relaxed);
        fullLoadFinishedAt_=0;
        ClearFullLoadFailedPaths();
        const bool wasFailureFilter=loadFailureFilterActive_;
        ClearLoadFailureFilter();
        if(wasFailureFilter && searchQuery_.empty()) searchVisible_=false;
        filteredIndices_.clear(); filterDirty_=true;
        SetFullLoadCurrentFile(L"");
        fullLoadRunning_.store(true,std::memory_order_release);
        InvalidateLoadingPopupArea();

        fullLoadThread_=std::thread([this,jobs=std::move(jobs)]() mutable {
            const HRESULT coHr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            // Stay below interactive UI/playback work, but do not deliberately starve
            // an idle batch. WaitForBackgroundPermit still yields immediately whenever
            // scrolling, playback or other foreground work asks for priority.
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
            int processed=0;
            for(const auto& job:jobs) {
                if(fullLoadStop_.load(std::memory_order_acquire)) break;
                if(!WaitForBackgroundPermit(fullLoadStop_,true)) break;
                SetFullLoadCurrentFile(job.source);
                fullLoadCurrent_.store(std::min(processed+1,static_cast<int>(jobs.size())),std::memory_order_relaxed);
                if(hwnd_) PostMessageW(hwnd_,WM_APP_FULL_LOAD_PROGRESS,1,0);

                std::error_code sourceEc;
                const bool sourceReady=fs::is_regular_file(job.source,sourceEc) && !sourceEc;
                bool jobOk=false;
                bool claimHeld=sourceReady && WaitForGenerationClaim(job.source,fullLoadStop_);
                if(claimHeld) {
                    // Foreground Info/Timeline generation may arrive while this batch owns
                    // a source. Finish the current atomic operation, release ownership,
                    // wait for the foreground worker, then resume/re-check this same job.
                    auto yieldToForeground=[&]()->bool{
                        const bool requested=fullLoadYieldCurrent_.exchange(false,std::memory_order_acq_rel) || ForegroundGenerationPriorityActive();
                        if(!requested) return true;
                        if(claimHeld){ ReleaseGenerationClaim(job.source); claimHeld=false; }
                        while(!fullLoadStop_.load(std::memory_order_acquire) && ForegroundGenerationPriorityActive()) Sleep(10);
                        if(fullLoadStop_.load(std::memory_order_acquire)) return false;
                        if(!WaitForBackgroundPermit(fullLoadStop_,true)) return false;
                        claimHeld=WaitForGenerationClaim(job.source,fullLoadStop_);
                        return claimHeld;
                    };
                    yieldToForeground();

                    // Always re-check after acquiring the claim. The selected-media worker
                    // may have completed this exact file while Load everything was waiting.
                    if(job.isVideo && claimHeld) {
                        ThumbJob videoJob{job.source,job.cachePath,job.uiCachePath,true,job.vr};

                        double libraryTime=-1.0,infoTime=-1.0;
                        bool libraryReady=LibraryPreviewMasterHealthy(job.uiCachePath) &&
                                          ReadBannerTimestamp(job.uiCachePath,libraryTime);
                        bool infoReady=CacheFileLooksHealthy(job.cachePath,1024) &&
                                       ReadBannerTimestamp(job.cachePath,infoTime);

                        // Resolution can be harvested from either Media Foundation session
                        // below. Only fall back to a dedicated metadata probe when all visual
                        // caches were already complete and the sidecar itself is missing.
                        UINT resolutionW=0,resolutionH=0;
                        bool resolutionReady=ReadResolutionMetadata(job.uiCachePath,resolutionW,resolutionH);

                        if(libraryReady && infoReady && std::abs(infoTime-libraryTime)>0.001){
                            // Library's 10% timestamp is authoritative. A mismatched native
                            // Info image is cheap to rebuild and must never point at a
                            // neighboring keyframe.
                            RemoveGeneratedCacheFile(job.cachePath);
                            RemoveGeneratedCacheFile(BannerTimestampPath(job.cachePath));
                            infoReady=false;
                        }

                        if(!libraryReady && !infoReady && !fullLoadStop_.load(std::memory_order_acquire)){
                            // Both derivatives come from one native 10% decode/seek. If a
                            // codec rejects the shared native path, the partial repair below
                            // falls back to the long-established single-output generators.
                            GenerateVideoVisualCachesMF(videoJob,&fullLoadStop_,&backgroundPauseUntil_,
                                                        &resolutionW,&resolutionH);
                            yieldToForeground();
                        }

                        // Re-check and repair either remaining derivative independently.
                        // This keeps the optimization transparent to difficult/legacy codecs.
                        libraryTime=-1.0; infoTime=-1.0;
                        libraryReady=LibraryPreviewMasterHealthy(job.uiCachePath) &&
                                     ReadBannerTimestamp(job.uiCachePath,libraryTime);
                        infoReady=CacheFileLooksHealthy(job.cachePath,1024) &&
                                  ReadBannerTimestamp(job.cachePath,infoTime);
                        if(claimHeld && !libraryReady && !fullLoadStop_.load(std::memory_order_acquire)){
                            GenerateGridThumb(videoJob,&fullLoadStop_,&backgroundPauseUntil_);
                            yieldToForeground();
                        }

                        libraryTime=-1.0; infoTime=-1.0;
                        libraryReady=LibraryPreviewMasterHealthy(job.uiCachePath) &&
                                     ReadBannerTimestamp(job.uiCachePath,libraryTime);
                        infoReady=CacheFileLooksHealthy(job.cachePath,1024) &&
                                  ReadBannerTimestamp(job.cachePath,infoTime);
                        if(libraryReady && infoReady && std::abs(infoTime-libraryTime)>0.001){
                            RemoveGeneratedCacheFile(job.cachePath);
                            RemoveGeneratedCacheFile(BannerTimestampPath(job.cachePath));
                            infoReady=false;
                        }
                        if(claimHeld && libraryReady && !infoReady && !fullLoadStop_.load(std::memory_order_acquire)){
                            GenerateVideoCache(videoJob,&fullLoadStop_,&backgroundPauseUntil_);
                            yieldToForeground();
                        }

                        libraryTime=-1.0; infoTime=-1.0;
                        libraryReady=LibraryPreviewMasterHealthy(job.uiCachePath) &&
                                     ReadBannerTimestamp(job.uiCachePath,libraryTime);
                        infoReady=CacheFileLooksHealthy(job.cachePath,1024) &&
                                  ReadBannerTimestamp(job.cachePath,infoTime);

                        // complete.txt is authoritative for an already-finished Timeline.
                        // Individual corrupt/missing JPEGs are detected and repaired lazily
                        // when displayed; once that happens the marker is invalidated and a
                        // later Load Everything fills the gap. This avoids opening every
                        // 1920x1080 JPEG twice during a healthy batch.
                        bool previewsReady=PreviewCacheIsCompleteForDir(job.previewDir);
                        while(claimHeld && !fullLoadStop_.load(std::memory_order_acquire) && !previewsReady) {
                            if(!yieldToForeground()) break;
                            std::error_code markerEc;
                            fs::remove(fs::path(job.previewDir)/L"complete.txt",markerEc);
                            UINT timelineW=0,timelineH=0;
                            fullLoadYieldCurrent_.store(false,std::memory_order_release);
                            GenerateVideoPreviewsMF(job.source,job.previewDir,job.vr,fullLoadStop_,
                                                    nullptr,nullptr,nullptr,nullptr,
                                                    &backgroundPauseUntil_,&timelineW,&timelineH,&fullLoadYieldCurrent_);
                            if(!resolutionW || !resolutionH){
                                resolutionW=timelineW;
                                resolutionH=timelineH;
                            }
                            previewsReady=PreviewCacheIsCompleteForDir(job.previewDir);
                            if(fullLoadYieldCurrent_.load(std::memory_order_acquire)){
                                if(!yieldToForeground()) break;
                                // Healthy frames already written remain reusable. Resume the
                                // same Timeline after foreground generation releases priority.
                                continue;
                            }
                            break;
                        }

                        if(!resolutionReady && resolutionW && resolutionH)
                            resolutionReady=WriteResolutionMetadata(job.uiCachePath,resolutionW,resolutionH);
                        if(!resolutionReady && !fullLoadStop_.load(std::memory_order_acquire))
                            resolutionReady=EnsureResolutionMetadataCached(job.source,job.uiCachePath,resolutionW,resolutionH);
                        if(resolutionReady) QueueResolutionMetadata(job.source,job.uiCachePath,true);
                        HideCacheRootIfCreated(job.source);

                        libraryTime=-1.0; infoTime=-1.0;
                        libraryReady=LibraryPreviewMasterHealthy(job.uiCachePath) &&
                                     ReadBannerTimestamp(job.uiCachePath,libraryTime);
                        infoReady=CacheFileLooksHealthy(job.cachePath,1024) &&
                                  ReadBannerTimestamp(job.cachePath,infoTime);
                        previewsReady=PreviewCacheIsCompleteForDir(job.previewDir);
                        jobOk=libraryReady && infoReady && resolutionReady &&
                              std::abs(infoTime-libraryTime)<=0.001 && previewsReady;
                    } else if(claimHeld) {
                        ThumbJob imageJob{job.source,job.cachePath,job.uiCachePath,false,job.vr};
                        if(GenerateGridThumb(imageJob,&fullLoadStop_,&backgroundPauseUntil_)) HideCacheRootIfCreated(job.source);
                        jobOk=LibraryPreviewMasterHealthy(job.uiCachePath);
                    }
                    if(claimHeld) ReleaseGenerationClaim(job.source);
                }
                if(!jobOk && !fullLoadStop_.load(std::memory_order_acquire)){
                    fullLoadFailures_.fetch_add(1,std::memory_order_relaxed);
                    RecordFullLoadFailedPath(job.source);
                }

                ++processed;
                if(hwnd_) PostMessageW(hwnd_,WM_APP_FULL_LOAD_PROGRESS,0,0);
                if(fullLoadStop_.load(std::memory_order_acquire)) break;
            }
            const bool completed=!fullLoadStop_.load(std::memory_order_acquire);
            SetFullLoadCurrentFile(L"");
            fullLoadRunning_.store(false,std::memory_order_release);
            if(hwnd_) PostMessageW(hwnd_,WM_APP_FULL_LOAD_DONE,completed?1:0,0);
            if(SUCCEEDED(coHr)) { ReleaseThreadWicFactory(); CoUninitialize(); }
        });
    }

    HBITMAP LoadBitmapViaWic(const std::wstring& file, int maxW, int maxH) {
        IWICImagingFactory* factory=ThreadWicFactory();
        if(!factory) return nullptr;
        ComPtr<IWICBitmapDecoder> decoder;
        if(FAILED(factory->CreateDecoderFromFilename(file.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,decoder.GetAddressOf()))) return nullptr;
        ComPtr<IWICBitmapFrameDecode> frame;
        if(FAILED(decoder->GetFrame(0,frame.GetAddressOf())) || !frame) return nullptr;
        UINT sw=0,sh=0;
        if(FAILED(frame->GetSize(&sw,&sh)) || !sw || !sh) return nullptr;

        const vmp::SafeDecodeSize safeSize=vmp::CalculateSafeDecodeSize(sw,sh,maxW,maxH);
        if(!safeSize.valid) return nullptr;
        const UINT dw=safeSize.width,dh=safeSize.height;

        IWICBitmapSource* source=frame.Get();
        ComPtr<IWICBitmapScaler> scaler;
        if(dw!=sw || dh!=sh){
            if(FAILED(factory->CreateBitmapScaler(scaler.GetAddressOf())) || !scaler) return nullptr;
            if(FAILED(scaler->Initialize(frame.Get(),dw,dh,WICBitmapInterpolationModeFant))) return nullptr;
            source=scaler.Get();
        }

        ComPtr<IWICFormatConverter> converter;
        if(FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) || !converter) return nullptr;
        if(FAILED(converter->Initialize(source,GUID_WICPixelFormat32bppBGRA,WICBitmapDitherTypeNone,nullptr,0.0,WICBitmapPaletteTypeCustom))) return nullptr;
        if(dw>UINT_MAX/4u) return nullptr;
        const UINT stride=dw*4u;
        if(stride && dh>UINT_MAX/stride) return nullptr;
        const UINT bytes=stride*dh;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth=static_cast<LONG>(dw);
        bmi.bmiHeader.biHeight=-static_cast<LONG>(dh);
        bmi.bmiHeader.biPlanes=1;
        bmi.bmiHeader.biBitCount=32;
        bmi.bmiHeader.biCompression=BI_RGB;
        void* bits=nullptr;
        HBITMAP hbmp=CreateDIBSection(nullptr,&bmi,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!hbmp || !bits){ if(hbmp) DeleteObject(hbmp); return nullptr; }
        if(FAILED(converter->CopyPixels(nullptr,stride,bytes,static_cast<BYTE*>(bits)))){ DeleteObject(hbmp); return nullptr; }
        return hbmp;
    }

    HBITMAP LoadScaledBitmap(const std::wstring& file, int maxW, int maxH) {
        // WIC is the normal disk-image decoder. Direct2D owns final compositing/scaling
        // on the GPU; this CPU-side bucket decode exists only to keep RAM/VRAM bounded.
        // GDI+ is retained solely for codecs/files that WIC cannot decode.
        if(HBITMAP wic=LoadBitmapViaWic(file,maxW,maxH)) return wic;
        Gdiplus::Image src(file.c_str());
        if(src.GetLastStatus()==Gdiplus::Ok){
            const UINT sw=src.GetWidth(),sh=src.GetHeight();
            if(sw&&sh){
                const vmp::SafeDecodeSize safeSize=vmp::CalculateSafeDecodeSize(sw,sh,maxW,maxH);
                if(!safeSize.valid) return nullptr;
                const int dw=static_cast<int>(safeSize.width),dh=static_cast<int>(safeSize.height);
                Gdiplus::Bitmap out(dw,dh,PixelFormat32bppARGB); Gdiplus::Graphics g(&out);
                g.Clear(Gdiplus::Color(255,0,0,0)); g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality); g.DrawImage(&src,Gdiplus::Rect(0,0,dw,dh));
                HBITMAP hbmp=nullptr;
                if(out.GetHBITMAP(Gdiplus::Color(255,0,0,0),&hbmp)==Gdiplus::Ok && hbmp) return hbmp;
            }
        }
        return nullptr;
    }

    HBITMAP LoadShellThumbByPath(const std::wstring& path, int w, int h, bool cacheOnly) {
        ComPtr<IShellItem> shell;
        if(FAILED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&shell)))) return nullptr;
        ComPtr<IShellItemImageFactory> factory;
        if(FAILED(shell.As(&factory))) return nullptr;
        SIZE size{w,h}; HBITMAP bmp=nullptr;
        SIIGBF flags=static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY|SIIGBF_BIGGERSIZEOK);
        if(cacheOnly) flags=static_cast<SIIGBF>(flags|SIIGBF_INCACHEONLY);
        if(SUCCEEDED(factory->GetImage(size,flags,&bmp)) && bmp) return bmp;
        return nullptr;
    }

    HBITMAP LoadShellCachedThumbByPath(const std::wstring& path, int w, int h) {
        return LoadShellThumbByPath(path,w,h,true);
    }

    static bool ProbeVideoFrameSizeMF(const std::wstring& path, UINT& width, UINT& height) {
        width=height=0;
        ComPtr<IMFSourceReader> reader;
        if(FAILED(MFCreateSourceReaderFromURL(path.c_str(),nullptr,&reader))) return false;
        ComPtr<IMFMediaType> type;
        if(FAILED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM),0,&type)) || !type) return false;
        UINT w=0,h=0;
        if(FAILED(MFGetAttributeSize(type.Get(),MF_MT_FRAME_SIZE,&w,&h)) || !w || !h) return false;
        width=w; height=h;
        return true;
    }

    bool EnsureResolutionMetadataCached(const std::wstring& source,const std::wstring& uiCachePath,UINT& width,UINT& height) {
        width=height=0;
        std::lock_guard<std::mutex> cacheLock(resolutionMetadataCacheMutex_);
        if(ReadResolutionMetadata(uiCachePath,width,height)) return true;
        if(!ProbeVideoFrameSizeMF(source,width,height)) return false;
        if(!WriteResolutionMetadata(uiCachePath,width,height)) return false;
        HideCacheRootIfCreated(source);
        return true;
    }

    void StartResolutionMetadataWorker() {
        if(resolutionMetadataThread_.joinable()) return;
        resolutionMetadataStop_.store(false,std::memory_order_release);
        resolutionMetadataThread_=std::thread([this](){
            const HRESULT coHr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
            for(;;){
                ResolutionMetadataJob job;
                {
                    std::unique_lock<std::mutex> lock(resolutionMetadataMutex_);
                    resolutionMetadataCv_.wait(lock,[this]{
                        return resolutionMetadataStop_.load(std::memory_order_acquire) || !resolutionMetadataJobs_.empty();
                    });
                    if(resolutionMetadataStop_.load(std::memory_order_acquire)) break;
                    job=std::move(resolutionMetadataJobs_.front());
                    resolutionMetadataJobs_.pop_front();
                }
                if(job.generation!=resolutionMetadataGeneration_.load(std::memory_order_acquire)) continue;
                if((ProcessMemoryBytes()>=CurrentProcessMemoryPolicy().allocationGuard || !job.highPriority) &&
                   !WaitForBackgroundPermit(resolutionMetadataStop_)) break;

                auto* result=new ResolutionMetadataResult();
                result->itemPath=job.itemPath; result->uiCachePath=job.uiCachePath;
                result->generation=job.generation; result->attempted=true;
                EnsureResolutionMetadataCached(job.itemPath,job.uiCachePath,result->sourceWidth,result->sourceHeight);

                {
                    std::lock_guard<std::mutex> lock(resolutionMetadataMutex_);
                    resolutionMetadataPendingPaths_.erase(ToLower(fs::path(job.uiCachePath).lexically_normal().wstring()));
                }
                if(job.generation!=resolutionMetadataGeneration_.load(std::memory_order_acquire)) { delete result; continue; }
                if(!hwnd_ || !PostMessageW(hwnd_,WM_APP_RESOLUTION_METADATA_READY,0,reinterpret_cast<LPARAM>(result))) delete result;
            }
            if(SUCCEEDED(coHr)) { ReleaseThreadWicFactory(); CoUninitialize(); }
        });
    }

    void StopResolutionMetadataWorker() {
        resolutionMetadataStop_.store(true,std::memory_order_release);
        resolutionMetadataCv_.notify_all();
        if(resolutionMetadataThread_.joinable()) resolutionMetadataThread_.join();
        resolutionMetadataStop_.store(false,std::memory_order_release);
        std::lock_guard<std::mutex> lock(resolutionMetadataMutex_);
        resolutionMetadataJobs_.clear(); resolutionMetadataPendingPaths_.clear();
    }

    void ResetResolutionMetadataWork() {
        resolutionMetadataGeneration_.fetch_add(1,std::memory_order_acq_rel);
        std::lock_guard<std::mutex> lock(resolutionMetadataMutex_);
        resolutionMetadataJobs_.clear(); resolutionMetadataPendingPaths_.clear();
    }

    void QueueResolutionMetadata(const std::wstring& source,const std::wstring& uiCachePath,bool highPriority) {
        if(source.empty()||uiCachePath.empty()) return;
        if(!highPriority && ProcessMemoryBytes()>=CurrentProcessMemoryPolicy().emergency) return;
        const std::wstring key=ToLower(fs::path(uiCachePath).lexically_normal().wstring());
        ResolutionMetadataJob job;
        job.itemPath=source; job.uiCachePath=uiCachePath; job.highPriority=highPriority;
        job.generation=resolutionMetadataGeneration_.load(std::memory_order_acquire);
        {
            std::lock_guard<std::mutex> lock(resolutionMetadataMutex_);
            if(resolutionMetadataPendingPaths_.find(key)!=resolutionMetadataPendingPaths_.end()) return;
            resolutionMetadataPendingPaths_.insert(key);
            if(highPriority) resolutionMetadataJobs_.push_front(std::move(job));
            else resolutionMetadataJobs_.push_back(std::move(job));
        }
        resolutionMetadataCv_.notify_one();
    }

    void HandleResolutionMetadataResult(ResolutionMetadataResult* result) {
        if(!result) return;
        if(result->generation!=resolutionMetadataGeneration_.load(std::memory_order_acquire)){delete result;return;}
        bool changed=false;
        for(auto& item:videos_){
            if(!PathEquals(item.path,result->itemPath)) continue;
            item.resolutionMetadataQueued=false;
            item.resolutionProbeAttempted=result->attempted;
            if(result->sourceWidth&&result->sourceHeight){
                changed=item.sourceWidth!=result->sourceWidth || item.sourceHeight!=result->sourceHeight;
                item.sourceWidth=result->sourceWidth; item.sourceHeight=result->sourceHeight;
            }
            break;
        }
        if(searchVisible_&&!searchQuery_.empty()) filterDirty_=true;
        if((changed || (searchVisible_&&!searchQuery_.empty())) && mode_==Mode::Library) InvalidateLibraryScrollableArea();
        if(mode_==Mode::Details && category_==Category::Videos && selected_<videos_.size() && PathEquals(videos_[selected_].path,result->itemPath))
            InvalidateRect(hwnd_,nullptr,FALSE);
        delete result;
    }

    static int ResolutionBadgeClass(const MediaItem& item) {
        if(!item.isVideo || !item.sourceWidth || !item.sourceHeight) return 0;
        const UINT span=std::max(item.sourceWidth,item.sourceHeight);
        // Map encoded widths into the existing visual badge set. The project has
        // dedicated PNG artwork for 4K, 5K and 8K only, so intermediate VR widths
        // such as 5760/6144/7168 use the 5K badge rather than a mismatched text badge.
        if(span>=7680u) return 8;
        if(span>=5120u) return 5;
        if(span>=3840u) return 4;
        return 0;
    }

    Gdiplus::Bitmap* ResolutionBadgeBitmap(const MediaItem& item) const {
        switch(ResolutionBadgeClass(item)){
            case 4: return resolution4kBitmap_.get();
            case 5: return resolution5kBitmap_.get();
            case 8: return resolution8kBitmap_.get();
            default: return nullptr;
        }
    }

    void StartLibraryThumbLoader() {
        if(!libraryThumbLoadThreads_.empty()) return;
        libraryThumbLoadStop_.store(false,std::memory_order_release);
        auto worker=[this]() {
            const HRESULT coHr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            // Keep the UI/paint thread responsive even while several cached thumbnails are
            // being decoded in parallel. More workers provide throughput; lower priority
            // prevents them from stealing the frame budget from fullscreen scrolling.
            SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_LOWEST);
            for(;;){
                LibraryThumbLoadJob job;
                {
                    std::unique_lock<std::mutex> lock(libraryThumbLoadMutex_);
                    libraryThumbLoadCv_.wait(lock,[this]{
                        return libraryThumbLoadStop_.load(std::memory_order_acquire) || !libraryThumbLoadJobs_.empty();
                    });
                    if(libraryThumbLoadStop_.load(std::memory_order_acquire)) break;
                    job=std::move(libraryThumbLoadJobs_.front());
                    libraryThumbLoadJobs_.pop_front();
                }

                if(job.epoch!=libraryThumbViewEpoch_.load(std::memory_order_acquire)) continue;

                // Loading stays independent from scrolling. Workers continue decoding the
                // current viewport/protected neighborhood at low priority while the user
                // moves the scrollbar; only stale-epoch jobs are discarded.
                if(libraryThumbLoadStop_.load(std::memory_order_acquire) ||
                   job.epoch!=libraryThumbViewEpoch_.load(std::memory_order_acquire)) continue;

                const uint64_t memoryBefore=ProcessMemoryBytes();
                // Existing disk-cache banners are cheap reconstructable UI state. Always
                // allow queued jobs to read them, even when process RAM is high. The old
                // early-continue path could drop a visible request without returning a
                // result, leaving that card gray indefinitely. Only opening the original
                // media to generate a missing banner remains pressure-gated.
                const bool allowSourceWork=memoryBefore<CurrentProcessMemoryPolicy().allocationGuard && !SystemMemoryCriticallyLow();

                LibraryThumbLoadResult result;
                result.category=job.category;
                result.index=job.index;
                result.itemPath=job.itemPath;
                result.cachePath=job.cachePath;
                result.epoch=job.epoch;
                result.bitmapRequest=job.loadBitmap;

                // Thumbnail filesystem access and JPEG decoding happen here, never in
                // WM_PAINT. A missing/corrupt private cache is generated only for a
                // currently visible card. Off-screen prefetch may read an existing cache,
                // but it never opens the original media file to create new work.
                bool cacheReady=job.loadBitmap && LibraryPreviewMasterHealthy(job.cachePath);
                if(cacheReady){
                    result.bitmap=LoadScaledBitmap(job.cachePath,std::max(1,job.width),std::max(1,job.height));
                    if(result.bitmap) result.fromPrivateCache=true;
                    else if(job.allowGenerate){
                        RemoveGeneratedCacheFile(job.cachePath);
                        if(job.isVideo) RemoveGeneratedCacheFile(BannerTimestampPath(job.cachePath));
                        cacheReady=false;
                    } else {
                        result.privateDecodeFailed=true;
                    }
                }

                if(job.loadBitmap && !result.bitmap && !cacheReady && job.allowGenerate && allowSourceWork &&
                   !fullLoadRunning_.load(std::memory_order_acquire) && PathExistsNoThrow(job.itemPath)){
                    // Eight cache-read workers are useful once banners exist, but opening
                    // eight original videos at once can saturate Media Foundation/disk/GPU
                    // and starve hover/timeline animation. Permit only one lazy source
                    // generator at a time. Other workers remain available for cheap cached
                    // JPEG reads and may show a Shell fallback until the private cache is ready.
                    std::unique_lock<std::mutex> sourceSlot(librarySourceGenerationMutex_,std::try_to_lock);
                    if(sourceSlot.owns_lock()){
                        const bool claimed=TryClaimGeneration(job.itemPath);
                        if(claimed){
                            ThumbJob generated{job.itemPath,L"",job.cachePath,job.isVideo,job.vr};
                            // Unlike the old lazy path, source generation now honors the
                            // same foreground-pause gate as Timeline/Load Everything work.
                            if(GenerateGridThumb(generated,&libraryThumbLoadStop_,&backgroundPauseUntil_)){
                                HideCacheRootIfCreated(job.itemPath);
                                result.bitmap=LoadScaledBitmap(job.cachePath,std::max(1,job.width),std::max(1,job.height));
                                if(result.bitmap) result.fromPrivateCache=true;
                            }
                            ReleaseGenerationClaim(job.itemPath);
                        }
                    }
                }

                // A Shell thumbnail is a one-shot fallback only while no valid VMP cache
                // exists. Re-check here because Load Everything or another generation worker
                // may have completed the private cache after this job began. Once that file
                // exists, never return a Windows thumbnail for the item.
                if(job.loadBitmap && !result.bitmap && LibraryPreviewMasterHealthy(job.cachePath)){
                    result.bitmap=LoadScaledBitmap(job.cachePath,std::max(1,job.width),std::max(1,job.height));
                    if(result.bitmap) result.fromPrivateCache=true;
                }
                if(job.loadBitmap && job.allowGenerate && allowSourceWork && !result.bitmap &&
                   !LibraryPreviewMasterHealthy(job.cachePath)){
                    result.bitmap=LoadShellCachedThumbByPath(job.itemPath,job.width,job.height);
                    // Images should never remain as gray cards just because no Shell
                    // thumbnail happened to be cached yet. If VMP's WIC-first private generation
                    // (including its GDI+ compatibility fallback) genuinely failed, let the Shell provider create the
                    // visible backup on this worker thread.  The VMP cache still wins
                    // immediately whenever it later becomes healthy.
                    if(!result.bitmap && !job.isVideo)
                        result.bitmap=LoadShellThumbByPath(job.itemPath,job.width,job.height,false);
                }

                if(result.bitmap){
                    BITMAP bm{}; GetObjectW(result.bitmap,sizeof(bm),&bm);
                    result.width=bm.bmWidth; result.height=bm.bmHeight;
                }

                if(libraryThumbLoadStop_.load(std::memory_order_acquire) ||
                   job.epoch!=libraryThumbViewEpoch_.load(std::memory_order_acquire)){
                    if(result.bitmap) DeleteObject(result.bitmap);
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
                    libraryThumbLoadResults_.push_back(std::move(result));
                }
                if(hwnd_ && !libraryThumbResultMessagePending_.exchange(true,std::memory_order_acq_rel))
                    PostMessageW(hwnd_,WM_APP_LIBRARY_THUMB_LOADED,0,0);
            }
            if(SUCCEEDED(coHr)) { ReleaseThreadWicFactory(); CoUninitialize(); }
        };

        // Smoothness-first Library policy: keep enough parallel decode work in flight that
        // a fast fullscreen scroll is normally drawing from RAM rather than waiting on disk.
        // The cache remains bounded, but deliberately much larger than the old low-RAM setup.
        for(int i=0;i<8;++i) libraryThumbLoadThreads_.emplace_back(worker);
    }

    void StopLibraryThumbLoader() {
        libraryThumbLoadStop_.store(true,std::memory_order_release);
        libraryThumbLoadCv_.notify_all();
        for(auto& thread:libraryThumbLoadThreads_) if(thread.joinable()) thread.join();
        libraryThumbLoadThreads_.clear();

        std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
        libraryThumbLoadJobs_.clear();
        for(auto& result:libraryThumbLoadResults_) if(result.bitmap) DeleteObject(result.bitmap);
        libraryThumbLoadResults_.clear();
        libraryThumbResultMessagePending_.store(false,std::memory_order_release);
    }

    void ResetLibraryThumbLoadView() {
        if(hwnd_){
            KillTimer(hwnd_,kLibraryPrefetchPulseTimerId);
            KillTimer(hwnd_,kLibraryThumbApplyTimerId);
            KillTimer(hwnd_,kLibraryScrollSettleTimerId);
        }
        libraryPrefetchPulseArmed_=false;
        libraryThumbApplyTimerArmed_=false;
        libraryWorkingSetNeedsEpochCancel_=false;
        libraryThumbDecodePauseUntil_.store(0,std::memory_order_release);
        libraryThumbViewEpoch_.fetch_add(1,std::memory_order_acq_rel);
        libraryThumbViewportScrollY_=-1;
        libraryThumbViewportCardWidth_=-1;
        libraryThumbViewportClientWidth_=-1;
        libraryThumbViewportFolder_.clear();
        libraryThumbViewportSearch_.clear();
        protectedLibraryThumbPaths_.clear();
        visibleLibraryGpuThumbPaths_.clear();
        libraryWorkingSetThumbPaths_.clear();
        libraryWorkingSetQueueIndices_.clear();libraryWorkingSetQueueCursor_=0;
        libraryProtectedQueueIndices_.clear();libraryProtectedQueueCursor_=0;
        libraryProtectedWindowFirstRow_=-1;libraryProtectedWindowLastRow_=-1;libraryProtectedWindowCols_=-1;
        {
            std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
            libraryThumbLoadJobs_.clear();
            for(auto& result:libraryThumbLoadResults_) if(result.bitmap) DeleteObject(result.bitmap);
            libraryThumbLoadResults_.clear();
        }
        libraryThumbResultMessagePending_.store(false,std::memory_order_release);
    }

    void RefreshLibraryThumbViewport(RECT rc) {
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left));
        const bool scrollChanged=libraryThumbViewportScrollY_!=scrollY_;
        const bool identityChanged=libraryThumbViewportCardWidth_!=libraryCardWidth_ ||
                                   libraryThumbViewportClientWidth_!=clientWidth ||
                                   libraryThumbViewportCategory_!=category_ ||
                                   libraryThumbViewportFolder_!=currentFolder_ ||
                                   libraryThumbViewportSearch_!=searchQuery_;
        if(!scrollChanged && !identityChanged) return;
        MarkLibraryBrowsingActivity();

        if(libraryThumbViewportScrollY_>=0 && scrollChanged)
            libraryThumbPrefetchDirection_=(scrollY_>libraryThumbViewportScrollY_)?1:-1;

        if(identityChanged){
            // Epoch invalidation is O(1). Do not synchronously destroy a large old-folder
            // queue/result batch from the navigation click; workers skip stale jobs and the
            // deferred result installer discards stale bitmaps after input becomes quiet.
            libraryThumbViewEpoch_.fetch_add(1,std::memory_order_acq_rel);
        }else if(scrollChanged){
            // Do not cancel cache-only work on every wheel/trackpad tick. V3 rebuilt a
            // several-hundred-thumbnail queue for each scroll frame, which made the UI
            // fight its own prefetcher. Keep in-flight decodes; newly visible/nearby jobs
            // are promoted ahead of them, and the coalesced settle pass rebuilds the wider
            // working set after the gesture quiets down. The +/-10-row protection set is
            // rebuilt only when its row boundaries actually change.
            visibleLibraryGpuThumbPaths_.clear();
        }

        // A folder/category/layout identity change invalidates the old working set. Mere
        // scrolling keeps it alive until the coalesced planner replaces it, preventing
        // decode/evict/requeue oscillation while the viewport is moving.
        if(identityChanged){
            libraryWorkingSetThumbPaths_.clear();
            libraryProtectedWindowFirstRow_=-1;libraryProtectedWindowLastRow_=-1;libraryProtectedWindowCols_=-1;
            libraryProtectedQueueCursor_=libraryProtectedQueueIndices_.size();
        }
        libraryThumbViewportScrollY_=scrollY_;
        libraryThumbViewportCardWidth_=libraryCardWidth_;
        libraryThumbViewportClientWidth_=clientWidth;
        libraryThumbViewportCategory_=category_;
        libraryThumbViewportFolder_=currentFolder_;
        libraryThumbViewportSearch_=searchQuery_;
    }

    void QueueVisibleLibraryThumbsForViewport(RECT rc) {
        if(mode_!=Mode::Library || !hwnd_) return;
        const auto& filtered=FilteredIndices();
        const auto visibleFolders=VisibleFolderIndices();
        const size_t totalCards=visibleFolders.size()+filtered.size();
        visibleLibraryGpuThumbPaths_.clear();
        if(totalCards==0) return;
        auto& list=CurrentItems();
        const int pad=kLibraryPad,gap=kLibraryGap,cardW=libraryCardWidth_;
        const int imageH=std::max(113,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int rowStride=imageH+kLibraryTitleHeight+gap;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left)-kLibraryScrollbarReserve);
        const int cols=std::max(1,(clientWidth-pad*2+gap)/(cardW+gap));
        const int rows=static_cast<int>((totalCards+static_cast<size_t>(cols)-1)/static_cast<size_t>(cols));
        const int visibleBottom=std::max(pad,static_cast<int>(rc.bottom)-68);
        const int firstRow=std::clamp(scrollY_/std::max(1,rowStride)-1,0,std::max(0,rows-1));
        const int lastRow=std::clamp((scrollY_+std::max(0,visibleBottom-pad))/std::max(1,rowStride)+1,0,std::max(0,rows-1));
        const size_t first=static_cast<size_t>(firstRow)*static_cast<size_t>(cols);
        const size_t last=std::min(totalCards,static_cast<size_t>(lastRow+1)*static_cast<size_t>(cols));
        for(size_t displayIndex=first;displayIndex<last;++displayIndex){
            if(displayIndex<visibleFolders.size()) continue;
            const size_t mdi=displayIndex-visibleFolders.size();
            if(mdi>=filtered.size()) continue;
            const size_t itemIndex=filtered[mdi];
            if(itemIndex>=list.size()) continue;
            protectedLibraryThumbPaths_.insert(list[itemIndex].path);
            visibleLibraryGpuThumbPaths_.insert(list[itemIndex].path);
            GetLibraryItemThumb(list[itemIndex],itemIndex,cardW,imageH,true);
        }
    }

    // Define the latency-sensitive +/-10-row buffer, but do not enqueue all of it in
    // one UI message. The protection set is cheap state; actual loader requests are
    // issued by QueueLibraryProtectedRowsChunkFromPrivateCache() in bounded slices.
    void QueueLibraryProtectedRowsFromPrivateCache(RECT rc) {
        if(mode_!=Mode::Library || !hwnd_) return;
        const auto& filtered=FilteredIndices();
        const auto visibleFolders=VisibleFolderIndices();
        const size_t totalCards=visibleFolders.size()+filtered.size();
        if(totalCards==0 || filtered.empty()) return;

        auto& list=CurrentItems();
        const int pad=kLibraryPad,gap=kLibraryGap,cardW=libraryCardWidth_;
        const int imageH=std::max(113,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int rowStride=imageH+kLibraryTitleHeight+gap;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left)-kLibraryScrollbarReserve);
        const int cols=std::max(1,(clientWidth-pad*2+gap)/(cardW+gap));
        const int rows=static_cast<int>((totalCards+static_cast<size_t>(cols)-1)/static_cast<size_t>(cols));
        if(rows<=0) return;
        const int visibleBottom=std::max(pad,static_cast<int>(rc.bottom)-68);
        const int firstVisible=std::clamp(scrollY_/std::max(1,rowStride)-1,0,rows-1);
        const int lastVisible=std::clamp((scrollY_+std::max(0,visibleBottom-pad))/std::max(1,rowStride)+1,0,rows-1);
        const int firstRow=std::max(0,firstVisible-kLibraryProtectedRows);
        const int lastRow=std::min(rows-1,lastVisible+kLibraryProtectedRows);
        if(firstRow==libraryProtectedWindowFirstRow_ && lastRow==libraryProtectedWindowLastRow_ &&
           cols==libraryProtectedWindowCols_ &&
           libraryProtectedQueueEpoch_==libraryThumbViewEpoch_.load(std::memory_order_acquire) &&
           libraryProtectedQueueCategory_==category_) return;

        libraryProtectedWindowFirstRow_=firstRow;libraryProtectedWindowLastRow_=lastRow;libraryProtectedWindowCols_=cols;
        protectedLibraryThumbPaths_.clear();
        libraryProtectedQueueIndices_.clear();
        libraryProtectedQueueCursor_=0;
        libraryProtectedQueueEpoch_=libraryThumbViewEpoch_.load(std::memory_order_acquire);
        libraryProtectedQueueCategory_=category_;
        libraryProtectedQueueCardW_=cardW;
        libraryProtectedQueueImageH_=imageH;

        const int rowStart=libraryThumbPrefetchDirection_>=0?firstRow:lastRow;
        const int rowEnd=libraryThumbPrefetchDirection_>=0?lastRow:firstRow;
        const int rowStep=libraryThumbPrefetchDirection_>=0?1:-1;
        for(int row=rowStart;;row+=rowStep){
            const size_t rowFirst=static_cast<size_t>(row)*static_cast<size_t>(cols);
            const size_t rowLast=std::min(totalCards,rowFirst+static_cast<size_t>(cols));
            for(size_t displayIndex=rowFirst;displayIndex<rowLast;++displayIndex){
                if(displayIndex<visibleFolders.size()) continue;
                const size_t mdi=displayIndex-visibleFolders.size();
                if(mdi>=filtered.size()) continue;
                const size_t itemIndex=filtered[mdi];
                if(itemIndex>=list.size()) continue;
                protectedLibraryThumbPaths_.insert(list[itemIndex].path);
                libraryProtectedQueueIndices_.push_back(itemIndex);
            }
            if(row==rowEnd) break;
        }
    }

    bool QueueLibraryProtectedRowsChunkFromPrivateCache(size_t maxItems=16) {
        if(mode_!=Mode::Library || !hwnd_ || maxItems==0) return false;
        if(libraryProtectedQueueEpoch_!=libraryThumbViewEpoch_.load(std::memory_order_acquire) ||
           libraryProtectedQueueCategory_!=category_){
            libraryProtectedQueueIndices_.clear();
            libraryProtectedQueueCursor_=0;
            return false;
        }
        auto& list=CurrentItems();
        const int decodeW=QuantizedLibraryDecodeWidth(libraryProtectedQueueCardW_);
        size_t processed=0;
        while(libraryProtectedQueueCursor_<libraryProtectedQueueIndices_.size() && processed<maxItems){
            const size_t itemIndex=libraryProtectedQueueIndices_[libraryProtectedQueueCursor_++];
            ++processed;
            if(itemIndex>=list.size()) continue;
            MediaItem& item=list[itemIndex];
            if(protectedLibraryThumbPaths_.find(item.path)==protectedLibraryThumbPaths_.end()) continue;
            if(item.thumb && item.thumbFromPrivateCache && item.thumbW>=decodeW) continue;
            GetLibraryItemThumb(item,itemIndex,libraryProtectedQueueCardW_,libraryProtectedQueueImageH_,false,true);
        }
        return libraryProtectedQueueCursor_<libraryProtectedQueueIndices_.size();
    }

    // The desired Library working set is spatial, not byte-targeted. The persistent
    // 1920x1080 master stays on disk; RAM contains only the decode bucket needed for the
    // visible/protected neighborhood. Recently visited thumbnails may remain resident as
    // cold LRU state, but this planner never loads extra cards just because RAM is free.
    void QueueLibraryWorkingSetFromPrivateCache(RECT /*rc*/) {
        libraryWorkingSetThumbPaths_=protectedLibraryThumbPaths_;
        libraryWorkingSetQueueIndices_.clear();
        libraryWorkingSetQueueCursor_=0;
        libraryWorkingSetQueueEpoch_=libraryThumbViewEpoch_.load(std::memory_order_acquire);
        libraryWorkingSetQueueCategory_=category_;
    }

    bool QueueLibraryWorkingSetChunkFromPrivateCache(size_t /*maxItems*/=24) {
        // No deep byte-target warm-up. Visible and +/-10 rows are queued by the protected
        // viewport loader; everything else is retained only if it was actually used.
        return false;
    }

    // Folder/category navigation is cache-first but fully asynchronous. Persistent VMP
    // masters are decoded by the loader pool after the navigation frame is presented;
    // opening a folder never waits for JPEG decode or whole-folder working-set planning.
    void PrepareLibraryViewportFromPrivateCache(UINT prefetchDelay=8) {
        if(mode_!=Mode::Library || !hwnd_) return;
        ClampScroll();
        RECT rc{};
        GetClientRect(hwnd_,&rc);

        // Navigation must feel instantaneous. Updating the logical viewport/epoch is cheap
        // and prevents old-folder work from being treated as current, but all actual cache
        // loading is deferred until after the first frame has had a chance to paint.
        RefreshLibraryThumbViewport(rc);
        MarkLibraryBrowsingActivity();
        // Give the navigation frame first priority, then start cache loading immediately
        // afterward. Unlike actual scrolling, folder entry does not pause the decode pool.
        ScheduleLibraryPrefetchPulse(std::max<UINT>(1,prefetchDelay));
        ScheduleLibraryScrollSettle();
    }

    // Render-only accessor used from WM_PAINT. A lower decode bucket is deliberately kept
    // visible while the asynchronous loader upgrades it; paint never queues or validates.
    HBITMAP GetResidentLibraryItemThumb(MediaItem& item) {
        if(!item.thumb) return nullptr;
        item.thumbLastUsed=GetTickCount64();
        return item.thumb;
    }

    HBITMAP GetLibraryItemThumb(MediaItem& item, size_t index, int w, int h, bool highPriority=false, bool cachePriority=false) {
        const ULONGLONG now=GetTickCount64();
        const int decodeW=QuantizedLibraryDecodeWidth(w);
        const int decodeH=LibraryDecodeHeightForWidth(decodeW);
        w=decodeW; h=decodeH;
        item.thumbLastUsed=now;
        // Resolution probing is also demand-driven: only a visible video may cause a
        // one-time source metadata read. Search-specific probing still has its own
        // explicit high-priority path.
        if(highPriority && item.isVideo && !item.resolutionProbeAttempted && !item.resolutionMetadataQueued){
            item.resolutionMetadataQueued=true;
            QueueResolutionMetadata(item.path,item.uiCachePath,true);
        }

        // Keep the resident bitmap through BOTH shrink and grow operations. Never probe
        // the filesystem from the UI thread just to decide whether a Shell fallback should
        // be replaced; the loader worker owns that decision. Private-cache thumbnails that
        // are already large enough need no request. A fallback may be retried on its normal
        // backoff timer while remaining visible.
        if(item.thumb && item.thumbFromPrivateCache && item.thumbW>=w && item.thumbH>=h) return item.thumb;

        const uint64_t epoch=libraryThumbViewEpoch_.load(std::memory_order_acquire);
        const bool alreadyRequested=item.thumbLoadRequestEpoch==epoch &&
                                    item.thumbLoadRequestW>=w && item.thumbLoadRequestH>=h;

        // Absolute visible-card priority: if this card was merely prefetched and then
        // scrolls into view, promote the existing queued request instead of leaving it
        // buried behind obsolete prefetch work. If a worker already owns the request,
        // it is already actively being decoded and needs no promotion.
        if((highPriority || cachePriority) && alreadyRequested){
            std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
            for(auto it=libraryThumbLoadJobs_.begin();it!=libraryThumbLoadJobs_.end();++it){
                if(it->epoch!=epoch || it->category!=category_ || it->index!=index || !PathEquals(it->itemPath,item.path)) continue;
                LibraryThumbLoadJob promoted=std::move(*it);
                libraryThumbLoadJobs_.erase(it);
                if(highPriority) promoted.allowGenerate=true;
                libraryThumbLoadJobs_.push_front(std::move(promoted));
                libraryThumbLoadCv_.notify_one();
                break;
            }
        }
        if(!alreadyRequested && now>=item.thumbNextLoadAttempt){
            item.thumbLoadRequestEpoch=epoch;
            item.thumbLoadRequestW=w;
            item.thumbLoadRequestH=h;
            LibraryThumbLoadJob job;
            job.category=category_;
            job.index=index;
            job.itemPath=item.path;
            job.cachePath=item.uiCachePath;
            job.isVideo=item.isVideo;
            job.isVr=item.vr.vr;
            job.loadBitmap=true;
            job.allowGenerate=highPriority;
            job.vr=item.vr;
            job.width=w; job.height=h; job.epoch=epoch;
            {
                std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
                if(highPriority || cachePriority) libraryThumbLoadJobs_.push_front(std::move(job));
                else libraryThumbLoadJobs_.push_back(std::move(job));
            }
            libraryThumbLoadCv_.notify_one();
        }
        return item.thumb;
    }

    void NoteLibraryScrollInput() {
        const ULONGLONG now=GetTickCount64();
        lastLibraryScrollInputTick_=now;
        libraryWorkingSetNeedsEpochCancel_=true;
        MarkLibraryBrowsingActivity();

        // Scrolling and loading are independent: input only updates the viewport and
        // the loader keeps decoding the current visible/protected region in the background.
        // Defer only heavyweight maintenance, not the thumbnail decode pool itself.
        libraryThumbDecodePauseUntil_.store(0,std::memory_order_release);
        DeferBackgroundWork(80);
    }

    bool IsLibraryInteractionHot() const {
        if(mode_!=Mode::Library) return false;
        if(libraryScrollDragging_) return true;
        if(lastLibraryScrollInputTick_==0) return false;
        const ULONGLONG now=GetTickCount64();
        return now-lastLibraryScrollInputTick_<kLibraryInteractionQuietMs;
    }

    void ScheduleLibraryPrefetchPulse(UINT delay=kLibraryPrefetchPulseMs) {
        if(!hwnd_ || mode_!=Mode::Library) return;
        // WM_TIMER is intentionally used here: it is low-priority relative to mouse input,
        // so a continuous scrollbar drag wins and loading catches up in the gaps.
        if(libraryPrefetchPulseArmed_) return;
        libraryPrefetchPulseArmed_=true;
        SetTimer(hwnd_,kLibraryPrefetchPulseTimerId,std::max<UINT>(1,delay),nullptr);
    }

    void ScheduleLibraryThumbApply(UINT delay=kLibraryThumbApplyDelayMs) {
        if(!hwnd_ || mode_!=Mode::Library) return;
        if(libraryThumbApplyTimerArmed_) return;
        libraryThumbApplyTimerArmed_=true;
        SetTimer(hwnd_,kLibraryThumbApplyTimerId,std::max<UINT>(1,delay),nullptr);
    }

    void RunLibraryPrefetchPulse() {
        if(mode_!=Mode::Library || !hwnd_) return;

        RECT rc{};GetClientRect(hwnd_,&rc);
        RefreshLibraryThumbViewport(rc);

        if(IsLibraryInteractionHot()){
            // Keep loading alive during scrolling, but restrict it to the current location
            // and its protected +/-10-row neighborhood. WM_TIMER keeps this off the input
            // path, so scrollbar movement never waits for queueing or decode work.
            QueueVisibleLibraryThumbsForViewport(rc);
            QueueLibraryProtectedRowsFromPrivateCache(rc);
            QueuePriorityResolutionMetadataForSearch();
            const bool moreProtected=QueueLibraryProtectedRowsChunkFromPrivateCache(8);
            ScheduleLibraryThumbApply(kLibraryActiveThumbApplyDelayMs);
            if(moreProtected || IsLibraryInteractionHot()) ScheduleLibraryPrefetchPulse(kLibraryActivePrefetchPulseMs);
            return;
        }

        // Once interaction is quiet, visible and +/-10-row jobs remain the complete demand set.
        // There is no byte-target deep warm-up.
        QueueVisibleLibraryThumbsForViewport(rc);
        QueueLibraryProtectedRowsFromPrivateCache(rc);
        QueuePriorityResolutionMetadataForSearch();
        const bool moreProtected=QueueLibraryProtectedRowsChunkFromPrivateCache(16);
        ScheduleLibraryThumbApply(1);
        if(moreProtected) ScheduleLibraryPrefetchPulse(kLibraryPrefetchPulseMs);
    }

    void ScheduleLibraryScrollSettle() {
        if(!hwnd_ || mode_!=Mode::Library) return;
        KillTimer(hwnd_,kLibraryScrollSettleTimerId);
        SetTimer(hwnd_,kLibraryScrollSettleTimerId,kLibraryScrollSettleMs,nullptr);
    }

    void TrimThumbMemoryThrottled() {
        const ULONGLONG now=GetTickCount64();
        if(lastLibraryThumbTrimTick_!=0 && now-lastLibraryThumbTrimTick_<180) return;
        lastLibraryThumbTrimTick_=now;
        TrimThumbMemory();
    }

    void TrimLibraryGpuTexturesThrottled(bool force=false) {
        const ULONGLONG now=GetTickCount64();
        if(!force && lastLibraryGpuTrimTick_!=0 && now-lastLibraryGpuTrimTick_<80) return;
        lastLibraryGpuTrimTick_=now;
        TrimLibraryGpuTextures();
    }

    void SettleLibraryScrollWorkingSet() {
        if(mode_!=Mode::Library || !hwnd_) return;
        RECT rc{};GetClientRect(hwnd_,&rc);

        // A completed scroll gesture gets a fresh request generation. This invalidates old
        // queued viewport work in O(1); workers discard stale jobs as they pop them instead
        // of the UI synchronously walking/erasing a potentially large deque.
        if(libraryWorkingSetNeedsEpochCancel_){
            libraryThumbViewEpoch_.fetch_add(1,std::memory_order_acq_rel);
            libraryWorkingSetNeedsEpochCancel_=false;
        }

        RefreshLibraryThumbViewport(rc);
        QueueVisibleLibraryThumbsForViewport(rc);
        QueueLibraryProtectedRowsFromPrivateCache(rc);

        // The desired set is only the current spatial neighborhood; 12 GB is a ceiling,
        // never a load target. Memory/GPU trimming remains outside the input path.
        QueueLibraryWorkingSetFromPrivateCache(rc);
        ScheduleLibraryPrefetchPulse(1);
        ScheduleLibraryThumbApply(1);
    }

    void ApplyLibraryThumbLoadResults(size_t maxResults=std::numeric_limits<size_t>::max()) {
        std::vector<LibraryThumbLoadResult> ready;
        {
            std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
            if(maxResults==std::numeric_limits<size_t>::max() || libraryThumbLoadResults_.size()<=maxResults){
                ready.swap(libraryThumbLoadResults_);
            }else{
                const size_t take=std::min(maxResults,libraryThumbLoadResults_.size());
                ready.insert(ready.end(),
                             std::make_move_iterator(libraryThumbLoadResults_.begin()),
                             std::make_move_iterator(libraryThumbLoadResults_.begin()+static_cast<std::ptrdiff_t>(take)));
                libraryThumbLoadResults_.erase(libraryThumbLoadResults_.begin(),
                                               libraryThumbLoadResults_.begin()+static_cast<std::ptrdiff_t>(take));
            }
        }
        libraryThumbResultMessagePending_.store(false,std::memory_order_release);

        const uint64_t activeEpoch=libraryThumbViewEpoch_.load(std::memory_order_acquire);
        const ULONGLONG now=GetTickCount64();
        const ProcessMemoryPolicy memoryPolicy=CurrentProcessMemoryPolicy();
        const uint64_t processBytesAtStart=ProcessMemoryBytes();
        const bool allocationGuardAtStart=processBytesAtStart>=memoryPolicy.allocationGuard;
        bool changed=false;
        for(auto& result:ready){
            if(result.epoch!=activeEpoch){ if(result.bitmap) DeleteObject(result.bitmap); continue; }
            auto& list=result.category==Category::Videos?videos_:images_;
            if(result.index>=list.size() || !PathEquals(list[result.index].path,result.itemPath)){
                if(result.bitmap) DeleteObject(result.bitmap);
                continue;
            }

            MediaItem& item=list[result.index];
            if(result.bitmapRequest && item.thumbLoadRequestEpoch==result.epoch) item.thumbLoadRequestEpoch=0;


            if(result.privateDecodeFailed && !thumbWorkerRunning_.load(std::memory_order_acquire)){
                // Corrupt generated files are repaired outside WM_PAINT. Never remove a
                // cache file while the thumbnail generator may still be writing it.
                RemoveGeneratedCacheFile(result.cachePath);
                RemoveGeneratedCacheFile(BannerTimestampPath(result.cachePath));
                if(hwnd_) PostMessageW(hwnd_,WM_APP_CACHE_REPAIR,0,0);
            }

            if(result.bitmap && !result.fromPrivateCache && LibraryPreviewMasterHealthy(item.uiCachePath)){
                // The fallback was decoded before the VMP cache finished but arrived after it.
                // Drop that stale Windows image and any older resident fallback so the next
                // paint can only use the authoritative VMP cache.
                DeleteObject(result.bitmap); result.bitmap=nullptr;
                if(item.thumb && !item.thumbFromPrivateCache){
                    DeleteObject(item.thumb); item.thumb=nullptr; item.thumbW=item.thumbH=0;
                    item.libraryGpuThumb.Reset(); item.libraryGpuThumbSource=nullptr; item.libraryGpuGeneration=0;
                    changed=true;
                }
                item.thumbAttempted=false; item.thumbNextLoadAttempt=0;
            }

            if(result.bitmap){
                const bool protectedNow=protectedLibraryThumbPaths_.find(item.path)!=protectedLibraryThumbPaths_.end();
                const bool visibleNow=visibleLibraryGpuThumbPaths_.find(item.path)!=visibleLibraryGpuThumbPaths_.end();
                const bool desiredNow=libraryWorkingSetThumbPaths_.find(item.path)!=libraryWorkingSetThumbPaths_.end();
                const bool hotLibraryScroll=mode_==Mode::Library &&
                    (libraryScrollDragging_ || (lastLibraryActivityTick_!=0 && now-lastLibraryActivityTick_<350));
                // A decode that belonged to a viewport already left behind must not refill
                // RAM while the user is rapidly moving through the Library. Visible and
                // current ±prefetch-window cards remain protected and install normally.
                if(hotLibraryScroll && result.bitmapRequest && !protectedNow && !visibleNow && !desiredNow){
                    DeleteObject(result.bitmap);result.bitmap=nullptr;
                    item.thumbLoadRequestEpoch=0;item.thumbNextLoadAttempt=0;
                    continue;
                }
                if(allocationGuardAtStart && !protectedNow && !visibleNow){DeleteObject(result.bitmap);result.bitmap=nullptr;item.thumbNextLoadAttempt=now+1500;continue;}
                const bool shouldReplace=!item.thumb || result.fromPrivateCache || !item.thumbFromPrivateCache;
                if(shouldReplace){
                    if(item.thumb) DeleteObject(item.thumb);
                    item.libraryGpuThumb.Reset(); item.libraryGpuThumbSource=nullptr; item.libraryGpuGeneration=0;
                    item.thumb=result.bitmap; result.bitmap=nullptr;
                    item.thumbW=result.width; item.thumbH=result.height;
                    item.thumbFromPrivateCache=result.fromPrivateCache;
                    item.thumbAttempted=true;
                    item.thumbLastUsed=now;
                    changed=true;
                }
                if(result.bitmap) DeleteObject(result.bitmap);
                // Shell fallback is deliberately retried later so it upgrades to the
                // private thumbnail once background generation completes.
                item.thumbNextLoadAttempt=result.fromPrivateCache?0:(now+1500);
            } else if(!item.thumb) {
                item.thumbNextLoadAttempt=now+600;
            }
        }

        // Do not run the O(n log n) multi-GB LRU trim as part of result installation.
        // The settle/idle/process-pressure controller owns trimming. This keeps worker
        // completion bursts from stealing time from scrollbar movement.
        if(changed && mode_==Mode::Library) InvalidateLibraryScrollableArea();

        // If the worker raced with the message handler and produced another result after
        // our swap, make sure a follow-up message is posted.
        bool hasMore=false;
        {
            std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
            hasMore=!libraryThumbLoadResults_.empty();
        }
        if(hasMore && hwnd_ && !libraryThumbResultMessagePending_.exchange(true,std::memory_order_acq_rel))
            PostMessageW(hwnd_,WM_APP_LIBRARY_THUMB_LOADED,0,0);
    }

    void InvalidateLibraryScrollableArea() {
        if(!hwnd_) return;
        RECT rc{}; GetClientRect(hwnd_,&rc);
        RECT dirty{0,0,rc.right,std::max<LONG>(0,rc.bottom-64)};
        InvalidateRect(hwnd_,&dirty,FALSE);
    }

    bool ScrollBufferedRegionVertical(int pixelDelta, RECT region) {
        if(!hwnd_ || pixelDelta==0 || !backDC_) return false;
        RECT rc{}; GetClientRect(hwnd_,&rc);
        RECT clipped{};
        if(!IntersectRect(&clipped,&region,&rc) || IsRectEmpty(&clipped)) return false;
        const int regionH=static_cast<int>(clipped.bottom-clipped.top);
        if(std::abs(pixelDelta)>=regionH || backW_!=rc.right-rc.left || backH_!=rc.bottom-rc.top) return false;

        // Shift the retained backbuffer and the visible client pixels first. This makes
        // the scroll response immediate; WM_PAINT then redraws only newly exposed areas.
        if(!ScrollDC(backDC_,0,pixelDelta,&clipped,&clipped,nullptr,nullptr)) return false;
        HRGN update=CreateRectRgn(0,0,0,0);
        if(!update) return false;
        ScrollWindowEx(hwnd_,0,pixelDelta,&clipped,&clipped,update,nullptr,0);
        InvalidateRgn(hwnd_,update,FALSE);
        DeleteObject(update);
        return true;
    }

    void InvalidateLibraryScrollWithFooter(int /*oldScrollY*/) {
        if(!hwnd_) return;
        RECT rc{}; GetClientRect(hwnd_,&rc);
        // Library scrolling is deliberately rendered as one fresh buffered frame.
        // Reusing/shifting pixels from the previous frame creates exposed-edge seams
        // at high resolutions. The thumbnail cache keeps the redraw RAM-backed and
        // the painter only walks rows that intersect the viewport.
        InvalidateRect(hwnd_,&rc,FALSE);
    }

    void InvalidateDetailsScrollOptimized(int /*oldScrollY*/) {
        if(!hwnd_) return;
        RECT rc{}; GetClientRect(hwnd_,&rc);
        // Details/Info now uses the same GPU compositor as Library. Repaint from the
        // current scroll offset instead of shifting old GDI pixels with ScrollWindowEx.
        InvalidateRect(hwnd_,&rc,FALSE);
    }

    HBITMAP LoadNativeBitmap(const std::wstring& file) {
        // Native Info/banner decode follows the same policy as Library thumbnails:
        // WIC first, Direct2D/GPU for presentation, GDI+ only as a codec fallback.
        if(HBITMAP wic=LoadBitmapViaWic(file,0,0)) return wic;
        Gdiplus::Image src(file.c_str());
        if(src.GetLastStatus()==Gdiplus::Ok){
            const UINT sw=src.GetWidth(),sh=src.GetHeight();
            if(sw&&sh){
                const vmp::SafeDecodeSize safeSize=vmp::CalculateSafeDecodeSize(sw,sh,0,0);
                if(!safeSize.valid) return nullptr;
                const INT dw=static_cast<INT>(safeSize.width),dh=static_cast<INT>(safeSize.height);
                Gdiplus::Bitmap out(dw,dh,PixelFormat32bppARGB); Gdiplus::Graphics g(&out);
                g.Clear(Gdiplus::Color(255,0,0,0));
                g.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                g.DrawImage(&src,Gdiplus::Rect(0,0,dw,dh));
                HBITMAP hbmp=nullptr;
                if(out.GetHBITMAP(Gdiplus::Color(255,0,0,0),&hbmp)==Gdiplus::Ok && hbmp) return hbmp;
            }
        }
        return nullptr;
    }

    HBITMAP TryShellCachedThumb(MediaItem& item, int w, int h) {
        if(item.thumb || item.thumbAttempted) return item.thumb;
        item.thumbAttempted=true;
        // Windows' cached thumbnail is the universal last-resort static backup. It may be
        // used only until VMP has a healthy private cache; the private cache is authoritative.
        ComPtr<IShellItem> shell; if(FAILED(SHCreateItemFromParsingName(item.path.c_str(),nullptr,IID_PPV_ARGS(&shell)))) return nullptr;
        ComPtr<IShellItemImageFactory> f; if(FAILED(shell.As(&f))) return nullptr;
        SIZE size{w,h}; HBITMAP bmp=nullptr;
        SIIGBF flags=static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY|SIIGBF_INCACHEONLY|SIIGBF_BIGGERSIZEOK);
        HRESULT shellHr=f->GetImage(size,flags,&bmp);
        if((FAILED(shellHr) || !bmp) && !item.isVideo){
            if(bmp){DeleteObject(bmp);bmp=nullptr;}
            // For still images the Shell provider is the final compatibility backup.
            // This is only reached when VMP has no valid private master.
            flags=static_cast<SIIGBF>(SIIGBF_THUMBNAILONLY|SIIGBF_BIGGERSIZEOK);
            shellHr=f->GetImage(size,flags,&bmp);
        }
        if(SUCCEEDED(shellHr)&&bmp){
            item.thumb=bmp; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); item.thumbW=bm.bmWidth; item.thumbH=bm.bmHeight;
            item.thumbFromPrivateCache=false;
        }
        return item.thumb;
    }

    HBITMAP GetItemThumb(MediaItem& item, int w, int h) {
        item.thumbLastUsed=GetTickCount64();
        // Library-style thumbnails have one authoritative source at every display size:
        // the 1920x1080 VMP .ui.jpg master.  Do not switch to the native Info banner or
        // the original still image merely because a card/slideshow request is large.
        // RAM remains display-sized through LoadScaledBitmap(); native media is reserved
        // for GetDetailsBanner()/the Info panel.
        const std::wstring preferred = item.uiCachePath;
        const bool generated = true;
        bool cached = LibraryPreviewMasterHealthy(preferred);
        if(!cached && generated && PathExistsNoThrow(preferred)){
            RemoveGeneratedCacheFile(preferred);
            if(preferred==item.uiCachePath) RemoveGeneratedCacheFile(BannerTimestampPath(item.uiCachePath));
            item.thumbAttempted=false;
            if(hwnd_) PostMessageW(hwnd_,WM_APP_CACHE_REPAIR,0,0);
        }
        if(cached){
            const bool tooSmall=item.thumb && (item.thumbW < w || item.thumbH < h);
            if(!item.thumbFromPrivateCache || tooSmall){
                if(item.thumb){ DeleteObject(item.thumb); item.thumb=nullptr; }
                item.libraryGpuThumb.Reset(); item.libraryGpuThumbSource=nullptr; item.libraryGpuGeneration=0;
                item.thumb=LoadScaledBitmap(preferred,std::max(1,w),std::max(1,h));
                if(item.thumb){
                    BITMAP bm{}; GetObjectW(item.thumb,sizeof(bm),&bm); item.thumbW=bm.bmWidth; item.thumbH=bm.bmHeight; item.thumbFromPrivateCache=true;
                } else if(generated) {
                    RemoveGeneratedCacheFile(preferred);
                    if(preferred==item.uiCachePath) RemoveGeneratedCacheFile(BannerTimestampPath(item.uiCachePath));
                    item.thumbAttempted=false;
                    if(hwnd_) PostMessageW(hwnd_,WM_APP_CACHE_REPAIR,0,0);
                    cached=false;
                }
            }
            if(item.thumb) return item.thumb;
        }
        // Windows Media/Shell remains the final backup only while no valid VMP grid cache
        // exists. A last-moment cache completion must suppress the fallback immediately.
        if(LibraryPreviewMasterHealthy(item.uiCachePath)){
            if(item.thumb && !item.thumbFromPrivateCache){DeleteObject(item.thumb);item.thumb=nullptr;item.thumbW=item.thumbH=0;item.libraryGpuThumb.Reset();item.libraryGpuThumbSource=nullptr;item.libraryGpuGeneration=0;}
            return nullptr;
        }
        return TryShellCachedThumb(item,w,h);
    }

    HBITMAP GetDetailsBanner(MediaItem& item) {
        const std::wstring preferred=item.isVideo ? item.cachePath : item.path;
        if(item.isVideo && PathExistsNoThrow(preferred) && !CacheFileLooksHealthy(preferred,1024)){
            RemoveGeneratedCacheFile(preferred);
            RemoveGeneratedCacheFile(BannerTimestampPath(preferred));
            if(hwnd_) PostMessageW(hwnd_,WM_APP_CACHE_REPAIR,0,0);
            return nullptr;
        }
        if(!PathExistsNoThrow(preferred)) return nullptr;
        if(!item.isVideo && item.detailDecodeUnsupported) return nullptr;
        if(!item.detailThumb){
            item.detailThumb=LoadNativeBitmap(preferred);
            if(item.detailThumb){
                item.detailDecodeUnsupported=false;
                BITMAP bm{}; GetObjectW(item.detailThumb,sizeof(bm),&bm); item.detailThumbW=bm.bmWidth; item.detailThumbH=bm.bmHeight;
            } else if(item.isVideo) {
                RemoveGeneratedCacheFile(preferred);
                RemoveGeneratedCacheFile(BannerTimestampPath(preferred));
                if(hwnd_) PostMessageW(hwnd_,WM_APP_CACHE_REPAIR,0,0);
            } else {
                item.detailDecodeUnsupported=true;
                ShowInAppNotice(L"This media is unsupported.",5000);
            }
        }
        return item.detailThumb;
    }

    std::vector<size_t> DetailWindowIndices() const {
        const auto& items=CurrentItems();
        std::vector<size_t> indices;
        if(mode_!=Mode::Details || selected_>=items.size()) return indices;
        indices.push_back(selected_);
        size_t left=selected_,right=selected_;
        for(int distance=1;distance<=4;++distance){
            size_t next=0;
            if(FindAdjacentInSameFolder(items,right,1,next)){right=next;indices.push_back(right);}
            if(FindAdjacentInSameFolder(items,left,-1,next)){left=next;indices.push_back(left);}
        }
        return indices;
    }

    bool IsPathInDetailWindow(const std::wstring& path,const std::vector<size_t>& indices) const {
        const auto& items=CurrentItems();
        for(size_t idx:indices) if(idx<items.size() && items[idx].path==path) return true;
        return false;
    }

    void TrimDetailInfoToWindow(const std::vector<size_t>& indices) {
        const auto allowed=[this,&indices](const std::wstring& path){return IsPathInDetailWindow(path,indices);};
        auto trim=[&](std::vector<MediaItem>& list,bool activeList){
            for(auto& item:list){
                const bool keep=activeList && allowed(item.path);
                if(!keep && item.detailThumb){
                    DeleteObject(item.detailThumb); item.detailThumb=nullptr; item.detailThumbW=0; item.detailThumbH=0;
                    item.detailsGpuThumb.Reset(); item.detailsGpuThumbSource=nullptr; item.detailsGpuGeneration=0;
                }
            }
        };
        trim(videos_,category_==Category::Videos);
        trim(images_,category_==Category::Images);
        for(auto it=prefetchedPreviewSets_.begin();it!=prefetchedPreviewSets_.end();){
            if(!allowed(it->first)){
                DeletePreviewFrameBitmaps(it->second.frames);
                it=prefetchedPreviewSets_.erase(it);
            } else ++it;
        }
    }

    void EnsureDetailPrefetchWorker() {
        if(detailPrefetchThread_.joinable()) return;
        detailPrefetchStop_=false;
        detailPrefetchThread_=std::thread([this](){
            while(true){
                DetailPrefetchJob job;
                {
                    std::unique_lock<std::mutex> lock(detailPrefetchMutex_);
                    detailPrefetchCv_.wait(lock,[this]{return detailPrefetchStop_ || !detailPrefetchJobs_.empty();});
                    if(detailPrefetchStop_) break;
                    job=std::move(detailPrefetchJobs_.front());
                    detailPrefetchJobs_.erase(detailPrefetchJobs_.begin());
                }
                auto* result=new DetailPrefetchResult();
                result->generation=job.generation; result->category=job.category; result->index=job.index; result->mediaPath=job.mediaPath;
                const bool bannerFileReady=job.isVideo ? CacheFileLooksHealthy(job.bannerPath,1024) : FileHasData(fs::path(job.bannerPath));
                if(job.loadBanner && !job.bannerPath.empty() && bannerFileReady){
                    result->banner=LoadNativeBitmap(job.bannerPath);
                    if(result->banner){BITMAP bm{};GetObjectW(result->banner,sizeof(bm),&bm);result->bannerW=bm.bmWidth;result->bannerH=bm.bmHeight;}
                }
                if(job.loadPreviews && job.isVideo){
                    result->previewSet=LoadPrefetchedPreviewSet(job.previewDir);
                    result->hasPreviewSet=result->previewSet.duration>0.0;
                }
                if(job.generation!=detailPrefetchGeneration_.load(std::memory_order_acquire)){
                    if(result->banner) DeleteObject(result->banner);
                    DeletePreviewFrameBitmaps(result->previewSet.frames);
                    delete result; continue;
                }
                if(!hwnd_ || !PostMessageW(hwnd_,WM_APP_DETAIL_PREFETCH_READY,0,reinterpret_cast<LPARAM>(result))){
                    if(result->banner) DeleteObject(result->banner);
                    DeletePreviewFrameBitmaps(result->previewSet.frames);
                    delete result;
                }
            }
        });
    }

    void StopDetailPrefetchWorker() {
        {
            std::lock_guard<std::mutex> lock(detailPrefetchMutex_);
            detailPrefetchStop_=true;
            detailPrefetchJobs_.clear();
        }
        detailPrefetchGeneration_.fetch_add(1,std::memory_order_acq_rel);
        detailPrefetchCv_.notify_all();
        if(detailPrefetchThread_.joinable()) detailPrefetchThread_.join();
        detailPrefetchStop_=false;
    }

    void QueueDetailPrefetchWindow() {
        if(mode_!=Mode::Details) { CancelDetailPrefetchJobs(); return; }
        // Keep the existing nine-media RAM window for items the user has actually
        // visited, but never read banners/timelines for neighboring media in advance.
        // This makes an idle Info screen stop touching the media volume.
        const auto indices=DetailWindowIndices();
        if(!indices.empty()) TrimDetailInfoToWindow(indices);
        CancelDetailPrefetchJobs();
    }

    void MergePrefetchedFramesIntoActive(PrefetchedPreviewSet& set) {
        std::map<int,HBITMAP> incoming;
        for(auto& f:set.frames){if(f.bitmap){incoming[f.seconds]=f.bitmap;f.bitmap=nullptr;}}
        for(auto& active:previewFrames_){
            if(active.bitmap) continue;
            auto it=incoming.find(active.seconds);
            if(it!=incoming.end()){active.bitmap=it->second;active.lastUsed=GetTickCount64();incoming.erase(it);}
        }
        for(auto& kv:incoming) if(kv.second) DeleteObject(kv.second);
    }

    void HandleDetailPrefetchResult(DetailPrefetchResult* result) {
        if(!result) return;
        const auto cleanup=[&](){
            if(result->banner) DeleteObject(result->banner);
            DeletePreviewFrameBitmaps(result->previewSet.frames);
            delete result;
        };
        if(result->generation!=detailPrefetchGeneration_.load(std::memory_order_acquire) || mode_!=Mode::Details || result->category!=category_){cleanup();return;}
        auto& items=CurrentItems();
        if(result->index>=items.size() || items[result->index].path!=result->mediaPath){cleanup();return;}
        const auto indices=DetailWindowIndices();
        if(!IsPathInDetailWindow(result->mediaPath,indices)){cleanup();return;}
        auto& item=items[result->index];
        if(result->banner && !item.detailThumb){
            item.detailThumb=result->banner; item.detailThumbW=result->bannerW; item.detailThumbH=result->bannerH; result->banner=nullptr;
        }
        if(result->hasPreviewSet && item.isVideo){
            if(previewMediaPath_==result->mediaPath){
                MergePrefetchedFramesIntoActive(result->previewSet);
                if(result->previewSet.duration>0.0) detailsDurationSeconds_.store(result->previewSet.duration,std::memory_order_relaxed);
            } else {
                auto existing=prefetchedPreviewSets_.find(result->mediaPath);
                if(existing!=prefetchedPreviewSets_.end()){
                    DeletePreviewFrameBitmaps(existing->second.frames);
                    prefetchedPreviewSets_.erase(existing);
                }
                prefetchedPreviewSets_.emplace(result->mediaPath,std::move(result->previewSet));
            }
        }
        cleanup();
        TrimDetailInfoToWindow(indices);
        InvalidateRect(hwnd_,nullptr,FALSE);
    }

    static void DrawBitmapCover(HDC dc,HBITMAP bmp,RECT r) {
        if(!bmp) return; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,bmp); SetStretchBltMode(dc,HALFTONE);
        const double sx=static_cast<double>(r.right-r.left)/bm.bmWidth, sy=static_cast<double>(r.bottom-r.top)/bm.bmHeight;
        const double s=std::max(sx,sy); const int sw=std::max(1,static_cast<int>((r.right-r.left)/s)); const int sh=std::max(1,static_cast<int>((r.bottom-r.top)/s));
        const int srcx=(bm.bmWidth-sw)/2,srcy=(bm.bmHeight-sh)/2;
        StretchBlt(dc,r.left,r.top,r.right-r.left,r.bottom-r.top,mem,srcx,srcy,sw,sh,SRCCOPY); SelectObject(mem,old); DeleteDC(mem);
    }

    static void DrawBitmapCoverWithSourceDC(HDC dc,HDC sourceDc,HBITMAP bmp,RECT r) {
        if(!bmp || !sourceDc) return;
        BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        HGDIOBJ old=SelectObject(sourceDc,bmp);
        const double sx=static_cast<double>(r.right-r.left)/bm.bmWidth, sy=static_cast<double>(r.bottom-r.top)/bm.bmHeight;
        const double scale=std::max(sx,sy);
        const int sw=std::max(1,static_cast<int>((r.right-r.left)/scale));
        const int sh=std::max(1,static_cast<int>((r.bottom-r.top)/scale));
        const int srcx=(bm.bmWidth-sw)/2,srcy=(bm.bmHeight-sh)/2;
        StretchBlt(dc,r.left,r.top,r.right-r.left,r.bottom-r.top,sourceDc,srcx,srcy,sw,sh,SRCCOPY);
        SelectObject(sourceDc,old);
    }

    static int PushRoundedCardClip(HDC dc,RECT card,int radius) {
        if(!dc || card.right<=card.left || card.bottom<=card.top) return 0;
        const int saved=SaveDC(dc);
        if(saved==0) return 0;
        HRGN region=CreateRoundRectRgn(card.left,card.top,card.right+1,card.bottom+1,radius,radius);
        if(!region){RestoreDC(dc,saved);return 0;}
        const int result=ExtSelectClipRgn(dc,region,RGN_AND);
        DeleteObject(region);
        if(result==ERROR){RestoreDC(dc,saved);return 0;}
        return saved;
    }

    static void PopRoundedCardClip(HDC dc,int saved) {
        if(dc && saved!=0) RestoreDC(dc,saved);
    }

    static void DrawBitmapContain(HDC dc,HBITMAP bmp,RECT r) {
        if(!bmp) return; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,bmp); SetStretchBltMode(dc,HALFTONE);
        const double s=std::min(static_cast<double>(r.right-r.left)/bm.bmWidth,static_cast<double>(r.bottom-r.top)/bm.bmHeight);
        const int dw=std::max(1,static_cast<int>(bm.bmWidth*s)),dh=std::max(1,static_cast<int>(bm.bmHeight*s));
        const int dx=r.left+((r.right-r.left)-dw)/2,dy=r.top+((r.bottom-r.top)-dh)/2;
        StretchBlt(dc,dx,dy,dw,dh,mem,0,0,bm.bmWidth,bm.bmHeight,SRCCOPY); SelectObject(mem,old); DeleteDC(mem);
    }

    static void DrawBitmapContainAlpha(HDC dc,HBITMAP bmp,RECT r,BYTE alpha) {
        if(!bmp || alpha==0) return; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,bmp);
        const double scale=std::min(static_cast<double>(r.right-r.left)/bm.bmWidth,static_cast<double>(r.bottom-r.top)/bm.bmHeight);
        const int dw=std::max(1,static_cast<int>(bm.bmWidth*scale)),dh=std::max(1,static_cast<int>(bm.bmHeight*scale));
        const int dx=r.left+((r.right-r.left)-dw)/2,dy=r.top+((r.bottom-r.top)-dh)/2;
        BLENDFUNCTION bf{AC_SRC_OVER,0,alpha,0};
        AlphaBlend(dc,dx,dy,dw,dh,mem,0,0,bm.bmWidth,bm.bmHeight,bf);
        SelectObject(mem,old); DeleteDC(mem);
    }

    static void DrawBitmapContainNoUpscale(HDC dc,HBITMAP bmp,RECT r) {
        if(!bmp) return; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,bmp); SetStretchBltMode(dc,HALFTONE);
        const int dw=bm.bmWidth,dh=bm.bmHeight;
        const int dx=r.left+((r.right-r.left)-dw)/2,dy=r.top+((r.bottom-r.top)-dh)/2;
        const int saved=SaveDC(dc);IntersectClipRect(dc,r.left,r.top,r.right,r.bottom);
        BitBlt(dc,dx,dy,dw,dh,mem,0,0,SRCCOPY);RestoreDC(dc,saved);SelectObject(mem,old);DeleteDC(mem);
    }

    static void DrawBitmapContainAlphaNoUpscale(HDC dc,HBITMAP bmp,RECT r,BYTE alpha) {
        if(!bmp || alpha==0) return; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,bmp);
        const int dw=bm.bmWidth,dh=bm.bmHeight;
        const int dx=r.left+((r.right-r.left)-dw)/2,dy=r.top+((r.bottom-r.top)-dh)/2;
        BLENDFUNCTION bf{AC_SRC_OVER,0,alpha,0};
        const int saved=SaveDC(dc);IntersectClipRect(dc,r.left,r.top,r.right,r.bottom);
        AlphaBlend(dc,dx,dy,dw,dh,mem,0,0,bm.bmWidth,bm.bmHeight,bf);
        RestoreDC(dc,saved);SelectObject(mem,old);DeleteDC(mem);
    }

    static void DrawBitmapContainFitNoUpscale(HDC dc,HBITMAP bmp,RECT r) {
        if(!bmp) return; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,bmp); SetStretchBltMode(dc,HALFTONE);
        const int rw=std::max(1,static_cast<int>(r.right-r.left)),rh=std::max(1,static_cast<int>(r.bottom-r.top));
        const double scale=std::min(1.0,std::min(static_cast<double>(rw)/bm.bmWidth,static_cast<double>(rh)/bm.bmHeight));
        const int dw=std::max(1,static_cast<int>(std::lround(bm.bmWidth*scale))),dh=std::max(1,static_cast<int>(std::lround(bm.bmHeight*scale)));
        const int dx=r.left+(rw-dw)/2,dy=r.top+(rh-dh)/2;
        StretchBlt(dc,dx,dy,dw,dh,mem,0,0,bm.bmWidth,bm.bmHeight,SRCCOPY);
        SelectObject(mem,old);DeleteDC(mem);
    }

    static void DrawBitmapContainAlphaFitNoUpscale(HDC dc,HBITMAP bmp,RECT r,BYTE alpha) {
        if(!bmp || alpha==0) return; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,bmp);
        const int rw=std::max(1,static_cast<int>(r.right-r.left)),rh=std::max(1,static_cast<int>(r.bottom-r.top));
        const double scale=std::min(1.0,std::min(static_cast<double>(rw)/bm.bmWidth,static_cast<double>(rh)/bm.bmHeight));
        const int dw=std::max(1,static_cast<int>(std::lround(bm.bmWidth*scale))),dh=std::max(1,static_cast<int>(std::lround(bm.bmHeight*scale)));
        const int dx=r.left+(rw-dw)/2,dy=r.top+(rh-dh)/2;
        BLENDFUNCTION bf{AC_SRC_OVER,0,alpha,0};
        AlphaBlend(dc,dx,dy,dw,dh,mem,0,0,bm.bmWidth,bm.bmHeight,bf);
        SelectObject(mem,old);DeleteDC(mem);
    }

    static void DrawBitmapContainZoom(HDC dc,HBITMAP bmp,RECT r,float zoom,float centerU,float centerV) {
        if(!bmp) return; BITMAP bm{}; GetObjectW(bmp,sizeof(bm),&bm); if(!bm.bmWidth||!bm.bmHeight) return;
        const int rw=std::max(1,static_cast<int>(r.right-r.left)),rh=std::max(1,static_cast<int>(r.bottom-r.top));
        const double fit=std::min(static_cast<double>(rw)/bm.bmWidth,static_cast<double>(rh)/bm.bmHeight);
        const double scale=fit*std::clamp(zoom,0.25f,8.0f);
        const int dw=std::max(1,static_cast<int>(std::lround(bm.bmWidth*scale))),dh=std::max(1,static_cast<int>(std::lround(bm.bmHeight*scale)));
        const int cx=r.left+rw/2,cy=r.top+rh/2;
        const int dx=static_cast<int>(std::lround(static_cast<double>(cx)-static_cast<double>(centerU)*dw));
        const int dy=static_cast<int>(std::lround(static_cast<double>(cy)-static_cast<double>(centerV)*dh));
        HDC mem=CreateCompatibleDC(dc); HGDIOBJ old=SelectObject(mem,bmp); SetStretchBltMode(dc,HALFTONE);
        const int saved=SaveDC(dc); IntersectClipRect(dc,r.left,r.top,r.right,r.bottom);
        StretchBlt(dc,dx,dy,dw,dh,mem,0,0,bm.bmWidth,bm.bmHeight,SRCCOPY);
        RestoreDC(dc,saved); SelectObject(mem,old); DeleteDC(mem);
    }

    void ClearThumbs(std::vector<MediaItem>& list) {
        for(auto& v:list){ if(v.thumb) DeleteObject(v.thumb); if(v.detailThumb) DeleteObject(v.detailThumb); v.thumb=nullptr; v.detailThumb=nullptr; v.libraryGpuThumb.Reset(); v.libraryGpuThumbSource=nullptr; v.libraryGpuGeneration=0; v.detailsGpuThumb.Reset(); v.detailsGpuThumbSource=nullptr; v.detailsGpuGeneration=0; v.thumbW=v.thumbH=0; v.detailThumbW=v.detailThumbH=0; v.thumbLoadRequestEpoch=0; v.thumbNextLoadAttempt=0; }
    }

    void TrimThumbMemory() {
        const uint64_t processBytes=ProcessMemoryBytes();
        const ProcessMemoryPolicy policy=CurrentProcessMemoryPolicy();
        const vmp::SystemMemoryPressure systemPressure=CurrentSystemMemoryPressure();
        const DWORD gdiObjects=ProcessGdiObjectCount();
        const size_t targetHandles=LibraryBitmapHandleBudget();

        struct Ref{
            MediaItem* item;
            ULONGLONG tick;
            uint64_t bytes;
            bool desired;
            bool protectedNear;
            bool visible;
            bool playbackReturn;
        };
        std::vector<Ref> loaded;
        uint64_t totalBytes=0;
        auto collect=[&](std::vector<MediaItem>& list){
            for(auto& v:list){
                if(!v.thumb) continue;
                const uint64_t bytes=static_cast<uint64_t>(std::max(1,v.thumbW))*
                                     static_cast<uint64_t>(std::max(1,v.thumbH))*4ull;
                loaded.push_back({&v,v.thumbLastUsed,bytes,
                    libraryWorkingSetThumbPaths_.find(v.path)!=libraryWorkingSetThumbPaths_.end(),
                    protectedLibraryThumbPaths_.find(v.path)!=protectedLibraryThumbPaths_.end(),
                    visibleLibraryGpuThumbPaths_.find(v.path)!=visibleLibraryGpuThumbPaths_.end(),
                    playbackLibraryWarmPaths_.find(v.path)!=playbackLibraryWarmPaths_.end()});
                totalBytes+=bytes;
            }
        };
        collect(videos_);collect(images_);
        size_t loadedHandles=loaded.size();

        const bool softLibraryPressure=totalBytes>policy.libraryCpuSoftCeiling;
        const bool hardLibraryPressure=totalBytes>policy.libraryCpuHardCap;
        const bool systemElevated=systemPressure!=vmp::SystemMemoryPressure::Normal;
        const bool systemCritical=systemPressure==vmp::SystemMemoryPressure::Critical;
        const bool processElevated=processBytes>=policy.highPressure;
        const bool processCritical=processBytes>=policy.panicRelease;
        const bool forceEviction=processBytes>=policy.allocationGuard || hardLibraryPressure || systemCritical;
        const bool hardGdiPressure=gdiObjects>=kGdiObjectHardPressure;
        const bool handlePressure=loadedHandles>targetHandles || gdiObjects>=kGdiObjectSoftPressure;

        // Normal operation uses a real high/low watermark:
        //   32-GiB+ tier: keep everything <=10 GiB; crossing it trims cold LRU state to 8 GiB.
        // The lower tiers use the same shape with smaller but still useful warm targets.
        // Elevated machine/process pressure may trim excess above the warm target earlier.
        const bool bytePressure=
            softLibraryPressure ||
            forceEviction ||
            ((systemElevated || processElevated) && totalBytes>policy.libraryWarmTarget);
        if(!bytePressure && !handlePressure) return;

        uint64_t trimTo=vmp::LibraryTrimTarget(policy,systemPressure);
        if(processCritical) trimTo=std::min<uint64_t>(trimTo,policy.libraryCriticalTarget);

        // Evict cold LRU state first. Visible cards, the return-to-Library working set and
        // the nearby row neighborhood are last to go. Nearby state is broken only at a hard
        // process/cache/GDI boundary; ordinary soft trimming never destroys it first.
        std::sort(loaded.begin(),loaded.end(),[](const Ref&a,const Ref&b){
            if(a.visible!=b.visible) return !a.visible;
            if(a.playbackReturn!=b.playbackReturn) return !a.playbackReturn;
            if(a.protectedNear!=b.protectedNear) return !a.protectedNear;
            if(a.desired!=b.desired) return !a.desired;
            return a.tick<b.tick;
        });
        for(const auto& ref:loaded){
            if(totalBytes<=trimTo && loadedHandles<=targetHandles) break;
            auto* v=ref.item;
            if(ref.visible || ref.playbackReturn) continue;
            if(ref.protectedNear && !forceEviction && !hardGdiPressure) continue;
            if(v->thumb){
                DeleteObject(v->thumb);v->thumb=nullptr;
                v->libraryGpuThumb.Reset();v->libraryGpuThumbSource=nullptr;v->libraryGpuGeneration=0;
                v->thumbW=v->thumbH=0;v->thumbAttempted=false;v->thumbFromPrivateCache=false;
                v->thumbLoadRequestEpoch=0;v->thumbNextLoadAttempt=0;
                totalBytes=ref.bytes>=totalBytes?0:totalBytes-ref.bytes;
                if(loadedHandles>0)--loadedHandles;
            }
        }
    }

    std::vector<MediaItem>& CurrentItems() { return category_==Category::Videos?videos_:images_; }
    const std::vector<MediaItem>& CurrentItems() const { return category_==Category::Videos?videos_:images_; }

    bool PathEquals(const std::wstring& a, const std::wstring& b) const {
        return ToLower(fs::path(a).lexically_normal().wstring()) == ToLower(fs::path(b).lexically_normal().wstring());
    }

    std::wstring FolderViewKey(const std::wstring& folder) const {
        return ToLower(fs::path(folder).lexically_normal().wstring());
    }

    void SaveCurrentFolderViewState(const std::wstring& selectedPathOverride = L"") {
        if (currentFolder_.empty()) return;
        FolderViewState state;
        state.scrollY = std::max(0, scrollY_);
        state.selectedPath = selectedPathOverride;
        if (state.selectedPath.empty()) {
            const auto& list = CurrentItems();
            if (selected_ < list.size()) {
                if (list[selected_].parentFolderKey == FolderViewKey(currentFolder_)) state.selectedPath = list[selected_].path;
            }
        }
        folderViewStates_[FolderViewKey(currentFolder_)] = std::move(state);
    }

    void RestoreFolderViewState(const std::wstring& folder) {
        selected_ = 0;
        scrollY_ = 0;
        const auto it = folderViewStates_.find(FolderViewKey(folder));
        if (it == folderViewStates_.end()) { filterDirty_ = true; ClampScroll(); return; }
        const auto& list = CurrentItems();
        if (!it->second.selectedPath.empty()) {
            const std::wstring selectedKey = ToLower(fs::path(it->second.selectedPath).lexically_normal().wstring());
            const auto* candidates=CurrentFolderIndexedMedia();
            if(candidates){
                for(const size_t i:*candidates){
                    if(i<list.size() && ToLower(list[i].path)==selectedKey){ selected_=i; break; }
                }
            }else{
                for(size_t i=0;i<list.size();++i){
                    if(ToLower(list[i].path)==selectedKey){ selected_=i; break; }
                }
            }
        }
        scrollY_ = std::max(0, it->second.scrollY);
        filterDirty_ = true;
        ClampScroll();
    }

    bool IsAtLibraryRoot() const {
        if(externalMediaSession_) return true;
        return currentFolder_.empty() || PathEquals(currentFolder_, folder_);
    }

    bool IsAtChosenLibraryRoot() const {
        if(externalMediaSession_) return false;
        return !persistentFolder_.empty() && !currentFolder_.empty() && PathEquals(currentFolder_, persistentFolder_);
    }

    void RebuildLibraryFolderIndexCaches() {
        videoFolderIndices_.clear();
        imageFolderIndices_.clear();
        childFolderIndices_.clear();
        for(size_t i=0;i<videos_.size();++i)
            videoFolderIndices_[videos_[i].parentFolderKey].push_back(i);
        for(size_t i=0;i<images_.size();++i)
            imageFolderIndices_[images_[i].parentFolderKey].push_back(i);
        for(size_t i=0;i<folders_.size();++i)
            childFolderIndices_[folders_[i].parentFolderKey].push_back(i);
    }

    const std::vector<size_t>* CurrentFolderIndexedMedia() const {
        if(externalMediaSession_) return nullptr;
        const std::wstring key=FolderViewKey(currentFolder_);
        const auto& byFolder=category_==Category::Videos?videoFolderIndices_:imageFolderIndices_;
        const auto it=byFolder.find(key);
        return it==byFolder.end()?nullptr:&it->second;
    }

    size_t VisibleFolderCount() const {
        if(externalMediaSession_ || loadFailureFilterActive_ || !searchQuery_.empty()) return 0;
        const auto it=childFolderIndices_.find(FolderViewKey(currentFolder_));
        return it==childFolderIndices_.end()?0:it->second.size();
    }

    size_t CurrentFolderMediaCount() const {
        const auto& list=CurrentItems();
        if(loadFailureFilterActive_){
            size_t count=0;
            for(const auto& item:list){
                const std::wstring key=ToLower(item.path);
                if(loadFailureFilterPaths_.find(key)!=loadFailureFilterPaths_.end()) ++count;
            }
            return count;
        }
        if(externalMediaSession_) return list.size();
        const auto* indexed=CurrentFolderIndexedMedia();
        return indexed?indexed->size():0;
    }

    std::vector<size_t> VisibleFolderIndices() const {
        if(externalMediaSession_ || loadFailureFilterActive_ || !searchQuery_.empty()) return {};
        const auto it=childFolderIndices_.find(FolderViewKey(currentFolder_));
        if(it==childFolderIndices_.end()) return {};
        return it->second;
    }

    struct SearchMediaFilters {
        int minResolutionClass=0;
        bool requireVr=false;
        std::wstring text;
    };

    static SearchMediaFilters ParseSearchMediaFilters(const std::wstring& query) {
        SearchMediaFilters out;
        std::wstring token;
        auto flush=[&](){
            if(token.empty()) return;
            if(token==L"vr") out.requireVr=true;
            else if(token==L"4k") out.minResolutionClass=std::max(out.minResolutionClass,4);
            else if(token==L"5k") out.minResolutionClass=std::max(out.minResolutionClass,5);
            else if(token==L"8k") out.minResolutionClass=std::max(out.minResolutionClass,8);
            else { if(!out.text.empty()) out.text.push_back(L' '); out.text+=token; }
            token.clear();
        };
        for(wchar_t ch:ToLower(query)){
            if(iswspace(ch)) flush(); else token.push_back(ch);
        }
        flush();
        return out;
    }

    void QueuePriorityResolutionMetadataForSearch() {
        if(category_!=Category::Videos || searchQuery_.empty()) return;
        const SearchMediaFilters filters=ParseSearchMediaFilters(searchQuery_);
        if(filters.minResolutionClass==0) return;
        if(externalMediaSession_){
            for(auto& item:videos_){
                if(item.resolutionProbeAttempted || item.resolutionMetadataQueued) continue;
                item.resolutionMetadataQueued=true;
                QueueResolutionMetadata(item.path,item.uiCachePath,true);
            }
            return;
        }
        const auto it=videoFolderIndices_.find(FolderViewKey(currentFolder_));
        if(it==videoFolderIndices_.end()) return;
        for(const size_t i:it->second){
            if(i>=videos_.size()) continue;
            auto& item=videos_[i];
            if(item.resolutionProbeAttempted || item.resolutionMetadataQueued) continue;
            item.resolutionMetadataQueued=true;
            QueueResolutionMetadata(item.path,item.uiCachePath,true);
        }
    }

    const std::vector<size_t>& FilteredIndices() {
        if(!filterDirty_) return filteredIndices_;
        filteredIndices_.clear();
        const auto& list=CurrentItems();
        const SearchMediaFilters filters=ParseSearchMediaFilters(searchQuery_);

        // Normal folder navigation is indexed. This turns entering a folder from an
        // O(entire-library filesystem-normalization pass) into O(items-in-this-folder),
        // and the no-search case becomes a direct vector copy.
        const std::vector<size_t>* candidates=nullptr;
        if(!externalMediaSession_ && !loadFailureFilterActive_) candidates=CurrentFolderIndexedMedia();

        if(!loadFailureFilterActive_ && filters.text.empty() &&
           !filters.requireVr && filters.minResolutionClass==0){
            if(externalMediaSession_){
                filteredIndices_.resize(list.size());
                for(size_t i=0;i<list.size();++i) filteredIndices_[i]=i;
            }else if(candidates){
                filteredIndices_=*candidates;
            }
            filterDirty_=false;
            return filteredIndices_;
        }

        const size_t reserveCount=candidates?candidates->size():list.size();
        filteredIndices_.reserve(reserveCount);
        auto consider=[&](size_t i){
            if(i>=list.size()) return;
            const auto& item=list[i];
            if(loadFailureFilterActive_){
                if(loadFailureFilterPaths_.find(ToLower(item.path))==loadFailureFilterPaths_.end()) return;
                filteredIndices_.push_back(i);
                return;
            }
            if(filters.requireVr && (!item.isVideo || !item.vr.vr)) return;
            if(filters.minResolutionClass>0){
                if(!item.isVideo || !item.resolutionProbeAttempted) return;
                if(ResolutionBadgeClass(item)<filters.minResolutionClass) return;
            }
            if(!filters.text.empty()){
                bool textMatch=true; size_t start=0;
                while(start<filters.text.size()){
                    const size_t end=filters.text.find(L' ',start);
                    const std::wstring word=filters.text.substr(start,end==std::wstring::npos?std::wstring::npos:end-start);
                    if(!word.empty()){
                        const bool normalNameMatch=item.searchText.find(word)!=std::wstring::npos;
                        static const std::wstring favoriteSearchWords=L"favorite favorites favourite favourites";
                        const bool favoriteNameMatch=item.favorite && favoriteSearchWords.find(word)!=std::wstring::npos;
                        if(!normalNameMatch && !favoriteNameMatch){textMatch=false;break;}
                    }
                    if(end==std::wstring::npos) break;
                    start=end+1;
                }
                if(!textMatch) return;
            }
            filteredIndices_.push_back(i);
        };

        if(candidates){
            for(const size_t i:*candidates) consider(i);
        }else{
            for(size_t i=0;i<list.size();++i) consider(i);
        }

        if(!filters.text.empty() && filteredIndices_.size()>1){
            std::stable_sort(filteredIndices_.begin(),filteredIndices_.end(),[&](size_t ia,size_t ib){
                const auto& a=list[ia]; const auto& b=list[ib];
                const MediaNameSortKey ka=BuildSearchAwareMediaNameSortKey(a.title,filters.text);
                const MediaNameSortKey kb=BuildSearchAwareMediaNameSortKey(b.title,filters.text);
                if(ka.primary!=kb.primary) return ka.primary<kb.primary;
                if(ka.group!=kb.group) return ka.group<kb.group;
                if(ka.secondary!=kb.secondary) return ka.secondary<kb.secondary;
                if(ka.hasNumber!=kb.hasNumber) return !ka.hasNumber;
                if(ka.hasNumber){ const int cmp=CompareSortNumbers(ka.number,kb.number); if(cmp!=0) return cmp<0; }
                if(ka.fallback!=kb.fallback) return ka.fallback<kb.fallback;
                return ToLower(a.path)<ToLower(b.path);
            });
        }
        filterDirty_=false;
        return filteredIndices_;
    }

    bool HoveredLibraryMediaIndex(size_t& mediaIndex) const {
        mediaIndex=static_cast<size_t>(-1);
        if(mode_!=Mode::Library || !hwnd_) return false;
        POINT p{};
        if(!GetCursorPos(&p) || !ScreenToClient(hwnd_,&p)) return false;
        for(const auto& hit:libraryMediaHoverHits_){
            if(PtInRect(&hit.hit,p)){
                mediaIndex=hit.id;
                return true;
            }
        }
        return false;
    }

    RECT StandardBackRect(RECT rc) const {
        return RECT{20, rc.bottom - 51, 100, rc.bottom - 13};
    }

    void PaintFooterBackground(HDC dc, RECT rc) {
        const int footerH = 64;
        RECT footer{0, std::max<LONG>(0, rc.bottom - footerH), rc.right, rc.bottom};
        // One semi-opaque footer surface across the full width.  Hit-testing uses the
        // same full-width rectangle, so media underneath can never receive clicks.
        Gdiplus::Graphics g(dc);
        Gdiplus::SolidBrush fb(Gdiplus::Color(238,16,19,25));
        g.FillRectangle(&fb, footer.left, footer.top, footer.right-footer.left, footer.bottom-footer.top);
        HPEN line = CreatePen(PS_SOLID, 1, RGB(42,47,60)); HGDIOBJ oldPen = SelectObject(dc, line);
        MoveToEx(dc, 0, footer.top, nullptr); LineTo(dc, rc.right, footer.top); SelectObject(dc, oldPen); DeleteObject(line);
    }

    int LibraryMaxScroll(RECT rc) {
        const int clientWidth = std::max(1, static_cast<int>(rc.right - rc.left) - kLibraryScrollbarReserve);
        const int clientHeight = std::max(1, static_cast<int>(rc.bottom - rc.top));
        const int cardW = libraryCardWidth_;
        const int imageH = std::max(113, static_cast<int>(std::lround(static_cast<double>(cardW) * 9.0 / 16.0)));
        const int cardH = imageH + kLibraryTitleHeight;
        const int cols = std::max(1, (clientWidth - kLibraryPad * 2 + kLibraryGap) / (cardW + kLibraryGap));
        const auto& filtered = FilteredIndices();
        const size_t count = filtered.size() + VisibleFolderCount();
        const int rows = static_cast<int>((count + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols));
        const int total = kLibraryPad + rows * (cardH + kLibraryGap) + 86;
        return std::max(0, total - clientHeight);
    }

    void UpdateLibraryScrollbarRects(RECT rc) {
        libraryScrollTrackRect_ = RECT{};
        libraryScrollThumbRect_ = RECT{};
        if (mode_ != Mode::Library) return;
        const int maxScroll = LibraryMaxScroll(rc);
        libraryLastKnownMaxScroll_ = maxScroll;
        if (maxScroll <= 0) return;

        const int top = kLibraryPad;
        const int bottom = std::max(top + 1, static_cast<int>(rc.bottom) - 72);
        libraryScrollTrackRect_ = RECT{std::max<LONG>(0, rc.right - 12), top, std::max<LONG>(0, rc.right - 5), bottom};
        const int trackH = std::max(1, bottom - top);
        const int visibleContentH = std::max(1, static_cast<int>(rc.bottom - rc.top));
        const int contentH = visibleContentH + maxScroll;
        const int thumbH = std::clamp(static_cast<int>((static_cast<long long>(trackH) * visibleContentH) / std::max(1, contentH)), 44, trackH);
        const int travel = std::max(0, trackH - thumbH);
        const int thumbTop = top + (maxScroll > 0 ? static_cast<int>((static_cast<long long>(travel) * scrollY_) / maxScroll) : 0);
        libraryScrollThumbRect_ = RECT{rc.right - 13, thumbTop, rc.right - 4, thumbTop + thumbH};
    }

    void PaintLibraryScrollbar(HDC dc, RECT rc) {
        UpdateLibraryScrollbarRects(rc);
        if (IsRectEmpty(&libraryScrollTrackRect_) || IsRectEmpty(&libraryScrollThumbRect_)) return;
        FillRound(dc, libraryScrollTrackRect_, RGB(24, 28, 36), 5);
        FillRound(dc, libraryScrollThumbRect_, libraryScrollDragging_ ? RGB(145, 152, 166) : RGB(94, 101, 116), 6);
    }

    void PaintLibraryNavigator(HDC dc, RECT rc) {
        PaintFooterBackground(dc, rc);
        const int footerTop = std::max(0, static_cast<int>(rc.bottom) - 64);
        libraryFooterRect_ = RECT{0, footerTop, rc.right, rc.bottom};
        const int buttonTop = footerTop + 13;
        const int buttonBottom = rc.bottom - 13;
        // Match the video-player control geometry exactly: 48 px icons, 20 px right
        // margin and 10 px bottom margin.
        constexpr int iconW=48;
        constexpr int iconGap=10;
        constexpr int groupGap=24;
        constexpr int rightMargin=20;
        const int iconButtonBottom=rc.bottom-10;
        const int iconButtonTop=iconButtonBottom-iconW;

        // Left side: optional folder Back, then one persistent Videos/Images toggle.
        int navLeft = 20;
        if (!IsAtLibraryRoot()) {
            backRect_ = {20, iconButtonTop, 20+iconW, iconButtonBottom};
            DrawSvgIconButton(dc,backRect_,backSvgBitmap_.get(),false,5);
            navLeft = backRect_.right + 10;
        } else {
            backRect_ = RECT{};
        }
        categoryToggleRect_ = {navLeft, iconButtonTop, navLeft + iconW, iconButtonBottom};
        DrawSvgIconButton(dc,categoryToggleRect_,category_==Category::Videos?videosSvgBitmap_.get():imageSvgBitmap_.get(),false,5);
        const std::wstring countText=L"("+std::to_wstring(CurrentFolderMediaCount())+L")";
        mediaCountRect_={categoryToggleRect_.right+10,buttonTop,categoryToggleRect_.right+110,buttonBottom};
        DrawTextSimple(dc,countText,mediaCountRect_,14,FW_SEMIBOLD,RGB(175,181,194),DT_LEFT|DT_VCENTER|DT_SINGLELINE);

        // Right group: identical spacing to video playback.
        int cursor=std::max(0,static_cast<int>(rc.right)-rightMargin);
        libraryFullRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};
        DrawFullscreenButton(dc,libraryFullRect_);
        cursor=libraryFullRect_.left-iconGap;

        // Never offer a slideshow for an empty Images folder.
        const bool showSlideshow=(category_==Category::Images && CurrentFolderMediaCount()>0);
        if(showSlideshow){
            slideshowRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};
            DrawAutoAdvanceIcon(dc,slideshowRect_,slideshowActive_);
            cursor=slideshowRect_.left-iconGap;
        }else{
            slideshowRect_=RECT{};
        }

        // Root-only maintenance group.  Left-to-right: Download, Refresh, Folder,
        // then a deliberate larger gap before slideshow/fullscreen.
        const bool showRootMaintenance=IsAtChosenLibraryRoot();
        const bool showChooseOnly=!showRootMaintenance && (currentFolder_.empty() || externalMediaSession_);
        if(showRootMaintenance){
            cursor-=groupGap-iconGap;
            chooseRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};
            cursor=chooseRect_.left-iconGap;
            rescanRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};
            cursor=rescanRect_.left-iconGap;
            loadEverythingRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};
            DrawFolderIconButton(dc,chooseRect_);
            DrawRefreshIconButton(dc,rescanRect_);
            DrawDownloadIconButton(dc,loadEverythingRect_);
        }else if(showChooseOnly){
            rescanRect_=RECT{};
            loadEverythingRect_=RECT{};
            cursor-=groupGap-iconGap;
            chooseRect_={cursor-iconW,iconButtonTop,cursor,iconButtonBottom};
            DrawFolderIconButton(dc,chooseRect_);
        }else{
            chooseRect_=RECT{};
            rescanRect_=RECT{};
            loadEverythingRect_=RECT{};
        }
    }

    void DrawFolderCard(HDC dc, const LibraryFolder& folder, RECT card) {
        FillRound(dc, card, RGB(31,35,46), 12);
        const int imageH = std::max(113, static_cast<int>(std::lround(static_cast<double>(card.right - card.left) * 9.0 / 16.0)));
        RECT image = card; image.bottom = image.top + imageH;
        HBRUSH bg = CreateSolidBrush(RGB(37,42,54)); FillRect(dc, &image, bg); DeleteObject(bg);

        const int iconW = 118, iconH = 82;
        const int cx = (image.left + image.right) / 2;
        const int cy = (image.top + image.bottom) / 2 + 4;
        RECT body{cx - iconW/2, cy - iconH/2 + 12, cx + iconW/2, cy + iconH/2};
        RECT tab{body.left + 8, body.top - 17, body.left + 54, body.top + 5};
        FillRound(dc, tab, RGB(210,170,73), 7);
        FillRound(dc, body, RGB(225,184,82), 10);

        RECT title{card.left+10,image.bottom+2,card.right-10,card.bottom-3};
        DrawTextSimple(dc,folder.name,title,14,FW_SEMIBOLD);
    }

    void PaintLibrary(HDC dc, RECT rc) {
        libraryMediaHoverHits_.clear();
        libraryReturnHighlightRect_=RECT{};
        visibleLibraryGpuThumbPaths_.clear();
        auto& mutableList=CurrentItems();
        const auto& filtered=FilteredIndices();
        const auto visibleFolders=VisibleFolderIndices();
        const size_t totalCards=visibleFolders.size()+filtered.size();
        // GDI fallback follows the same render-only contract as the Direct2D path.
        // Loading/prefetch is driven by timers, never by WM_PAINT.
        if(totalCards==0){
            std::wstring msg;
            if(libraryScanRunning_) msg=L"Scanning library...";
            else if(folder_.empty() || currentFolder_.empty()) msg=L"Choose a folder to load videos and images.";
            else if(loadFailureFilterActive_) msg=category_==Category::Videos?L"No failed videos.":L"No failed images.";
            else if(!searchQuery_.empty()) msg=L"No matching media.";
            else msg=category_==Category::Videos?L"No videos or subfolders here.":L"No images or subfolders here.";
            if(!msg.empty()){
                RECT mr{40,40,rc.right-40,128};
                DrawTextSimple(dc,msg,mr,25,FW_SEMIBOLD,RGB(180,185,197),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            }
            PaintLibraryNavigator(dc,rc);
            if(searchVisible_) PaintLibrarySearch(dc,rc);
            return;
        }

        const int pad=kLibraryPad,gap=kLibraryGap,cardW=libraryCardWidth_;
        const int imageH=std::max(113,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int cardH=imageH+kLibraryTitleHeight;
        const int rowStride=cardH+gap;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left)-kLibraryScrollbarReserve);
        const int cols=std::max(1,(clientWidth-pad*2+gap)/(cardW+gap));
        const int rows=static_cast<int>((totalCards+static_cast<size_t>(cols)-1)/static_cast<size_t>(cols));
        const int startY=pad-scrollY_;
        const int visibleBottom=std::max(pad,static_cast<int>(rc.bottom)-68);

        // Paint only rows that can intersect the viewport. The previous implementation
        // walked every card in the folder on every wheel tick and rejected most of them.
        const int firstVisibleRow=std::clamp(scrollY_/std::max(1,rowStride)-1,0,std::max(0,rows-1));
        const int lastVisibleRow=std::clamp((scrollY_+std::max(0,visibleBottom-pad))/std::max(1,rowStride)+1,
                                            0,std::max(0,rows-1));
        const size_t firstDisplay=static_cast<size_t>(firstVisibleRow)*static_cast<size_t>(cols);
        const size_t lastDisplay=std::min(totalCards,static_cast<size_t>(lastVisibleRow+1)*static_cast<size_t>(cols));

        // Re-evaluate media hover from the real cursor against the NEW card geometry
        // before drawing. Wheel scrolling can move cards underneath a stationary mouse,
        // so WM_MOUSEMOVE alone is not enough to keep the highlight attached correctly.
        POINT libraryCursor{};
        bool libraryCursorValid=GetCursorPos(&libraryCursor)!=FALSE && ScreenToClient(hwnd_,&libraryCursor)!=FALSE;
        bool libraryHoverFound=false;
        size_t libraryHoverId=static_cast<size_t>(-1);
        RECT libraryHoverRect{};
        if(libraryCursorValid){
            RECT mediaViewport{0,pad,rc.right,visibleBottom};
            for(size_t displayIndex=firstDisplay;displayIndex<lastDisplay;++displayIndex){
                if(displayIndex<visibleFolders.size()) continue;
                const int col=static_cast<int>(displayIndex)%cols,row=static_cast<int>(displayIndex)/cols;
                RECT card{pad+col*(cardW+gap),startY+row*rowStride,pad+col*(cardW+gap)+cardW,startY+row*rowStride+cardH};
                if(card.bottom<pad||card.top>visibleBottom) continue;
                RECT mediaHit{};
                if(!IntersectRect(&mediaHit,&card,&mediaViewport) || !PtInRect(&mediaHit,libraryCursor)) continue;
                const size_t mediaDisplayIndex=displayIndex-visibleFolders.size();
                if(mediaDisplayIndex>=filtered.size()) continue;
                libraryHoverFound=true;
                libraryHoverId=filtered[mediaDisplayIndex];
                libraryHoverRect=card;
                break;
            }
        }
        SetMediaHoverTarget(MediaHoverSurface::Library,libraryHoverId,libraryHoverRect,libraryHoverFound);

        HDC libraryImageDc=CreateCompatibleDC(dc);
        SetStretchBltMode(dc,HALFTONE);

        for(size_t displayIndex=firstDisplay;displayIndex<lastDisplay;++displayIndex){
            const int col=static_cast<int>(displayIndex)%cols,row=static_cast<int>(displayIndex)/cols;
            RECT card{pad+col*(cardW+gap),startY+row*rowStride,pad+col*(cardW+gap)+cardW,startY+row*rowStride+cardH};
            if(card.bottom<pad||card.top>visibleBottom) continue;

            if(displayIndex<visibleFolders.size()) {
                if(RectVisible(dc,&card)) DrawFolderCard(dc, folders_[visibleFolders[displayIndex]], card);
                continue;
            }

            const size_t mediaDisplayIndex=displayIndex-visibleFolders.size();
            const size_t i=filtered[mediaDisplayIndex];
            RECT mediaViewport{0,pad,rc.right,visibleBottom};
            RECT mediaHit{};
            if(IntersectRect(&mediaHit,&card,&mediaViewport)) libraryMediaHoverHits_.push_back({mediaHit,card,i});
            // Visibility protection and return-highlight geometry are state, not paint work;
            // keep them current even when this card lies outside the current dirty region.
            protectedLibraryThumbPaths_.insert(mutableList[i].path);
            visibleLibraryGpuThumbPaths_.insert(mutableList[i].path);
            const float returnAmount=LibraryReturnHighlightAmount(i);
            if(returnAmount>0.0f) libraryReturnHighlightRect_=card;
            if(!RectVisible(dc,&card)) continue;

            FillRound(dc,card,RGB(31,35,46),12);
            RECT image=card; image.bottom=image.top+imageH;
            const int imageClip=PushRoundedCardClip(dc,card,12);
            if(LibraryHoverPreviewFrame* hoverFrame=ActiveLibraryHoverPreviewFrame(i)){
                if(hoverFrame->bitmap){ if(libraryImageDc) DrawBitmapCoverWithSourceDC(dc,libraryImageDc,hoverFrame->bitmap,image); else DrawBitmapCover(dc,hoverFrame->bitmap,image); }
                else { HBRUSH pb=CreateSolidBrush(RGB(43,48,61)); FillRect(dc,&image,pb); DeleteObject(pb); }
            }else{
                HBITMAP bmp=GetResidentLibraryItemThumb(mutableList[i]);
                if(bmp){ if(libraryImageDc) DrawBitmapCoverWithSourceDC(dc,libraryImageDc,bmp,image); else DrawBitmapCover(dc,bmp,image); }
                else { HBRUSH pb=CreateSolidBrush(RGB(43,48,61)); FillRect(dc,&image,pb); DeleteObject(pb); }
            }
            PopRoundedCardClip(dc,imageClip);
            RECT title{card.left+10,image.bottom+2,card.right-10,card.bottom-3}; DrawTextSimple(dc,mutableList[i].title,title,14,FW_SEMIBOLD);
            if(mutableList[i].favorite) DrawFavoriteBadge(dc,image);
            if(mutableList[i].isVideo){
                int badgeRight=card.right-8;
                if(mutableList[i].vr.vr){
                    RECT vrTag{badgeRight-28,card.top+8,badgeRight,card.top+36};
                    if(vrBadgeWhiteBitmap_) DrawBitmapCentered(dc,vrTag,vrBadgeWhiteBitmap_.get(),0,0);
                    else { FillRound(dc,vrTag,RGB(16,19,25),8); DrawTextSimple(dc,L"VR",vrTag,11,FW_BOLD,RGB(220,225,235),DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
                    badgeRight=vrTag.left-6;
                }
                const int resolutionClass=ResolutionBadgeClass(mutableList[i]);
                if(resolutionClass){
                    RECT resolutionTag{badgeRight-28,card.top+8,badgeRight,card.top+36};
                    if(Gdiplus::Bitmap* resolutionIcon=ResolutionBadgeBitmap(mutableList[i])) DrawBitmapCentered(dc,resolutionTag,resolutionIcon,0,0);
                    else { FillRound(dc,resolutionTag,RGB(16,19,25),8); DrawTextSimple(dc,std::to_wstring(resolutionClass)+L"K",resolutionTag,10,FW_BOLD,RGB(230,234,242),DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
                }
            }
            DrawMediaHoverBorder(dc,card,std::max(MediaHoverAmount(MediaHoverSurface::Library,i,card),returnAmount),12);
        }
        if(libraryImageDc) DeleteDC(libraryImageDc);

        // Keep the top spacing fixed while the library content scrolls underneath it.
        // This is a permanent 20px mask matching the left/right library padding; it is
        // not a header and contains no text or controls.
        RECT topMask{0,0,rc.right,kLibraryPad};
        HBRUSH topMaskBrush=CreateSolidBrush(RGB(13,15,20));
        FillRect(dc,&topMask,topMaskBrush);
        DeleteObject(topMaskBrush);

        PaintLibraryScrollbar(dc,rc);
        PaintLibraryNavigator(dc,rc);
        if(searchVisible_) PaintLibrarySearch(dc,rc);
        playbackLibraryWarmPaths_.clear();
    }

    void PaintLibrarySearch(HDC dc, RECT rc) {
        const int right=std::max(20,static_cast<int>(rc.right)-20);
        const int width=std::min(430,std::max(220,static_cast<int>(rc.right)/3));
        searchBoxRect_={right-width,12,right,52};
        FillRound(dc,searchBoxRect_,RGB(31,35,46),11);
        RECT text{searchBoxRect_.left+14,searchBoxRect_.top,searchBoxRect_.right-14,searchBoxRect_.bottom};
        const std::wstring shown=loadFailureFilterActive_ ? (L"Load failures ("+std::to_wstring(loadFailureFilterTotal_)+L")") : (searchQuery_.empty()?L"Search...":searchQuery_);
        if(searchSelectAll_ && !searchQuery_.empty()) {
            HFONT f=GetFont(16,FW_SEMIBOLD); HGDIOBJ oldFont=SelectObject(dc,f);
            SIZE sz{}; GetTextExtentPoint32W(dc,searchQuery_.c_str(),static_cast<int>(searchQuery_.size()),&sz);
            SelectObject(dc,oldFont);
            RECT selected{text.left-3,text.top+8,std::min<LONG>(text.right,text.left+sz.cx+5),text.bottom-8};
            FillRound(dc,selected,RGB(72,82,105),5);
        }
        const bool muted=!loadFailureFilterActive_&&searchQuery_.empty();
        DrawTextSimple(dc,shown,text,16,FW_SEMIBOLD,muted?RGB(145,151,164):RGB(244,246,250));
    }

    void PaintDetails(HDC dc, RECT rc) {
        previewHitRects_.clear();
        previewMediaHoverHits_.clear();
        previewZoomRect_ = RECT{0,0,0,0};
        detailsMediaRect_ = RECT{};
        timelineReturnHighlightRect_ = RECT{};
        auto& list=CurrentItems(); if(selected_>=list.size()) return; MediaItem& item=list[selected_];
        ClampDetailsScroll();
        const int footerTop=std::max(0,static_cast<int>(rc.bottom)-64);
        const int contentOffset=detailsScrollY_;
        int y=18-contentOffset;
        RECT title{40,y,rc.right-40,y+42}; DrawTextSimple(dc,item.title,title,30,FW_BOLD); y+=54;

        const int heroH=DetailsHeroHeightForViewport(static_cast<int>(rc.right-rc.left),footerTop,item.isVideo);
        RECT media{40,y,rc.right-40,y+heroH};
        if(!item.isVideo && nativeImageSizing_) media=RECT{0,0,rc.right,rc.bottom};
        detailsMediaRect_=media;
        if(media.bottom>0 && media.top<footerTop){
            HBRUSH b=CreateSolidBrush(RGB(20,23,31)); FillRect(dc,&media,b); DeleteObject(b);
            const int reqW=std::min(2560,std::max(1,static_cast<int>(media.right-media.left)));
            const int reqH=std::min(1440,std::max(1,static_cast<int>(media.bottom-media.top)));
            HBITMAP bmp=nullptr;
            if(item.isVideo && !PathExistsNoThrow(item.cachePath)) bmp=GetItemThumb(item,640,360);
            else bmp=GetDetailsBanner(item);
            if(bmp){
                if(item.isVideo) {
                    // Info-screen video banner: enlarge proportionally to fill the hero
                    // area. Aspect ratio is preserved; excess edges are cropped rather than stretched.
                    DrawBitmapCover(dc,bmp,media);
                } else if(slideshowFadeActive_ && slideshowPreviousIndex_ < images_.size()) {
                    HBITMAP previous=nativeImageSizing_ ? GetDetailsBanner(images_[slideshowPreviousIndex_]) : GetItemThumb(images_[slideshowPreviousIndex_],reqW,reqH);
                    const float progress=EaseUi(static_cast<float>(GetTickCount64()-slideshowFadeStart_) / static_cast<float>(kUiAnimationDurationMs));
                    if(previous){
                        if(nativeImageSizing_ && fullscreen_) DrawBitmapContainFitNoUpscale(dc,previous,media);
                        else if(nativeImageSizing_) DrawBitmapContainNoUpscale(dc,previous,media);
                        else DrawBitmapContain(dc,previous,media);
                    }
                    if(nativeImageSizing_ && fullscreen_) DrawBitmapContainAlphaFitNoUpscale(dc,bmp,media,static_cast<BYTE>(std::clamp<int>(static_cast<int>(std::lround(progress*255.0f)),0,255)));
                    else if(nativeImageSizing_) DrawBitmapContainAlphaNoUpscale(dc,bmp,media,static_cast<BYTE>(std::clamp<int>(static_cast<int>(std::lround(progress*255.0f)),0,255)));
                    else DrawBitmapContainAlpha(dc,bmp,media,static_cast<BYTE>(std::clamp<int>(static_cast<int>(std::lround(progress*255.0f)),0,255)));
                } else {
                    // Still images keep their original aspect ratio in the Info view.
                    // Do not use the video-banner cover/crop behavior here.
                    if(nativeImageSizing_ && fullscreen_) DrawBitmapContainFitNoUpscale(dc,bmp,media);
                    else if(nativeImageSizing_) DrawBitmapContainNoUpscale(dc,bmp,media);
                    else if(ImageZoomActive()) DrawBitmapContainZoom(dc,bmp,media,imageZoomScale_,imageZoomCenterU_,imageZoomCenterV_);
                    else DrawBitmapContain(dc,bmp,media);
                }
                if(item.isVideo){
                    int badgeRight=media.right-10;
                    const int badgeTop=media.top+10;
                    const int badgeSize=30;
                    if(item.vr.vr){
                        RECT vrTag{badgeRight-badgeSize,badgeTop,badgeRight,badgeTop+badgeSize};
                        if(vrBadgeWhiteBitmap_) DrawBitmapCentered(dc,vrTag,vrBadgeWhiteBitmap_.get(),0,0);
                        else { FillRound(dc,vrTag,RGB(16,19,25),8); DrawTextSimple(dc,L"VR",vrTag,11,FW_BOLD,RGB(220,225,235),DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
                        badgeRight=vrTag.left-6;
                    }
                    const int resolutionClass=ResolutionBadgeClass(item);
                    if(resolutionClass){
                        RECT resolutionTag{badgeRight-badgeSize,badgeTop,badgeRight,badgeTop+badgeSize};
                        if(Gdiplus::Bitmap* resolutionIcon=ResolutionBadgeBitmap(item)) DrawBitmapCentered(dc,resolutionTag,resolutionIcon,0,0);
                        else { FillRound(dc,resolutionTag,RGB(16,19,25),8); DrawTextSimple(dc,std::to_wstring(resolutionClass)+L"K",resolutionTag,10,FW_BOLD,RGB(230,234,242),DT_CENTER|DT_VCENTER|DT_SINGLELINE); }
                    }
                }
            }
            if(item.favorite) DrawFavoriteBadge(dc,media);
        }
        y+=heroH+22;

        if(item.isVideo){
            // The whole visible secondary-preview section is a zoom target, including the
            // loading/empty state and the gaps between cards. This avoids wheel zoom becoming
            // unavailable while previews are still arriving from the background worker.
            const int zoomTop = std::max(0, y);
            if (zoomTop < footerTop && rc.right > 80)
                previewZoomRect_ = RECT{40, zoomTop, rc.right-40, footerTop};

            const int gap=12;
            const int cardW=DetailsPreviewCardWidthForViewport(static_cast<int>(rc.right - rc.left));
            const int imageH=std::max(79,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
            const int labelH=24;
            const int cardH=imageH+labelH;
            const int availW=std::max(1,static_cast<int>(rc.right)-80);
            const int cols=std::max(1,(availW+gap)/(cardW+gap));

            if(previewFrames_.empty()){
                SetMediaHoverTarget(MediaHoverSurface::Preview,static_cast<size_t>(-1),RECT{},false);
                RECT note{40,y,rc.right-40,y+54};
                const bool complete=!previewDir_.empty()&&PreviewCacheIsComplete();
                DrawTextSimple(dc,complete?L"No secondary previews were available for this video.":L"Loading Timeline",note,14,FW_NORMAL,RGB(160,167,180));
                y+=64;
            } else {
                const int rows=static_cast<int>((previewFrames_.size()+static_cast<size_t>(cols)-1)/static_cast<size_t>(cols));
                const int rowStride=cardH+gap;
                const int startRow=std::max(0, ((0 - y) / std::max(1,rowStride)) - 1);
                const int endRow=std::min(rows-1, ((footerTop - y) / std::max(1,rowStride)) + 1);

                POINT previewCursor{};
                const bool previewCursorValid=GetCursorPos(&previewCursor)!=FALSE && ScreenToClient(hwnd_,&previewCursor)!=FALSE;
                bool previewHoverFound=false;
                size_t previewHoverId=static_cast<size_t>(-1);
                RECT previewHoverRect{};
                if(previewCursorValid){
                    RECT previewViewport{0,0,rc.right,footerTop};
                    for(int row=startRow;row<=endRow && !previewHoverFound;++row){
                        for(int col=0;col<cols;++col){
                            const size_t i=static_cast<size_t>(row)*static_cast<size_t>(cols)+static_cast<size_t>(col);
                            if(i>=previewFrames_.size()) break;
                            RECT card{40+col*(cardW+gap),y+row*(cardH+gap),40+col*(cardW+gap)+cardW,y+row*(cardH+gap)+cardH};
                            if(card.bottom<0||card.top>footerTop) continue;
                            RECT previewHit{};
                            if(IntersectRect(&previewHit,&card,&previewViewport) && PtInRect(&previewHit,previewCursor)){
                                previewHoverFound=true;
                                previewHoverId=i;
                                previewHoverRect=card;
                                break;
                            }
                        }
                    }
                }
                SetMediaHoverTarget(MediaHoverSurface::Preview,previewHoverId,previewHoverRect,previewHoverFound);

                for(int row=startRow; row<=endRow; ++row){
                    for(int col=0; col<cols; ++col){
                        const size_t i=static_cast<size_t>(row)*static_cast<size_t>(cols)+static_cast<size_t>(col);
                        if(i>=previewFrames_.size()) break;
                        RECT card{40+col*(cardW+gap),y+row*(cardH+gap),40+col*(cardW+gap)+cardW,y+row*(cardH+gap)+cardH};
                        if(card.bottom<0||card.top>footerTop) continue;
                        // Keep preview input strictly inside the scrollable content area.
                        // A card can be partially visible behind the fixed footer, but that hidden
                        // portion must never remain clickable through the footer controls.
                        RECT previewHit{};
                        RECT previewViewport{0,0,rc.right,footerTop};
                        if (IntersectRect(&previewHit, &card, &previewViewport)) {
                            previewHitRects_.push_back({previewHit, previewFrames_[i].seekSeconds});
                            previewMediaHoverHits_.push_back({previewHit,card,i});
                        }
                        if(!RectVisible(dc,&card)) continue;
                        FillRound(dc,card,RGB(28,32,42),9);
                        RECT image=card; image.bottom=image.top+imageH;
                        const int imageClip=PushRoundedCardClip(dc,card,9);
                        if(LibraryHoverPreviewFrame* hoverFrame=ActiveTimelineHoverPreviewFrame(i)){
                            if(hoverFrame->bitmap) DrawBitmapCover(dc,hoverFrame->bitmap,image);
                            else { HBRUSH ph=CreateSolidBrush(RGB(43,48,61)); FillRect(dc,&image,ph); DeleteObject(ph); }
                        }else{
                            HBITMAP pbmp=GetPreviewBitmap(previewFrames_[i],cardW,imageH);
                            if(pbmp) DrawBitmapCover(dc,pbmp,image);
                            else { HBRUSH ph=CreateSolidBrush(RGB(43,48,61)); FillRect(dc,&image,ph); DeleteObject(ph); }
                        }
                        PopRoundedCardClip(dc,imageClip);
                        RECT label{card.left+8,image.bottom,card.right-8,card.bottom};
                        DrawTextSimple(dc,PreviewLabel(previewFrames_[i].seconds),label,11,FW_SEMIBOLD,RGB(200,206,218),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                        const float returnAmount=TimelineReturnHighlightAmount(i);
                        if(returnAmount>0.0f) timelineReturnHighlightRect_=card;
                        DrawMediaHoverBorder(dc,card,std::max(MediaHoverAmount(MediaHoverSurface::Preview,i,card),returnAmount),9);
                    }
                }
                y+=rows*(cardH+gap)+10;
                TrimPreviewMemory();
            }
        }

        detailsContentBottom_=y+20+contentOffset;

        // Previous / next media controls stay fixed at the screen edges while the
        // open-media view scrolls underneath them. Navigation is restricted to the
        // same media type and exact current folder.
        constexpr int edgeW=48, edgeH=76, edgePad=16;
        const int edgeCenterY=footerTop/2;
        const int edgeTop=std::max(8,edgeCenterY-edgeH/2);
        detailsPrevRect_={edgePad,edgeTop,edgePad+edgeW,edgeTop+edgeH};
        detailsNextRect_={std::max(edgePad,static_cast<int>(rc.right)-edgePad-edgeW),edgeTop,std::max(edgePad+edgeW,static_cast<int>(rc.right)-edgePad),edgeTop+edgeH};
        DrawEdgeArrowButton(dc,detailsPrevRect_,false,CanNavigateDetailsMedia(-1));
        DrawEdgeArrowButton(dc,detailsNextRect_,true,CanNavigateDetailsMedia(1));

        // Info footer is a fixed top interaction layer.  It may visually sit over
        // scrolling preview cards, but nothing underneath it is allowed to receive clicks.
        detailsFooterRect_ = RECT{0, footerTop, rc.right, rc.bottom};
        PaintFooterBackground(dc, rc);
        backRect_={20,rc.bottom-58,68,rc.bottom-10}; DrawSvgIconButton(dc,backRect_,backSvgBitmap_.get(),false,5);
        if(item.isVideo){
            imageDetailsSlideshowRect_=RECT{};
            const int gap=10;
            playRect_={backRect_.right+gap,backRect_.top,backRect_.right+gap+48,backRect_.bottom};
            DrawSvgIconButton(dc,playRect_,playSvgBitmap_.get(),false,6);
        } else {
            playRect_=RECT{};
        }

        constexpr int footerIconW=48, footerGap=10, footerRightMargin=20;
        const int footerRight=std::max(0,static_cast<int>(rc.right)-footerRightMargin);
        const int detailsIconBottom=rc.bottom-10;
        const int detailsIconTop=detailsIconBottom-footerIconW;
        detailsFullRect_={footerRight-footerIconW,detailsIconTop,footerRight,detailsIconBottom};
        DrawFullscreenButton(dc,detailsFullRect_);
        imageDetailsNativeRect_=RECT{};
        if(!item.isVideo){
            imageDetailsNativeRect_={detailsFullRect_.left-footerGap-footerIconW,detailsIconTop,detailsFullRect_.left-footerGap,detailsIconBottom};
            DrawNativeSizeButton(dc,imageDetailsNativeRect_);
            imageDetailsSlideshowRect_={imageDetailsNativeRect_.left-footerGap-footerIconW,detailsIconTop,imageDetailsNativeRect_.left-footerGap,detailsIconBottom};
            DrawAutoAdvanceIcon(dc,imageDetailsSlideshowRect_,slideshowActive_);
        }
        std::wstring meta=item.isVideo?(item.vr.vr?(item.vr.projection==2?L"VR180":L"VR"):L"Video"):L"Image";
        if(item.isVideo && item.sourceWidth && item.sourceHeight){
            meta += L"  \u2022  ";
            meta += std::to_wstring(item.sourceWidth);
            meta += L"\u00D7";
            meta += std::to_wstring(item.sourceHeight);
        }
        const LONG metaLeftLimit=item.isVideo?playRect_.right+20:backRect_.right+20;
        const LONG metaRightLimit=(item.isVideo?detailsFullRect_.left:imageDetailsSlideshowRect_.left)-20;
        const LONG metaLeft=std::max<LONG>(metaLeftLimit,rc.right/2-150);
        const LONG metaRight=std::max<LONG>(metaLeft+40,std::min<LONG>(metaRightLimit,rc.right/2+150));
        RECT metaTop{metaLeft,rc.bottom-62,metaRight,rc.bottom-43};
        DrawTextSimple(dc,meta,metaTop,13,FW_SEMIBOLD,RGB(165,172,185),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(item.isVideo){
            const double duration=detailsDurationSeconds_.load(std::memory_order_relaxed);
            RECT durationRect{metaTop.left,rc.bottom-43,metaTop.right,rc.bottom-8};
            DrawTextSimple(dc,duration>0.0?FormatTime(duration):L"--:--",durationRect,22,FW_SEMIBOLD,RGB(205,210,220),DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
    }

    void ResetPreviewZoom() {
        CancelPreviewBitmapDecodeJobs();
        previewZoomGestureActive_=false;
        previewScrollGestureActive_=false;
        previewAsyncDecodePreferred_=false;
        if(hwnd_){ KillTimer(hwnd_,kPreviewZoomSettleTimerId); KillTimer(hwnd_,kPreviewScrollSettleTimerId); }
        previewCardWidth_ = kDefaultPreviewCardWidth;
        previewWheelRemainder_ = 0;
        previewZoomOverridden_ = false;
        preFullscreenPreviewCardWidth_ = -1;
        preFullscreenPreviewZoomOverridden_ = false;
        preFullscreenPreviewStateValid_ = false;
    }

    void ResetLibraryZoom() {
        libraryZoomOverridden_ = false;
        preFullscreenLibraryCardWidth_ = -1;
        preFullscreenLibraryZoomOverridden_ = false;
        preFullscreenLibraryStateValid_ = false;
        if(hwnd_){
            RECT rc{}; GetClientRect(hwnd_,&rc);
            libraryCardWidth_ = DefaultLibraryCardWidthForViewport(std::max(1,static_cast<int>(rc.right-rc.left)));
        }else{
            libraryCardWidth_ = kDefaultLibraryCardWidth;
        }
    }

    bool CenterLibraryOnMedia(size_t mediaIndex) {
        if(mode_!=Mode::Library) return false;
        RECT rc{}; GetClientRect(hwnd_,&rc);
        const auto visibleFolders=VisibleFolderIndices();
        const auto& filtered=FilteredIndices();
        const auto found=std::find(filtered.begin(),filtered.end(),mediaIndex);
        if(found==filtered.end()) return false;

        const int pad=kLibraryPad,gap=kLibraryGap,cardW=libraryCardWidth_;
        const int imageH=std::max(113,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int cardH=imageH+kLibraryTitleHeight;
        const int rowStride=cardH+gap;
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left)-kLibraryScrollbarReserve);
        const int cols=std::max(1,(clientWidth-pad*2+gap)/(cardW+gap));
        const size_t mediaPosition=static_cast<size_t>(std::distance(filtered.begin(),found));
        const size_t displayIndex=visibleFolders.size()+mediaPosition;
        const int row=static_cast<int>(displayIndex/static_cast<size_t>(cols));
        const int logicalTop=pad+row*rowStride;
        const int logicalBottom=logicalTop+cardH;
        const int visibleTop=pad;
        const int visibleBottom=std::max(pad,static_cast<int>(rc.bottom)-68);

        // Preserve the user's Library position whenever the returning card is already
        // completely visible. Only recenter when some part of the card is clipped or the
        // card is outside the viewport. The return highlight itself still always runs.
        const int screenTop=logicalTop-scrollY_;
        const int screenBottom=logicalBottom-scrollY_;
        if(screenTop>=visibleTop && screenBottom<=visibleBottom) return true;

        const int logicalCenter=logicalTop+cardH/2;
        const int viewportCenter=(visibleTop+visibleBottom)/2;
        scrollY_=logicalCenter-viewportCenter;
        ClampScroll();
        return true;
    }

    void StartLibraryReturnHighlight(size_t mediaIndex) {
        libraryReturnHighlightCategory_=category_;
        libraryReturnHighlightIndex_=mediaIndex;
        libraryReturnHighlightStart_=GetTickCount64();
        libraryReturnHighlightRect_=RECT{};
        StartUiAnimationTimer();
    }

    bool CenterDetailsOnPreview(size_t previewIndex) {
        if(mode_!=Mode::Details || category_!=Category::Videos || previewIndex>=previewFrames_.size() || !hwnd_) return false;
        RECT rc{}; GetClientRect(hwnd_,&rc);
        const int footerTop=std::max(0,static_cast<int>(rc.bottom)-64);
        const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left));
        const int gap=12;
        const int cardW=DetailsPreviewCardWidthForViewport(clientWidth);
        const int imageH=std::max(79,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int cardH=imageH+24;
        const int availW=std::max(1,static_cast<int>(rc.right)-80);
        const int cols=std::max(1,(availW+gap)/(cardW+gap));
        const int row=static_cast<int>(previewIndex/static_cast<size_t>(cols));
        const int heroH=DetailsHeroHeightForViewport(clientWidth,footerTop,true);
        const int timelineTop=18+54+heroH+22;
        const int logicalCenter=timelineTop+row*(cardH+gap)+cardH/2;
        const int viewportCenter=std::max(1,footerTop)/2;
        detailsScrollY_=logicalCenter-viewportCenter;
        ClampDetailsScroll();
        return true;
    }

    void StartTimelineReturnHighlight(size_t previewIndex) {
        timelineReturnHighlightMediaIndex_=selected_;
        timelineReturnHighlightIndex_=previewIndex;
        timelineReturnHighlightStart_=GetTickCount64();
        timelineReturnHighlightRect_=RECT{};
        StartUiAnimationTimer();
    }

    void TryApplyPendingTimelineReturnFocus() {
        if(!pendingTimelineReturnFocus_ || mode_!=Mode::Details || category_!=Category::Videos || previewFrames_.empty()) return;
        size_t best=0;
        double bestDistance=std::numeric_limits<double>::infinity();
        for(size_t i=0;i<previewFrames_.size();++i){
            const double distance=std::abs(previewFrames_[i].seekSeconds-pendingTimelineReturnSeconds_);
            if(distance<bestDistance){bestDistance=distance;best=i;}
        }
        pendingTimelineReturnFocus_=false;
        CenterDetailsOnPreview(best);
        StartTimelineReturnHighlight(best);
    }

    void QueueTimelineReturnFocus(double playbackSeconds) {
        pendingTimelineReturnSeconds_=std::max(0.0,playbackSeconds);
        pendingTimelineReturnFocus_=true;
        TryApplyPendingTimelineReturnFocus();
    }

    void ReturnFromDetailsToLibrary() {
        if(mode_!=Mode::Details) return;
        KillTimer(hwnd_,kResumeDetailsWorkersTimerId);
        if(player_) player_->CloseSource();
        const Category returnCategory=category_;
        const size_t returnIndex=selected_;
        std::wstring returnPath;
        const auto& returnList=CurrentItems();
        if(returnIndex<returnList.size()) returnPath=returnList[returnIndex].path;

        StopImageSlideshow();
        ResetImageZoom();
        // Leaving the image viewer ends its Native Size session. Restore the exact
        // pre-native window geometry (or make fullscreen restore to it later).
        if(nativeImageSizing_ || nativeImageSizingRestoreRectValid_){
            nativeImageSizing_=false;
            if(fullscreen_){
                SetFullscreenRestoreToStandardWindow();
            }else{
                RestoreStandardWindowSize();
            }
            nativeImageSizingRestoreRectValid_=false;
        }
        StopPreviewWorker();
        ClearAllDetailInfoMemory();
        ResetPreviewZoom();
        if(hoverOwner_==hwnd_ || hoverPreviousOwner_==hwnd_){
            hoverOwner_=nullptr; hoverPreviousOwner_=nullptr; hoverRect_=RECT{}; hoverPreviousRect_=RECT{}; hoverTransitionStart_=0;
        }
        mode_=Mode::Library;
        detailsScrollY_=0;
        if(fullscreen_){
            RECT libraryRc{}; GetClientRect(hwnd_,&libraryRc);
            ApplyLibraryWidthForViewport(std::max(1,static_cast<int>(libraryRc.right-libraryRc.left)));
        }

        std::error_code navEc;
        if(!detailsOriginFolder_.empty() && fs::exists(detailsOriginFolder_,navEc) && !navEc)
            currentFolder_=fs::path(detailsOriginFolder_).lexically_normal().wstring();
        else if(!folder_.empty() && fs::exists(folder_,navEc) && !navEc)
            currentFolder_=fs::path(folder_).lexically_normal().wstring();

        if(searchQuery_.empty() && !loadFailureFilterActive_) searchVisible_=false;
        filteredIndices_.clear();
        filterDirty_=true;
        RestoreFolderViewState(currentFolder_);

        // The media that was actually open wins over the old Library scroll state.
        // This matters after using the Info-screen left/right arrows.
        category_=returnCategory;
        selected_=returnIndex;
        filteredIndices_.clear();
        filterDirty_=true;
        const bool centered=CenterLibraryOnMedia(returnIndex);
        if(centered){
            if(!returnPath.empty()) SaveCurrentFolderViewState(returnPath);
            StartLibraryReturnHighlight(returnIndex);
        }

        detailsSearchNavigationActive_=false;
        detailsSearchNavigationIndices_.clear();
        PrepareLibraryViewportFromPrivateCache();
        StartThumbnailWorker();
        InvalidateRect(hwnd_,nullptr,FALSE);
    }

    std::vector<size_t> ImageIndicesInCurrentFolder() const {
        std::vector<size_t> out;
        if(externalMediaSession_){
            out.reserve(images_.size());
            for(size_t i=0;i<images_.size();++i) out.push_back(i);
            return out;
        }
        const std::wstring currentKey = ToLower(fs::path(currentFolder_).lexically_normal().wstring());
        for (size_t i = 0; i < images_.size(); ++i) {
            const std::wstring parentKey = ToLower(fs::path(images_[i].path).parent_path().lexically_normal().wstring());
            if (parentKey == currentKey) out.push_back(i);
        }
        return out;
    }

    void StopImageSlideshow() {
        const bool wasActive = slideshowActive_;
        if (slideshowActive_) KillTimer(hwnd_, kSlideshowTimerId);
        slideshowActive_ = false;
        slideshowIndices_.clear();
        slideshowPos_ = 0;
        slideshowFadeActive_ = false;
        slideshowPreviousIndex_ = static_cast<size_t>(-1);
        if (wasActive && hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void StartImageSlideshow() {
        StopImageSlideshow();
        ResetImageZoom();
        if (category_ != Category::Images) return;
        slideshowIndices_ = ImageIndicesInCurrentFolder();
        if (slideshowIndices_.empty()) return;
        SaveCurrentFolderViewState();
        slideshowActive_ = true;
        slideshowPos_ = 0;
        detailsOriginFolder_ = currentFolder_;
        selected_ = slideshowIndices_.front();
        detailsScrollY_ = 0;
        ResetPreviewZoom();
        ResetLibraryZoom();
        mode_ = Mode::Details;
        if(nativeImageSizing_ && !fullscreen_) ApplyNativeImageWindowSize();
        SetTimer(hwnd_, kSlideshowTimerId, 3000, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void StartImageSlideshowFromSelected() {
        StopImageSlideshow();
        ResetImageZoom();
        if (category_ != Category::Images || selected_ >= images_.size()) return;
        slideshowIndices_ = ImageIndicesInCurrentFolder();
        if (slideshowIndices_.empty()) return;

        const auto it = std::find(slideshowIndices_.begin(), slideshowIndices_.end(), selected_);
        if (it == slideshowIndices_.end()) return;

        slideshowActive_ = true;
        slideshowPos_ = static_cast<size_t>(std::distance(slideshowIndices_.begin(), it));
        detailsOriginFolder_ = currentFolder_;
        detailsScrollY_ = 0;
        mode_ = Mode::Details;
        if(slideshowIndices_.size()>1 && slideshowPos_+1>=slideshowIndices_.size()){
            slideshowPreviousIndex_=selected_;
            slideshowPos_=0;
            selected_=slideshowIndices_.front();
            ResetImageZoom();
            slideshowFadeStart_=GetTickCount64();
            slideshowFadeActive_=true;
            StartUiAnimationTimer();
        }
        if(nativeImageSizing_ && !fullscreen_) ApplyNativeImageWindowSize();
        SetTimer(hwnd_, kSlideshowTimerId, 3000, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void AdvanceImageSlideshow() {
        if (!slideshowActive_ || mode_ != Mode::Details || category_ != Category::Images || slideshowIndices_.empty()) {
            StopImageSlideshow();
            return;
        }
        if (slideshowPos_ + 1 >= slideshowIndices_.size()) {
            StopImageSlideshow();
            return;
        }
        slideshowPreviousIndex_ = selected_;
        ++slideshowPos_;
        selected_ = slideshowIndices_[slideshowPos_];
        ResetImageZoom();
        if(nativeImageSizing_ && !fullscreen_) ApplyNativeImageWindowSize();
        slideshowFadeStart_ = GetTickCount64();
        slideshowFadeActive_ = true;
        StartUiAnimationTimer();
        detailsScrollY_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void HandleClick(int x,int y) {
        POINT p{x,y};
        if(FullLoadFailurePopupVisible()){
            RECT rc{}; GetClientRect(hwnd_,&rc);
            const RECT action=FullLoadFailureActionRect(rc);
            if(PtInRect(&action,p)){ ShowFullLoadFailureResults(); return; }
        }
        if(mode_==Mode::Library){
            if(!IsAtLibraryRoot() && PtInRect(&backRect_,p)){
                StopImageSlideshow();

                // Search belongs to the folder where it was started. Leaving that
                // folder cancels it completely so the parent folder always opens in
                // its normal unfiltered state.
                ClearLoadFailureFilter();
                searchQuery_.clear();
                searchVisible_=false;
                filteredIndices_.clear();
                filterDirty_=true;

                SaveCurrentFolderViewState();
                fs::path parent=fs::path(currentFolder_).parent_path();
                currentFolder_=parent.empty()?folder_:parent.lexically_normal().wstring();
                RestoreFolderViewState(currentFolder_);
                PrepareLibraryViewportFromPrivateCache();
                InvalidateRect(hwnd_,nullptr,FALSE); return;
            }
            if(PtInRect(&categoryToggleRect_,p)){
                StopImageSlideshow();
                category_=category_==Category::Videos?Category::Images:Category::Videos;
                selected_=0; scrollY_=0; filterDirty_=true;
                SaveSettings(); PrepareLibraryViewportFromPrivateCache(); InvalidateRect(hwnd_,nullptr,FALSE); return;
            }
            if(category_==Category::Images && PtInRect(&slideshowRect_,p)){ StartImageSlideshow(); return; }
            if(PtInRect(&libraryFullRect_,p)){ ToggleFullscreen(); return; }
            if(IsAtChosenLibraryRoot() && PtInRect(&loadEverythingRect_,p)){ StopImageSlideshow(); StartFullLoadEverything(); return; }
            if((IsAtChosenLibraryRoot() || currentFolder_.empty() || externalMediaSession_) && PtInRect(&chooseRect_,p)){ StopImageSlideshow(); ChooseFolder(); return; }
            if(IsAtChosenLibraryRoot() && PtInRect(&rescanRect_,p)){ StopImageSlideshow(); Scan(true); return; }
            // The entire fixed footer is an input barrier, including empty/semi-transparent areas.
            if(PtInRect(&libraryFooterRect_,p)) return;
            RECT rc{}; GetClientRect(hwnd_,&rc);
            const int pad=kLibraryPad,gap=kLibraryGap,cardW=libraryCardWidth_;
            const int imageH=std::max(113,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
            const int cardH=imageH+kLibraryTitleHeight; const int clientWidth=std::max(1,static_cast<int>(rc.right-rc.left)-kLibraryScrollbarReserve);
            // The fixed top padding is a visual/input mask. Cards may scroll underneath it,
            // but they cannot be clicked through the masked area.
            if(y<pad) return;
            const int cols=std::max(1,(clientWidth-pad*2+gap)/(cardW+gap)); const int ly=y-pad+scrollY_; if(ly<0) return;
            const int row=ly/(cardH+gap),col=(x-pad)/(cardW+gap); if(col<0||col>=cols) return;
            const int localX=(x-pad)%(cardW+gap),localY=ly%(cardH+gap); if(localX<0||localX>=cardW||localY<0||localY>=cardH) return;
            const size_t displayIndex=static_cast<size_t>(row)*static_cast<size_t>(cols)+static_cast<size_t>(col);
            const auto visibleFolders=VisibleFolderIndices();
            if(displayIndex<visibleFolders.size()){
                StopImageSlideshow();
                SaveCurrentFolderViewState();
                currentFolder_=folders_[visibleFolders[displayIndex]].path;
                RestoreFolderViewState(currentFolder_);
                PrepareLibraryViewportFromPrivateCache();
                InvalidateRect(hwnd_,nullptr,FALSE); return;
            }
            const auto& filtered=FilteredIndices();
            const size_t mediaDisplayIndex=displayIndex-visibleFolders.size();
            if(mediaDisplayIndex<filtered.size()){
                StopImageSlideshow(); thumbStop_.store(true,std::memory_order_release); ClearLoadingStateIf(1); ResetPreviewZoom();
                detailsOriginFolder_=currentFolder_;
                detailsSearchNavigationActive_ = loadFailureFilterActive_ || (searchVisible_ && !searchQuery_.empty());
                if(detailsSearchNavigationActive_) detailsSearchNavigationIndices_ = filtered;
                else detailsSearchNavigationIndices_.clear();
                selected_=filtered[mediaDisplayIndex];
                const auto& list=CurrentItems();
                if(selected_<list.size()) SaveCurrentFolderViewState(list[selected_].path);
                ResetLibraryZoom();
                if(category_==Category::Images) ResetImageZoom();
                mode_=Mode::Details; detailsScrollY_=0;
                if(category_==Category::Videos) StartPreviewWorkerForSelected(); else ClearLoadingState();
                QueueDetailPrefetchWindow();
                InvalidateRect(hwnd_,nullptr,FALSE);
            }
        } else if(mode_==Mode::Details){
            if(PtInRect(&detailsPrevRect_,p) && CanNavigateDetailsMedia(-1)){ NavigateDetailsMedia(-1); return; }
            if(PtInRect(&detailsNextRect_,p) && CanNavigateDetailsMedia(1)){ NavigateDetailsMedia(1); return; }

            // Fixed footer controls always win hit-testing over scrollable content.
            if(PtInRect(&backRect_,p)){
                ReturnFromDetailsToLibrary();
                return;
            }
            if(category_==Category::Videos&&PtInRect(&playRect_,p)){ EnterPlayerAt(0.0); return; }
            if(category_==Category::Images&&PtInRect(&imageDetailsSlideshowRect_,p)){
                if(slideshowActive_) StopImageSlideshow();
                else StartImageSlideshowFromSelected();
                return;
            }
            if(category_==Category::Images&&PtInRect(&imageDetailsNativeRect_,p)){ ToggleNativeImageSizing(); return; }
            if(PtInRect(&detailsFullRect_,p)){ ToggleFullscreen(); return; }

            // Treat the entire footer as an input barrier, including its transparent/empty
            // areas, so a preview card underneath can never receive a click through it.
            if(PtInRect(&detailsFooterRect_,p)) return;

            if(category_==Category::Videos){
                for(const auto& hit : previewHitRects_){
                    RECT r = hit.first;
                    if(PtInRect(&r,p)){ EnterPlayerAt(hit.second); return; }
                }
            }
        }
    }

    static bool PathExistsNoThrow(const fs::path& path) {
        std::error_code ec;
        return fs::exists(path,ec) && !ec;
    }

    void ArmLibraryAccessMonitor(ULONGLONG milliseconds=5000) {
        const ULONGLONG until=GetTickCount64()+milliseconds;
        if(until>libraryAccessMonitorUntil_) libraryAccessMonitorUntil_=until;
    }

    bool IsLibraryRootAccessible() const {
        if(folder_.empty()) return false;
        const DWORD attrs=GetFileAttributesW(folder_.c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES || (attrs&FILE_ATTRIBUTE_DIRECTORY)==0) return false;
        HANDLE h=CreateFileW(folder_.c_str(),FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        if(h==INVALID_HANDLE_VALUE) return false;
        CloseHandle(h);
        return true;
    }

    static wchar_t DriveLetterFromPath(const std::wstring& path) {
        if(path.size()<2 || path[1]!=L':' || !iswalpha(path[0])) return 0;
        return static_cast<wchar_t>(towupper(path[0]));
    }

    bool IsLibraryVolumePresent() const {
        // For a normal drive-letter library (for example X:\Media), monitor only
        // the Windows drive mount state. GetLogicalDrives does not open the selected
        // folder or any media file, so this once-per-second health check does not keep
        // a VeraCrypt volume busy with a long-lived filesystem handle.
        const std::wstring& path = externalMediaSession_ ? folder_ : (!persistentFolder_.empty() ? persistentFolder_ : folder_);
        const wchar_t drive = DriveLetterFromPath(path);
        if(drive){
            const DWORD mask=GetLogicalDrives();
            if(mask==0) return false;
            const unsigned bit=static_cast<unsigned>(drive-L'A');
            return bit<26u && (mask&(1u<<bit))!=0;
        }

        // UNC paths and unusual mount-point paths have no drive-letter bit to query.
        // Fall back to the existing short-lived root check for those cases.
        return IsLibraryRootAccessible();
    }

    bool LibraryIoWorkActive() {
        if(GetTickCount64()<libraryAccessMonitorUntil_) return true;
        if(libraryScanRunning_ ||
           thumbWorkerRunning_.load(std::memory_order_acquire) ||
           fullLoadRunning_.load(std::memory_order_acquire) ||
           loadingKind_.load(std::memory_order_acquire)!=0) return true;
        {
            std::lock_guard<std::mutex> lock(libraryThumbLoadMutex_);
            if(!libraryThumbLoadJobs_.empty()) return true;
        }
        {
            std::lock_guard<std::mutex> lock(resolutionMetadataMutex_);
            if(!resolutionMetadataJobs_.empty()) return true;
        }
        {
            std::lock_guard<std::mutex> lock(detailPrefetchMutex_);
            if(!detailPrefetchJobs_.empty()) return true;
        }
        return false;
    }

    void UnloadUnavailableLibrarySession() {
        if(libraryUnavailableLatched_) return;
        libraryUnavailableLatched_=true;
        libraryAccessFailCount_=3;
        libraryAccessRetryNeedsRescan_=false;

        StopImageSlideshow();
        autoNext_=false;
        nativeVideoSizing_=false;
        nativeImageSizing_=false;
        if(player_) player_->ResetFlatZoom();
        ResetImageZoom();
        DestroyPlayerFooterTransition();
        KillTimer(hwnd_,kResumeDetailsWorkersTimerId);

        if(player_) player_->Pause();
        if(videoHwnd_) ShowWindow(videoHwnd_,SW_HIDE);
        if(controlsHwnd_) ShowWindow(controlsHwnd_,SW_HIDE);
        if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_HIDE);
        if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_HIDE);
        playerControlsVisible_=false; controlsFading_=false; controlsAlpha_=0;
        player_.reset();

        StopLibraryScanWorker();
        StopPreviewWorker();
        CancelDetailPrefetchJobs();
        StopFullLoadWorker();
        StopThumbnailWorker();
        ResetLibraryThumbLoadView();
        ResetResolutionMetadataWork();
        ClearAllDetailInfoMemory();
        ClearThumbs(videos_); ClearThumbs(images_);
        videos_.clear(); images_.clear(); folders_.clear(); videoFolderIndices_.clear(); imageFolderIndices_.clear(); childFolderIndices_.clear();
        filteredIndices_.clear(); filterDirty_=true;
        detailsSearchNavigationIndices_.clear(); detailsSearchNavigationActive_=false;
        folderViewStates_.clear();
        currentFolder_.clear(); detailsOriginFolder_.clear();
        ClearLoadFailureFilter();
        searchQuery_.clear(); searchVisible_=false; searchSelectAll_=false;
        selected_=0; scrollY_=0; detailsScrollY_=0;
        ResetPreviewZoom(); ResetLibraryZoom();
        libraryMediaHoverHits_.clear(); previewMediaHoverHits_.clear();
        libraryReturnHighlightIndex_=static_cast<size_t>(-1); libraryReturnHighlightStart_=0; libraryReturnHighlightRect_=RECT{};
        mode_=Mode::Library;
        // Losing the backing drive must not resize or recenter the main window. The
        // unavailable notice is purely an in-app overlay; preserve the user's geometry.
        SetWindowTextW(hwnd_,L"Visual MediaPlayer");
        ApplyMainWindowCornerPreference();
        Layout();
        ShowInAppNotice(L"This folder is unavailable.",5000);
        InvalidateRect(hwnd_,nullptr,TRUE);
    }

    void NoteLibraryAccessFailure(bool retryNeedsRescan) {
        if(libraryUnavailableLatched_) return;
        libraryAccessRetryNeedsRescan_=libraryAccessRetryNeedsRescan_ || retryNeedsRescan;
        libraryAccessFailCount_=std::min(3,libraryAccessFailCount_+1);
        if(libraryAccessFailCount_>=3) UnloadUnavailableLibrarySession();
    }

    void CheckLibraryAccessHealth() {
        if(folder_.empty()){
            libraryAccessFailCount_=0; libraryUnavailableLatched_=false; libraryAccessRetryNeedsRescan_=false;
            return;
        }

        // Once the library is latched unavailable, never run the unload/notice path
        // again. That used to restart the notice every second and made its pulse appear
        // endless. Wait only for the drive to return, then verify the saved folder once.
        if(libraryUnavailableLatched_){
            if(!IsLibraryVolumePresent()) return;
            if(IsLibraryRootAccessible()){
                libraryAccessFailCount_=0;
                libraryUnavailableLatched_=false;
                libraryAccessRetryNeedsRescan_=false;
                ClearInAppNotice();
                if(mode_==Mode::Library){
                    if(externalMediaSession_ && !externalMediaPaths_.empty()){
                        const auto reopenPaths=externalMediaPaths_;
                        OpenExternalMediaBatch(reopenPaths);
                    }else Scan(false);
                }
            }
            return;
        }

        // While healthy, check only whether the backing drive/volume is still mounted.
        // For drive-letter libraries this uses GetLogicalDrives, so the once-per-second
        // health monitor does not touch the selected folder or any media file.
        if(!IsLibraryVolumePresent()){
            NoteLibraryAccessFailure(false);
            return;
        }

        // A short drive disappearance recovered before reaching the three-failure threshold.
        // Reset the counter without probing the library folder.
        if(libraryAccessFailCount_>0){
            libraryAccessFailCount_=0;
            libraryAccessRetryNeedsRescan_=false;
        }
    }

    void ChooseFolder() {
        StopImageSlideshow();
        ComPtr<IFileDialog> dlg; if(FAILED(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dlg)))) return;
        DWORD opts=0; dlg->GetOptions(&opts); dlg->SetOptions(opts|FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM);
        if(!folder_.empty()&&IsLibraryRootAccessible()){
            ComPtr<IShellItem> start; if(SUCCEEDED(SHCreateItemFromParsingName(folder_.c_str(),nullptr,IID_PPV_ARGS(&start)))) dlg->SetFolder(start.Get());
        }
        if(SUCCEEDED(dlg->Show(hwnd_))){
            ComPtr<IShellItem> item; if(SUCCEEDED(dlg->GetResult(&item))){
                PWSTR p=nullptr; if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p))){ folder_=p; persistentFolder_=folder_; CoTaskMemFree(p); RememberLibraryRoot(folder_); SaveSettings(); Scan(); }
            }
        }
    }

    void DrainExternalOpenMessages() {
        if(!hwnd_ || !IsWindow(hwnd_)) return;
        MSG message{};
        while(PeekMessageW(&message,hwnd_,WM_APP_EXTERNAL_OPEN,WM_APP_EXTERNAL_OPEN,PM_REMOVE)){
            delete reinterpret_cast<std::vector<std::wstring>*>(message.lParam);
        }
    }

    void DrainLibraryScanMessages() {
        if(!hwnd_ || !IsWindow(hwnd_)) return;
        MSG message{};
        while(PeekMessageW(&message,hwnd_,WM_APP_LIBRARY_SCAN_DONE,WM_APP_LIBRARY_SCAN_DONE,PM_REMOVE)){
            delete reinterpret_cast<LibraryScanCompletion*>(message.lParam);
        }
    }

    void StopLibraryScanWorker() {
        libraryScanStop_.store(true,std::memory_order_release);
        libraryScanGeneration_.fetch_add(1,std::memory_order_acq_rel);
        if(libraryScanThread_.joinable()){
            // std::filesystem can be waiting on a slow/disconnected drive. Ask Windows to
            // cancel synchronous I/O before joining so rescans/shutdown do not wait on it.
            CancelSynchronousIo(libraryScanThread_.native_handle());
            libraryScanThread_.join();
        }
        libraryScanRunning_=false;
        DrainLibraryScanMessages();
    }

    void ApplyLibraryScanCompletion(std::unique_ptr<LibraryScanCompletion> completion) {
        if(!completion) return;
        if(completion->generation!=libraryScanGeneration_.load(std::memory_order_acquire)) return;
        if(libraryScanThread_.joinable()) libraryScanThread_.join();
        libraryScanRunning_=false;
        if(completion->result.cancelled) return;
        if(ToLower(fs::path(completion->result.root).lexically_normal().wstring())!=
           ToLower(fs::path(folder_).lexically_normal().wstring())) return;

        for(const auto& rawFolder:completion->result.folders){
            const fs::path pth=fs::path(rawFolder).lexically_normal();
            LibraryFolder folderItem;
            folderItem.path=pth.wstring();
            folderItem.parentFolderKey=ToLower(pth.parent_path().lexically_normal().wstring());
            folderItem.name=pth.filename().wstring();
            folders_.push_back(std::move(folderItem));
        }
        for(const auto& scanned:completion->result.media){
            const fs::path pth=fs::path(scanned.path).lexically_normal();
            const bool video=scanned.kind==vmp::ScannedMediaKind::Video;
            MediaItem item;
            item.path=pth.wstring();
            item.parentFolderKey=ToLower(pth.parent_path().lexically_normal().wstring());
            item.title=pth.stem().wstring();
            std::replace(item.title.begin(),item.title.end(),L'_',L' ');
            item.isVideo=video;
            if(video) item.vr=DetectVR(item.path);
            else item.title=StripLeadingImageResolutionPrefix(std::move(item.title));
            item.cachePath=scanned.cachePath;
            item.uiCachePath=scanned.uiCachePath;
            item.favorite=scanned.favorite;
            item.searchText=ToLower(item.title+L"\n"+item.path);
            if(video) videos_.push_back(std::move(item));
            else images_.push_back(std::move(item));
        }

        auto mediaSorter=[](const MediaItem& a,const MediaItem& b){
            const MediaNameSortKey ka=BuildMediaNameSortKey(a.title);
            const MediaNameSortKey kb=BuildMediaNameSortKey(b.title);
            if(ka.primary!=kb.primary) return ka.primary<kb.primary;
            if(ka.group!=kb.group) return ka.group<kb.group;
            if(ka.secondary!=kb.secondary) return ka.secondary<kb.secondary;
            if(ka.hasNumber!=kb.hasNumber) return !ka.hasNumber;
            if(ka.hasNumber){
                const int numberCmp=CompareSortNumbers(ka.number,kb.number);
                if(numberCmp!=0) return numberCmp<0;
            }
            if(ka.fallback!=kb.fallback) return ka.fallback<kb.fallback;
            return ToLower(a.path)<ToLower(b.path);
        };
        auto folderSorter=[](const LibraryFolder&a,const LibraryFolder&b){return ToLower(a.name)<ToLower(b.name);};
        std::sort(videos_.begin(),videos_.end(),mediaSorter);
        std::sort(images_.begin(),images_.end(),mediaSorter);
        std::sort(folders_.begin(),folders_.end(),folderSorter);
        RebuildLibraryFolderIndexCaches();

        if(completion->cleanupOrphanCache && completion->result.reliable){
            std::vector<fs::path> cacheRoots;
            cacheRoots.reserve(completion->result.cacheRoots.size());
            for(const auto& root:completion->result.cacheRoots) cacheRoots.emplace_back(root);
            CleanupOrphanMediaCache(cacheRoots);
        }

        if(!completion->result.reliable && !IsLibraryRootAccessible()){
            NoteLibraryAccessFailure(true);
            filterDirty_=true;
            InvalidateRect(hwnd_,nullptr,TRUE);
            return;
        }

        filterDirty_=true;
        RestoreFolderViewState(currentFolder_);
        PrepareLibraryViewportFromPrivateCache();
        InvalidateRect(hwnd_,nullptr,FALSE);
        StartThumbnailWorker();
    }

    void Scan(bool cleanupOrphanCache=false) {
        StopLibraryScanWorker();
        StopImageSlideshow();
        externalMediaSession_=false;
        externalMediaPaths_.clear();
        if(!folder_.empty() && !IsLibraryRootAccessible()){
            NoteLibraryAccessFailure(true);
            InvalidateRect(hwnd_,nullptr,TRUE);
            return;
        }
        libraryAccessFailCount_=0; libraryUnavailableLatched_=false; libraryAccessRetryNeedsRescan_=false;
        ArmLibraryAccessMonitor(10000);
        StopFullLoadWorker();
        ClearLoadFailureFilter();
        const std::wstring previousFolder=currentFolder_;
        if(mode_==Mode::Library && !previousFolder.empty()) SaveCurrentFolderViewState();
        StopPreviewWorker();
        ClearAllDetailInfoMemory();
        StopThumbnailWorker();
        ResetLibraryThumbLoadView();
        ResetResolutionMetadataWork();
        ClearThumbs(videos_); ClearThumbs(images_);
        videos_.clear(); images_.clear(); folders_.clear(); videoFolderIndices_.clear(); imageFolderIndices_.clear(); childFolderIndices_.clear(); detailsOriginFolder_.clear();
        selected_=0; scrollY_=0; detailsScrollY_=0; ResetPreviewZoom(); ResetLibraryZoom();
        libraryMediaHoverHits_.clear(); previewMediaHoverHits_.clear();
        libraryReturnHighlightIndex_=static_cast<size_t>(-1); libraryReturnHighlightStart_=0; libraryReturnHighlightRect_=RECT{};
        if(folder_.empty()){ currentFolder_.clear(); InvalidateRect(hwnd_,nullptr,TRUE); return; }
        folder_=fs::path(folder_).lexically_normal().wstring();
        std::error_code previousEc;
        if(!previousFolder.empty() && PathIsWithin(previousFolder,folder_) && fs::exists(previousFolder,previousEc) && !previousEc)
            currentFolder_=fs::path(previousFolder).lexically_normal().wstring();
        else
            currentFolder_=folder_;

        const std::wstring scanRoot=folder_;
        const uint64_t generation=libraryScanGeneration_.fetch_add(1,std::memory_order_acq_rel)+1;
        libraryScanStop_.store(false,std::memory_order_release);
        libraryScanRunning_=true;
        filterDirty_=true;
        InvalidateRect(hwnd_,nullptr,FALSE);

        libraryScanThread_=std::thread([this,scanRoot,generation,cleanupOrphanCache]{
            auto completion=std::make_unique<LibraryScanCompletion>();
            completion->generation=generation;
            completion->cleanupOrphanCache=cleanupOrphanCache;
            completion->result=vmp::ScanLibrary(scanRoot,libraryScanStop_);
            if(libraryScanStop_.load(std::memory_order_acquire) ||
               generation!=libraryScanGeneration_.load(std::memory_order_acquire)) return;
            LibraryScanCompletion* raw=completion.release();
            if(!hwnd_ || !PostMessageW(hwnd_,WM_APP_LIBRARY_SCAN_DONE,0,reinterpret_cast<LPARAM>(raw))) delete raw;
        });
    }

    void ClampScroll() {
        RECT rc{}; GetClientRect(hwnd_,&rc);
        const int maxScroll = LibraryMaxScroll(rc);
        scrollY_ = std::clamp(scrollY_, 0, maxScroll);
        libraryLastKnownMaxScroll_ = maxScroll;
        UpdateLibraryScrollbarRects(rc);
    }

    void ClampDetailsScroll() {
        if(mode_!=Mode::Details){detailsScrollY_=0;return;}
        RECT rc{}; GetClientRect(hwnd_,&rc);
        if(category_!=Category::Videos){detailsScrollY_=0;return;}
        const int footerTop=std::max(0,static_cast<int>(rc.bottom)-64);
        const int gap=12;
        const int cardW=DetailsPreviewCardWidthForViewport(static_cast<int>(rc.right - rc.left));
        const int imageH=std::max(79,static_cast<int>(std::lround(static_cast<double>(cardW)*9.0/16.0)));
        const int cardH=imageH+24;
        const int availW=std::max(1,static_cast<int>(rc.right)-80);
        const int cols=std::max(1,(availW+gap)/(cardW+gap));
        const bool showTimeline=selected_<videos_.size();
        const int rows=!showTimeline||previewFrames_.empty()?0:static_cast<int>((previewFrames_.size()+static_cast<size_t>(cols)-1)/static_cast<size_t>(cols));
        const int previewsHeight=!showTimeline?0:(previewFrames_.empty()?64:rows*(cardH+gap)+10);
        const int heroH=DetailsHeroHeightForViewport(static_cast<int>(rc.right-rc.left),footerTop,true);
        const int contentBottom=18+54+heroH+22+(showTimeline?38:0)+previewsHeight+20;
        const int maxScroll=std::max(0,contentBottom-footerTop+16);
        detailsScrollY_=std::clamp(detailsScrollY_,0,maxScroll);
    }

    std::wstring SettingsPath() const {
#if defined(VMP_PORTABLE_BUILD)
        wchar_t exeBuf[32768]{};
        const DWORD len=GetModuleFileNameW(nullptr,exeBuf,static_cast<DWORD>(std::size(exeBuf)));
        if(!len || len>=std::size(exeBuf)) return L"VisualMediaPlayer.settings.ini";
        return (fs::path(std::wstring(exeBuf,len)).parent_path()/L"VisualMediaPlayer.settings.ini").wstring();
#else
        wchar_t local[MAX_PATH]{}; if(FAILED(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,local))) return L"VisualMediaPlayer.ini";
        fs::path dir=fs::path(local)/L"VisualMediaPlayer"; std::error_code ec; fs::create_directories(dir,ec); return (dir/L"settings.ini").wstring();
#endif
    }

    void RememberLibraryRoot(const std::wstring& rawRoot) {
        if(rawRoot.empty()) return;
        fs::path rootPath=fs::path(rawRoot).lexically_normal();
        const std::wstring key=ToLower(rootPath.wstring());
        if(key.empty()) return;
        for(const auto& existing:knownLibraryRoots_){
            if(ToLower(fs::path(existing).lexically_normal().wstring())==key) return;
        }
        knownLibraryRoots_.push_back(rootPath.wstring());
    }

    void LoadSettings() {
        const std::wstring settings=SettingsPath();
        wchar_t buf[32768]{};
        GetPrivateProfileStringW(L"Library",L"Folder",L"",buf,static_cast<DWORD>(std::size(buf)),settings.c_str());
        folder_=buf; persistentFolder_=folder_;

        wchar_t categoryBuf[32]{};
        GetPrivateProfileStringW(L"View",L"Category",L"Videos",categoryBuf,static_cast<DWORD>(std::size(categoryBuf)),settings.c_str());
        category_=ToLower(categoryBuf)==L"images"?Category::Images:Category::Videos;

        std::vector<wchar_t> section(65536, L'\0');
        const DWORD chars=GetPrivateProfileSectionW(L"CacheRoots",section.data(),static_cast<DWORD>(section.size()),settings.c_str());
        if(chars>0 && static_cast<size_t>(chars)<section.size()-2){
            const wchar_t* p=section.data();
            while(*p){
                std::wstring entry=p;
                const size_t eq=entry.find(L'=');
                if(eq!=std::wstring::npos && eq+1<entry.size()) RememberLibraryRoot(entry.substr(eq+1));
                p+=entry.size()+1;
            }
        }
        RememberLibraryRoot(persistentFolder_);
    }

    void SaveSettings() const {
        const std::wstring settings=SettingsPath();
        WritePrivateProfileStringW(L"Library",L"Folder",persistentFolder_.c_str(),settings.c_str());
        WritePrivateProfileStringW(L"View",L"Category",category_==Category::Images?L"Images":L"Videos",settings.c_str());
        WritePrivateProfileStringW(L"CacheRoots",nullptr,nullptr,settings.c_str());
        for(size_t i=0;i<knownLibraryRoots_.size();++i){
            wchar_t key[32]{}; swprintf_s(key,L"Root%zu",i);
            WritePrivateProfileStringW(L"CacheRoots",key,knownLibraryRoots_[i].c_str(),settings.c_str());
        }
    }

    bool CreatePlayerControls() {
        if(videoHwnd_&&controlsHwnd_&&player_) return true;
        if(!videoHwnd_){
            videoHwnd_=CreateWindowExW(0,L"VisualMediaPlayerVideo",nullptr,WS_CHILD|WS_CLIPSIBLINGS,0,0,100,100,hwnd_,nullptr,inst_,this);
            if(!videoHwnd_){MessageBoxW(hwnd_,L"Could not create the Direct3D video surface.",L"Visual MediaPlayer",MB_ICONERROR);return false;}
        }
        if(!controlsHwnd_){
            // A layered CHILD window is unreliable for this overlay.  Use an owned popup instead:
            // it can alpha-blend over the D3D child surface without changing the video viewport.
            const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            const DWORD style = WS_POPUP | WS_CLIPSIBLINGS;
            controlsHwnd_=CreateWindowExW(exStyle,L"VisualMediaPlayerControls",nullptr,style,0,0,100,100,hwnd_,nullptr,inst_,this);
            if(!controlsHwnd_){
                const DWORD err=GetLastError();
                const std::wstring msg=L"Could not create the player control overlay.\n\nWindows error: "+std::to_wstring(err);
                MessageBoxW(hwnd_,msg.c_str(),L"Visual MediaPlayer",MB_ICONERROR);
                return false;
            }
            controlsAlpha_ = 0;
            if(!SetLayeredWindowAttributes(controlsHwnd_,0,controlsAlpha_,LWA_ALPHA)){
                const DWORD err=GetLastError();
                DestroyWindow(controlsHwnd_); controlsHwnd_=nullptr;
                const std::wstring msg=L"Could not enable the semi-transparent player overlay.\n\nWindows error: "+std::to_wstring(err);
                MessageBoxW(hwnd_,msg.c_str(),L"Visual MediaPlayer",MB_ICONERROR);
                return false;
            }
        }
        if(!playerPrevHwnd_ || !playerNextHwnd_){
            const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
            const DWORD style = WS_POPUP | WS_CLIPSIBLINGS;
            if(!playerPrevHwnd_) playerPrevHwnd_=CreateWindowExW(exStyle,L"VisualMediaPlayerEdgeArrow",nullptr,style,0,0,48,76,hwnd_,nullptr,inst_,this);
            if(!playerNextHwnd_) playerNextHwnd_=CreateWindowExW(exStyle,L"VisualMediaPlayerEdgeArrow",nullptr,style,0,0,48,76,hwnd_,nullptr,inst_,this);
            if(!playerPrevHwnd_ || !playerNextHwnd_){
                MessageBoxW(hwnd_,L"Could not create the previous / next media controls.",L"Visual MediaPlayer",MB_ICONERROR);
                return false;
            }
            SetLayeredWindowAttributes(playerPrevHwnd_,0,controlsAlpha_,LWA_ALPHA);
            SetLayeredWindowAttributes(playerNextHwnd_,0,controlsAlpha_,LWA_ALPHA);
        }
        player_=std::make_unique<NativePlayer>(); const HRESULT hr=player_->Initialize(hwnd_,videoHwnd_);
        if(FAILED(hr)){MessageBoxW(hwnd_,(L"Could not initialize Direct3D 11 / Media Foundation.\n\n"+HrText(hr)).c_str(),L"Visual MediaPlayer",MB_ICONERROR);player_.reset();return false;}
        player_->SetVolume(volumeFraction_); Layout(); return true;
    }

    void DestroyPlayerFooterTransition() {
        if(playerFooterTransitionHwnd_){ DestroyWindow(playerFooterTransitionHwnd_); playerFooterTransitionHwnd_=nullptr; }
        if(playerFooterTransitionBitmap_){ DeleteObject(playerFooterTransitionBitmap_); playerFooterTransitionBitmap_=nullptr; }
        playerFooterTransitionStart_=0;
    }

    void StartPlayerFooterTransitionSnapshot(bool captureScreen=false) {
        DestroyPlayerFooterTransition();
        if(!hwnd_) return;
        RECT client{}; GetClientRect(hwnd_,&client);
        const int width=std::max(1,static_cast<int>(client.right-client.left));
        const int height=std::min(64,std::max(1,static_cast<int>(client.bottom-client.top)));
        const int top=std::max(0,static_cast<int>(client.bottom)-height);
        POINT screen{0,top}; ClientToScreen(hwnd_,&screen);
        HDC src=captureScreen?GetDC(nullptr):GetDC(hwnd_);
        if(!src) return;
        HDC mem=CreateCompatibleDC(src);
        HBITMAP bmp=CreateCompatibleBitmap(src,width,height);
        if(!mem || !bmp){ if(bmp) DeleteObject(bmp); if(mem) DeleteDC(mem); ReleaseDC(captureScreen?nullptr:hwnd_,src); return; }
        HGDIOBJ old=SelectObject(mem,bmp);
        const int srcX=captureScreen?screen.x:0;
        const int srcY=captureScreen?screen.y:top;
        const DWORD rop=captureScreen?(SRCCOPY|CAPTUREBLT):SRCCOPY;
        const BOOL copied=BitBlt(mem,0,0,width,height,src,srcX,srcY,rop);
        SelectObject(mem,old); DeleteDC(mem); ReleaseDC(captureScreen?nullptr:hwnd_,src);
        if(!copied){DeleteObject(bmp);return;}

        const DWORD exStyle=WS_EX_LAYERED|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT;
        const DWORD style=WS_POPUP|SS_BITMAP;
        HWND overlay=CreateWindowExW(exStyle,L"STATIC",nullptr,style,screen.x,screen.y,width,height,hwnd_,nullptr,inst_,nullptr);
        if(!overlay){DeleteObject(bmp);return;}
        SendMessageW(overlay,STM_SETIMAGE,IMAGE_BITMAP,reinterpret_cast<LPARAM>(bmp));
        SetLayeredWindowAttributes(overlay,0,255,LWA_ALPHA);
        if(!fullscreen_){
            constexpr int cornerDiameter=20;
            HRGN rounded=CreateRoundRectRgn(0,0,width+1,height+1,cornerDiameter,cornerDiameter);
            HRGN squareTop=CreateRectRgn(0,0,width,std::min(height,cornerDiameter));
            if(rounded&&squareTop){
                CombineRgn(rounded,rounded,squareTop,RGN_OR);
                if(SetWindowRgn(overlay,rounded,FALSE)!=0) rounded=nullptr;
            }
            if(rounded) DeleteObject(rounded);
            if(squareTop) DeleteObject(squareTop);
        }
        playerFooterTransitionHwnd_=overlay;
        playerFooterTransitionBitmap_=bmp;
        playerFooterTransitionStart_=GetTickCount64();
        ShowWindow(overlay,SW_SHOWNOACTIVATE);
        StartUiAnimationTimer();
    }

    void UpdatePlayerFooterTransition(ULONGLONG now, bool& active) {
        if(!playerFooterTransitionHwnd_ || playerFooterTransitionStart_==0) return;
        const ULONGLONG elapsed=now-playerFooterTransitionStart_;
        if(elapsed>=kPlayerFooterTransitionDurationMs){ DestroyPlayerFooterTransition(); return; }
        active=true;
        const float raw=static_cast<float>(elapsed)/static_cast<float>(kPlayerFooterTransitionDurationMs);
        const float smooth=raw*raw*(3.0f-2.0f*raw);
        const BYTE alpha=static_cast<BYTE>(std::clamp<int>(static_cast<int>(std::lround(255.0f*(1.0f-smooth))),0,255));
        SetLayeredWindowAttributes(playerFooterTransitionHwnd_,0,alpha,LWA_ALPHA);
    }

    void RequestBackgroundStopForPlayback() {
        // Playback has absolute priority, but entering the player must never block the UI
        // waiting for a thumbnail/preview worker to join. Signal cancellation immediately;
        // the threads are joined later when Details loading is restarted or at shutdown.
        // Keep batch/resolution producers from starting another heavy decode while the
        // playback source is opening. After this short grace period, Load Everything is
        // allowed to resume at below-normal priority during playback.
        DeferBackgroundWork(700);
        previewStop_.store(true, std::memory_order_release);
        thumbStop_.store(true, std::memory_order_release);
        CancelPreviewBitmapDecodeJobs();
        CancelDetailPrefetchJobs();
        ClearLoadingState();
    }

    void SyncDetailsTimelineToSelectedVideoFromCache() {
        if(category_!=Category::Videos || selected_>=videos_.size()){
            DeletePreviewFrameBitmaps(previewFrames_);
            previewFrames_.clear();
            previewMediaPath_.clear();
            previewDir_.clear();
            detailsDurationSeconds_.store(0.0,std::memory_order_relaxed);
            return;
        }

        const MediaItem& item=videos_[selected_];
        if(!previewMediaPath_.empty() && previewMediaPath_!=item.path) ParkActivePreviewSet();
        previewDir_=BuildPreviewDirectory(item.path);
        const bool restored=(previewMediaPath_==item.path) || RestorePrefetchedPreviewSet(item.path);
        if(!restored){
            DeletePreviewFrameBitmaps(previewFrames_);
            previewFrames_.clear();
            previewMediaPath_=item.path;
            detailsDurationSeconds_.store(0.0,std::memory_order_relaxed);
        }

        // Rebuild only from B's existing deterministic timeline cache. Generation remains
        // deferred until the normal post-player worker resumes, so Back stays responsive.
        RefreshPreviewFrames();
        detailsDurationSeconds_.store(ReadCachedPreviewDuration(),std::memory_order_relaxed);
        if(PreviewCacheIsComplete()) QueueAllPreviewBitmapsForCurrentView();
    }

    void EnterPlayerAt(double startSeconds) {
        if(category_!=Category::Videos||selected_>=videos_.size()) return;
        StopHoverPreviewAudio();
        // Every newly opened video starts at the standard 30% volume.
        volumeFraction_ = 0.30;
        lastAudibleVolumeFraction_ = 0.30;
        RequestBackgroundStopForPlayback();

        // Initialize the reusable player once, then start the asynchronous media open as
        // early as possible. Do not spend the first decoder milliseconds building the
        // footer snapshot/layout while the engine is still idle at 00:00.
        if(!CreatePlayerControls()){mode_=Mode::Details;StartPreviewWorkerForSelected();QueueDetailPrefetchWindow();InvalidateRect(hwnd_,nullptr,TRUE);return;}
        const std::wstring openingPath=videos_[selected_].path;
        const HRESULT hr=player_->Open(openingPath,videos_[selected_].vr,std::max(0.0,startSeconds));
        if(FAILED(hr)){
            // Opening failed before entering Player mode. Restore the background workers
            // that were cancelled for playback priority instead of leaving Info dormant.
            if(player_) player_->CloseSource();
            StartPreviewWorkerForSelected();
            QueueDetailPrefetchWindow();
            StartThumbnailWorker();
            if(!PathExistsNoThrow(openingPath) || !IsLibraryRootAccessible()){
                NoteLibraryAccessFailure(true);
                ArmLibraryAccessMonitor(3000);
            }else{
                ShowInAppNotice(L"This media is unsupported.",5000);
            }
            return;
        }

        StartPlayerFooterTransitionSnapshot();
        mode_=Mode::Player;
        ApplyMainWindowCornerPreference();
        ShowWindow(videoHwnd_,SW_SHOW); playerControlsVisible_=false; controlsHideDeadline_=0; lastCursorValid_=false; controlsFading_=false; controlsAlpha_=0;
        if(controlsHwnd_) SetLayeredWindowAttributes(controlsHwnd_,0,controlsAlpha_,LWA_ALPHA);
        if(playerPrevHwnd_) SetLayeredWindowAttributes(playerPrevHwnd_,0,controlsAlpha_,LWA_ALPHA);
        if(playerNextHwnd_) SetLayeredWindowAttributes(playerNextHwnd_,0,controlsAlpha_,LWA_ALPHA);
        Layout();
        // Open() is asynchronous and already running. Trim Details/Library working sets
        // only after playback initialization has been queued.
        ReleaseDetailsGpuWorkingSet();
        TrimForPlayback();
        player_->SetVolume(volumeFraction_); SetFocus(videoHwnd_); UpdateWindowTitle();
        PlayerActivity(true);
        if(playerFooterTransitionHwnd_) SetWindowPos(playerFooterTransitionHwnd_,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }

    void LeavePlayer() {
        // Reverse the opening transition: keep the player controls at exactly their
        // current geometry and fade them out over the Info footer. No screenshot/capture
        // is needed on close, which also removes the short capture/join hitch.
        const double timelineReturnSeconds=player_?std::max(0.0,player_->CurrentTime()):0.0;
        DestroyPlayerFooterTransition();
        if(player_) player_->Pause();

        // Native Size stays latched while moving through videos (including intervening
        // VR videos), but leaving Player ends that session and returns to normal sizing.
        if(nativeVideoSizing_ || nativeSizingRestoreRectValid_){
            if(player_) player_->SetNativePixelSizing(false);
            nativeVideoSizing_=false;
            if(fullscreen_){
                // Leaving Player keeps fullscreen active. If fullscreen is later disabled,
                // restore the fixed standard app window rather than a user/native resize.
                SetFullscreenRestoreToStandardWindow();
            }else{
                RestoreStandardWindowSize();
            }
            nativeSizingRestoreRectValid_=false;
        }
        // Auto Next is a playback-session control. Back/Escape always starts the next
        // Player session with it off.
        autoNext_=false;
        // The source is shut down just after the reverse footer fade. Deferring the
        // Media Foundation shutdown keeps the close animation smooth while still
        // releasing the file handle within a fraction of a second.
        volumeFraction_=0.30;
        lastAudibleVolumeFraction_=0.30;
        controlsHideDeadline_=0;
        const bool fadePlayerControls=playerControlsVisible_ && controlsHwnd_ && IsWindowVisible(controlsHwnd_);
        if(videoHwnd_) ShowWindow(videoHwnd_,SW_HIDE);
        mode_=Mode::Details;
        // Playback navigation may have changed selected_ from A to B while the parked
        // Timeline still belongs to A. Switch the Details cache context before its first
        // repaint so the screen can never display A's timeline under B's selection.
        SyncDetailsTimelineToSelectedVideoFromCache();
        QueueTimelineReturnFocus(timelineReturnSeconds);
        SetWindowTextW(hwnd_,L"Visual MediaPlayer");
        ApplyMainWindowCornerPreference();
        InvalidateRect(hwnd_,nullptr,TRUE);
        UpdateWindow(hwnd_);
        if(fadePlayerControls) BeginControlsFade(0);
        else {
            playerControlsVisible_=false; controlsFading_=false; controlsAlpha_=0;
            if(controlsHwnd_) ShowWindow(controlsHwnd_,SW_HIDE);
            if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_HIDE);
            if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_HIDE);
        }
        // Existing Info bitmaps remain available immediately. Give cancelled background
        // workers time to exit before reaping/restarting them, outside the visual transition.
        SetTimer(hwnd_,kResumeDetailsWorkersTimerId,320,nullptr);
    }

    void BeginControlsFade(BYTE target) {
        if(!controlsHwnd_) return;
        UpdateControlsFade();
        controlsFadeFrom_=controlsAlpha_;
        controlsFadeTo_=target;
        controlsFadeStart_=GetTickCount64();
        controlsFading_=controlsFadeFrom_!=controlsFadeTo_;
        if(controlsFading_) StartUiAnimationTimer();
        if(target>0){
            if(!IsWindowVisible(controlsHwnd_)) ShowWindow(controlsHwnd_,SW_SHOWNOACTIVATE);
            if(playerPrevHwnd_ && !IsWindowVisible(playerPrevHwnd_)) ShowWindow(playerPrevHwnd_,SW_SHOWNOACTIVATE);
            if(playerNextHwnd_ && !IsWindowVisible(playerNextHwnd_)) ShowWindow(playerNextHwnd_,SW_SHOWNOACTIVATE);
        }
        if(!controlsFading_ && target==0){
            playerControlsVisible_=false; ShowWindow(controlsHwnd_,SW_HIDE);
            if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_HIDE);
            if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_HIDE);
        }
    }

    void UpdateControlsFade() {
        if(!controlsFading_ || !controlsHwnd_) return;
        const ULONGLONG now=GetTickCount64();
        const float raw=static_cast<float>(now-controlsFadeStart_) / static_cast<float>(kPlayerControlsFadeDurationMs);
        const float t=EaseUi(raw);
        const int value=static_cast<int>(std::lround(controlsFadeFrom_ + (static_cast<int>(controlsFadeTo_)-static_cast<int>(controlsFadeFrom_))*t));
        controlsAlpha_=static_cast<BYTE>(std::clamp(value,0,255));
        SetLayeredWindowAttributes(controlsHwnd_,0,controlsAlpha_,LWA_ALPHA);
        if(playerPrevHwnd_) SetLayeredWindowAttributes(playerPrevHwnd_,0,controlsAlpha_,LWA_ALPHA);
        if(playerNextHwnd_) SetLayeredWindowAttributes(playerNextHwnd_,0,controlsAlpha_,LWA_ALPHA);
        if(raw>=1.0f){
            controlsAlpha_=controlsFadeTo_; controlsFading_=false;
            SetLayeredWindowAttributes(controlsHwnd_,0,controlsAlpha_,LWA_ALPHA);
            if(playerPrevHwnd_) SetLayeredWindowAttributes(playerPrevHwnd_,0,controlsAlpha_,LWA_ALPHA);
            if(playerNextHwnd_) SetLayeredWindowAttributes(playerNextHwnd_,0,controlsAlpha_,LWA_ALPHA);
            if(controlsAlpha_==0){
                playerControlsVisible_=false; ShowWindow(controlsHwnd_,SW_HIDE);
                if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_HIDE);
                if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_HIDE);
            }
        }
    }

    void PlayerActivity(bool force) {
        if(mode_!=Mode::Player) return;
        POINT pt{}; GetCursorPos(&pt);
        if(!force&&lastCursorValid_&&pt.x==lastCursorScreen_.x&&pt.y==lastCursorScreen_.y) return;
        lastCursorScreen_=pt; lastCursorValid_=true; controlsHideDeadline_=GetTickCount64()+2200;
        if(!playerControlsVisible_){
            playerControlsVisible_=true; controlsAlpha_=0; controlsFading_=false;
            if(controlsHwnd_) SetLayeredWindowAttributes(controlsHwnd_,0,controlsAlpha_,LWA_ALPHA);
            if(playerPrevHwnd_) SetLayeredWindowAttributes(playerPrevHwnd_,0,controlsAlpha_,LWA_ALPHA);
            if(playerNextHwnd_) SetLayeredWindowAttributes(playerNextHwnd_,0,controlsAlpha_,LWA_ALPHA);
            Layout();
            if(controlsHwnd_) ShowWindow(controlsHwnd_,SW_SHOWNOACTIVATE);
            if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_SHOWNOACTIVATE);
            if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_SHOWNOACTIVATE);
            BeginControlsFade(kControlsVisibleAlpha); InvalidateControls();
        } else if(controlsFading_ && controlsFadeTo_==0) {
            BeginControlsFade(kControlsVisibleAlpha);
        }
    }

    void UpdatePlayerControlVisibility() {
        UpdateControlsFade();
        if(mode_!=Mode::Player||!playerControlsVisible_||seekDragging_||volumeDragging_) return;
        if(liveWindowMove_){
            // Resizing/moving the player is active interaction. Keep the footer stable
            // instead of allowing it to fade away halfway through a long border drag.
            controlsHideDeadline_=GetTickCount64()+2200;
            if(controlsFading_ && controlsFadeTo_==0) BeginControlsFade(kControlsVisibleAlpha);
            return;
        }
        if(controlsHideDeadline_&&GetTickCount64()>=controlsHideDeadline_){
            controlsHideDeadline_=0;
            if(!controlsFading_ || controlsFadeTo_!=0) BeginControlsFade(0);
        }
    }

    void InvalidatePlayerFooterControls() {
        if(controlsHwnd_&&playerControlsVisible_) InvalidateRect(controlsHwnd_,nullptr,FALSE);
    }

    void InvalidateControls() {
        InvalidatePlayerFooterControls();
        if(playerPrevHwnd_&&playerControlsVisible_) InvalidateRect(playerPrevHwnd_,nullptr,FALSE);
        if(playerNextHwnd_&&playerControlsVisible_) InvalidateRect(playerNextHwnd_,nullptr,FALSE);
    }

    bool FindAnimatedButtonRect(HWND owner, POINT p, RECT& out) const {
        auto hit=[&](const RECT& r)->bool{ if(!EmptyRectValue(r) && PtInRect(&r,p)){ out=r; return true; } return false; };
        if(owner==controlsHwnd_){
            if(hit(playerBackRect_)) return true;
            if(!IsRectEmpty(&volumeLabelRect_) && hit(volumeLabelRect_)) return true;
            if(player_ && player_->VR().vr && hit(playerVrToggleRect_)) return true;
            if(hit(playerSkipBackRect_)) return true;
            if(hit(playerPlayRect_)) return true;
            if(hit(playerSkipForwardRect_)) return true;
            if(hit(playerAutoNextRect_)) return true;
            if(NativeVideoSizingAvailable() && hit(playerNativeSizeRect_)) return true;
            if(hit(playerFullRect_)) return true;
            return false;
        }
        if(owner==playerPrevHwnd_ || owner==playerNextHwnd_){
            const int direction=owner==playerPrevHwnd_?-1:1;
            if(!CanNavigatePlayerMedia(direction)) return false;
            RECT client{}; GetClientRect(owner,&client);
            if(PtInRect(&client,p)){ out=client; return true; }
            return false;
        }
        if(owner!=hwnd_) return false;
        if(mode_==Mode::Library){
            if(!IsAtLibraryRoot() && hit(backRect_)) return true;
            if(hit(categoryToggleRect_)) return true;
            if(category_==Category::Images && hit(slideshowRect_)) return true;
            if(hit(libraryFullRect_)) return true;
            if(IsAtChosenLibraryRoot() && hit(loadEverythingRect_)) return true;
            if((IsAtChosenLibraryRoot() || currentFolder_.empty() || externalMediaSession_) && hit(chooseRect_)) return true;
            if(IsAtChosenLibraryRoot() && hit(rescanRect_)) return true;
        } else if(mode_==Mode::Details){
            if(CanNavigateDetailsMedia(-1) && hit(detailsPrevRect_)) return true;
            if(CanNavigateDetailsMedia(1) && hit(detailsNextRect_)) return true;
            if(hit(backRect_)) return true;
            if(category_==Category::Videos && hit(playRect_)) return true;
            if(category_==Category::Images && hit(imageDetailsSlideshowRect_)) return true;
            if(category_==Category::Images && hit(imageDetailsNativeRect_)) return true;
            if(hit(detailsFullRect_)) return true;
        }
        return false;
    }

    float MediaHoverAmount(MediaHoverSurface surface, size_t id, const RECT& visual) const {
        if(!IsAppForegroundForHover()) return 0.0f;
        if(mediaHoverSurface_!=surface || mediaHoverId_!=id || EmptyRectValue(visual)) return 0.0f;

        // Validate against the real cursor position as well as mouse messages. This
        // prevents a stale border after scrolling, resizing, view changes, or a missed
        // mouse-leave transition. Only the one currently tracked media item pays for
        // this check during painting.
        POINT cursor{};
        if(!GetCursorPos(&cursor)) return 0.0f;
        if(!ScreenToClient(hwnd_,&cursor) || !PtInRect(&visual,cursor)) return 0.0f;

        if(mediaHoverStart_==0) return 1.0f;
        const ULONGLONG elapsed=GetTickCount64()-mediaHoverStart_;
        return EaseUi(static_cast<float>(elapsed)/static_cast<float>(kMediaHoverFadeInMs));
    }

    void SetMediaHoverTarget(MediaHoverSurface nextSurface,size_t nextId,RECT nextRect,bool found) {
        if(!IsAppForegroundForHover()) found=false;
        if(!found){
            nextSurface=MediaHoverSurface::None;
            nextId=static_cast<size_t>(-1);
            nextRect=RECT{};
        }

        // Scrolling/zooming moves the card rectangle even when the cursor remains over
        // the same media item. Update that rectangle in place without restarting the
        // fade-in; restarting it on every wheel tick is what made the border flicker.
        if(nextSurface==mediaHoverSurface_ && nextId==mediaHoverId_){
            if(SameRect(nextRect,mediaHoverRect_)) return;
            const RECT oldRect=mediaHoverRect_;
            mediaHoverRect_=nextRect;
            if(!EmptyRectValue(oldRect)) InvalidateAnimatedRegion(hwnd_,oldRect);
            if(found && !EmptyRectValue(nextRect)) InvalidateAnimatedRegion(hwnd_,nextRect);
            return;
        }

        const RECT oldRect=mediaHoverRect_;
        mediaHoverSurface_=nextSurface;
        mediaHoverId_=nextId;
        mediaHoverRect_=nextRect;
        mediaHoverStart_=found?GetTickCount64():0;

        // There is deliberately no media fade-out state. The old border disappears
        // on the very next repaint, so rapid movement can never strand a highlighted
        // card. The new card still gets the requested subtle fade-in.
        if(!EmptyRectValue(oldRect)) InvalidateAnimatedRegion(hwnd_,oldRect);
        if(found){
            InvalidateAnimatedRegion(hwnd_,nextRect);
            StartUiAnimationTimer();
        } else if(libraryHoverPreviewItemId_!=static_cast<size_t>(-1) || libraryHoverPreviewLoadingId_!=static_cast<size_t>(-1)) {
            StartUiAnimationTimer();
        }
    }

    void UpdateMediaHover(int x,int y) {
        if(!IsAppForegroundForHover()){ ClearMediaHoverImmediate(); return; }
        POINT p{x,y};
        MediaHoverSurface nextSurface=MediaHoverSurface::None;
        size_t nextId=static_cast<size_t>(-1);
        RECT nextRect{};

        const std::vector<AnimatedMediaHit>* hits=nullptr;
        if(mode_==Mode::Library){
            nextSurface=MediaHoverSurface::Library;
            hits=&libraryMediaHoverHits_;
        } else if(mode_==Mode::Details && category_==Category::Videos){
            nextSurface=MediaHoverSurface::Preview;
            hits=&previewMediaHoverHits_;
        }

        bool found=false;
        if(hits){
            for(const auto& mediaHit:*hits){
                if(!EmptyRectValue(mediaHit.hit) && PtInRect(&mediaHit.hit,p)){
                    nextId=mediaHit.id;
                    nextRect=mediaHit.visual;
                    found=true;
                    break;
                }
            }
        }
        SetMediaHoverTarget(nextSurface,nextId,nextRect,found);
    }

    void ClearMediaHoverImmediate() {
        const RECT oldRect=mediaHoverRect_;
        mediaHoverSurface_=MediaHoverSurface::None;
        mediaHoverId_=static_cast<size_t>(-1);
        mediaHoverRect_=RECT{};
        mediaHoverStart_=0;
        if(!EmptyRectValue(oldRect)) InvalidateAnimatedRegion(hwnd_,oldRect);
        if(libraryHoverPreviewItemId_!=static_cast<size_t>(-1) || libraryHoverPreviewLoadingId_!=static_cast<size_t>(-1)) StartUiAnimationTimer();
    }

    void StartUiAnimationTimer() {
        if(hwnd_) SetTimer(hwnd_,kUiAnimationTimerId,16,nullptr);
    }

    void InvalidateAnimatedRegion(HWND owner, RECT r) {
        if(!owner || EmptyRectValue(r)) return;
        InflateRect(&r,3,3);
        InvalidateRect(owner,&r,FALSE);
    }

    bool IsEdgeArrowHoverTarget(HWND owner,const RECT& r) const {
        if(!owner || EmptyRectValue(r)) return false;
        if(owner==playerPrevHwnd_ || owner==playerNextHwnd_) return true;
        return owner==hwnd_ && mode_==Mode::Details &&
               (SameRect(r,detailsPrevRect_) || SameRect(r,detailsNextRect_));
    }

    void UpdateAnimatedHover(HWND owner,int x,int y) {
        if(!IsAppForegroundForHover()){ ResetAnimatedHoverImmediate(owner); return; }
        POINT p{x,y}; RECT next{}; HWND nextOwner=nullptr;
        if(FindAnimatedButtonRect(owner,p,next)) nextOwner=owner;
        if(nextOwner==hoverOwner_ && SameRect(next,hoverRect_)) return;
        const HWND oldOwner=hoverOwner_;
        const RECT oldRect=hoverRect_;
        const HWND stalePreviousOwner=hoverPreviousOwner_;
        const RECT stalePreviousRect=hoverPreviousRect_;

        // The large Previous/Next edge controls used to run the generic 160 ms hover
        // animation. In Details that repainted the full D2D surface every 16 ms; in
        // Player it repeatedly repainted a layered popup over the video. Both can show
        // as a brief flicker. Edge arrows switch hover state atomically instead.
        const bool instantEdgeTransition=IsEdgeArrowHoverTarget(oldOwner,oldRect) ||
                                         IsEdgeArrowHoverTarget(nextOwner,next);
        if(instantEdgeTransition){
            hoverPreviousOwner_=nullptr; hoverPreviousRect_=RECT{};
            hoverOwner_=nextOwner; hoverRect_=next; hoverTransitionStart_=0;
            InvalidateAnimatedRegion(stalePreviousOwner,stalePreviousRect);
            InvalidateAnimatedRegion(oldOwner,oldRect);
            InvalidateAnimatedRegion(hoverOwner_,hoverRect_);
            return;
        }

        hoverPreviousOwner_=hoverOwner_; hoverPreviousRect_=hoverRect_;
        hoverOwner_=nextOwner; hoverRect_=next; hoverTransitionStart_=GetTickCount64();
        StartUiAnimationTimer();
        // If a second transition begins before the first fade-out finishes, repaint
        // the displaced previous rectangle once so it can never remain stranded.
        InvalidateAnimatedRegion(stalePreviousOwner,stalePreviousRect);
        InvalidateAnimatedRegion(oldOwner,oldRect);
        InvalidateAnimatedRegion(hoverOwner_,hoverRect_);
    }

    void ClearAnimatedHover(HWND owner) {
        if(hoverOwner_!=owner) return;
        const RECT oldRect=hoverRect_;
        const HWND stalePreviousOwner=hoverPreviousOwner_;
        const RECT stalePreviousRect=hoverPreviousRect_;
        if(IsEdgeArrowHoverTarget(owner,oldRect)){
            hoverOwner_=nullptr; hoverRect_=RECT{};
            hoverPreviousOwner_=nullptr; hoverPreviousRect_=RECT{};
            hoverTransitionStart_=0;
            InvalidateAnimatedRegion(stalePreviousOwner,stalePreviousRect);
            InvalidateAnimatedRegion(owner,oldRect);
            return;
        }
        hoverPreviousOwner_=hoverOwner_; hoverPreviousRect_=hoverRect_;
        hoverOwner_=nullptr; hoverRect_=RECT{}; hoverTransitionStart_=GetTickCount64();
        StartUiAnimationTimer();
        InvalidateAnimatedRegion(stalePreviousOwner,stalePreviousRect);
        InvalidateAnimatedRegion(owner,oldRect);
    }

    void ResetAnimatedHoverImmediate(HWND owner) {
        if(!owner) return;
        RECT current{},previous{};
        bool hadCurrent=false,hadPrevious=false;
        if(hoverOwner_==owner){ current=hoverRect_; hadCurrent=!EmptyRectValue(current); }
        if(hoverPreviousOwner_==owner){ previous=hoverPreviousRect_; hadPrevious=!EmptyRectValue(previous); }
        if(!hadCurrent && !hadPrevious && hoverOwner_!=owner && hoverPreviousOwner_!=owner) return;
        if(hoverOwner_==owner){ hoverOwner_=nullptr; hoverRect_=RECT{}; }
        if(hoverPreviousOwner_==owner){ hoverPreviousOwner_=nullptr; hoverPreviousRect_=RECT{}; }
        hoverTransitionStart_=0;
        if(hadCurrent) InvalidateAnimatedRegion(owner,current);
        if(hadPrevious && (!hadCurrent || !SameRect(previous,current))) InvalidateAnimatedRegion(owner,previous);
    }

    void TickUiAnimations() {
        const ULONGLONG now=GetTickCount64();
        bool active=false;
        UpdatePlayerFooterTransition(now,active);
        if(controlsFading_){
            UpdateControlsFade();
            if(controlsFading_) active=true;
        }
        HWND expiredHoverOwner=nullptr;
        RECT expiredHoverRect{};
        if(hoverTransitionStart_!=0){
            if(now-hoverTransitionStart_>=kUiAnimationDurationMs){
                expiredHoverOwner=hoverPreviousOwner_;
                expiredHoverRect=hoverPreviousRect_;
                hoverTransitionStart_=0; hoverPreviousOwner_=nullptr; hoverPreviousRect_=RECT{};
            } else active=true;
        }

        if(mediaHoverSurface_!=MediaHoverSurface::None && mediaHoverStart_!=0){
            if(now-mediaHoverStart_>=kMediaHoverFadeInMs){
                mediaHoverStart_=0;
                if(!EmptyRectValue(mediaHoverRect_)) InvalidateAnimatedRegion(hwnd_,mediaHoverRect_);
            } else {
                active=true;
                if(!EmptyRectValue(mediaHoverRect_)) InvalidateAnimatedRegion(hwnd_,mediaHoverRect_);
            }
        }

        bool repaintSlideshow=false;
        if(slideshowFadeActive_){
            repaintSlideshow=true;
            if(now-slideshowFadeStart_>=kUiAnimationDurationMs){
                slideshowFadeActive_=false; slideshowPreviousIndex_=static_cast<size_t>(-1);
            } else active=true;
        }

        bool returnHighlightActive=false;
        RECT expiredReturnRect{};
        if(libraryReturnHighlightStart_!=0){
            if(now-libraryReturnHighlightStart_>=kLibraryReturnHighlightDurationMs){
                expiredReturnRect=libraryReturnHighlightRect_;
                libraryReturnHighlightStart_=0;
                libraryReturnHighlightIndex_=static_cast<size_t>(-1);
                libraryReturnHighlightRect_=RECT{};
            } else {
                active=true;
                returnHighlightActive=true;
            }
        }

        bool timelineReturnHighlightActive=false;
        RECT expiredTimelineReturnRect{};
        if(timelineReturnHighlightStart_!=0){
            if(now-timelineReturnHighlightStart_>=kLibraryReturnHighlightDurationMs){
                expiredTimelineReturnRect=timelineReturnHighlightRect_;
                timelineReturnHighlightStart_=0;
                timelineReturnHighlightIndex_=static_cast<size_t>(-1);
                timelineReturnHighlightRect_=RECT{};
            } else {
                active=true;
                timelineReturnHighlightActive=true;
            }
        }

        if(!appNoticeText_.empty() && appNoticeStart_!=0){
            if(appNoticeUntil_==0 || now>=appNoticeUntil_){
                RECT expiredNoticeRect{};
                if(hwnd_){
                    RECT noticeClient{}; GetClientRect(hwnd_,&noticeClient);
                    expiredNoticeRect=AppNoticeRect(noticeClient);
                }
                appNoticeText_.clear();
                appNoticeUntil_=0;
                appNoticeStart_=0;
                if(hwnd_){
                    KillTimer(hwnd_,kAppNoticeTimerId);
                    if(!EmptyRectValue(expiredNoticeRect)) InvalidateRect(hwnd_,&expiredNoticeRect,FALSE);
                }
            }else{
                const ULONGLONG noticeElapsed=now-appNoticeStart_;
                if(noticeElapsed<kAppNoticePulseDurationMs) active=true;
                if(hwnd_){
                    // Repaint the notice for its full five-second pulse lifetime.
                    RECT noticeClient{}; GetClientRect(hwnd_,&noticeClient);
                    RECT noticeRect=AppNoticeRect(noticeClient);
                    InvalidateRect(hwnd_,&noticeRect,FALSE);
                }
            }
        }

        if(fullLoadFinishedAt_!=0){
            if(now-fullLoadFinishedAt_>=FullLoadDonePopupDuration()){
                fullLoadFinishedAt_=0;
                InvalidateLoadingPopupArea();
            } else {
                active=true;
            }
        }

        UpdateLibraryHoverPreview(now,active);

        // Main-window hover animations repaint only their cards/buttons. This keeps the
        // new media hover effect from undoing the optimized Library scrolling pipeline.
        if(repaintSlideshow && hwnd_) InvalidateRect(hwnd_,nullptr,FALSE);
        else {
            if(hoverOwner_==hwnd_) InvalidateAnimatedRegion(hwnd_,hoverRect_);
            if(hoverPreviousOwner_==hwnd_) InvalidateAnimatedRegion(hwnd_,hoverPreviousRect_);
            if(expiredHoverOwner==hwnd_) InvalidateAnimatedRegion(hwnd_,expiredHoverRect);
            if(returnHighlightActive && mode_==Mode::Library){
                if(!EmptyRectValue(libraryReturnHighlightRect_)) InvalidateAnimatedRegion(hwnd_,libraryReturnHighlightRect_);
                else InvalidateLibraryScrollableArea();
            }
            if(timelineReturnHighlightActive && mode_==Mode::Details && category_==Category::Videos){
                if(!EmptyRectValue(timelineReturnHighlightRect_)) InvalidateAnimatedRegion(hwnd_,timelineReturnHighlightRect_);
                else InvalidateRect(hwnd_,nullptr,FALSE);
            }
            if(!EmptyRectValue(expiredReturnRect) && hwnd_) InvalidateAnimatedRegion(hwnd_,expiredReturnRect);
            if(!EmptyRectValue(expiredTimelineReturnRect) && hwnd_) InvalidateAnimatedRegion(hwnd_,expiredTimelineReturnRect);
        }

        const bool controlsHoverActive=(hoverOwner_==controlsHwnd_ || hoverPreviousOwner_==controlsHwnd_ || expiredHoverOwner==controlsHwnd_);
        const bool prevHoverActive=(hoverOwner_==playerPrevHwnd_ || hoverPreviousOwner_==playerPrevHwnd_ || expiredHoverOwner==playerPrevHwnd_);
        const bool nextHoverActive=(hoverOwner_==playerNextHwnd_ || hoverPreviousOwner_==playerNextHwnd_ || expiredHoverOwner==playerNextHwnd_);
        if(controlsHwnd_ && playerControlsVisible_ && controlsHoverActive) InvalidateRect(controlsHwnd_,nullptr,FALSE);
        if(playerPrevHwnd_ && playerControlsVisible_ && prevHoverActive) InvalidateRect(playerPrevHwnd_,nullptr,FALSE);
        if(playerNextHwnd_ && playerControlsVisible_ && nextHoverActive) InvalidateRect(playerNextHwnd_,nullptr,FALSE);
        if(!active && hwnd_) KillTimer(hwnd_,kUiAnimationTimerId);
    }

    static bool FindAdjacentInSameFolder(const std::vector<MediaItem>& items, size_t current, int direction, size_t& target) {
        if(current>=items.size() || direction==0) return false;
        const std::wstring folderKey=ToLower(fs::path(items[current].path).parent_path().lexically_normal().wstring());
        if(direction<0){
            for(size_t i=current;i>0;){
                --i;
                const std::wstring parentKey=ToLower(fs::path(items[i].path).parent_path().lexically_normal().wstring());
                if(parentKey==folderKey){ target=i; return true; }
            }
        } else {
            for(size_t i=current+1;i<items.size();++i){
                const std::wstring parentKey=ToLower(fs::path(items[i].path).parent_path().lexically_normal().wstring());
                if(parentKey==folderKey){ target=i; return true; }
            }
        }
        return false;
    }

    bool FindAdjacentMedia(const std::vector<MediaItem>& items, size_t current, int direction, size_t& target) const {
        if(!externalMediaSession_) return FindAdjacentInSameFolder(items,current,direction,target);
        if(current>=items.size() || direction==0) return false;
        if(direction<0){ if(current==0) return false; target=current-1; return true; }
        if(current+1>=items.size()) return false;
        target=current+1; return true;
    }

    bool FindAdjacentDetailsMedia(size_t current, int direction, size_t& target) const {
        if(detailsSearchNavigationActive_ && !detailsSearchNavigationIndices_.empty()){
            const auto it=std::find(detailsSearchNavigationIndices_.begin(),detailsSearchNavigationIndices_.end(),current);
            if(it==detailsSearchNavigationIndices_.end()) return false;
            if(direction<0){
                if(it==detailsSearchNavigationIndices_.begin()) return false;
                target=*(it-1); return true;
            }
            if(direction>0){
                const auto next=it+1;
                if(next==detailsSearchNavigationIndices_.end()) return false;
                target=*next; return true;
            }
            return false;
        }
        return FindAdjacentMedia(CurrentItems(),current,direction,target);
    }

    bool CanNavigateDetailsMedia(int direction) const {
        if(mode_!=Mode::Details) return false;
        size_t target=0; return FindAdjacentDetailsMedia(selected_,direction,target);
    }

    bool CanNavigatePlayerMedia(int direction) const {
        if(mode_!=Mode::Player || category_!=Category::Videos) return false;
        size_t target=0; return FindAdjacentDetailsMedia(selected_,direction,target);
    }

    void NavigateDetailsMedia(int direction) {
        if(mode_!=Mode::Details) return;
        auto& items=CurrentItems(); size_t target=0;
        if(!FindAdjacentDetailsMedia(selected_,direction,target)) return;
        StopImageSlideshow();
        thumbStop_.store(true,std::memory_order_release); ClearLoadingStateIf(1);
        if(category_==Category::Videos) StopPreviewWorker();
        selected_=target; detailsScrollY_=0; ResetPreviewZoom();
        if(category_==Category::Images){
            ResetImageZoom();
            if(nativeImageSizing_ && !fullscreen_) ApplyNativeImageWindowSize();
        }
        if(selected_<items.size()) SaveCurrentFolderViewState(items[selected_].path);
        if(category_==Category::Videos) StartPreviewWorkerForSelected();
        QueueDetailPrefetchWindow();
        InvalidateRect(hwnd_,nullptr,FALSE);
    }

    void NavigatePlayerMedia(int direction) {
        if(mode_!=Mode::Player || !player_) return;
        size_t target=0; if(!FindAdjacentDetailsMedia(selected_,direction,target)) return;
        const size_t oldSelected=selected_;
        const double oldTime=player_->CurrentTime();
        const double oldVolume=volumeFraction_;
        const double oldLastAudibleVolume=lastAudibleVolumeFraction_;
        const bool oldPaused=player_->IsPaused();
        CancelPlayerSliderDrag();
        // Previous/Next stays inside the current Player session. Keep both the live
        // volume and remembered unmute level while changing only the selected media.
        selected_=target; seekFraction_=0.0;
        const std::wstring targetPath=videos_[selected_].path;
        const HRESULT hr=player_->Open(targetPath,videos_[selected_].vr,0.0);
        if(FAILED(hr)){
            const bool mediaMissing=!PathExistsNoThrow(targetPath) || !IsLibraryRootAccessible();
            selected_=oldSelected; volumeFraction_=oldVolume; lastAudibleVolumeFraction_=oldLastAudibleVolume;
            if(!mediaMissing){
                player_->Open(videos_[selected_].path,videos_[selected_].vr,std::max(0.0,oldTime));
                player_->SetVolume(volumeFraction_);
                if(oldPaused) player_->Pause();
                LeavePlayer();
                ShowInAppNotice(L"This media is unsupported.",5000);
            }else{
                if(player_) player_->Pause();
                NoteLibraryAccessFailure(true);
                ArmLibraryAccessMonitor(3000);
            }
            return;
        }
        SaveCurrentFolderViewState(videos_[selected_].path);
        player_->SetVolume(volumeFraction_); UpdateWindowTitle();
        if(nativeVideoSizing_ && videos_[selected_].vr.vr) SuspendNativeVideoSizingForVr();
        Layout();
        if(playerControlsVisible_) controlsHideDeadline_=GetTickCount64()+2200;
        InvalidateControls();
    }

    bool NextVideoInCurrentFolder(size_t current, size_t& next) const {
        // Auto Next follows the same context as the visible Previous/Next controls.
        // When Player was entered from filtered search results, advance through that
        // ordered result set; otherwise retain normal same-folder/library behavior.
        return FindAdjacentDetailsMedia(current,1,next);
    }

    bool CurrentVideoIsAtEnd() const {
        if(mode_!=Mode::Player || !player_) return false;
        const double duration=player_->Duration();
        if(!(duration>0.0)) return false;
        const double current=player_->CurrentTime();
        // Media Foundation can report the final timestamp a few milliseconds shy of
        // Duration(). Treat the last quarter second as completed so enabling Auto Next
        // after the ENDED event still advances immediately.
        return current >= std::max(0.0,duration-0.25);
    }

    void HandlePlaybackEnded() {
        if(!autoNext_||videos_.empty()) return;
        size_t next=0; if(!NextVideoInCurrentFolder(selected_,next)) return;
        selected_=next; seekFraction_=0.0;
        SaveCurrentFolderViewState(videos_[selected_].path);
        if(player_){
            // Auto Next stays inside the current playback session, so carry the user's
            // current volume into the next video. LeavePlayer() resets the next session to 30%.
            const std::wstring targetPath=videos_[selected_].path;
            const HRESULT hr=player_->Open(targetPath,videos_[selected_].vr);
            if(FAILED(hr)){
                if(!PathExistsNoThrow(targetPath) || !IsLibraryRootAccessible()){
                    if(player_) player_->Pause();
                    NoteLibraryAccessFailure(true);
                    ArmLibraryAccessMonitor(3000);
                }else{
                    LeavePlayer();
                    ShowInAppNotice(L"This media is unsupported.",5000);
                }
                return;
            }
            player_->SetVolume(volumeFraction_); UpdateWindowTitle();
            if(nativeVideoSizing_ && videos_[selected_].vr.vr) SuspendNativeVideoSizingForVr();
            Layout(); InvalidateControls();
        }
    }

    void RepositionPlayerOverlayWindows() {
        // controlsHwnd_ and the edge arrows are owned WS_POPUP windows rather than
        // children of the player. Windows therefore does not move them with hwnd_ while
        // the user is inside the modal move/size loop. Keep their existing sizes and
        // follow the owner in screen coordinates without touching the D3D video child or
        // its swap-chain buffers. This preserves smooth live moving and prevents the
        // controls from being stranded at the old position until WM_EXITSIZEMOVE.
        if(mode_!=Mode::Player || !hwnd_) return;

        RECT client{}; GetClientRect(hwnd_,&client);
        const int cw=std::max(1,static_cast<int>(client.right-client.left));
        const int ch=std::max(1,static_cast<int>(client.bottom-client.top));
        POINT origin{0,0}; ClientToScreen(hwnd_,&origin);

        if(controlsHwnd_){
            RECT wr{}; GetWindowRect(controlsHwnd_,&wr);
            const int ow=std::max(1,static_cast<int>(wr.right-wr.left));
            const int oh=std::max(1,static_cast<int>(wr.bottom-wr.top));
            SetWindowPos(controlsHwnd_,HWND_TOP,origin.x,origin.y+std::max(0,ch-oh),ow,oh,
                         SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        }

        auto moveEdge=[&](HWND edge,bool rightSide){
            if(!edge) return;
            RECT wr{}; GetWindowRect(edge,&wr);
            const int ew=std::max(1,static_cast<int>(wr.right-wr.left));
            const int eh=std::max(1,static_cast<int>(wr.bottom-wr.top));
            constexpr int edgePad=18;
            const int x=rightSide
                ? origin.x+std::max(edgePad,cw-edgePad-ew)
                : origin.x+edgePad;
            const int y=origin.y+std::max(0,(ch-eh)/2);
            SetWindowPos(edge,HWND_TOP,x,y,ew,eh,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        };
        moveEdge(playerPrevHwnd_,false);
        moveEdge(playerNextHwnd_,true);

        // The short footer cross-fade is another owned popup and can overlap a move if
        // the user grabs the title bar immediately after opening a video. Move that
        // snapshot with the owner as well so no transient overlay is left behind.
        if(playerFooterTransitionHwnd_){
            RECT wr{}; GetWindowRect(playerFooterTransitionHwnd_,&wr);
            const int ow=std::max(1,static_cast<int>(wr.right-wr.left));
            const int oh=std::max(1,static_cast<int>(wr.bottom-wr.top));
            SetWindowPos(playerFooterTransitionHwnd_,HWND_TOP,origin.x,origin.y+std::max(0,ch-oh),ow,oh,
                         SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        }
    }

    void Layout() {
        if(mode_!=Mode::Player||!videoHwnd_) return; RECT rc{}; GetClientRect(hwnd_,&rc);
        const int cw=std::max(1,static_cast<int>(rc.right-rc.left)),ch=std::max(1,static_cast<int>(rc.bottom-rc.top));
        // Do not ask USER32 to repaint/erase the D3D child. Its WM_SIZE handler resizes
        // the swap chain and immediately presents the retained frame itself.
        MoveWindow(videoHwnd_,0,0,cw,ch,FALSE);
        if(!controlsHwnd_) return;

        // Below this width the old single-row footer could make Auto Next / Native /
        // Fullscreen collide with transport and volume controls. Switch to a taller,
        // deliberately separated layout instead of allowing any rectangles to overlap.
        const bool compactControls = cw < 760;
        constexpr int hoverStrip = 28;
        const int oh=(compactControls?200:122)+hoverStrip;
        POINT clientOrigin{0,0}; ClientToScreen(hwnd_,&clientOrigin);
        const int overlayX=clientOrigin.x;
        const int overlayY=clientOrigin.y+std::max(0,ch-oh);
        SetWindowPos(controlsHwnd_,HWND_TOP,overlayX,overlayY,cw,oh,SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        ApplyPlayerOverlayCornerRegion(cw,oh);

        constexpr int edgeW=48,edgeH=76,edgePad=18;
        const int edgeY=clientOrigin.y+std::max(0,(ch-edgeH)/2);
        if(playerPrevHwnd_){
            SetWindowPos(playerPrevHwnd_,HWND_TOP,clientOrigin.x+edgePad,edgeY,edgeW,edgeH,SWP_NOACTIVATE|SWP_NOOWNERZORDER);
            HRGN region=CreateRoundRectRgn(0,0,edgeW+1,edgeH+1,12,12); SetWindowRgn(playerPrevHwnd_,region,FALSE);
        }
        if(playerNextHwnd_){
            SetWindowPos(playerNextHwnd_,HWND_TOP,clientOrigin.x+std::max(edgePad,cw-edgePad-edgeW),edgeY,edgeW,edgeH,SWP_NOACTIVATE|SWP_NOOWNERZORDER);
            HRGN region=CreateRoundRectRgn(0,0,edgeW+1,edgeH+1,12,12); SetWindowRgn(playerNextHwnd_,region,FALSE);
        }

        if(!playerControlsVisible_){
            ShowWindow(controlsHwnd_,SW_HIDE);
            if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_HIDE);
            if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_HIDE);
            return;
        }
        ShowWindow(controlsHwnd_,SW_SHOWNOACTIVATE);
        if(playerPrevHwnd_) ShowWindow(playerPrevHwnd_,SW_SHOWNOACTIVATE);
        if(playerNextHwnd_) ShowWindow(playerNextHwnd_,SW_SHOWNOACTIVATE);

        if(compactControls){
            const int sidePad=std::clamp(cw/30,8,12);
            seekRect_={sidePad,10+hoverStrip,std::max(sidePad+1,cw-sidePad),30+hoverStrip};
            playerTimeRect_={std::max(sidePad,cw/2-110),32+hoverStrip,std::min(cw-sidePad,cw/2+110),53+hoverStrip};

            // Dedicated transport row.
            const int playSize=54,skipSize=48,transportGap=10;
            const int playTop=56+hoverStrip;
            playerPlayRect_={cw/2-playSize/2,playTop,cw/2+(playSize-playSize/2),playTop+playSize};
            playerSkipBackRect_={playerPlayRect_.left-transportGap-skipSize,playTop+3,playerPlayRect_.left-transportGap,playTop+3+skipSize};
            playerSkipForwardRect_={playerPlayRect_.right+transportGap,playTop+3,playerPlayRect_.right+transportGap+skipSize,playTop+3+skipSize};

            // Volume gets its own row so it cannot be squeezed between transport and toggles.
            const int volumeAvailable=std::max(1,cw-sidePad*2);
            const int desiredVolumeW=std::max(60,std::min(190,cw-86));
            const int volumeW=std::min(volumeAvailable,desiredVolumeW);
            const int volumeLeft=(cw-volumeW)/2;
            volumeRect_={volumeLeft,121+hoverStrip,volumeLeft+volumeW,139+hoverStrip};
            if(cw>=390) {
                constexpr int volumeButtonSize=48;
                constexpr int volumeButtonGap=12;
                const int volumeCenterY=(volumeRect_.top+volumeRect_.bottom)/2;
                volumeLabelRect_={volumeLeft-volumeButtonGap-volumeButtonSize,volumeCenterY-volumeButtonSize/2,
                    volumeLeft-volumeButtonGap,volumeCenterY+volumeButtonSize/2};
            } else volumeLabelRect_=RECT{};

            // Bottom action row: Back/VR stay on the left; session toggles stay on the right.
            // The groups are sized from the edges inward, so even a native-sized narrow
            // player cannot paint one button on top of another.
            const int iconSize=(cw<300?44:48);
            const int iconGap=(cw<300?5:6);
            const int iconBottom=oh-10;
            const int iconTop=iconBottom-iconSize;
            playerBackRect_={sidePad,iconTop,sidePad+iconSize,iconBottom};
            if(player_ && player_->VR().vr){
                playerVrToggleRect_={playerBackRect_.right+iconGap,iconTop,playerBackRect_.right+iconGap+iconSize,iconBottom};
            }else playerVrToggleRect_=RECT{};

            playerFullRect_={cw-sidePad-iconSize,iconTop,cw-sidePad,iconBottom};
            int rightCursor=playerFullRect_.left-iconGap;
            if(NativeVideoSizingAvailable()){
                playerNativeSizeRect_={rightCursor-iconSize,iconTop,rightCursor,iconBottom};
                rightCursor=playerNativeSizeRect_.left-iconGap;
            }else playerNativeSizeRect_=RECT{};
            playerAutoNextRect_={rightCursor-iconSize,iconTop,rightCursor,iconBottom};
        }else{
            const int pad=20; seekRect_={pad,10+hoverStrip,cw-pad,30+hoverStrip};
            constexpr int rowDrop = 10;
            playerBackRect_={20,54+rowDrop+hoverStrip,68,102+rowDrop+hoverStrip};
            playerVrToggleRect_={playerBackRect_.right+12,playerBackRect_.top,playerBackRect_.right+12+48,playerBackRect_.bottom};
            playerPlayRect_={cw/2-27,50+rowDrop+hoverStrip,cw/2+27,104+rowDrop+hoverStrip};
            constexpr int skipSize=48;
            constexpr int skipGap=12;
            playerSkipBackRect_={playerPlayRect_.left-skipGap-skipSize,54+rowDrop+hoverStrip,playerPlayRect_.left-skipGap,102+rowDrop+hoverStrip};
            playerSkipForwardRect_={playerPlayRect_.right+skipGap,54+rowDrop+hoverStrip,playerPlayRect_.right+skipGap+skipSize,102+rowDrop+hoverStrip};
            playerTimeRect_={cw/2-120,32+hoverStrip,cw/2+120,56+hoverStrip};
            playerFullRect_={cw-pad-48,54+rowDrop+hoverStrip,cw-pad,102+rowDrop+hoverStrip};
            if(NativeVideoSizingAvailable()){
                playerNativeSizeRect_={playerFullRect_.left-58,54+rowDrop+hoverStrip,playerFullRect_.left-10,102+rowDrop+hoverStrip};
                playerAutoNextRect_={playerNativeSizeRect_.left-58,54+rowDrop+hoverStrip,playerNativeSizeRect_.left-10,102+rowDrop+hoverStrip};
            }else{
                playerNativeSizeRect_=RECT{};
                playerAutoNextRect_={playerFullRect_.left-58,54+rowDrop+hoverStrip,playerFullRect_.left-10,102+rowDrop+hoverStrip};
            }
            const int volumeRight=playerAutoNextRect_.left-18;
            const int volumeLeft=std::max(static_cast<int>(playerSkipForwardRect_.right)+18,volumeRight-190);
            volumeRect_={volumeLeft,68+rowDrop+hoverStrip,volumeRight,86+rowDrop+hoverStrip};
            constexpr int volumeButtonSize=48;
            constexpr int volumeButtonGap=12;
            const int volumeCenterY=(volumeRect_.top+volumeRect_.bottom)/2;
            volumeLabelRect_={volumeLeft-volumeButtonGap-volumeButtonSize,volumeCenterY-volumeButtonSize/2,
                volumeLeft-volumeButtonGap,volumeCenterY+volumeButtonSize/2};
        }
        InvalidateControls();
    }

    void UpdateSeekHover(int x, int) {
        // Timestamp is a seek-drag indicator only: never show it on hover.
        if (!seekDragging_) {
            if (seekHoverVisible_) {
                seekHoverVisible_ = false;
                InvalidatePlayerFooterControls();
            }
            return;
        }
        const int newX = std::clamp(x, static_cast<int>(seekRect_.left), static_cast<int>(seekRect_.right));
        if (!seekHoverVisible_ || newX != seekHoverX_) {
            seekHoverVisible_ = true;
            seekHoverX_ = newX;
            InvalidatePlayerFooterControls();
        }
    }

    void ClearSeekHover() {
        if (!seekHoverVisible_) return;
        seekHoverVisible_ = false;
        InvalidatePlayerFooterControls();
    }

    void SetSeekFromX(int x,bool commit) {
        if(!player_) return; const int width=std::max(1,static_cast<int>(seekRect_.right-seekRect_.left));
        const int cx=std::clamp(x,static_cast<int>(seekRect_.left),static_cast<int>(seekRect_.right)); seekFraction_=static_cast<double>(cx-seekRect_.left)/width;
        if(commit){const double d=player_->Duration();if(d>0.0)player_->Seek(d*seekFraction_);} InvalidatePlayerFooterControls();
    }

    void SetVolumeFromX(int x) {
        if(!player_) return;
        const int width=std::max(1,static_cast<int>(volumeRect_.right-volumeRect_.left));
        const int cx=std::clamp(x,static_cast<int>(volumeRect_.left),static_cast<int>(volumeRect_.right));
        const double nextVolume=static_cast<double>(cx-volumeRect_.left)/width;
        if(std::abs(nextVolume-volumeFraction_)<0.000001) return;
        volumeFraction_=nextVolume;
        if(volumeFraction_>0.001) lastAudibleVolumeFraction_=volumeFraction_;
        player_->SetVolume(volumeFraction_);
        InvalidatePlayerFooterControls();
    }

    void TogglePlayerMute() {
        if(!player_) return;
        if(volumeFraction_>0.001){
            lastAudibleVolumeFraction_=volumeFraction_;
            volumeFraction_=0.0;
        }else{
            volumeFraction_=std::clamp(lastAudibleVolumeFraction_,0.0,1.0);
            if(volumeFraction_<=0.001) volumeFraction_=0.30;
            lastAudibleVolumeFraction_=volumeFraction_;
        }
        player_->SetVolume(volumeFraction_);
        InvalidatePlayerFooterControls();
    }

    void SkipPlaybackSeconds(double deltaSeconds) {
        if(!player_) return;
        const double duration=player_->Duration();
        double target=player_->CurrentTime()+deltaSeconds;
        if(duration>0.0) target=std::clamp(target,0.0,duration);
        else target=std::max(0.0,target);
        player_->Seek(target);
        if(duration>0.0) seekFraction_=std::clamp(target/duration,0.0,1.0);
        InvalidateControls();
    }

    void CancelPlayerSliderDrag() {
        const bool changed=seekDragging_||volumeDragging_||seekHoverVisible_;
        seekDragging_=false; volumeDragging_=false; seekHoverVisible_=false;
        if(changed) InvalidatePlayerFooterControls();
    }

    void PlayerMouseDown(int x,int y) {
        POINT p{x,y}; if(PtInRect(&seekRect_,p)){seekDragging_=true;UpdateSeekHover(x,y);SetSeekFromX(x,false);SetCapture(controlsHwnd_);return;}
        if(PtInRect(&volumeRect_,p)){volumeDragging_=true;SetVolumeFromX(x);SetCapture(controlsHwnd_);return;}
    }

    void PlayerMouseMove(int x,int y) {
        if(seekDragging_){ UpdateSeekHover(x,y); SetSeekFromX(x,false); }
        if(volumeDragging_)SetVolumeFromX(x);
    }

    void PlayerMouseUp(int x,int y) {
        POINT p{x,y}; PlayerActivity(true);
        if(seekDragging_){SetSeekFromX(x,true);seekDragging_=false;if(GetCapture()==controlsHwnd_)ReleaseCapture();ClearSeekHover();return;}
        if(volumeDragging_){SetVolumeFromX(x);volumeDragging_=false;if(GetCapture()==controlsHwnd_)ReleaseCapture();InvalidatePlayerFooterControls();return;}
        if(!IsRectEmpty(&volumeLabelRect_) && PtInRect(&volumeLabelRect_,p)){TogglePlayerMute();return;}
        if(PtInRect(&playerBackRect_,p)){LeavePlayer();return;}
        if(player_ && player_->VR().vr && PtInRect(&playerVrToggleRect_,p)){player_->ToggleVrBackside();InvalidateControls();return;}
        if(PtInRect(&playerSkipBackRect_,p)){SkipPlaybackSeconds(-30.0);return;}
        if(PtInRect(&playerPlayRect_,p)){if(player_)player_->PlayPause();InvalidateControls();return;}
        if(PtInRect(&playerSkipForwardRect_,p)){SkipPlaybackSeconds(30.0);return;}
        if(PtInRect(&playerAutoNextRect_,p)){
            const bool enabling=!autoNext_;
            autoNext_=enabling;
            InvalidateControls();
            if(enabling && CurrentVideoIsAtEnd()) HandlePlaybackEnded();
            return;
        }
        if(NativeVideoSizingAvailable() && PtInRect(&playerNativeSizeRect_,p)){ToggleNativeVideoSizing();PlayerActivity(true);return;}
        if(PtInRect(&playerFullRect_,p)){ToggleFullscreen();PlayerActivity(true);return;}
        if(PtInRect(&seekRect_,p)){SetSeekFromX(x,true);return;}
        if(PtInRect(&volumeRect_,p)){SetVolumeFromX(x);return;}
    }

    void UpdateSeekUi() {
        static ULONGLONG last=0; const ULONGLONG now=GetTickCount64(); if(now-last<100)return;last=now;
        if(!player_)return;const double d=player_->Duration(),t=player_->CurrentTime();if(d>0.0&&!seekDragging_)seekFraction_=std::clamp(t/d,0.0,1.0);InvalidatePlayerFooterControls();
    }

    void HandlePlayerWheel(WPARAM w, LPARAM l) {
        if(!player_) return;
        const short delta=GET_WHEEL_DELTA_WPARAM(w);
        if(player_->VR().vr){
            // Preserve the established VR wheel/FOV behavior exactly.
            PlayerActivity(true);
            player_->Wheel(delta);
            return;
        }
        // Flat/non-VR playback uses the plain wheel for cursor-anchored zoom.
        // Native Size still owns the render scale while enabled, and VR keeps its
        // separate FOV wheel behavior above.
        if(nativeVideoSizing_) return;
        POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        if(videoHwnd_) ScreenToClient(videoHwnd_,&p);
        player_->FlatWheelZoom(delta,p.x,p.y);
        if(videoHwnd_) InvalidateRect(videoHwnd_,nullptr,FALSE);
    }

    bool ImageZoomActive() const { return std::abs(imageZoomScale_-1.0f)>0.001f; }
    void ResetImageZoom() {
        imageZoomScale_=1.0f; imageZoomCenterU_=0.5f; imageZoomCenterV_=0.5f;
        imageZoomDragging_=false;
        if(hwnd_ && GetCapture()==hwnd_) ReleaseCapture();
    }
    void ZoomImageAtPoint(short delta, POINT p) {
        if(delta==0 || nativeImageSizing_ || mode_!=Mode::Details || category_!=Category::Images || selected_>=images_.size()) return;
        if(!PtInRect(&detailsMediaRect_,p)) return;
        auto& item=images_[selected_];
        UINT sourceW=item.detailThumbW>0?static_cast<UINT>(item.detailThumbW):item.sourceWidth;
        UINT sourceH=item.detailThumbH>0?static_cast<UINT>(item.detailThumbH):item.sourceHeight;
        if(!sourceW || !sourceH){ if(!GetCurrentImageNativeSize(sourceW,sourceH)) return; }
        const float rw=static_cast<float>(std::max<LONG>(1L,detailsMediaRect_.right-detailsMediaRect_.left));
        const float rh=static_cast<float>(std::max<LONG>(1L,detailsMediaRect_.bottom-detailsMediaRect_.top));
        const float fit=std::min(rw/static_cast<float>(sourceW),rh/static_cast<float>(sourceH));
        const float oldZoom=std::clamp(imageZoomScale_,0.25f,8.0f);
        const float factor=std::pow(1.15f,static_cast<float>(delta)/120.0f);
        const float newZoom=std::clamp(oldZoom*factor,0.25f,8.0f);
        if(std::abs(newZoom-1.0f)<=0.001f){ ResetImageZoom(); InvalidateRect(hwnd_,nullptr,FALSE); return; }
        if(newZoom<1.0f){
            imageZoomScale_=newZoom; imageZoomCenterU_=0.5f; imageZoomCenterV_=0.5f;
            InvalidateRect(hwnd_,nullptr,FALSE); return;
        }
        const float oldDw=std::max(1.0f,static_cast<float>(sourceW)*fit*oldZoom);
        const float oldDh=std::max(1.0f,static_cast<float>(sourceH)*fit*oldZoom);
        const float newDw=std::max(1.0f,static_cast<float>(sourceW)*fit*newZoom);
        const float newDh=std::max(1.0f,static_cast<float>(sourceH)*fit*newZoom);
        const float cx=0.5f*static_cast<float>(detailsMediaRect_.left+detailsMediaRect_.right);
        const float cy=0.5f*static_cast<float>(detailsMediaRect_.top+detailsMediaRect_.bottom);
        const float sourceU=imageZoomCenterU_+(static_cast<float>(p.x)-cx)/oldDw;
        const float sourceV=imageZoomCenterV_+(static_cast<float>(p.y)-cy)/oldDh;
        float centerU=sourceU-(static_cast<float>(p.x)-cx)/newDw;
        float centerV=sourceV-(static_cast<float>(p.y)-cy)/newDh;
        const float halfU=rw/(2.0f*newDw),halfV=rh/(2.0f*newDh);
        centerU=halfU>=0.5f?0.5f:std::clamp(centerU,halfU,1.0f-halfU);
        centerV=halfV>=0.5f?0.5f:std::clamp(centerV,halfV,1.0f-halfV);
        imageZoomScale_=newZoom; imageZoomCenterU_=centerU; imageZoomCenterV_=centerV;
        InvalidateRect(hwnd_,nullptr,FALSE);
    }

    void PanImageZoomByDelta(int dx,int dy) {
        if(!imageZoomDragging_ || nativeImageSizing_ || imageZoomScale_<=1.001f || mode_!=Mode::Details || category_!=Category::Images || selected_>=images_.size()) return;
        auto& item=images_[selected_];
        UINT sourceW=item.detailThumbW>0?static_cast<UINT>(item.detailThumbW):item.sourceWidth;
        UINT sourceH=item.detailThumbH>0?static_cast<UINT>(item.detailThumbH):item.sourceHeight;
        if(!sourceW || !sourceH){ if(!GetCurrentImageNativeSize(sourceW,sourceH)) return; }
        const float rw=static_cast<float>(std::max<LONG>(1L,detailsMediaRect_.right-detailsMediaRect_.left));
        const float rh=static_cast<float>(std::max<LONG>(1L,detailsMediaRect_.bottom-detailsMediaRect_.top));
        const float fit=std::min(rw/static_cast<float>(sourceW),rh/static_cast<float>(sourceH));
        const float dw=std::max(1.0f,static_cast<float>(sourceW)*fit*imageZoomScale_);
        const float dh=std::max(1.0f,static_cast<float>(sourceH)*fit*imageZoomScale_);
        imageZoomCenterU_-=static_cast<float>(dx)/dw;
        imageZoomCenterV_-=static_cast<float>(dy)/dh;
        const float halfU=rw/(2.0f*dw),halfV=rh/(2.0f*dh);
        imageZoomCenterU_=halfU>=0.5f?0.5f:std::clamp(imageZoomCenterU_,halfU,1.0f-halfU);
        imageZoomCenterV_=halfV>=0.5f?0.5f:std::clamp(imageZoomCenterV_,halfV,1.0f-halfV);
        InvalidateRect(hwnd_,nullptr,FALSE);
    }

    RECT StandardWindowRectForCurrentMonitor() const {
        const int fallbackW=std::max(1,GetSystemMetrics(SM_CXSCREEN)/2);
        const int fallbackH=std::max(1,GetSystemMetrics(SM_CYSCREEN)/2);
        RECT r{0,0,fallbackW,fallbackH};
        if(!hwnd_) return r;
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST),&mi)) return r;
        const int monitorW=std::max(1,static_cast<int>(mi.rcMonitor.right-mi.rcMonitor.left));
        const int monitorH=std::max(1,static_cast<int>(mi.rcMonitor.bottom-mi.rcMonitor.top));
        const int w=std::max(1,monitorW/2);
        const int h=std::max(1,monitorH/2);
        // Standard/restored windows are centered against the full monitor rectangle,
        // not the work area, so Native/standard transitions never visibly drift.
        const int x=mi.rcMonitor.left+(monitorW-w)/2;
        const int y=mi.rcMonitor.top+(monitorH-h)/2;
        return RECT{x,y,x+w,y+h};
    }

    void RestoreStandardWindowSize() {
        if(!hwnd_ || fullscreen_) return;
        const RECT r=StandardWindowRectForCurrentMonitor();
        SetWindowPos(hwnd_,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
    }

    void SetFullscreenRestoreToStandardWindow() {
        savedRect_=StandardWindowRectForCurrentMonitor();
    }

    void SuspendNativeVideoSizingForVr() {
        if(!nativeVideoSizing_ || !player_ || !player_->VR().vr) return;
        player_->SetNativePixelSizing(false);
        // In windowed mode, VR uses the normal pre-native player window while the
        // preference remains latched. Keep the restore rectangle so the next flat
        // video can immediately resume its own native dimensions.
        if(!fullscreen_){
            RestoreStandardWindowSize();
        }
    }

    bool GetNativeMinimumWindowSize(SIZE& minimum) {
        minimum=SIZE{};if(fullscreen_||!hwnd_)return false;UINT sourceW=0,sourceH=0;
        if(nativeVideoSizing_&&mode_==Mode::Player&&player_&&!player_->VR().vr){const auto size=player_->EyeSize();sourceW=size.first;sourceH=size.second;}
        else if(nativeImageSizing_&&mode_==Mode::Details&&category_==Category::Images){if(!GetCurrentImageNativeSize(sourceW,sourceH))return false;}
        else return false;
        if(!sourceW||!sourceH)return false;RECT wr{},cr{};if(!GetWindowRect(hwnd_,&wr)||!GetClientRect(hwnd_,&cr))return false;
        const LONG nonClientW=std::max<LONG>(0,(wr.right-wr.left)-(cr.right-cr.left)),nonClientH=std::max<LONG>(0,(wr.bottom-wr.top)-(cr.bottom-cr.top));
        minimum.cx=static_cast<LONG>(std::min<uint64_t>(static_cast<uint64_t>(LONG_MAX),static_cast<uint64_t>(sourceW)+static_cast<uint64_t>(nonClientW)));
        minimum.cy=static_cast<LONG>(std::min<uint64_t>(static_cast<uint64_t>(LONG_MAX),static_cast<uint64_t>(sourceH)+static_cast<uint64_t>(nonClientH)));
        return true;
    }

    void ApplyNativeVideoWindowSize() {
        if (!nativeVideoSizing_ || fullscreen_ || mode_ != Mode::Player || !player_ || !hwnd_ || player_->VR().vr) return;
        const auto sourceSize=player_->EyeSize();
        if (!sourceSize.first || !sourceSize.second) return;

        RECT windowRect{}; RECT clientRect{};
        if (!GetWindowRect(hwnd_,&windowRect) || !GetClientRect(hwnd_,&clientRect)) return;
        const int currentWindowW=std::max(1,static_cast<int>(windowRect.right-windowRect.left));
        const int currentWindowH=std::max(1,static_cast<int>(windowRect.bottom-windowRect.top));
        const int currentClientW=std::max(1,static_cast<int>(clientRect.right-clientRect.left));
        const int currentClientH=std::max(1,static_cast<int>(clientRect.bottom-clientRect.top));
        const int nonClientW=std::max(0,currentWindowW-currentClientW);
        const int nonClientH=std::max(0,currentWindowH-currentClientH);

        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST),&mi)) return;
        const int monitorW=std::max(1,static_cast<int>(mi.rcMonitor.right-mi.rcMonitor.left));
        const int monitorH=std::max(1,static_cast<int>(mi.rcMonitor.bottom-mi.rcMonitor.top));
        // Strict Native Size: the client render area is exactly the source dimensions.
        // Oversized media may extend beyond the monitor; it is never silently downscaled.
        const int targetClientW=std::max(1,static_cast<int>(sourceSize.first));
        const int targetClientH=std::max(1,static_cast<int>(sourceSize.second));
        const int targetWindowW=targetClientW+nonClientW;
        const int targetWindowH=targetClientH+nonClientH;
        int x=mi.rcMonitor.left+(monitorW-targetWindowW)/2;
        int y=mi.rcMonitor.top+(monitorH-targetWindowH)/2;
        SetWindowPos(hwnd_,nullptr,x,y,targetWindowW,targetWindowH,SWP_NOZORDER|SWP_NOACTIVATE);
        // Correct after Windows has applied DPI/non-client metrics. This second pass
        // guarantees that the client surface itself is the requested pixel size.
        RECT actualClient{},actualWindow{};
        if(GetClientRect(hwnd_,&actualClient) && GetWindowRect(hwnd_,&actualWindow)){
            const int actualClientW=std::max(1,static_cast<int>(actualClient.right-actualClient.left));
            const int actualClientH=std::max(1,static_cast<int>(actualClient.bottom-actualClient.top));
            const int correctedW=std::max(1,static_cast<int>(actualWindow.right-actualWindow.left)+(targetClientW-actualClientW));
            const int correctedH=std::max(1,static_cast<int>(actualWindow.bottom-actualWindow.top)+(targetClientH-actualClientH));
            if(actualClientW!=targetClientW || actualClientH!=targetClientH){
                x=mi.rcMonitor.left+(monitorW-correctedW)/2;
                y=mi.rcMonitor.top+(monitorH-correctedH)/2;
                SetWindowPos(hwnd_,nullptr,x,y,correctedW,correctedH,SWP_NOZORDER|SWP_NOACTIVATE);
            }
        }
    }

    void ToggleNativeVideoSizing() {
        if(mode_!=Mode::Player || !player_ || !hwnd_ || player_->VR().vr) return;
        if(!nativeVideoSizing_){
            if(!nativeSizingRestoreRectValid_){
                nativeSizingRestoreRect_=StandardWindowRectForCurrentMonitor();
                nativeSizingRestoreRectValid_=true;
            }
            nativeVideoSizing_=true;
            player_->SetNativePixelSizing(true);
            if(!fullscreen_) ApplyNativeVideoWindowSize();
        }else{
            nativeVideoSizing_=false;
            player_->SetNativePixelSizing(false);
            if(fullscreen_){
                // Native OFF while fullscreen stays fullscreen. When fullscreen is later
                // left, return to the app's standard half-monitor window.
                SetFullscreenRestoreToStandardWindow();
            }else{
                RestoreStandardWindowSize();
            }
            nativeSizingRestoreRectValid_=false;
        }
        Layout();
        InvalidateControls();
        if(videoHwnd_) InvalidateRect(videoHwnd_,nullptr,FALSE);
    }

    bool GetCurrentImageNativeSize(UINT& width, UINT& height) {
        if(mode_!=Mode::Details || category_!=Category::Images || selected_>=images_.size()) return false;
        auto& item=images_[selected_];
        if(!item.sourceWidth || !item.sourceHeight){
            Gdiplus::Bitmap probe(item.path.c_str());
            if(probe.GetLastStatus()!=Gdiplus::Ok) return false;
            item.sourceWidth=probe.GetWidth();
            item.sourceHeight=probe.GetHeight();
        }
        if(!item.sourceWidth || !item.sourceHeight) return false;
        width=item.sourceWidth; height=item.sourceHeight; return true;
    }

    void ApplyNativeImageWindowSize() {
        if (!nativeImageSizing_ || fullscreen_ || mode_ != Mode::Details || category_ != Category::Images || !hwnd_) return;
        UINT sourceW=0, sourceH=0;
        if (!GetCurrentImageNativeSize(sourceW, sourceH)) return;

        RECT windowRect{}; RECT clientRect{};
        if (!GetWindowRect(hwnd_,&windowRect) || !GetClientRect(hwnd_,&clientRect)) return;
        const int currentWindowW=std::max(1,static_cast<int>(windowRect.right-windowRect.left));
        const int currentWindowH=std::max(1,static_cast<int>(windowRect.bottom-windowRect.top));
        const int currentClientW=std::max(1,static_cast<int>(clientRect.right-clientRect.left));
        const int currentClientH=std::max(1,static_cast<int>(clientRect.bottom-clientRect.top));
        const int nonClientW=std::max(0,currentWindowW-currentClientW);
        const int nonClientH=std::max(0,currentWindowH-currentClientH);

        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST),&mi)) return;
        const int monitorW=std::max(1,static_cast<int>(mi.rcMonitor.right-mi.rcMonitor.left));
        const int monitorH=std::max(1,static_cast<int>(mi.rcMonitor.bottom-mi.rcMonitor.top));
        const int targetClientW=std::max(1,static_cast<int>(sourceW));
        const int targetClientH=std::max(1,static_cast<int>(sourceH));
        const int targetWindowW=targetClientW+nonClientW;
        const int targetWindowH=targetClientH+nonClientH;
        int x=mi.rcMonitor.left+(monitorW-targetWindowW)/2;
        int y=mi.rcMonitor.top+(monitorH-targetWindowH)/2;
        SetWindowPos(hwnd_,nullptr,x,y,targetWindowW,targetWindowH,SWP_NOZORDER|SWP_NOACTIVATE);
        RECT actualClient{},actualWindow{};
        if(GetClientRect(hwnd_,&actualClient) && GetWindowRect(hwnd_,&actualWindow)){
            const int actualClientW=std::max(1,static_cast<int>(actualClient.right-actualClient.left));
            const int actualClientH=std::max(1,static_cast<int>(actualClient.bottom-actualClient.top));
            const int correctedW=std::max(1,static_cast<int>(actualWindow.right-actualWindow.left)+(targetClientW-actualClientW));
            const int correctedH=std::max(1,static_cast<int>(actualWindow.bottom-actualWindow.top)+(targetClientH-actualClientH));
            if(actualClientW!=targetClientW || actualClientH!=targetClientH){
                x=mi.rcMonitor.left+(monitorW-correctedW)/2;
                y=mi.rcMonitor.top+(monitorH-correctedH)/2;
                SetWindowPos(hwnd_,nullptr,x,y,correctedW,correctedH,SWP_NOZORDER|SWP_NOACTIVATE);
            }
        }
    }

    void ToggleNativeImageSizing() {
        if(mode_!=Mode::Details || category_!=Category::Images || !hwnd_ || selected_>=images_.size()) return;
        if(!nativeImageSizing_){
            if(!nativeImageSizingRestoreRectValid_){
                nativeImageSizingRestoreRect_=StandardWindowRectForCurrentMonitor();
                nativeImageSizingRestoreRectValid_=true;
            }
            ResetImageZoom();
            nativeImageSizing_=true;
            if(!fullscreen_) ApplyNativeImageWindowSize();
        }else{
            nativeImageSizing_=false;
            if(fullscreen_){
                SetFullscreenRestoreToStandardWindow();
            }else{
                RestoreStandardWindowSize();
            }
            nativeImageSizingRestoreRectValid_=false;
        }
        Layout();
        InvalidateRect(hwnd_,nullptr,FALSE);
    }

    void ApplyMainWindowCornerPreference() {
        if(!hwnd_) return;
        // DWMWA_WINDOW_CORNER_PREFERENCE (33): 1 = do not round, 2 = round.
        // Numeric values keep this compatible with older Windows SDK headers.
        const DWORD preference=fullscreen_?1u:2u;
        DwmSetWindowAttribute(hwnd_,33,&preference,sizeof(preference));
    }

    void ApplyPlayerOverlayCornerRegion(int width,int height) {
        if(!controlsHwnd_ || width<=0 || height<=0) return;
        if(fullscreen_){
            SetWindowRgn(controlsHwnd_,nullptr,FALSE);
            return;
        }
        // The overlay is a top-level layered popup. Give it square top corners but
        // rounded bottom corners so it cannot visually square off the main window.
        constexpr int cornerDiameter=20;
        HRGN rounded=CreateRoundRectRgn(0,0,width+1,height+1,cornerDiameter,cornerDiameter);
        HRGN squareTop=CreateRectRgn(0,0,width,std::min(height,cornerDiameter));
        if(rounded && squareTop){
            CombineRgn(rounded,rounded,squareTop,RGN_OR);
            if(SetWindowRgn(controlsHwnd_,rounded,FALSE)!=0) rounded=nullptr; // window owns it on success
        }
        if(rounded) DeleteObject(rounded);
        if(squareTop) DeleteObject(squareTop);
    }

    void ToggleFullscreen() {
        const bool entering=!fullscreen_;

        // Preserve the Library's logical scroll position across the fullscreen geometry
        // change. Fullscreen can fit many more cards per row, so carrying the old raw
        // pixel offset across can leave the viewport below the newly shortened grid.
        // Keep top/middle/bottom relative to the old scroll range, and make bottom exact.
        double libraryScrollFraction=0.0;
        bool libraryWasAtBottom=false;
        if(mode_==Mode::Library){
            RECT oldLibraryRc{}; GetClientRect(hwnd_,&oldLibraryRc);
            const int oldMaxScroll=LibraryMaxScroll(oldLibraryRc);
            const int oldScroll=std::clamp(scrollY_,0,oldMaxScroll);
            if(oldMaxScroll>0){
                libraryScrollFraction=static_cast<double>(oldScroll)/static_cast<double>(oldMaxScroll);
                libraryWasAtBottom=oldScroll>=oldMaxScroll-2;
            }
        }

        if(entering){
            if(mode_==Mode::Library){
                // Like the Timeline grid, fullscreen starts from the same automatic seven-across
                // default while preserving a custom windowed Library zoom for restoration on exit.
                preFullscreenLibraryCardWidth_=libraryCardWidth_;
                preFullscreenLibraryZoomOverridden_=libraryZoomOverridden_;
                preFullscreenLibraryStateValid_=true;
                libraryZoomOverridden_=false;
            }
            if(mode_==Mode::Details && category_==Category::Videos){
                // Fullscreen starts from its automatic seven-across default without
                // destroying a custom windowed Timeline size; restore it on exit.
                preFullscreenPreviewCardWidth_=previewCardWidth_;
                preFullscreenPreviewZoomOverridden_=previewZoomOverridden_;
                preFullscreenPreviewStateValid_=true;
                previewZoomOverridden_=false;
                previewWheelRemainder_=0;
            }
        }
        fullscreen_=entering;
        if(fullscreen_){
            savedStyle_=static_cast<DWORD>(GetWindowLongPtrW(hwnd_,GWL_STYLE));
            GetWindowRect(hwnd_,&savedRect_);
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST),&mi);
            const DWORD fsStyle=savedStyle_&~(WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU|WS_CAPTION);
            SetWindowLongPtrW(hwnd_,GWL_STYLE,fsStyle);
            SetWindowPos(hwnd_,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);
            if(mode_==Mode::Library){
                RECT libraryRc{}; GetClientRect(hwnd_,&libraryRc);
                ApplyLibraryWidthForViewport(std::max(1,static_cast<int>(libraryRc.right-libraryRc.left)));
            }
        }else{
            SetWindowLongPtrW(hwnd_,GWL_STYLE,savedStyle_);
            SetWindowPos(hwnd_,nullptr,savedRect_.left,savedRect_.top,savedRect_.right-savedRect_.left,savedRect_.bottom-savedRect_.top,SWP_FRAMECHANGED|SWP_NOZORDER);
            if(mode_==Mode::Library){
                if(preFullscreenLibraryStateValid_){
                    libraryCardWidth_=preFullscreenLibraryCardWidth_;
                    libraryZoomOverridden_=preFullscreenLibraryZoomOverridden_;
                }else{
                    libraryZoomOverridden_=false;
                }
                preFullscreenLibraryCardWidth_=-1;
                preFullscreenLibraryZoomOverridden_=false;
                preFullscreenLibraryStateValid_=false;
            }
            if(mode_==Mode::Details && category_==Category::Videos){
                if(preFullscreenPreviewStateValid_){
                    previewCardWidth_=preFullscreenPreviewCardWidth_;
                    previewZoomOverridden_=preFullscreenPreviewZoomOverridden_;
                }else{
                    // If Details was entered while already fullscreen, windowed mode
                    // returns to the normal seven-across default.
                    previewZoomOverridden_=false;
                }
                previewWheelRemainder_=0;
                preFullscreenPreviewCardWidth_=-1;
                preFullscreenPreviewZoomOverridden_=false;
                preFullscreenPreviewStateValid_=false;
            }
        }
        ApplyMainWindowCornerPreference();
        if(!fullscreen_ && nativeVideoSizing_ && mode_==Mode::Player) ApplyNativeVideoWindowSize();
        if(!fullscreen_ && nativeImageSizing_ && mode_==Mode::Details && category_==Category::Images) ApplyNativeImageWindowSize();
        Layout();
        if(mode_==Mode::Library){
            RECT newLibraryRc{}; GetClientRect(hwnd_,&newLibraryRc);
            const int newMaxScroll=LibraryMaxScroll(newLibraryRc);
            if(libraryWasAtBottom) scrollY_=newMaxScroll;
            else scrollY_=std::clamp(static_cast<int>(std::lround(libraryScrollFraction*static_cast<double>(newMaxScroll))),0,newMaxScroll);
            UpdateLibraryScrollbarRects(newLibraryRc);
            NoteLibraryScrollInput();
            ScheduleLibraryPrefetchPulse(1);
            ScheduleLibraryScrollSettle();
        }
        if(mode_!=Mode::Player) InvalidateRect(hwnd_,nullptr,FALSE);
        InvalidateRect(hwnd_,nullptr,FALSE); InvalidateControls();
    }

    void UpdateWindowTitle() {
        if(mode_!=Mode::Player||selected_>=videos_.size())return;
        SetWindowTextW(hwnd_,(L"Visual MediaPlayer - "+videos_[selected_].title).c_str());
    }

    HINSTANCE inst_{}; HWND hwnd_{}; HWND playerPrevHwnd_{},playerNextHwnd_{}; Mode mode_=Mode::Library; Category category_=Category::Videos;
    std::wstring folder_; std::wstring persistentFolder_; std::wstring currentFolder_; std::wstring detailsOriginFolder_; std::vector<MediaItem> videos_,images_; std::vector<LibraryFolder> folders_;
    std::map<std::wstring,std::vector<size_t>> videoFolderIndices_,imageFolderIndices_,childFolderIndices_;
    bool externalMediaSession_=false; std::vector<std::wstring> externalMediaPaths_,pendingExternalMediaPaths_; size_t selected_=0; int scrollY_=0; int detailsScrollY_=0; int detailsContentBottom_=0; double libraryScrollWheelPixelRemainder_=0.0,detailsScrollWheelPixelRemainder_=0.0; int previewCardWidth_=kDefaultPreviewCardWidth; int previewWheelRemainder_=0; bool previewZoomOverridden_=false; int preFullscreenPreviewCardWidth_=-1; bool preFullscreenPreviewZoomOverridden_=false; bool preFullscreenPreviewStateValid_=false; int libraryCardWidth_=kDefaultLibraryCardWidth; bool libraryZoomOverridden_=false; int preFullscreenLibraryCardWidth_=-1; bool preFullscreenLibraryZoomOverridden_=false; bool preFullscreenLibraryStateValid_=false;
    std::map<std::wstring,FolderViewState> folderViewStates_;
    std::vector<std::wstring> knownLibraryRoots_;
    std::wstring searchQuery_; bool searchVisible_=false; bool searchSelectAll_=false; bool filterDirty_=true; std::vector<size_t> filteredIndices_;
    bool detailsSearchNavigationActive_=false; std::vector<size_t> detailsSearchNavigationIndices_;
    bool slideshowActive_=false; size_t slideshowPos_=0; std::vector<size_t> slideshowIndices_;
    bool slideshowFadeActive_=false; size_t slideshowPreviousIndex_=static_cast<size_t>(-1); ULONGLONG slideshowFadeStart_=0;
    HWND paintOwner_=nullptr; HWND hoverOwner_=nullptr; HWND hoverPreviousOwner_=nullptr; RECT hoverRect_{},hoverPreviousRect_{}; ULONGLONG hoverTransitionStart_=0;
    std::vector<AnimatedMediaHit> libraryMediaHoverHits_,previewMediaHoverHits_;
    MediaHoverSurface mediaHoverSurface_=MediaHoverSurface::None; size_t mediaHoverId_=static_cast<size_t>(-1); RECT mediaHoverRect_{}; ULONGLONG mediaHoverStart_=0;
    Category libraryReturnHighlightCategory_=Category::Videos; size_t libraryReturnHighlightIndex_=static_cast<size_t>(-1); ULONGLONG libraryReturnHighlightStart_=0; RECT libraryReturnHighlightRect_{};
    size_t timelineReturnHighlightMediaIndex_=static_cast<size_t>(-1); size_t timelineReturnHighlightIndex_=static_cast<size_t>(-1); ULONGLONG timelineReturnHighlightStart_=0; RECT timelineReturnHighlightRect_{}; bool pendingTimelineReturnFocus_=false; double pendingTimelineReturnSeconds_=0.0;
    std::unique_ptr<Gdiplus::Bitmap> resolution4kBitmap_,resolution5kBitmap_,resolution8kBitmap_,vrBadgeBitmap_,vrBadgeWhiteBitmap_,favoriteIconBitmap_;
    std::unique_ptr<Gdiplus::Bitmap> autoNextSvgBitmap_,openFolderSvgBitmap_,reloadSvgBitmap_,loadEverythingSvgBitmap_,expandFullscreenSvgBitmap_,collapseFullscreenSvgBitmap_,backSvgBitmap_,imageSvgBitmap_,videosSvgBitmap_,nativeSvgBitmap_,previousSvgBitmap_,nextSvgBitmap_,volumeSvgBitmap_,volumeMuteSvgBitmap_,skip30SvgBitmap_,pauseSvgBitmap_,playSvgBitmap_,vrProjectionSvgBitmap_;
    std::unique_ptr<Gdiplus::Bitmap> rewindSvgBitmap_,forwardSvgBitmap_,buttonBackgroundActiveBitmap_,buttonBackgroundInactiveBitmap_;
    std::set<Gdiplus::Bitmap*> externalButtonAssets_;
    ComPtr<ID3D11Device> svgD3dDevice_; ComPtr<ID2D1Factory1> svgD2dFactory_; ComPtr<ID2D1Device> svgD2dDevice_; ComPtr<ID2D1DeviceContext5> svgD2dContext_;
    RECT chooseRect_{},rescanRect_{},loadEverythingRect_{},categoryToggleRect_{},mediaCountRect_{},slideshowRect_{},imageDetailsSlideshowRect_{},imageDetailsNativeRect_{},backRect_{},playRect_{},searchBoxRect_{},libraryFooterRect_{},detailsFooterRect_{},libraryFullRect_{},detailsFullRect_{},previewZoomRect_{},detailsMediaRect_{},detailsPrevRect_{},detailsNextRect_{};
    RECT libraryScrollTrackRect_{}, libraryScrollThumbRect_{}; bool libraryScrollDragging_=false; int libraryScrollDragOffset_=0; int libraryLastKnownMaxScroll_=0;
    std::map<uint64_t,HFONT> fontCache_; HDC backDC_{}; HBITMAP backBitmap_{}; HGDIOBJ backOldBitmap_{}; int backW_=0,backH_=0;
    HDC controlsBackDC_{}; HBITMAP controlsBackBitmap_{}; HGDIOBJ controlsBackOldBitmap_{}; int controlsBackW_=0,controlsBackH_=0;
    ComPtr<ID2D1Factory> libraryD2dFactory_; ComPtr<ID2D1HwndRenderTarget> libraryD2dTarget_; ComPtr<IWICImagingFactory> libraryWicFactory_; ComPtr<IDWriteFactory> libraryDWriteFactory_;
    std::map<uint64_t,ComPtr<IDWriteTextFormat>> libraryDWriteFormats_;
    ComPtr<ID2D1SolidColorBrush> libraryD2dCardBrush_,libraryD2dPlaceholderBrush_,libraryD2dUiBrush_;
    ComPtr<ID2D1Bitmap> libraryD2dFavoriteIcon_,libraryD2dVrIcon_,libraryD2dResolution4k_,libraryD2dResolution5k_,libraryD2dResolution8k_;
    ComPtr<ID2D1Bitmap> libraryD2dAutoNextSvg_,libraryD2dOpenFolderSvg_,libraryD2dReloadSvg_,libraryD2dLoadEverythingSvg_,libraryD2dExpandFullscreenSvg_,libraryD2dCollapseFullscreenSvg_,libraryD2dBackSvg_,libraryD2dImageSvg_,libraryD2dVideosSvg_,libraryD2dNativeSvg_,libraryD2dPreviousSvg_,libraryD2dNextSvg_,libraryD2dPlaySvg_;
    ComPtr<ID2D1Bitmap> libraryD2dButtonBackgroundActive_,libraryD2dButtonBackgroundInactive_;
    uint64_t libraryD2dGeneration_=1; int libraryD2dWidth_=0,libraryD2dHeight_=0; bool detailsGpuWorkingSetActive_=false;
    HWND videoHwnd_{},controlsHwnd_{}; std::unique_ptr<NativePlayer> player_;
    BYTE controlsAlpha_=0,controlsFadeFrom_=0,controlsFadeTo_=0; ULONGLONG controlsFadeStart_=0; bool controlsFading_=false;
    HWND playerFooterTransitionHwnd_{}; HBITMAP playerFooterTransitionBitmap_{}; ULONGLONG playerFooterTransitionStart_=0;
    RECT playerBackRect_{},playerVrToggleRect_{},playerSkipBackRect_{},playerPlayRect_{},playerSkipForwardRect_{},playerFullRect_{},playerNativeSizeRect_{},playerAutoNextRect_{},seekRect_{},volumeRect_{},volumeLabelRect_{},playerTimeRect_{};
    RECT nativeSizingRestoreRect_{}; bool nativeSizingRestoreRectValid_=false;
    RECT nativeImageSizingRestoreRect_{}; bool nativeImageSizingRestoreRectValid_=false;
    float imageZoomScale_=1.0f,imageZoomCenterU_=0.5f,imageZoomCenterV_=0.5f; bool imageZoomDragging_=false; POINT imageZoomLastPoint_{};
    double seekFraction_=0.0,volumeFraction_=0.30,lastAudibleVolumeFraction_=0.30; bool fullscreen_=false,seekDragging_=false,volumeDragging_=false,autoNext_=false,nativeVideoSizing_=false,nativeImageSizing_=false,playerControlsVisible_=false;
    bool seekHoverVisible_=false; int seekHoverX_=0;
    ULONGLONG controlsHideDeadline_=0; POINT lastCursorScreen_{}; bool lastCursorValid_=false; DWORD savedStyle_{}; RECT savedRect_{};
    bool comInitialized_=false,mfStarted_=false; ULONG_PTR gdiplusToken_=0; std::thread thumbThread_; std::atomic<bool> thumbStop_{false}; std::atomic<bool> thumbWorkerRunning_{false}; std::atomic<bool> thumbRepairRequested_{false};
    std::vector<std::thread> libraryThumbLoadThreads_; std::atomic<bool> libraryThumbLoadStop_{false};
    std::atomic<ULONGLONG> libraryThumbDecodePauseUntil_{0};
    // Cached JPEGs may decode in parallel, but uncached original-media generation is
    // globally serialized so foreground hover/timeline work keeps its frame budget.
    std::mutex librarySourceGenerationMutex_;
    std::mutex libraryThumbLoadMutex_; std::condition_variable libraryThumbLoadCv_;
    std::deque<LibraryThumbLoadJob> libraryThumbLoadJobs_; std::vector<LibraryThumbLoadResult> libraryThumbLoadResults_;
    std::atomic<bool> libraryThumbResultMessagePending_{false}; std::atomic<uint64_t> libraryThumbViewEpoch_{1};
    int libraryThumbViewportScrollY_=-1,libraryThumbViewportCardWidth_=-1,libraryThumbViewportClientWidth_=-1,libraryThumbPrefetchDirection_=1;
    int libraryProtectedWindowFirstRow_=-1,libraryProtectedWindowLastRow_=-1,libraryProtectedWindowCols_=-1;
    ULONGLONG lastLibraryThumbTrimTick_=0,lastLibraryGpuTrimTick_=0,lastLibraryScrollInputTick_=0;
    bool libraryPrefetchPulseArmed_=false,libraryThumbApplyTimerArmed_=false,libraryWorkingSetNeedsEpochCancel_=false;
    Category libraryThumbViewportCategory_=Category::Videos; std::wstring libraryThumbViewportFolder_,libraryThumbViewportSearch_;
    std::set<std::wstring> protectedLibraryThumbPaths_,visibleLibraryGpuThumbPaths_,libraryWorkingSetThumbPaths_,playbackLibraryWarmPaths_;
    std::vector<size_t> libraryWorkingSetQueueIndices_; size_t libraryWorkingSetQueueCursor_=0;
    uint64_t libraryWorkingSetQueueEpoch_=0; Category libraryWorkingSetQueueCategory_=Category::Videos;
    int libraryWorkingSetQueueCardW_=0,libraryWorkingSetQueueImageH_=0;
    std::vector<size_t> libraryProtectedQueueIndices_; size_t libraryProtectedQueueCursor_=0;
    uint64_t libraryProtectedQueueEpoch_=0; Category libraryProtectedQueueCategory_=Category::Videos;
    int libraryProtectedQueueCardW_=0,libraryProtectedQueueImageH_=0;
    std::thread resolutionMetadataThread_; std::atomic<bool> resolutionMetadataStop_{false};
    std::mutex resolutionMetadataMutex_,resolutionMetadataCacheMutex_; std::condition_variable resolutionMetadataCv_;
    std::deque<ResolutionMetadataJob> resolutionMetadataJobs_; std::set<std::wstring> resolutionMetadataPendingPaths_;
    std::atomic<uint64_t> resolutionMetadataGeneration_{1};
    std::atomic<ULONGLONG> backgroundPauseUntil_{0}; ULONGLONG lastMemoryPressureCheck_=0;
    ULONGLONG lastLibraryActivityTick_=0,lastLibraryIdleTrimTick_=0; bool libraryIdleCacheTrimmed_=false;
    int libraryAccessFailCount_=0; bool libraryUnavailableLatched_=false; bool libraryAccessRetryNeedsRescan_=false; ULONGLONG libraryAccessMonitorUntil_=0;
    std::thread libraryScanThread_; std::atomic<bool> libraryScanStop_{false}; std::atomic<uint64_t> libraryScanGeneration_{0}; bool libraryScanRunning_=false;
    vmp::SingleInstancePipeServer instancePipeServer_;
    bool liveWindowMove_=false;
    bool foregroundActivationClickPending_=false;
    std::wstring appNoticeText_; ULONGLONG appNoticeStart_=0,appNoticeUntil_=0;
    std::atomic<int> loadingKind_{0}, loadingCurrent_{0}, loadingTotal_{0};
    std::thread fullLoadThread_; std::atomic<bool> fullLoadStop_{false},fullLoadRunning_{false};
    std::atomic<bool> fullLoadYieldCurrent_{false};
    std::atomic<int> foregroundGenerationPriorityCount_{0};
    std::atomic<int> fullLoadCurrent_{0},fullLoadTotal_{0},fullLoadFailures_{0}; ULONGLONG fullLoadFinishedAt_=0;
    std::mutex fullLoadStatusMutex_; std::wstring fullLoadCurrentFile_; std::vector<std::wstring> fullLoadFailedPaths_;
    bool loadFailureFilterActive_=false; int loadFailureFilterTotal_=0; std::set<std::wstring> loadFailureFilterPaths_;
    std::mutex generationClaimMutex_; std::set<std::wstring> generationClaims_;
    std::wstring previewDir_,previewMediaPath_; std::vector<PreviewFrame> previewFrames_; std::map<std::wstring,PrefetchedPreviewSet> prefetchedPreviewSets_; std::vector<std::pair<RECT,double>> previewHitRects_; std::thread previewThread_; std::atomic<bool> previewStop_{false}; std::atomic<double> detailsDurationSeconds_{0.0};
    std::thread previewBitmapDecodeThread_; std::atomic<bool> previewBitmapDecodeStop_{false}; std::atomic<uint64_t> previewBitmapDecodeGeneration_{1};
    std::mutex previewBitmapDecodeMutex_; std::condition_variable previewBitmapDecodeCv_; std::deque<PreviewBitmapDecodeJob> previewBitmapDecodeJobs_; bool previewZoomGestureActive_=false,previewScrollGestureActive_=false,previewAsyncDecodePreferred_=false;
    std::thread libraryHoverPreviewThread_; std::atomic<bool> libraryHoverPreviewStop_{false}; std::atomic<bool> libraryHoverPreviewWorkerRunning_{false};
    std::mutex libraryHoverPreviewRequestMutex_; std::condition_variable libraryHoverPreviewCv_; std::atomic<uint64_t> libraryHoverPreviewGeneration_{0}; std::atomic<bool> libraryHoverPreviewFrameMessagePending_{false};
    MediaHoverSurface libraryHoverPreviewRequestedSurface_=MediaHoverSurface::None; size_t libraryHoverPreviewRequestedId_=static_cast<size_t>(-1); std::wstring libraryHoverPreviewRequestedPath_; VRInfo libraryHoverPreviewRequestedVr_{}; double libraryHoverPreviewRequestedStartSeconds_=-1.0; int libraryHoverPreviewRequestedWidth_=0,libraryHoverPreviewRequestedHeight_=0;
    std::vector<LibraryHoverPreviewFrame> libraryHoverPreviewFrames_; MediaHoverSurface libraryHoverPreviewSurface_=MediaHoverSurface::None; size_t libraryHoverPreviewItemId_=static_cast<size_t>(-1); MediaHoverSurface libraryHoverPreviewLoadingSurface_=MediaHoverSurface::None; size_t libraryHoverPreviewLoadingId_=static_cast<size_t>(-1); MediaHoverSurface libraryHoverPreviewPendingSurface_=MediaHoverSurface::None; size_t libraryHoverPreviewPendingId_=static_cast<size_t>(-1); MediaHoverSurface libraryHoverPreviewFailedSurface_=MediaHoverSurface::None; size_t libraryHoverPreviewFailedId_=static_cast<size_t>(-1);
    ULONGLONG libraryHoverPreviewPendingStart_=0,libraryHoverPreviewFrameTick_=0; size_t libraryHoverPreviewFrameIndex_=0; RECT libraryHoverPreviewRect_{};
    std::unique_ptr<HoverPreviewAudioPlayer> hoverPreviewAudio_; uint64_t hoverPreviewAudioGeneration_=1;
    MediaHoverSurface hoverPreviewAudioSurface_=MediaHoverSurface::None; size_t hoverPreviewAudioItemId_=static_cast<size_t>(-1);
    std::wstring hoverPreviewAudioPath_; bool hoverPreviewAudioDelayArmed_=false; double libraryHoverPreviewCurrentMediaSeconds_=0.0;
    std::thread detailPrefetchThread_; std::mutex detailPrefetchMutex_; std::condition_variable detailPrefetchCv_; std::vector<DetailPrefetchJob> detailPrefetchJobs_; bool detailPrefetchStop_=false; std::atomic<uint64_t> detailPrefetchGeneration_{0};
};

static std::vector<std::wstring> GetExternalCommandLineMediaPaths() {
    std::vector<std::wstring> paths;
    int argc=0;
    LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argv){
        for(int i=1;i<argc;++i) if(argv[i] && *argv[i]) paths.emplace_back(argv[i]);
        LocalFree(argv);
    }
    return paths;
}

static bool SendExternalMediaToRunningInstance(const std::vector<std::wstring>& paths) {
    return vmp::SendPathsToRunningInstance(kVmpIpcChannel,paths,5000);
}

int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::vector<std::wstring> externalPaths=GetExternalCommandLineMediaPaths();

    HANDLE instanceMutex=CreateMutexW(nullptr,FALSE,kVmpInstanceMutex);
    const bool anotherInstance=instanceMutex && GetLastError()==ERROR_ALREADY_EXISTS;
    if(anotherInstance && SendExternalMediaToRunningInstance(externalPaths)){
        if(instanceMutex) CloseHandle(instanceMutex);
        return 0;
    }

    App app;
    if(!app.Initialize(hInst)){
        if(instanceMutex) CloseHandle(instanceMutex);
        return 1;
    }
    if(!externalPaths.empty()) app.QueueExternalMediaOpen(externalPaths);
    const int result=app.Run();
    if(instanceMutex) CloseHandle(instanceMutex);
    return result;
}
