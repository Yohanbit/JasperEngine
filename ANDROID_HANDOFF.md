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

The short version of each, for the file the integration touches. The long version — every file, the
function and line, the dependencies, the expected behaviour and the Java — is
**`ANDROID_INTEGRATIONS.md`**, which covers the five as one story including the picker those four
below assume exists.

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

### 2d. The copy itself (a stream that can be cut short)

```cpp
// Java: an InputStream from the resolver, read into the temporary in chunks the plan names
android::TransferPlan plan = android::planTransfer(name, sizeFromOpenableColumns, temporaryBytesOnDisk);
file = open(plan.temporary, "wb");                       // same folder as the target
stream.skip(plan.resumeFrom == temporaryBytesOnDisk ? 0 : temporaryBytesOnDisk);  // a resumed copy
while (android::TransferStep step = android::transferStep(plan, copied, freeBytes, reserve, chunk),
       step.outcome == android::TransferOutcome::Complete && step.copyBytes > 0) {
    copy(stream, file, step.copyBytes);
    copied += <what the read returned>;
    if (<the stream ended>) break;
}
switch (android::settleTransfer(plan, copied, cancelled, streamEnded)) {
    case android::TransferOutcome::Complete:   rename(plan.temporary, plan.target); break;
    case android::TransferOutcome::Interrupted: keep(plan.temporary);   // resume next time
    case android::TransferOutcome::Cancelled:   // the user's choice: drop it, or keep it to resume
    default:                                    remove(plan.temporary); // Mismatched / OutOfSpace
}
```

At start, walk the import folder with `android::isImportTemporary` and either resume what it holds
(`android::temporaryTarget` names the asset) or drop it: otherwise a killed copy sits in the folder
forever. That scan is also what `AssetTypes` needs to never see, which the leading dot gives.

### 2e. A model with sidecars (the flat folder)

```cpp
const android::ModelImportPlan plan = android::planModelImport(gltfText, pickedLeaf);
// 1. ask the picker for exactly plan.sidecars[i].uri (a multi-select filtered to those names)
// 2. write each file as plan.sidecars[i].assetName, in the project's asset folder
// 3. write plan.gltfText as the .gltf: it names those files, so the model finds them
```
Without step 3 the model keeps pointing at `textures/skin.png`, and without step 2's names two
models that ship a `common.png` overwrite each other (bug 10m).

### 3. Project place

`android::classifyLocation(path, filesDir, externalFilesDir, treeUri, removable)` →
`android::accessFor(...)`. Use `access.warning` verbatim in the project picker; `access.atomicRename`
decides whether a save may rely on `io::writeFileAtomic`.

## 4. Two crashes in the core file I/O, found but not touched (yours)

`engine/core/io/Data.cpp` is the main agent's core, so this is reported, not edited.
`tools/android/data-io-overflow-repro.cpp` is a standalone reproduction with the real arithmetic
copied beside the patch, so the fix can be seen working before it is applied:

```
g++ -std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
    tools/android/data-io-overflow-repro.cpp -o /tmp/datarepro
/tmp/datarepro A     # aborts: AddressSanitizer SEGV in the read
/tmp/datarepro B     # runs every case, including the patch's answers
```

### The verdict on the SIGSEGV, asked for explicitly

The question was whether the crash is a known issue, a deliberate reproduction, or an unfixed defect.
It is the third, and the three answers are worth separating:

| | |
|---|---|
| A known issue? | **No.** Nothing in the repository documents it: no entry in `docs/`, no `TODO`/`FIXME` in `Data.cpp`, no test that asserts the current behaviour. (This checkout is shallow — 30 commits — so "since it was written" is not something it can show; what it can show is that nothing present describes the behaviour as intended.) |
| A deliberate reproduction? | **No.** `tools/android/data-io-overflow-repro.cpp` is this branch's file and it is deliberate — but it is a *reproduction of the defect*, not a defect introduced to be reproduced. `engine/core/io/Data.cpp` is unmodified: `git diff` against the branch point lists no change to it (point 3's `git diff --name-status` has 19 files and none of them is `Data.cpp`). |
| An unfixed defect? | **Yes.** It crashes the shipped code, and it is the main agent's file to fix. |

The stack trace is from the **real bodies**, not from the copy in the repro: `tools/android/data-overflow-real.py`
lifts `Data::seek` and `Data::read` out of `engine/core/io/Data.cpp` by brace matching, puts the
verbatim text inside a struct with the same fields, and compiles it with ASan+UBSan. So the trace
below is about `Data.cpp` as it ships, with the only edit being the dropped `Data::` qualifier that a
definition inside a class cannot carry:

```
$ python3 tools/android/data-overflow-real.py
   unsigned int read(unsigned char* aDst, unsigned int aBytes) {
       if (offset + aBytes >= dataLength)
           aBytes = dataLength - offset;
       memcpy(aDst, dataPtr + offset, aBytes);
       offset += aBytes;
       return aBytes;
   }

   void seek(int aOffset) {
       if (aOffset >= 0)
           offset = aOffset;
       else
           offset = dataLength + aOffset;
       if (offset > dataLength-1)
           offset = dataLength-1;
   }

A. an empty Data, seek(-4), read(8)  (FileData.newFile -> seek -> read8)
   pos() after seek(-4) = 4294967292
   reading, expecting AddressSanitizer below
AddressSanitizer:DEADLYSIGNAL
=================================================================
==8413==ERROR: AddressSanitizer: SEGV on unknown address 0x00009fff7fff (pc 0x7fd6bb83bd5a ...)
==8413==The signal is caused by a READ memory access.
    #0 0x7fd6bb83bd5a in AddressIsPoisoned ../../../../src/libsanitizer/asan/asan_mapping.h:379
    #1 0x7fd6bb83bd5a in QuickCheckForUnpoisonedRegion .../asan_interceptors_memintrinsics.h:31
    #2 0x7fd6bb847fe3 in __interceptor_memcpy .../sanitizer_common_interceptors.inc:827
    #3 0x560ddbafdbdf in jasperengine::DataLike::read(unsigned char*, unsigned int) /tmp/.../lifted.h:8
    #4 0x560ddbafd50a in main /tmp/.../harness.cpp:39
    #5 0x7fd6bb645249  (/lib/x86_64-linux-gnu/libc.so.6+0x27249)
    #6 0x7fd6bb645304 in __libc_start_main (/lib/x86_64-linux-gnu/libc.so.6+0x27304)
SUMMARY: AddressSanitizer: SEGV ../../../../src/libsanitizer/asan/asan_mapping.h:379 in AddressIsPoisoned
==8413==ABORTING
```

`lifted.h:8` is the `memcpy` line of `Data::read` as the file writes it. The chain is one line long:
`seek` computes `offset = dataLength + aOffset` in `unsigned int` (0 + -4 = 4294967292), the
`offset > dataLength-1` clamp then compares 4294967292 against 4294967295 and lets it through, and
`read` computes `dataLength - offset` in the same unsigned type: 0 - 4294967292 wraps to 4, so a
request for 8 bytes becomes a 4-byte `memcpy` from `dataPtr + 4294967292`.

The script is a gate, not a demonstration: it exits **1** while the defect is present and **0** once
the patch below is applied. Both states were checked — the patch was applied to a copy of the tree in
`/tmp` and the same script then printed `the two defects are fixed in Data.cpp` and exited 0.

### A. A read past the end, with an unsigned wrap (a crash)

```
jasperengine::Data empty(nullptr, 0, true, false);
empty.seek(-4);
empty.read(out, 8);     // AddressSanitizer: SEGV ... in Data::read (Data.cpp:59)
```

`Data::seek`:

```cpp
void Data::seek(int aOffset) {
    if (aOffset >= 0) offset = aOffset;
    else              offset = dataLength + aOffset;   // 0 + (-4) wraps to 4294967292
    if (offset > dataLength - 1)                       // dataLength is unsigned: 0 - 1 wraps too
        offset = dataLength - 1;                       // ...so this never runs, and offset is huge
}
```

and `Data::read` then `memcpy`s from `dataPtr + offset`. Reachable from script:
`FileData.newFile(false)` gives a `Data`, `seek` is bound in `IOClassesLua.cpp:457`, and
`read8`/`read16`/`read32`/`readString` follow. A truncated or empty asset is the everyday way to
reach it — and on a phone a provider handing over a partial file makes one, which is exactly what
`AndroidImportTransfer.h` exists to prevent.

### B. A read at the end returns the wrong bytes instead of nothing (a silent corruption)

```cpp
read(4) on a 4-byte buffer;  // offset is now 4
read(1);                    // the guard is `offset + aBytes >= dataLength`, so aBytes = 4 - 5 -> wraps
                            // ...and the buffer's *first* byte comes back
```

A reader that loops until `read()` returns 0 never reaches it, and a parser that trusts the count
reads a byte that is not there. `>=` should be `>`, and a read with the cursor already past the end
must return 0.

### The patch (no behaviour change for a valid seek)

```cpp
void Data::seek(int aOffset) {
    // signed arithmetic, a real lower bound, and the upper bound is dataLength -- not dataLength - 1,
    // which made "seek past the end, then read" return that last byte again (defect B, one line up)
    long long target = aOffset >= 0 ? (long long)aOffset : (long long)dataLength + aOffset;
    if (target < 0) target = 0;
    if (target > (long long)dataLength) target = (long long)dataLength;
    offset = (unsigned int)target;
}

unsigned int Data::read(unsigned char *aDst, unsigned int aBytes) {
    if (!dataPtr || offset >= dataLength || aBytes == 0) return 0;   // the guard, before any maths
    const unsigned int available = dataLength - offset;
    if (aBytes > available) aBytes = available;
    memcpy(aDst, dataPtr + offset, aBytes);
    offset += aBytes;
    return aBytes;
}
```

`tools/android/data-io-overflow-repro.cpp` runs both defects and both fixes side by side; its
`fixed::` version is the patch above, verbatim. A test belongs beside `AssetIOServiceTest` /
`DataOpenTest` in `engine/tests/core/io/`: an empty `Data` and a 4-byte one, `seek(-1)` /
`seek(-100)` / `seek(past)` then `read`, no crash, `read` at the end returning 0, and `pos()` sane.

## Nothing here is device-verified, on purpose

Say so in the PR body if you integrate it: the four subsystems are verified at the SDK-free layer
(25 gtest cases) and **not** on a phone. The first device verification each needs is one line in
`ANDROID_TEST_MATRIX.md`, "Not covered".
