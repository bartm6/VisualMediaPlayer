#pragma once

#include <cstdint>

namespace vmp {

inline constexpr std::uint64_t kMemoryPolicyMiB = 1024ull * 1024ull;
inline constexpr std::uint64_t kMemoryPolicyGiB = 1024ull * kMemoryPolicyMiB;

enum class SystemMemoryPressure {
    Normal,
    Elevated,
    Critical,
};

struct MemoryCachePolicy {
    // CPU thumbnail-cache hysteresis. The cache may grow freely below softCeiling.
    // Crossing softCeiling trims cold LRU thumbnails back to warmTarget. hardCap is
    // a safety boundary for sustained/active loading, not a normal operating target.
    std::uint64_t libraryWarmTarget = 768ull * kMemoryPolicyMiB;
    std::uint64_t libraryCpuSoftCeiling = 1024ull * kMemoryPolicyMiB;
    std::uint64_t libraryCpuHardCap = 1280ull * kMemoryPolicyMiB;

    // Only genuine machine-wide critical memory pressure may trim below warmTarget.
    std::uint64_t libraryCriticalTarget = 384ull * kMemoryPolicyMiB;

    // Process-level boundaries used to shed reconstructable preview/detail state. These
    // are capped by tier; machines above 32 GiB intentionally use the same 32-GiB+ tier.
    std::uint64_t normalProcess = 1536ull * kMemoryPolicyMiB;
    std::uint64_t highPressure = 2ull * kMemoryPolicyGiB;
    std::uint64_t emergency = 2560ull * kMemoryPolicyMiB;
    std::uint64_t allocationGuard = 2816ull * kMemoryPolicyMiB;
    std::uint64_t panicRelease = 3ull * kMemoryPolicyGiB;

    // Available-RAM thresholds for machine-wide pressure classification.
    std::uint64_t elevatedAvailableBytes = 512ull * kMemoryPolicyMiB;
    std::uint64_t criticalAvailableBytes = 256ull * kMemoryPolicyMiB;
};

MemoryCachePolicy SelectMemoryCachePolicy(std::uint64_t installedPhysicalBytes);

SystemMemoryPressure ClassifySystemMemoryPressure(const MemoryCachePolicy& policy,
                                                  std::uint64_t availablePhysicalBytes,
                                                  std::uint32_t memoryLoadPercent);

// Returns the byte target to use when a CPU thumbnail trim is actually necessary.
// Elevated pressure never collapses the cache below its normal warm target; Critical
// pressure is the only state allowed to use the smaller critical target.
std::uint64_t LibraryTrimTarget(const MemoryCachePolicy& policy,
                                SystemMemoryPressure pressure);

} // namespace vmp
