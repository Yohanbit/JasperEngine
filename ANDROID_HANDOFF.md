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

### 2b. Before anything is packaged: the pre-flight (one call, four answers)

```cpp
#include "AndroidImportPreflight.h"

std::vector<android::PreflightInput> inputs;   // {fileName = the picker's leaf, prefix = the first 64 KiB}
android::PreflightOptions options;
options.androidBitmapFactory = true;           // the phone has BitmapFactory (every API this engine targets)
options.androidMediaCodec = true;              // and MediaCodec (API 16+)
options.budget.maxTextureSize = <the tier's limit>;              // Device:: activeTier, not a constant
options.residentLimitBytes = <the device's texture heap>;        // see below; 0 = say nothing
const android::PreflightReport report = android::preflightImports(inputs, options);

for (const android::PreflightFinding& finding : report.findings) {
    if (finding.severity != android::PreflightSeverity::Fine)
        Out::warning(finding.fileName + ": " + finding.message);
    for (const std::string& sidecar : finding.missingSidecars)
        Out::warning("  also import: " + sidecar);
}
if (!report.summary.empty()) Out::message(report.summary);   // the first thing to fix
```

`report.suggestDownscale` names the textures to shrink when `report.overResidentBudget`, largest
first. `report.broken` counts the assets that will not draw or play. Nothing here reads a file: pass
the prefix bytes you already read for `importedAssetName`.

The same answer from a terminal, without writing any code:
`tools/android-asset-report.py <project folder> --resident-mb 128`.

### 2c. A photo that is stored turned

```cpp
const uint32_t tag = android::readOrientation(prefix);          // 0 when the file has none
const android::OrientationTransform turn = android::transformFor(android::orientationFromValue(tag));
if (turn.needed) {
    // BitmapFactory + Matrix: one decode that is both scaled and turned, which is also the cheapest
    // fix for a 120 MP photo (bug 6): no decode at full size, no second copy for the rotate
    matrix.postRotate((float)turn.rotateClockwise);
    if (turn.flipHorizontal) matrix.postScale(-1.f, 1.f);
    if (turn.flipVertical)   matrix.postScale(1.f, -1.f);
    inSampleSize = <the power of two from android::decideImageImport(...).scale>;
}
```
`android::orientedSize(width, height, orientation)` gives the size the picture is shown at, which is
what a texture's aspect ratio must use.

### 3. Project place

`android::classifyLocation(path, filesDir, externalFilesDir, treeUri, removable)` →
`android::accessFor(...)`. Use `access.warning` verbatim in the project picker; `access.atomicRename`
decides whether a save may rely on `io::writeFileAtomic`.

## 4. A crash in the core file I/O, found but not touched (yours)

`engine/core/io/Data.cpp` is the main agent's core, so this is reported, not edited. Reproduced
under ASan+UBSan on the host (`/tmp/datarepro2.cpp`, a two-line program):

```
jasperengine::Data empty(nullptr, 0, true, false);
empty.seek(-4);
empty.read(out, 8);     // AddressSanitizer: SEGV on unknown address ... in Data::read (Data.cpp:59)
```

Cause, `Data::seek`:

```cpp
void Data::seek(int aOffset) {
    if (aOffset >= 0) offset = aOffset;
    else              offset = dataLength + aOffset;   // 0 + (-4) wraps to 4294967292
    if (offset > dataLength - 1)                       // dataLength is unsigned: 0 - 1 wraps too
        offset = dataLength - 1;                       // ...so this never runs, and offset is huge
}
```

`Data::read` then does `memcpy(aDst, dataPtr + offset, ...)` with the wrapped offset. Reachable from
script: `FileData.newFile(false)` gives a `Data`, `seek` is bound in `IOClassesLua.cpp:457`, and
`read8`/`read16`/`read32`/`readString` follow. A truncated or empty asset is the everyday way to
reach it — on a phone, a provider handing over a partial file makes one.

The patch (three lines, no behaviour change for a valid seek):

```cpp
void Data::seek(int aOffset) {
    if (dataLength == 0){ offset = 0; return; }
    long long target = aOffset >= 0 ? (long long)aOffset : (long long)dataLength + aOffset;
    if (target < 0) target = 0;
    const long long last = (long long)dataLength - 1;
    if (target > last) target = last;
    offset = (unsigned int)target;
}
```

and, so a caller that reads before seeking cannot do it either, a guard at the top of `read`:
`if (!dataPtr || offset >= dataLength || aBytes == 0) return 0;`.

A test belongs beside `AssetIOServiceTest`/`DataOpenTest` in `engine/tests/core/io/` (an empty `Data`
and a 4-byte one, `seek(-1)` / `seek(-100)` / `seek(past)` then `read`, no crash and `pos()` sane).

## Nothing here is device-verified, on purpose

Say so in the PR body if you integrate it: the four subsystems are verified at the SDK-free layer
(25 gtest cases) and **not** on a phone. The first device verification each needs is one line in
`ANDROID_TEST_MATRIX.md`, "Not covered".
