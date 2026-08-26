#include "SingleInstanceIpc.h"

#include <sddl.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

#pragma comment(lib, "advapi32.lib")

namespace vmp {
namespace {

struct PipeHeader {
    std::uint32_t magic = kSingleInstanceIpcMagic;
    std::uint32_t version = kSingleInstanceIpcVersion;
    std::uint32_t pathCount = 0;
    std::uint32_t payloadBytes = 0;
};

bool ReadExact(HANDLE pipe, void* buffer, DWORD bytes) {
    auto* out = static_cast<std::uint8_t*>(buffer);
    DWORD done = 0;
    while (done < bytes) {
        DWORD read = 0;
        if (!ReadFile(pipe, out + done, bytes - done, &read, nullptr) || read == 0) return false;
        done += read;
    }
    return true;
}

bool WriteExact(HANDLE pipe, const void* buffer, DWORD bytes) {
    const auto* in = static_cast<const std::uint8_t*>(buffer);
    DWORD done = 0;
    while (done < bytes) {
        DWORD written = 0;
        if (!WriteFile(pipe, in + done, bytes - done, &written, nullptr) || written == 0) return false;
        done += written;
    }
    return true;
}

std::wstring CurrentExecutablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::wstring ProcessExecutablePath(DWORD processId) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return {};
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    if (!ok || length == 0) return {};
    path.resize(length);
    return path;
}

std::wstring NormalizeExecutablePath(std::wstring path) {
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (!path.empty()) CharLowerBuffW(path.data(), static_cast<DWORD>(path.size()));
    return path;
}

bool IsSameExecutableProcess(DWORD processId) {
    const std::wstring self = NormalizeExecutablePath(CurrentExecutablePath());
    const std::wstring other = NormalizeExecutablePath(ProcessExecutablePath(processId));
    return !self.empty() && self == other;
}

bool CurrentUserSidString(std::wstring& sidString) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (bytes == 0) {
        CloseHandle(token);
        return false;
    }
    std::vector<std::uint8_t> buffer(bytes);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        CloseHandle(token);
        return false;
    }
    CloseHandle(token);

    const auto* tokenUser = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sid) || !sid) return false;
    sidString.assign(sid);
    LocalFree(sid);
    return !sidString.empty();
}

std::wstring BuildPipeName(const std::wstring& channelName) {
    std::wstring sid;
    if (!CurrentUserSidString(sid)) return {};
    return L"\\\\.\\pipe\\" + channelName + L"." + sid;
}

bool BuildPipeSecurity(SECURITY_ATTRIBUTES& attributes, PSECURITY_DESCRIPTOR& descriptor) {
    std::wstring sid;
    if (!CurrentUserSidString(sid)) return false;
    // Protected DACL: only LocalSystem and the current user may connect.
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
    descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return false;
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return true;
}

bool ValidatePipePeer(HANDLE pipe, bool peerIsClient) {
    ULONG processId = 0;
    const BOOL ok = peerIsClient
        ? GetNamedPipeClientProcessId(pipe, &processId)
        : GetNamedPipeServerProcessId(pipe, &processId);
    return ok && processId != 0 && IsSameExecutableProcess(static_cast<DWORD>(processId));
}

void WakePipeServer(const std::wstring& pipeName) {
    if (pipeName.empty()) return;
    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE pipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            return;
        }
        Sleep(10);
    }
}

} // namespace

bool EncodePathPayload(const std::vector<std::wstring>& paths, std::vector<std::uint8_t>& payload) {
    payload.clear();
    if (paths.size() > kSingleInstanceMaxPaths) return false;

    std::size_t bytes = 0;
    for (const auto& path : paths) {
        if (path.size() > 32767u) return false;
        const std::size_t recordBytes = sizeof(std::uint32_t) + path.size() * sizeof(wchar_t);
        if (recordBytes > kSingleInstanceMaxPayloadBytes - bytes) return false;
        bytes += recordBytes;
    }

    payload.reserve(bytes);
    for (const auto& path : paths) {
        const std::uint32_t length = static_cast<std::uint32_t>(path.size());
        const auto* lengthBytes = reinterpret_cast<const std::uint8_t*>(&length);
        payload.insert(payload.end(), lengthBytes, lengthBytes + sizeof(length));
        if (!path.empty()) {
            const auto* textBytes = reinterpret_cast<const std::uint8_t*>(path.data());
            payload.insert(payload.end(), textBytes, textBytes + path.size() * sizeof(wchar_t));
        }
    }
    return payload.size() <= kSingleInstanceMaxPayloadBytes;
}

bool DecodePathPayload(const std::vector<std::uint8_t>& payload, std::uint32_t pathCount,
                       std::vector<std::wstring>& paths) {
    paths.clear();
    if (pathCount > kSingleInstanceMaxPaths || payload.size() > kSingleInstanceMaxPayloadBytes) return false;
    std::size_t offset = 0;
    paths.reserve(pathCount);
    for (std::uint32_t index = 0; index < pathCount; ++index) {
        if (payload.size() - offset < sizeof(std::uint32_t)) return false;
        std::uint32_t length = 0;
        std::memcpy(&length, payload.data() + offset, sizeof(length));
        offset += sizeof(length);
        if (length > 32767u) return false;
        const std::size_t textBytes = static_cast<std::size_t>(length) * sizeof(wchar_t);
        if (textBytes > payload.size() - offset) return false;
        std::wstring path(length, L'\0');
        if (textBytes != 0) std::memcpy(path.data(), payload.data() + offset, textBytes);
        offset += textBytes;
        if (path.find(L'\0') != std::wstring::npos) return false;
        paths.push_back(std::move(path));
    }
    return offset == payload.size();
}

SingleInstancePipeServer::~SingleInstancePipeServer() {
    Stop();
}

bool SingleInstancePipeServer::Start(const std::wstring& channelName, HWND notifyWindow, UINT notifyMessage) {
    Stop();
    if (channelName.empty() || !notifyWindow || notifyMessage < WM_APP) return false;
    channelName_ = channelName;
    pipeName_ = BuildPipeName(channelName_);
    if (pipeName_.empty()) return false;
    notifyWindow_ = notifyWindow;
    notifyMessage_ = notifyMessage;
    stopping_.store(false, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void SingleInstancePipeServer::Stop() {
    stopping_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        WakePipeServer(pipeName_);
        thread_.join();
    }
    notifyWindow_ = nullptr;
    notifyMessage_ = 0;
    channelName_.clear();
    pipeName_.clear();
}

void SingleInstancePipeServer::Run() {
    while (!stopping_.load(std::memory_order_acquire)) {
        SECURITY_ATTRIBUTES attributes{};
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!BuildPipeSecurity(attributes, descriptor)) return;
        HANDLE pipe = CreateNamedPipeW(
            pipeName_.c_str(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 4096, 4096, 0, &attributes);
        LocalFree(descriptor);
        if (pipe == INVALID_HANDLE_VALUE) return;

        const BOOL connected = ConnectNamedPipe(pipe, nullptr)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!connected) {
            CloseHandle(pipe);
            if (stopping_.load(std::memory_order_acquire)) return;
            continue;
        }
        if (stopping_.load(std::memory_order_acquire)) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            return;
        }
        if (!ValidatePipePeer(pipe, true)) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        PipeHeader header{};
        bool valid = ReadExact(pipe, &header, sizeof(header));
        valid = valid && header.magic == kSingleInstanceIpcMagic &&
            header.version == kSingleInstanceIpcVersion &&
            header.pathCount <= kSingleInstanceMaxPaths &&
            header.payloadBytes <= kSingleInstanceMaxPayloadBytes;

        std::vector<std::uint8_t> payload;
        if (valid && header.payloadBytes != 0) {
            payload.resize(header.payloadBytes);
            valid = ReadExact(pipe, payload.data(), header.payloadBytes);
        }

        std::vector<std::wstring> paths;
        if (valid) valid = DecodePathPayload(payload, header.pathCount, paths);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);

        if (valid && notifyWindow_) {
            auto* messagePaths = new (std::nothrow) std::vector<std::wstring>(std::move(paths));
            if (messagePaths && !PostMessageW(notifyWindow_, notifyMessage_, 0,
                                               reinterpret_cast<LPARAM>(messagePaths))) {
                delete messagePaths;
            }
        }
    }
}

bool SendPathsToRunningInstance(const std::wstring& channelName,
                                const std::vector<std::wstring>& paths,
                                DWORD timeoutMs) {
    const std::wstring pipeName = BuildPipeName(channelName);
    if (pipeName.empty()) return false;

    std::vector<std::uint8_t> payload;
    if (!EncodePathPayload(paths, payload)) return false;
    const PipeHeader header{
        kSingleInstanceIpcMagic,
        kSingleInstanceIpcVersion,
        static_cast<std::uint32_t>(paths.size()),
        static_cast<std::uint32_t>(payload.size())
    };

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    do {
        pipe = CreateFileW(pipeName.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) return false;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return false;
        const DWORD remaining = static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 100));
        if (error == ERROR_PIPE_BUSY) WaitNamedPipeW(pipeName.c_str(), remaining);
        else Sleep(std::min<DWORD>(remaining, 25));
    } while (GetTickCount64() < deadline);
    if (pipe == INVALID_HANDLE_VALUE) return false;
    if (!ValidatePipePeer(pipe, false)) {
        CloseHandle(pipe);
        return false;
    }

    const bool ok = WriteExact(pipe, &header, sizeof(header)) &&
        (payload.empty() || WriteExact(pipe, payload.data(), static_cast<DWORD>(payload.size())));
    FlushFileBuffers(pipe);
    CloseHandle(pipe);
    return ok;
}

} // namespace vmp
