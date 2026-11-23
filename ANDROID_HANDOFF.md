# ANDROID_HANDOFF.md

For agent 3 (integration) and the main agent. What this branch adds, what it does not touch, and
the exact edits it needs from files it does not own.

## The one rule this branch follows

It does not edit a single file of the Android start-up: no `MainActivity.java`, no
`JasperPlatform.java`, no `NativeEngine.*`, no `AndroidMain.cpp`, no `CMakeLists.txt`, no
`engine/CMakeLists.txt`, nothing under `engine/workspaces/androidstudio/`. Everything is a new
header under `engine/platform/android/` plus one new test file, following the existing
`AndroidTextInput.h` + `AndroidTextInputTest.cpp` pattern, so there is nothing to merge and nothing
to conflict with.

## What the integration agent should call (in priority order)

### 1. Free space before a write — no file of yours changes to *use* it, one to *expose* it

```cpp
// proposed in System.h (your file, so not done here), next to platformFopen:
virtual SpaceInfo getSpaceInfo() const { return {}; }   // core reads it through System
```

Then the Android `System` subclass:

```cpp
// platform/android: StatFs is not statvfs and not GetDiskFreeSpaceEx
SpaceInfo JasperEngineAndroid::getSpaceInfo() const {
    android::SpaceInfo info;
    struct statfs stats;
    const std::string dir = getUserDataPath();
    if (statfs(dir.c_str(), &stats) != 0) return info;          // unknown: never refuses a save
    info.known = true;
    info.totalBytes = (uint64_t)stats.f_blocks * stats.f_bsize;
    info.availableBytes = (uint64_t)stats.f_bavail * stats.f_bsize;
    info.reservedBytes = (uint64_t)(stats.f_bfree - stats.f_bavail) * stats.f_bsize;
    return info;
}
```

Call site (anywhere, no override needed): `android::allowWrite(System::instance().getSpaceInfo(), bytes)`.
`allowWrite` with an unknown reading returns `allowed = true`, so a platform that does not implement
it behaves exactly as today.

### 2. An import from the picker

`MainActivity` (yours) registers `ActivityResultContracts.OpenDocument`, reads
`OpenableColumns.DISPLAY_NAME` and `SIZE`, and passes the bytes + name to a native entry the
runtime owns. The C++ rules are done and tested:

```cpp
const std::string name = android::uniqueFileName(
    android::importedAssetName(displayName, bytes),
    [&](const std::string& candidate) { return FileUtils::exists("data://" + candidate); });
FileUtils::writeText("data://" + name, std::string(bytes.begin(), bytes.end()), error);
```

`importedAssetName` gives a lowercase identifier plus the extension the *bytes* say, so a provider's
opaque `document/1234` becomes `_1234.png` and not an import the editor cannot open.

### 3. Project place

`android::classifyLocation(path, filesDir, externalFilesDir, treeUri, removable)` →
`android::accessFor(...)`. Use `access.warning` verbatim in the project picker; `access.atomicRename`
decides whether a save may rely on `io::writeFileAtomic`.

## Nothing here is device-verified, on purpose

Say so in the PR body if you integrate it: the four subsystems are verified at the SDK-free layer
(25 gtest cases) and **not** on a phone. The first device verification each needs is one line in
`ANDROID_TEST_MATRIX.md`, "Not covered".
