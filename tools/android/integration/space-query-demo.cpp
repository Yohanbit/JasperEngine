// The querySpace the document gives, compiled and run on a Linux host: the same bionic call the
// phone uses, on a real volume.
#include "AndroidStorageSpace.h"
#include <string>
#include <sys/statvfs.h>
#include <cstdio>

static jasperengine::android::SpaceInfo querySpace(const std::string& folder) {
    struct statvfs stats;
    if (statvfs(folder.c_str(), &stats) != 0) return jasperengine::android::unknownSpace();
    jasperengine::android::SpaceInfo space;
    space.known = true;
    space.totalBytes = (uint64_t)stats.f_blocks * stats.f_frsize;
    space.availableBytes = (uint64_t)stats.f_bavail * stats.f_frsize;
    space.reservedBytes = (uint64_t)(stats.f_bfree - stats.f_bavail) * stats.f_frsize;
    return space;
}

int main() {
    const auto space = querySpace("/tmp");
    std::printf("known=%d total=%.1f GB available=%.1f GB reserved=%.1f MB\n",
                space.known ? 1 : 0, space.totalBytes / 1073741824.0,
                space.availableBytes / 1073741824.0, space.reservedBytes / 1048576.0);
    const auto allowance = jasperengine::android::allowWrite(space, 1u << 20);
    std::printf("a 1 MB save: allowed=%d low=%d critical=%d\n",
                allowance.allowed ? 1 : 0, allowance.low ? 1 : 0, allowance.critical ? 1 : 0);
    // a folder that is not there: unknown, and then nothing is refused
    const auto missing = querySpace("/nonexistent/folder");
    std::printf("missing folder: known=%d, and a 999 GB write is allowed=%d\n", missing.known ? 1 : 0,
                jasperengine::android::allowWrite(missing, 999ull << 30).allowed ? 1 : 0);
    return 0;
}
