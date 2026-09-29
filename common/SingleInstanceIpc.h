#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace vmp {

inline constexpr std::uint32_t kSingleInstanceIpcMagic = 0x32504D56u; // "VMP2"
inline constexpr std::uint32_t kSingleInstanceIpcVersion = 2u;
inline constexpr std::size_t kSingleInstanceMaxPaths = 256u;
inline constexpr std::size_t kSingleInstanceMaxPayloadBytes = 1024u * 1024u;

bool EncodePathPayload(const std::vector<std::wstring>& paths, std::vector<std::uint8_t>& payload);
bool DecodePathPayload(const std::vector<std::uint8_t>& payload, std::uint32_t pathCount,
                       std::vector<std::wstring>& paths);

class SingleInstancePipeServer {
public:
    SingleInstancePipeServer() = default;
    ~SingleInstancePipeServer();
    SingleInstancePipeServer(const SingleInstancePipeServer&) = delete;
    SingleInstancePipeServer& operator=(const SingleInstancePipeServer&) = delete;

    bool Start(const std::wstring& channelName, HWND notifyWindow, UINT notifyMessage);
    void Stop();

private:
    void Run();

    std::wstring channelName_;
    std::wstring pipeName_;
    HWND notifyWindow_ = nullptr;
    UINT notifyMessage_ = 0;
    std::atomic<bool> stopping_{false};
    std::thread thread_;
};

bool SendPathsToRunningInstance(const std::wstring& channelName,
                                const std::vector<std::wstring>& paths,
                                DWORD timeoutMs = 5000);

} // namespace vmp
