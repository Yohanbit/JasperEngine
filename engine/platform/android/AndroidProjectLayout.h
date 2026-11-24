// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_PROJECT_LAYOUT_H
#define ANDROID_PROJECT_LAYOUT_H

// A JasperEngine project's folders, and what may be written into them from a phone, without
// Android headers (as AndroidTextInput.h), so the core tests check the rules on any host.
//
// The layout is the engine's, unchanged from desktop (`editor/Project.h`: an `assetsDir` and a
// `luaDir` the project.yaml names, "." and "scripts" by default; scenes at the root; the
// `settings.yaml` an editor keeps beside them). What changes on Android is the storage the project
// sits on:
//
//  - a project folder the user picked through the Storage Access Framework is a tree grant, not a
//    path: every write goes through a DocumentFile, and a rename over a file is not the atomic
//    rename of `io::writeFileAtomic` (providers implement it as copy + delete, or not at all);
//  - a folder under /Android/data or /Android/obb is readable and writable by this app only, and
//    is wiped when the app is uninstalled, so a project there is lost with the app;
//  - a folder on a removable volume may be gone next time the app starts (the SD card taken out);
//  - /Android/data on another app's package is not readable at all since Android 11.
//
// So before an editor opens, saves or imports, it asks where the project is (ProjectLocation) and
// what that place allows (ProjectAccess). The answers are the ones the platform has to give anyway
// to open the folder; keeping the decision here is what makes it testable without a phone.

#include <cstdint>
#include <string>
#include <string_view>

namespace jasperengine::android {

    // ---- the layout

    // The project's folder names, as the engine reads them
    struct ProjectLayout {
        // the folder assets live in, relative to the project's root ("." is the root itself)
        std::string assetsDir = ".";
        // the folder Lua scripts live in, relative to the project's root
        std::string luaDir = "scripts";
        // scenes, prefabs and settings sit at the root
        static constexpr const char* rootScenes = "";
        static constexpr const char* projectFile = "project.yaml";
        static constexpr const char* editorSettings = "settings.yaml";
        static constexpr const char* libraryDir = "Library";
        static constexpr const char* tempDir = "Temp";
    };

    // The folders a phone build may keep inside the project, and how it treats them. Library and
    // Temp are the editor's own caches: regenerable, deleted when the volume is low, and never part
    // of a copy of the project. Everything else is the project.
    inline bool isProjectCacheFolder(std::string_view name) {
        // case-insensitive: a desktop wrote "Library", a user's file manager may rename it
        auto lower = [](std::string_view text) {
            std::string out(text);
            for (char& c : out) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            return out;
        };
        const std::string lowered = lower(name);
        return lowered == "library" || lowered == "temp" || lowered == ".git" || lowered == ".vs" ||
               lowered == ".idea" || lowered == "obj" || lowered == "node_modules";
    }

    // Where a path inside a project may live: the folders a phone's editor may write. `relative`
    // is project-relative with '/' separators. False when the path leaves the project (".."), which
    // is the check an import must do before it writes: a name from a provider is not trusted.
    inline bool isInsideProject(std::string_view relative) {
        // An absolute path is not "inside" anything: "/etc/passwd" joined to a project folder is
        // the volume's root, and on Linux/macOS a leading '/' makes a join discard the prefix
        // outright. Android hands these over as a provider's name and a path built from one must
        // never escape, so a leading separator is refused, as is the empty name (which joins to
        // the folder itself, not to a file in it).
        if (relative.empty()) return false;
        if (relative.front() == '/') return false;
        size_t i = 0;
        while (i <= relative.size()) {
            size_t end = relative.find('/', i);
            if (end == std::string_view::npos) end = relative.size();
            const std::string_view segment = relative.substr(i, end - i);
            // "." and ".." are the two names a path walker treats as navigation rather than a
            // file, and a segment that is empty ("a//b") is a root-ish absolute on some systems
            if (segment == ".." || segment == ".") return false;
            if (segment.find(':') != std::string_view::npos) return false;
            if (segment.find('\\') != std::string_view::npos) return false;
            i = end + 1;
        }
        return true;
    }

    // ---- where the project is

    enum class ProjectPlace : uint8_t {
        Unknown,
        // an app-private folder (/data/user/0/<package>/files/...): always writable, never visible
        // to the user without a PC
        AppPrivate,
        // the app's own external folder (/storage/emulated/0/Android/data/<package>/files/...):
        // writable without a permission on every version, visible over USB, deleted with the app
        AppExternal,
        // a folder the user picked through the Storage Access Framework, or one under shared
        // storage the user granted: writable through the provider, or (API 30+, a folder the app
        // created) as a plain directory
        UserPicked,
        // shared storage read-only: a grant that allows reading but not writing
        SharedReadOnly,
    };

    struct ProjectLocation {
        ProjectPlace place = ProjectPlace::Unknown;
        // the file system path, when there is one (a tree grant may have none)
        std::string systemPath;
        // the SAF tree URI of a picked folder, "" otherwise
        std::string treeUri;
        // true when the volume holding it is removable (an SD card): the project may simply not be
        // there next start
        bool removable = false;
    };

    // What an editor may do with the project at that place.
    struct ProjectAccess {
        bool canOpen = false;
        bool canWrite = false;
        bool canCreateFiles = false;
        // a rename within the folder replaces the target (the atomic save's requirement): false
        // through most SAF providers, where a save must write a new document and delete the old
        bool atomicRename = false;
        // a finished project survives reinstalling the app
        bool survivesUninstall = false;
        // the user can find the project in a file manager, without adb
        bool userVisible = false;
        // what the editor must warn about before the user spends an hour in it, "" when nothing
        std::string warning;
    };

    inline ProjectAccess accessFor(const ProjectLocation& location) {
        ProjectAccess access;
        switch (location.place) {
            case ProjectPlace::AppPrivate:
                access.canOpen = access.canWrite = access.canCreateFiles = access.atomicRename = true;
                access.survivesUninstall = false;
                access.userVisible = false;
                access.warning = "the project is inside the app's private storage: uninstalling the app deletes it, "
                                 "and no file manager can see it";
                break;
            case ProjectPlace::AppExternal:
                access.canOpen = access.canWrite = access.canCreateFiles = access.atomicRename = true;
                access.survivesUninstall = false;
                access.userVisible = true;
                access.warning = "the project is in Android/data: uninstalling the app deletes it";
                break;
            case ProjectPlace::UserPicked:
                access.canOpen = access.canWrite = access.canCreateFiles = true;
                // a picked folder is a provider: the platform says whether it can replace a file
                access.atomicRename = false;
                access.survivesUninstall = true;
                access.userVisible = true;
                break;
            case ProjectPlace::SharedReadOnly:
                access.canOpen = true;
                access.warning = "this folder is read-only for the app: the project opens, and nothing can be saved";
                break;
            case ProjectPlace::Unknown:
                access.warning = "the project's storage is not reachable (the volume may be gone)";
                break;
        }
        if (location.removable) {
            const std::string extra = "the project is on a removable volume: it is gone if the card is taken out";
            access.warning = access.warning.empty() ? extra : access.warning + "; " + extra;
        }
        return access;
    }

    // Where a path is, from the roots Android has (no file system call: the caller passes the
    // paths it already knows). `internalRoot` is getFilesDir(), `externalRoot` the app's external
    // files dir, `treeUri` the grant a picked folder came with ("" when the path was not picked).
    inline ProjectLocation classifyLocation(const std::string& systemPath, const std::string& internalRoot,
                                            const std::string& externalRoot, const std::string& treeUri = {},
                                            bool removable = false) {
        ProjectLocation location;
        location.systemPath = systemPath;
        location.treeUri = treeUri;
        location.removable = removable;
        auto atOrBelow = [](const std::string& path, const std::string& root) {
            if (root.empty() || path.size() < root.size()) return false;
            if (path.compare(0, root.size(), root) != 0) return false;
            return path.size() == root.size() || path[root.size()] == '/' || root.back() == '/';
        };
        if (!treeUri.empty()) {
            location.place = ProjectPlace::UserPicked;
        } else if (atOrBelow(systemPath, externalRoot)) {
            location.place = ProjectPlace::AppExternal;
        } else if (atOrBelow(systemPath, internalRoot)) {
            location.place = ProjectPlace::AppPrivate;
        } else if (systemPath.empty()) {
            location.place = ProjectPlace::Unknown;
        } else {
            // a path outside both roots that did not come with a grant: shared storage the app was
            // not given, which is readable at most on API 29 and below
            location.place = ProjectPlace::SharedReadOnly;
        }
        return location;
    }

    // ---- a picked folder's project

    // A folder the user picked opens as a project only with a readable project.yaml at its root
    // (the engine's test for "this is a game project", AGENTS.md). `found` is the caller's answer
    // from the provider's listing, which is why it is a parameter and not a file system call.
    inline bool isOpenableProject(bool projectFileFound) {
        return projectFileFound;
    }

    // The sub-folder an import lands in: the project's assets folder, given the layout. A picked
    // file keeps its name; the caller runs it through uniqueFileName() against the listing.
    inline std::string importFolderOf(const ProjectLayout& layout) {
        return layout.assetsDir == "." || layout.assetsDir.empty() ? std::string() : layout.assetsDir;
    }

}

#endif // ANDROID_PROJECT_LAYOUT_H
