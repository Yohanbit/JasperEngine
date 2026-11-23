// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_STORAGE_SPACE_H
#define ANDROID_STORAGE_SPACE_H

// Free space of a folder, without Android headers (as AndroidTextInput.h): the game thread asks
// before a save, a captured screenshot or an imported asset, and the platform answers from its own
// API (Android: StatFs, which is not statvfs and not GetDiskFreeSpaceEx; iOS: volumeAvailableCapacity).
//
// Why the runtime wants this at all: a phone's internal storage is small and shared, a save that
// fails halfway is a corrupt save (io::writeFileAtomic already refuses to truncate, and a full disk
// fails its rename), and "no space left on the device" as the only symptom tells a player nothing.
// The policy below is the engine's: what a write may take, what is reserved, and the warning
// thresholds a runtime uses to free its caches before the OS kills it.
//
// The system call is a seam (SpaceQuery), so the policy runs on any host and the tests do not
// need a full disk: one platform each fills it (platform/android reads StatFs, platform/apple
// NSFileManager) and every test passes its own.

#include <cstddef>
#include <cstdint>

namespace jasperengine::android {

    // Bytes, of the volume that holds a folder.
    struct SpaceInfo {
        bool known = false;
        uint64_t totalBytes = 0;
        // free to any app (StatFs::getAvailableBytes: what this app may actually write)
        uint64_t availableBytes = 0;
        // free but kept back for the system's own use (reserved blocks, a provider's quota)
        uint64_t reservedBytes = 0;
        // the app's own quota, 0 when it has none (Android's scoped storage can cap one)
        uint64_t appQuotaBytes = 0;
        uint64_t appUsedBytes = 0;

        // what this app may still write: its quota's rest when it has one, else the volume's
        uint64_t writableBytes() const {
            uint64_t writable = availableBytes;
            if (appQuotaBytes > 0) {
                const uint64_t rest = appQuotaBytes > appUsedBytes ? appQuotaBytes - appUsedBytes : 0;
                writable = writable < rest ? writable : rest;
            }
            return writable;
        }
    };

    // The platform's own answer: the folder's path in the file system (UTF-8)
    using SpaceQuery = SpaceInfo (*)(const char* systemPath);

    // A space reading that no call made: the policy treats it as unknown and never refuses
    // (a platform without the call must not stop a save it could have written)
    inline SpaceInfo unknownSpace() {
        return SpaceInfo();
    }

    struct WriteAllowance {
        bool allowed = true;
        // bytes the write may take
        uint64_t budget = 0;
        // true when the reading was unknown: the write proceeds whatever its size
        bool unknown = true;
        // a warning threshold was crossed, false ones do not
        bool low = false;
        bool critical = false;
    };

    struct SpacePolicy {
        // Kept free past the write itself: a full volume breaks the OS, not just this app (a
        // save that leaves 0 bytes free stops the next boot of the launcher). 8 MiB is about a
        // hundred saves and a screenshot.
        uint64_t reserveBytes = 8u * 1024u * 1024u;
        // Below this the runtime frees its own caches (low); below the second one it stops
        // taking on new optional work (critical). Percentages of the volume's total, not of what
        // is free, so a 32 GB phone and a 256 GB one warn at the same proportion.
        uint32_t lowPercent = 10;
        uint32_t criticalPercent = 4;
    };

    // Whether a write of `bytes` may be attempted, and how close the volume is to full.
    //
    // With a known reading: allowed when the writable bytes exceed reserveBytes + bytes. A write
    // bigger than what a volume has is refused before it starts, which is the point: the atomic
    // writer would otherwise write the temporary file, hit ENOSPC on the flush and leave the
    // target untouched but the folder full of temporaries.
    inline WriteAllowance allowWrite(const SpaceInfo& space, uint64_t bytes, const SpacePolicy& policy = {}) {
        WriteAllowance allowance;
        if (!space.known) {
            allowance.allowed = true;
            allowance.unknown = true;
            allowance.budget = 0;
            return allowance;
        }
        allowance.unknown = false;
        const uint64_t writable = space.writableBytes();
        // the write itself plus the reserve; a sum that would wrap means "as much as there is"
        const uint64_t needed = bytes > UINT64_MAX - policy.reserveBytes ? UINT64_MAX : bytes + policy.reserveBytes;
        allowance.budget = writable > policy.reserveBytes ? writable - policy.reserveBytes : 0;
        allowance.allowed = writable >= needed;

        if (space.totalBytes > 0) {
            // percent of total free
            const uint64_t lowMark = space.totalBytes / 100u * policy.lowPercent;
            const uint64_t criticalMark = space.totalBytes / 100u * policy.criticalPercent;
            allowance.critical = writable <= criticalMark;
            allowance.low = !allowance.critical && writable <= lowMark;
        }
        return allowance;
    }

    // The cache a runtime may drop when `allowance.critical` (or `.low`): the sizes the folder
    // already knows, from largest to smallest, so the caller frees as little as it must.
    struct CacheCandidate {
        const char* label = "";
        uint64_t bytes = 0;
    };

    // `needed` bytes below the reserve: which candidates to delete to get back above it, largest
    // first; `freed` is the sum of what they hold. Empty when nothing needs freeing, and every
    // candidate is listed when even all of them are not enough (the caller reports it, rather
    // than deleting all of them and still failing).
    template <size_t N>
    inline size_t planCacheRelease(const SpaceInfo& space, const CacheCandidate (&candidates)[N], const SpacePolicy& policy = {}) {
        // an unknown volume, or one that is not critical: nothing to free
        if (!space.known) return 0;
        if (!allowWrite(space, 0, policy).critical) return 0;

        // how much must come back for the volume to sit above the reserve again
        const uint64_t writable = space.writableBytes();
        uint64_t needed = policy.reserveBytes > writable ? policy.reserveBytes - writable : 0;
        if (needed == 0) return 0;

        // largest first, without sorting the caller's array (indices, N is small)
        bool used[N] = {};
        size_t chosen = 0;
        uint64_t freed = 0;
        while (freed < needed) {
            size_t best = N;
            for (size_t i = 0; i < N; i++) {
                if (used[i]) continue;
                if (best == N || candidates[i].bytes > candidates[best].bytes) best = i;
            }
            if (best == N) break;
            used[best] = true;
            if (candidates[best].bytes == 0) continue;
            freed += candidates[best].bytes;
            chosen++;
        }
        return chosen;
    }

}

#endif // ANDROID_STORAGE_SPACE_H
