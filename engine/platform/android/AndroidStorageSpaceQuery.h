// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_STORAGE_SPACE_QUERY_H
#define ANDROID_STORAGE_SPACE_QUERY_H

// The one Android call AndroidStorageSpace.h's policy needs: statfs() of a folder, turned into a
// SpaceInfo. Kept in its own header, and in its own tiny translation unit, because every other
// Android subsystem header is Android-free on purpose (the core tests compile them on a desktop);
// this one is the only place the platform call lives, so a build that does not care about free
// space still links the rest.
//
// Android's statfs is not statvfs and not GetDiskFreeSpaceEx (nor iOS's volumeAvailableCapacity):
//   f_bsize  the fundamental block size
//   f_blocks total blocks
//   f_bfree  blocks free (including the system's reserved ones)
//   f_bavail blocks free to this app
// Scoped storage can cap an app's own quota; statfs cannot see it, so appQuotaBytes stays 0 and
// writableBytes() reports the volume's free bytes -- the conservative answer, since the OS refuses
// the write when the quota is what ran out and the caller's error path is the one that already
// exists for ENOSPC.

#include "AndroidStorageSpace.h"

#include <sys/statfs.h>

#include <cerrno>
#include <cstdint>
#include <string>

namespace jasperengine::android {

    // The volume that holds `systemPath`, or an unknown reading (never a refusal) when the call
    // fails: an app that cannot stat its own files directory has bigger problems than a save, and
    // stopping the save would not fix them. `known == false` means exactly that to allowWrite().
    inline SpaceInfo querySpace(const char* systemPath) {
        SpaceInfo info;
        if (systemPath == nullptr || systemPath[0] == '\0')
            return info;

        struct statfs stats {};
        if (statfs(systemPath, &stats) != 0)
            return info;

        // f_bsize is the block size everything else is counted in; some filesystems report 0 there
        // and a valid f_frsize, so fall back rather than divide the whole answer by nothing.
        const uint64_t blockSize = stats.f_bsize > 0 ? static_cast<uint64_t>(stats.f_bsize)
                                                     : static_cast<uint64_t>(stats.f_frsize);
        if (blockSize == 0)
            return info;

        info.known = true;
        info.totalBytes = static_cast<uint64_t>(stats.f_blocks) * blockSize;
        info.availableBytes = static_cast<uint64_t>(stats.f_bavail) * blockSize;
        // the blocks the system keeps for itself: f_bfree counts them, f_bavail does not, and the
        // difference is what "reserved" means to a caller deciding how full the volume really is
        info.reservedBytes = stats.f_bfree > stats.f_bavail
            ? static_cast<uint64_t>(stats.f_bfree - stats.f_bavail) * blockSize
            : 0;
        return info;
    }

    // The same reading for a std::string path, which is what System::getUserDataPath() returns.
    inline SpaceInfo querySpace(const std::string& systemPath) {
        return querySpace(systemPath.c_str());
    }

    // A SpaceQuery that answers from this device (AndroidStorageSpace.h's seam).
    inline SpaceQuery platformSpaceQuery() {
        return &querySpace;
    }

    // What a write of `bytes` may do where `folder` lives, reading the volume once.
    inline WriteAllowance allowanceFor(const std::string& folder, uint64_t bytes,
                                       const SpacePolicy& policy = {}) {
        return allowWrite(querySpace(folder), bytes, policy);
    }

} // namespace jasperengine::android

#endif // ANDROID_STORAGE_SPACE_QUERY_H
