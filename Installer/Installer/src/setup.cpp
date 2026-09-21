#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <knownfolders.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>
#include <cwchar>
#include <filesystem>
#include <system_error>
#include "../res/resource.h"
#include "../../../generated/Version.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {
namespace fs = std::filesystem;
constexpr wchar_t kProductName[] = L"VisualMediaPlayer";
constexpr wchar_t kVersion[] = VMP_VERSION_WSTR;
constexpr wchar_t kAppExe[] = L"VisualMediaPlayer.exe";
constexpr wchar_t kUninstallExe[] = L"Uninstall.exe";
constexpr wchar_t kMainWindowClass[] = L"VisualMediaPlayerMain";
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\VisualMediaPlayer";
constexpr wchar_t kAppPathsKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\VisualMediaPlayer.exe";
constexpr wchar_t kMachineAppKey[] = L"SOFTWARE\\Classes\\Applications\\VisualMediaPlayer.exe";
constexpr wchar_t kUserAppKey[] = L"Software\\Classes\\Applications\\VisualMediaPlayer.exe";

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\') return a + b;
    return a + L"\\" + b;
}

std::wstring ModulePath() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) return {};
    return std::wstring(buf.data(), n);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &p)) || !p) return {};
    std::wstring result(p);
    CoTaskMemFree(p);
    return result;
}

bool SetRegString(HKEY root, const std::wstring& key, const wchar_t* valueName, const std::wstring& value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG rc = RegSetValueExW(h, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

bool SetRegDword(HKEY root, const std::wstring& key, const wchar_t* valueName, DWORD value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    const LONG rc = RegSetValueExW(h, valueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

bool SetRegEmptyString(HKEY root, const std::wstring& key, const wchar_t* valueName) {
    return SetRegString(root, key, valueName, L"");
}

void DeleteRegTree(HKEY root, const std::wstring& key) {
    RegDeleteTreeW(root, key.c_str());
}

std::wstring SettingsPathNoCreate() {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) return {};
    return JoinPath(JoinPath(local, L"VisualMediaPlayer"), L"settings.ini");
}

std::wstring NormalizePathKey(const std::wstring& raw) {
    if (raw.empty()) return {};
    std::wstring value = fs::path(raw).lexically_normal().wstring();
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

void AddUniqueRoot(std::vector<std::wstring>& roots, const std::wstring& raw) {
    if (raw.empty()) return;
    const std::wstring key = NormalizePathKey(raw);
    if (key.empty()) return;
    for (const auto& existing : roots) {
        if (NormalizePathKey(existing) == key) return;
    }
    roots.push_back(fs::path(raw).lexically_normal().wstring());
}

std::vector<std::wstring> ReadKnownLibraryRoots() {
    std::vector<std::wstring> roots;
    const std::wstring settings = SettingsPathNoCreate();
    if (settings.empty() || GetFileAttributesW(settings.c_str()) == INVALID_FILE_ATTRIBUTES) return roots;

    wchar_t folder[32768]{};
    GetPrivateProfileStringW(L"Library", L"Folder", L"", folder, static_cast<DWORD>(_countof(folder)), settings.c_str());
    AddUniqueRoot(roots, folder);

    std::vector<wchar_t> section(65536, L'\0');
    const DWORD chars = GetPrivateProfileSectionW(L"CacheRoots", section.data(), static_cast<DWORD>(section.size()), settings.c_str());
    if (chars > 0 && static_cast<size_t>(chars) < section.size() - 2) {
        const wchar_t* p = section.data();
        while (*p) {
            std::wstring entry = p;
            const size_t eq = entry.find(L'=');
            if (eq != std::wstring::npos && eq + 1 < entry.size()) AddUniqueRoot(roots, entry.substr(eq + 1));
            p += entry.size() + 1;
        }
    }
    return roots;
}

bool IsDirectoryReparsePoint(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
        (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool TreeContainsReparsePoint(const fs::path& root) {
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) return true;
    for (; it != end; it.increment(ec)) {
        if (ec) return true;
        const DWORD attrs = GetFileAttributesW(it->path().c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) return true;
        if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
    }
    return false;
}

struct CacheRemovalSummary {
    size_t removed = 0;
    size_t skipped = 0;
    size_t failed = 0;
};

CacheRemovalSummary RemoveVisualMediaPlayerCaches(const std::vector<std::wstring>& roots) {
    CacheRemovalSummary summary;
    for (const auto& rawRoot : roots) {
        const fs::path root = fs::path(rawRoot).lexically_normal();
        const DWORD rootAttrs = GetFileAttributesW(root.c_str());
        if (rootAttrs == INVALID_FILE_ATTRIBUTES || (rootAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (rootAttrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            ++summary.skipped;
            continue;
        }

        std::vector<fs::path> cacheDirs;
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) {
            ++summary.skipped;
            continue;
        }
        bool scanFailed = false;
        for (; it != end; it.increment(ec)) {
            if (ec) { scanFailed = true; break; }
            const fs::path entry = it->path();
            const DWORD attrs = GetFileAttributesW(entry.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) { scanFailed = true; break; }
            if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                it.disable_recursion_pending();
                continue;
            }
            std::wstring name = entry.filename().wstring();
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (name == L".visualmediaplayer-cache") {
                cacheDirs.push_back(entry.lexically_normal());
                it.disable_recursion_pending();
            }
        }
        if (scanFailed) {
            ++summary.skipped;
            continue;
        }

        for (const auto& cache : cacheDirs) {
            // Never recurse through junctions/symlinks that somebody may have placed
            // inside the cache folder. If anything is uncertain, leave the cache alone.
            if (IsDirectoryReparsePoint(cache) || TreeContainsReparsePoint(cache)) {
                ++summary.skipped;
                continue;
            }
            ec.clear();
            const uintmax_t removed = fs::remove_all(cache, ec);
            if (ec) ++summary.failed;
            else if (removed > 0) ++summary.removed;
        }
    }
    return summary;
}

bool ConfirmUninstall(bool& deleteCaches) {
    deleteCaches = false;
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hInstance = GetModuleHandleW(nullptr);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    cfg.dwCommonButtons = TDCBF_YES_BUTTON | TDCBF_NO_BUTTON;
    cfg.pszWindowTitle = L"Uninstall VisualMediaPlayer";
    cfg.pszMainInstruction = L"Do you wish to uninstall VisualMediaPlayer?";
    cfg.pszContent = L"Do you want to also delete the created cache folders?";
    cfg.pszVerificationText = L"Delete created cache folders";
    cfg.nDefaultButton = IDNO;

    int button = IDNO;
    BOOL checked = FALSE;
    const HRESULT hr = TaskDialogIndirect(&cfg, &button, nullptr, &checked);
    if (SUCCEEDED(hr)) {
        deleteCaches = checked != FALSE;
        return button == IDYES;
    }

    // TaskDialog is available on supported Windows versions. If it cannot be
    // shown, keep cache deletion disabled and fall back to a simple confirmation.
    const int answer = MessageBoxW(nullptr,
        L"Do you wish to uninstall VisualMediaPlayer?",
        L"Uninstall VisualMediaPlayer", MB_YESNO | MB_DEFBUTTON2);
    deleteCaches = false;
    return answer == IDYES;
}

bool WriteResourceToFile(int resourceId, const std::wstring& path) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(nullptr, resource);
    if (!loaded) return false;
    const DWORD size = SizeofResource(nullptr, resource);
    const void* data = LockResource(loaded);
    if (!data || !size) return false;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(file, data, size, &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    return ok && written == size;
}

bool WriteResourceToFileIfMissing(int resourceId,const std::wstring& path) {
    if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) return true;
    return WriteResourceToFile(resourceId,path);
}

bool InstallButtonAssets(const std::wstring& installDir) {
    const std::wstring assetDir=JoinPath(installDir,L"ButtonAssets");
    const int dirResult=SHCreateDirectoryExW(nullptr,assetDir.c_str(),nullptr);
    if(dirResult!=ERROR_SUCCESS && dirResult!=ERROR_ALREADY_EXISTS && dirResult!=ERROR_FILE_EXISTS &&
       GetFileAttributesW(assetDir.c_str())==INVALID_FILE_ATTRIBUTES) return false;
    struct Entry { int id; const wchar_t* name; };
    static const Entry assets[]={
        {IDR_BUTTON_ASSET_0,L"README.txt"},
        {IDR_BUTTON_ASSET_1,L"Background_Active.slot"},
        {IDR_BUTTON_ASSET_2,L"Background_Inactive.slot"},
        {IDR_BUTTON_ASSET_3,L"AutoNext.slot"},
        {IDR_BUTTON_ASSET_4,L"Back.slot"},
        {IDR_BUTTON_ASSET_5,L"FullscreenExit.slot"},
        {IDR_BUTTON_ASSET_6,L"FullscreenEnter.slot"},
        {IDR_BUTTON_ASSET_7,L"Image.slot"},
        {IDR_BUTTON_ASSET_8,L"LoadEverything.slot"},
        {IDR_BUTTON_ASSET_9,L"Native.slot"},
        {IDR_BUTTON_ASSET_10,L"Next.slot"},
        {IDR_BUTTON_ASSET_11,L"OpenFolder.slot"},
        {IDR_BUTTON_ASSET_12,L"Pause.slot"},
        {IDR_BUTTON_ASSET_13,L"Play.slot"},
        {IDR_BUTTON_ASSET_14,L"Previous.slot"},
        {IDR_BUTTON_ASSET_15,L"Refresh.slot"},
        {IDR_BUTTON_ASSET_16,L"Rewind.slot"},
        {IDR_BUTTON_ASSET_17,L"Forward.slot"},
        {IDR_BUTTON_ASSET_18,L"Video.slot"},
        {IDR_BUTTON_ASSET_19,L"Volume.slot"},
        {IDR_BUTTON_ASSET_20,L"VolumeMute.slot"},
        {IDR_BUTTON_ASSET_21,L"AlwaysOnTop.slot"},
    };
    for(const auto& asset:assets){
        if(!WriteResourceToFileIfMissing(asset.id,JoinPath(assetDir,asset.name))) return false;
    }
    return true;
}

bool CreateShortcut(const std::wstring& shortcutPath, const std::wstring& targetPath, const std::wstring& workingDir) {
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr) || !link) return false;
    link->SetPath(targetPath.c_str());
    link->SetWorkingDirectory(workingDir.c_str());
    link->SetDescription(kProductName);
    link->SetIconLocation(targetPath.c_str(), 0);

    IPersistFile* persist = nullptr;
    hr = link->QueryInterface(IID_PPV_ARGS(&persist));
    bool ok = false;
    if (SUCCEEDED(hr) && persist) {
        ok = SUCCEEDED(persist->Save(shortcutPath.c_str(), TRUE));
        persist->Release();
    }
    link->Release();
    return ok;
}

bool AppIsRunning() {
    return FindWindowW(kMainWindowClass, nullptr) != nullptr;
}

bool RemoveStartMenuFile(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return true;
    if (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(path.c_str())) return true;

    // Explorer can transiently hold .lnk files. Ask the shell to remove the entry now
    // before falling back to delayed deletion at reboot. pFrom must be double-NUL ended.
    std::wstring from = path;
    from.push_back(L'\0');
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int shellRc = SHFileOperationW(&op);
    if (shellRc == 0 && !op.fAnyOperationsAborted && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;

    MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

void RemoveStartMenuEntriesAtRoot(const std::wstring& programsRoot) {
    if (programsRoot.empty()) return;

    // Restrict cleanup to VisualMediaPlayer-owned entries under this Start Menu Programs directory.
    const wchar_t* shortcutNames[] = {
        L"VisualMediaPlayer.lnk"
    };
    for (const wchar_t* name : shortcutNames)
        RemoveStartMenuFile(JoinPath(programsRoot, name));

    const wchar_t* productFolders[] = {
        L"VisualMediaPlayer"
    };
    for (const wchar_t* name : productFolders) {
        const std::wstring folder = JoinPath(programsRoot, name);
        std::error_code ec;
        fs::remove_all(fs::path(folder), ec);
        if (GetFileAttributesW(folder.c_str()) != INVALID_FILE_ATTRIBUTES)
            MoveFileExW(folder.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, programsRoot.c_str(), nullptr);
}

void RemoveStartMenuEntries(const std::wstring& commonPrograms) {
    RemoveStartMenuEntriesAtRoot(commonPrograms);
    // Also clean per-user Start Menu entries left by older/non-elevated builds.
    const std::wstring userPrograms = KnownFolder(FOLDERID_Programs);
    if (!userPrograms.empty() && _wcsicmp(userPrograms.c_str(), commonPrograms.c_str()) != 0)
        RemoveStartMenuEntriesAtRoot(userPrograms);
}


bool IsUninstallCommand() {
    // The installed copy is named Uninstall.exe. Treat launching it directly as an
    // uninstall request too; otherwise it would accidentally enter the installer path.
    std::wstring selfName = ModulePath();
    const size_t slash = selfName.find_last_of(L"\\/");
    if (slash != std::wstring::npos) selfName = selfName.substr(slash + 1);
    std::transform(selfName.begin(), selfName.end(), selfName.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (selfName == L"uninstall.exe") return true;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool result = false;
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring arg(argv[i]);
            std::transform(arg.begin(), arg.end(), arg.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (arg == L"/uninstall" || arg == L"-uninstall") result = true;
        }
        LocalFree(argv);
    }
    return result;
}

bool RegisterOpenWith(const std::wstring& appPath) {
    const std::wstring command = L"\"" + appPath + L"\" \"%1\"";
    bool ok = true;
    ok = SetRegString(HKEY_LOCAL_MACHINE, kMachineAppKey, L"FriendlyAppName", kProductName) && ok;
    ok = SetRegString(HKEY_LOCAL_MACHINE, std::wstring(kMachineAppKey) + L"\\shell\\open\\command", nullptr, command) && ok;

    static const wchar_t* supported[] = {
        L".mp4", L".m4v", L".mkv", L".mk3d", L".webm", L".avi", L".divx", L".mov", L".qt", L".wmv", L".asf", L".mpg", L".mpeg", L".mpe", L".mpv", L".mpv2", L".m1v", L".m2v", L".m2p", L".ts", L".m2t", L".mts", L".m2ts", L".tp", L".trp", L".vob", L".vro", L".ogv", L".ogm", L".flv", L".f4v", L".f4p", L".3gp", L".3g2", L".3gp2", L".3gpp", L".rm", L".rmvb", L".rv", L".mxf", L".gxf", L".dv", L".dif", L".dvr-ms", L".wtv", L".mod", L".tod", L".amv", L".ivf", L".y4m", L".nut", L".nsv", L".roq", L".smk", L".bik", L".bk2", L".mjpeg", L".mjpg", L".mjp", L".h264", L".264", L".avc", L".h265", L".265", L".hevc", L".vp8", L".vp9", L".av1", L".r3d", L".braw", L".ari", L".cine", L".crm", L".insv", L".lrv", L".360", L".evo", L".mj2",
        L".jpg", L".jpeg", L".jpe", L".jfif", L".jif", L".jfi", L".png", L".apng", L".bmp", L".dib", L".gif", L".tif", L".tiff", L".webp", L".heic", L".heif", L".hif", L".avif", L".avifs", L".jxl", L".jp2", L".j2k", L".j2c", L".jpf", L".jpx", L".jpm", L".jxr", L".wdp", L".hdp", L".tga", L".targa", L".icb", L".vda", L".vst", L".dds", L".pcx", L".ico", L".cur", L".mng", L".psd", L".psb", L".exr", L".hdr", L".rgbe", L".pic", L".pfm", L".pnm", L".ppm", L".pgm", L".pbm", L".pam", L".qoi", L".sgi", L".rgb", L".rgba", L".bw", L".ras", L".sun", L".xbm", L".xpm", L".svg", L".svgz", L".dng", L".cr2", L".cr3", L".crw", L".nef", L".nrw", L".arw", L".srf", L".sr2", L".raf", L".orf", L".rw2", L".rwl", L".pef", L".x3f", L".3fr", L".fff", L".iiq", L".erf", L".mef", L".mos", L".mrw", L".kdc", L".dcr", L".raw", L".srw", L".bay", L".cap", L".eip", L".mdc", L".rwz"
    };
    const std::wstring typesKey = std::wstring(kMachineAppKey) + L"\\SupportedTypes";
    for (const wchar_t* ext : supported)
        ok = SetRegEmptyString(HKEY_LOCAL_MACHINE, typesKey, ext) && ok;
    return ok;
}

bool RegisterInstalledApp(const std::wstring& installDir, const std::wstring& appPath, const std::wstring& uninstallPath) {
    bool ok = true;
    ok = SetRegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayName", kProductName) && ok;
    ok = SetRegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion", kVersion) && ok;
    ok = SetRegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayIcon", appPath) && ok;
    ok = SetRegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"InstallLocation", installDir) && ok;
    ok = SetRegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString", L"\"" + uninstallPath + L"\" /uninstall") && ok;
    ok = SetRegString(HKEY_LOCAL_MACHINE, kUninstallKey, L"Publisher", L"VisualMediaPlayer") && ok;
    ok = SetRegDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoModify", 1) && ok;
    ok = SetRegDword(HKEY_LOCAL_MACHINE, kUninstallKey, L"NoRepair", 1) && ok;

    ok = SetRegString(HKEY_LOCAL_MACHINE, kAppPathsKey, nullptr, appPath) && ok;
    ok = SetRegString(HKEY_LOCAL_MACHINE, kAppPathsKey, L"Path", installDir) && ok;
    ok = RegisterOpenWith(appPath) && ok;
    return ok;
}

bool Install() {
    if (AppIsRunning()) {
        MessageBoxW(nullptr, L"Close VisualMediaPlayer before installing or updating it.", kProductName, MB_OK | MB_ICONWARNING);
        return false;
    }

    const int answer = MessageBoxW(nullptr,
        L"Install VisualMediaPlayer on this PC?\n\n"
        L"It will be installed in Program Files and added to the Start Menu and Windows Installed Apps.",
        L"VisualMediaPlayer Setup", MB_OKCANCEL | MB_ICONINFORMATION);
    if (answer != IDOK) return false;

    const std::wstring programFiles = KnownFolder(FOLDERID_ProgramFiles);
    const std::wstring commonPrograms = KnownFolder(FOLDERID_CommonPrograms);
    if (programFiles.empty() || commonPrograms.empty()) {
        MessageBoxW(nullptr, L"Windows installation folders could not be located.", L"Setup error", MB_OK | MB_ICONERROR);
        return false;
    }

    const std::wstring installDir = JoinPath(programFiles, kProductName);
    const std::wstring appPath = JoinPath(installDir, kAppExe);
    const std::wstring uninstallPath = JoinPath(installDir, kUninstallExe);
    const std::wstring startShortcut = JoinPath(commonPrograms, L"VisualMediaPlayer.lnk");

    const int dirResult = SHCreateDirectoryExW(nullptr, installDir.c_str(), nullptr);
    if (dirResult != ERROR_SUCCESS && dirResult != ERROR_ALREADY_EXISTS && dirResult != ERROR_FILE_EXISTS) {
        if (GetFileAttributesW(installDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            MessageBoxW(nullptr, L"Could not create the installation folder.", L"Setup error", MB_OK | MB_ICONERROR);
            return false;
        }
    }

    if (!WriteResourceToFile(IDR_APP_PAYLOAD, appPath)) {
        MessageBoxW(nullptr, L"Could not install VisualMediaPlayer.exe.", L"Setup error", MB_OK | MB_ICONERROR);
        return false;
    }
    if(!InstallButtonAssets(installDir)) {
        DeleteFileW(appPath.c_str());
        MessageBoxW(nullptr,L"Could not install the editable ButtonAssets folder.",L"Setup error",MB_OK|MB_ICONERROR);
        return false;
    }

    const std::wstring self = ModulePath();
    if (self.empty() || !CopyFileW(self.c_str(), uninstallPath.c_str(), FALSE)) {
        DeleteFileW(appPath.c_str());
        MessageBoxW(nullptr, L"Could not create the uninstaller.", L"Setup error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Remove stale Start Menu entries from older builds before writing the current one.
    RemoveStartMenuEntries(commonPrograms);
    if (!CreateShortcut(startShortcut, appPath, installDir)) {
        MessageBoxW(nullptr,
            L"The application files were installed, but the Start Menu shortcut could not be created. Setup will not report this installation as complete.",
            L"Setup error", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!RegisterInstalledApp(installDir, appPath, uninstallPath)) {
        RemoveStartMenuFile(startShortcut);
        MessageBoxW(nullptr,
            L"The application files were installed, but one or more required Windows registration entries could not be written. Setup will not report this installation as complete.",
            L"Setup error", MB_OK | MB_ICONERROR);
        return false;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    const int launch = MessageBoxW(nullptr,
        L"VisualMediaPlayer was installed successfully.\n\nOpen it now?",
        L"Installation complete", MB_YESNO | MB_ICONINFORMATION);
    if (launch == IDYES) ShellExecuteW(nullptr, L"open", appPath.c_str(), nullptr, installDir.c_str(), SW_SHOWNORMAL);
    return true;
}

void RemoveUserSettings() {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    if (local.empty()) return;
    const std::wstring settingsDir = JoinPath(local, L"VisualMediaPlayer");
    DeleteFileW(JoinPath(settingsDir, L"settings.ini").c_str());
    RemoveDirectoryW(settingsDir.c_str());
}

bool Uninstall() {
    if (AppIsRunning()) {
        MessageBoxW(nullptr, L"Close VisualMediaPlayer before uninstalling it.", kProductName, MB_OK | MB_ICONWARNING);
        return false;
    }

    bool deleteCaches = false;
    if (!ConfirmUninstall(deleteCaches)) return false;

    // Read cache roots before settings are removed. Cache cleanup is strictly opt-in.
    const std::vector<std::wstring> cacheRoots = deleteCaches ? ReadKnownLibraryRoots() : std::vector<std::wstring>{};

    const std::wstring programFiles = KnownFolder(FOLDERID_ProgramFiles);
    const std::wstring commonPrograms = KnownFolder(FOLDERID_CommonPrograms);
    const std::wstring installDir = JoinPath(programFiles, kProductName);

    if (deleteCaches) RemoveVisualMediaPlayerCaches(cacheRoots);

    RemoveStartMenuEntries(commonPrograms);
    DeleteRegTree(HKEY_LOCAL_MACHINE, kUninstallKey);
    DeleteRegTree(HKEY_LOCAL_MACHINE, kAppPathsKey);
    DeleteRegTree(HKEY_LOCAL_MACHINE, kMachineAppKey);
    DeleteRegTree(HKEY_CURRENT_USER, kUserAppKey);
    RemoveUserSettings();
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    // Security-first self-cleanup: never copy and execute the elevated uninstaller
    // from a user-writable temporary directory. Remove everything except the running
    // uninstaller now, then let Windows delete that final file and directory at reboot.
    const std::wstring self = ModulePath();
    std::error_code cleanupError;
    std::vector<fs::path> cleanupEntries;
    if (GetFileAttributesW(installDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        fs::directory_iterator it(fs::path(installDir), fs::directory_options::skip_permission_denied, cleanupError), end;
        for (; !cleanupError && it != end; it.increment(cleanupError)) {
            const fs::path entry = it->path();
            if (!self.empty() && _wcsicmp(entry.c_str(), self.c_str()) == 0) continue;
            cleanupEntries.push_back(entry);
        }
    }
    for (const auto& entry : cleanupEntries) {
        std::error_code removeError;
        fs::remove_all(entry, removeError);
    }

    bool delayedSelfDelete = false;
    if (!self.empty() && GetFileAttributesW(self.c_str()) != INVALID_FILE_ATTRIBUTES) {
        delayedSelfDelete = MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
    }
    if (GetFileAttributesW(installDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        // The directory cannot disappear until this running executable exits; schedule
        // it after the file so Windows can finish cleanup safely on the next restart.
        MoveFileExW(installDir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    }

    const wchar_t* message = delayedSelfDelete
        ? L"VisualMediaPlayer has been uninstalled. Windows will remove the final cleanup file after the next restart."
        : L"VisualMediaPlayer has been uninstalled. A small cleanup file may remain in Program Files until it can be removed manually.";
    MessageBoxW(nullptr, message, kProductName, MB_OK | MB_ICONINFORMATION);
    return true;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninstall = IsUninstallCommand();
    bool ok = false;
    // Run the installed uninstaller in place. Self-deletion is scheduled with
    // MoveFileEx rather than executing an elevated copy from a writable temp path.
    ok = uninstall ? Uninstall() : Install();
    if (SUCCEEDED(coHr)) CoUninitialize();
    return ok ? 0 : 1;
}
