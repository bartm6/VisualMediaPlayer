#include "MemoryCachePolicy.h"

namespace vmp {
namespace {
constexpr std::uint64_t MiB = kMemoryPolicyMiB;
constexpr std::uint64_t GiB = kMemoryPolicyGiB;
}

MemoryCachePolicy SelectMemoryCachePolicy(std::uint64_t installedPhysicalBytes) {
    // Round to the advertised/nominal GiB tier so firmware reservations do not turn a
    // 16-GiB machine into the 8-GiB tier or a 32-GiB machine into the 16-GiB tier.
    const std::uint64_t installedGiBRounded = (installedPhysicalBytes + GiB / 2ull) / GiB;
    MemoryCachePolicy policy{};

    if (installedGiBRounded >= 32ull) {
        // Highest tier by design. 64/96/128+ GiB machines use these same values.
        policy.libraryWarmTarget = 8ull * GiB;
        policy.libraryCpuSoftCeiling = 10ull * GiB;
        policy.libraryCpuHardCap = 12ull * GiB;
        policy.libraryCriticalTarget = 3ull * GiB;
        policy.normalProcess = 12ull * GiB;
        policy.highPressure = 13824ull * MiB;       // 13.5 GiB
        policy.emergency = 14848ull * MiB;          // 14.5 GiB
        policy.allocationGuard = 15872ull * MiB;    // 15.5 GiB
        policy.panicRelease = 16ull * GiB;
        policy.elevatedAvailableBytes = 3ull * GiB;
        policy.criticalAvailableBytes = 1ull * GiB;
    } else if (installedGiBRounded >= 16ull) {
        policy.libraryWarmTarget = 5ull * GiB;
        policy.libraryCpuSoftCeiling = 6656ull * MiB; // 6.5 GiB
        policy.libraryCpuHardCap = 8ull * GiB;
        policy.libraryCriticalTarget = 2ull * GiB;
        policy.normalProcess = 8ull * GiB;
        policy.highPressure = 9728ull * MiB;        // 9.5 GiB
        policy.emergency = 11ull * GiB;
        policy.allocationGuard = 12ull * GiB;
        policy.panicRelease = 13ull * GiB;
        policy.elevatedAvailableBytes = 2ull * GiB;
        policy.criticalAvailableBytes = 768ull * MiB;
    } else if (installedGiBRounded >= 8ull) {
        policy.libraryWarmTarget = 2560ull * MiB;    // 2.5 GiB
        policy.libraryCpuSoftCeiling = 3328ull * MiB; // 3.25 GiB
        policy.libraryCpuHardCap = 4ull * GiB;
        policy.libraryCriticalTarget = 1ull * GiB;
        policy.normalProcess = 4608ull * MiB;        // 4.5 GiB
        policy.highPressure = 5376ull * MiB;         // 5.25 GiB
        policy.emergency = 6ull * GiB;
        policy.allocationGuard = 6656ull * MiB;      // 6.5 GiB
        policy.panicRelease = 7ull * GiB;
        policy.elevatedAvailableBytes = 1280ull * MiB;
        policy.criticalAvailableBytes = 512ull * MiB;
    } else if (installedGiBRounded >= 4ull) {
        policy.libraryWarmTarget = 1280ull * MiB;    // 1.25 GiB
        policy.libraryCpuSoftCeiling = 1792ull * MiB; // 1.75 GiB
        policy.libraryCpuHardCap = 2304ull * MiB;    // 2.25 GiB
        policy.libraryCriticalTarget = 512ull * MiB;
        policy.normalProcess = 2560ull * MiB;        // 2.5 GiB
        policy.highPressure = 3ull * GiB;
        policy.emergency = 3584ull * MiB;            // 3.5 GiB
        policy.allocationGuard = 3840ull * MiB;      // 3.75 GiB
        policy.panicRelease = 4ull * GiB;
        policy.elevatedAvailableBytes = 768ull * MiB;
        policy.criticalAvailableBytes = 384ull * MiB;
    }

    return policy;
}

SystemMemoryPressure ClassifySystemMemoryPressure(const MemoryCachePolicy& policy,
                                                  std::uint64_t availablePhysicalBytes,
                                                  std::uint32_t memoryLoadPercent) {
    if (memoryLoadPercent >= 97u || availablePhysicalBytes < policy.criticalAvailableBytes) {
        return SystemMemoryPressure::Critical;
    }
    if (memoryLoadPercent >= 92u || availablePhysicalBytes < policy.elevatedAvailableBytes) {
        return SystemMemoryPressure::Elevated;
    }
    return SystemMemoryPressure::Normal;
}

std::uint64_t LibraryTrimTarget(const MemoryCachePolicy& policy,
                                SystemMemoryPressure pressure) {
    return pressure == SystemMemoryPressure::Critical
        ? policy.libraryCriticalTarget
        : policy.libraryWarmTarget;
}

} // namespace vmp
