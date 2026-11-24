// AndroidStorageSpace and AndroidProjectLayout driven the way an editor or a save path would.
#include "AndroidProjectLayout.h"
#include "AndroidStorageSpace.h"
#include <cstdio>

using namespace jasperengine::android;

// the call the JNI side replaces with StatFs::getAvailableBytes(); here it is a 32 GB phone that
// is nearly full, to show the refusal
static SpaceInfo nearlyFullPhone(const char* systemPath) {
    (void)systemPath;
    SpaceInfo space;
    space.known = true;
    space.totalBytes = 32ull << 30;
    space.availableBytes = 6ull << 20;      // 6 MB free
    space.reservedBytes = 0;
    return space;
}

int main() {
    const SpaceInfo space = nearlyFullPhone("/storage/emulated/0");
    const WriteAllowance small = allowWrite(space, 1u << 20);       // a save of 1 MB
    const WriteAllowance large = allowWrite(space, 40u << 20);      // a texture import of 40 MB
    std::printf("1 MB save:  allowed=%d critical=%d budget=%.1f MB\n", small.allowed ? 1 : 0,
                small.critical ? 1 : 0, small.budget / 1048576.0);
    std::printf("40 MB copy: allowed=%d critical=%d (the import is refused before it starts)\n",
                large.allowed ? 1 : 0, large.critical ? 1 : 0);

    // an unknown reading never refuses: a platform without the call must not block a save
    const WriteAllowance unknown = allowWrite(unknownSpace(), 4ull << 30);
    std::printf("unknown reading, 4 GB write: allowed=%d unknown=%d\n",
                unknown.allowed ? 1 : 0, unknown.unknown ? 1 : 0);

    // ---- where the project is, and what may be done with it
    const std::string internalRoot = "/data/user/0/com.jaspergameengine.jasperengine/files";
    const ProjectLocation privateFolder = classifyLocation(internalRoot + "/MyGame", internalRoot, "");
    const ProjectAccess privateAccess = accessFor(privateFolder);
    std::printf("private: place=%d canOpen=%d canWrite=%d atomicRename=%d\n",
                (int)privateFolder.place, privateAccess.canOpen ? 1 : 0,
                privateAccess.canWrite ? 1 : 0, privateAccess.atomicRename ? 1 : 0);

    // a folder the user picked through SAF: a tree URI and no path
    const ProjectLocation picked = classifyLocation("", internalRoot, internalRoot,
                     "content://com.android.providers.downloads.documents/tree/primary%3AGames");
    const ProjectAccess pickedAccess = accessFor(picked);
    std::printf("SAF tree: place=%d canOpen=%d canWrite=%d canRename=%d\n",
                (int)picked.place, pickedAccess.canOpen ? 1 : 0,
                pickedAccess.canWrite ? 1 : 0, pickedAccess.atomicRename ? 1 : 0);

    // the paths under the picked folder the editor must refuse to follow
    const char* attempts[] = {"assets/../project.yaml", "/etc/passwd", "assets/CON.png", "assets/ok.png",
                              "assets\\ok.png", "C:/ok.png", "assets/"};
    for (const char* attempt : attempts) {
        std::printf("  %-24s inside=%d\n", attempt, isInsideProject(attempt) ? 1 : 0);
    }
    // isProjectCacheFolder takes a folder *name*, not a path: the caller splits first
    for (const char* folder : {"Library", "library", "Temp", "assets"}) {
        std::printf("  folder %-10s cache=%d\n", folder, isProjectCacheFolder(folder) ? 1 : 0);
    }
    return 0;
}
