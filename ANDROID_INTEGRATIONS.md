# The five integrations

`engine/platform/android/` holds eight headers of pure logic (no Android headers, no JNI, no file
system calls: they take what the platform knows and answer) with 80 gtest cases and a seven-invariant
selfcheck. **They are not connected to the engine.** This document is the connection: for each of the
five pieces of behaviour a phone editor needs, the file that has to call it, the exact function and
line, the call, its dependencies, what the user then sees, and the code.

Read the status honestly first: nothing on this branch makes any of this work on a phone yet. The
headers compile, their tests pass, and the code below compiles and runs against them — but no engine
file includes them, so today a phone build behaves exactly as it did before this branch. The three
demos under `tools/android/integration/` are the code from this document, built and run by
`tools/android/verify.py`.

| # | Integration | Caller today | Header | Blocked on |
|---|---|---|---|---|
| 1 | Import pipeline: pick, preflight, copy, land | nothing calls it; there is no picker | `AndroidImportPreflight.h`, `AndroidImportTransfer.h` | a SAF picker in Java and a JNI hop |
| 2 | Free space before a write | nothing | `AndroidStorageSpace.h` | a `StatFs` reading in Java |
| 3 | Where the project is, and what may be done with it | nothing | `AndroidProjectLayout.h` | a SAF tree URI, which needs #1's picker |
| 4 | Gestures: pinch, drag, long press, tap | `NativeEngine::handleGameActivityInput` feeds `Input` only | `AndroidTouchGestures.h` | one call in the frame loop |
| 5 | A crashed import's leftovers | nothing | `AndroidImportTransfer.h` (recovery half) | #1's picker |

Integration 4 is the only one that can be done entirely on this side of the JNI line, and it is the
one worth doing first: it is four calls in `NativeEngine::gameLoop`, and it makes the double tap and
the long press exist on a phone at all.

---

## 1. The import pipeline

**What the user does**: taps "Import", picks photos or a model from the Files app, and expects them
in the project's `assets/` with names the engine can open.

**Today**: there is no picker. `MainActivity` never starts an `ACTION_OPEN_DOCUMENT` intent, there is
no `ActivityResultLauncher`, and `JasperNative.dispatchActivityResult` reaches only plugins. The files
a phone's import would produce therefore cannot arrive at all.

### 1a. The picker, in Java

| | |
|---|---|
| File | `engine/platform/android/java/com/jaspergameengine/jasperengine/JasperPlatform.java` |
| Add | a picker entry point, and the result it posts back |
| Called from | the editor's Import button, through the bridge (`JasperNative`), or from Lua |
| Depends on | the activity, `registerForActivityResult` (androidx `activity` is already a dependency: `MainActivity` uses `OnBackPressedCallback`) |

```java
// JasperPlatform.java -- a SAF pick of any number of files (API 19+; no permission needed, which is
// the point on a phone: MANAGE_EXTERNAL_STORAGE is a store rejection)
private static final int IMPORT_REQUEST = 0x4A53;

public static void pickForImport(Activity activity, String[] mimeTypes) {
    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
    intent.addCategory(Intent.CATEGORY_OPENABLE);
    intent.setType("*/*");
    if (mimeTypes != null && mimeTypes.length > 0)
        intent.putExtra(Intent.EXTRA_MIME_TYPES, mimeTypes);
    intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
    // the grant outlives the process: a copy resumed tomorrow still reads its source
    intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
    activity.startActivityForResult(intent, IMPORT_REQUEST);
}

// JasperPlatform.onActivityResult(Activity, int, int, Intent), called from MainActivity's
// onActivityResult: the picker's result arrives there, next to the plugins' one
public static void onActivityResult(Activity activity, int requestCode, int resultCode, Intent data) {
    if (requestCode != IMPORT_REQUEST) return;
    if (resultCode != Activity.RESULT_OK || data == null) {
        nativePicked(0, new String[0], new long[0]);   // the user backed out
        return;
    }
    List<Uri> uris = new ArrayList<>();
    if (data.getClipData() != null) {
        for (int i = 0; i < data.getClipData().getItemCount(); i++)
            uris.add(data.getClipData().getItemAt(i).getUri());
    } else if (data.getData() != null) {
        uris.add(data.getData());
    }
    String[] names = new String[uris.size()];
    long[] sizes = new long[uris.size()];
    for (int i = 0; i < uris.size(); i++) {
        names[i] = uris.get(i).toString();             // the URI: the native side opens it
        try (android.database.Cursor c = activity.getContentResolver().query(uris.get(i),
                new String[]{android.provider.OpenableColumns.DISPLAY_NAME,
                             android.provider.OpenableColumns.SIZE}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                names[i] = names[i] + "\n" + c.getString(0);   // "uri\ndisplay name": the name the user saw
                sizes[i] = c.isNull(1) ? 0 : c.getLong(1);
            }
        } catch (Exception e) {
            Log.w("JasperEngine", "import: no name for " + uris.get(i) + ": " + e);
        }
    }
    nativePicked(uris.size(), names, sizes);
}

private static native void nativePicked(int count, String[] uriAndName, long[] sizes);
```

`MainActivity` already has the `onActivityResult` override that forwards to
`JasperNative.dispatchActivityResult` (line 178); the picker's result has to be forwarded there too,
which is one line in that method:

```java
// MainActivity.onActivityResult, before the plugin dispatch: a pick is the app's, not a plugin's
JasperPlatform.onActivityResult(this, requestCode, resultCode, data);
```

### 1b. The bytes, in C++

| | |
|---|---|
| File | `engine/platform/android/NativeEngine.cpp` |
| Add | the `nativePicked` native method, registered in `NativeEngine::setupPlatformJNI` |
| Called from | Java, on the UI thread; it only copies strings and posts the work |
| Depends on | `AndroidImportPreflight.h`, `AndroidImportTransfer.h`, a `ContentResolver` open in Java (`openInputStream`) |

The preflight needs the file's first bytes for its magic and image header. 8 KiB is enough for every
format `readImageSize` reads (it stops at the IHDR/APP1); a model's `.gltf` header is JSON and read
whole.

```cpp
// NativeEngine.cpp: the pick arrives here, the copy runs on the engine's own thread
struct PickedFile {
    std::string uri;
    std::string displayName;
    uint64_t size = 0;
    std::vector<uint8_t> prefix;
};

void JNICALL NativeEngine::nativePicked(JNIEnv* env, jclass, jint count, jobjectArray uriAndName, jlongArray sizes) {
    std::vector<PickedFile> picked;
    jsize n = std::min((jsize)count, env->GetArrayLength(uriAndName));
    jlong sizeBuffer[64];
    jsize sizeCount = sizes ? std::min(env->GetArrayLength(sizes), (jsize)64) : 0;
    if (sizeCount > 0) env->GetLongArrayRegion(sizes, 0, sizeCount, sizeBuffer);
    for (jsize i = 0; i < n; i++) {
        auto text = (jstring)env->GetObjectArrayElement(uriAndName, i);
        const char* utf = text ? env->GetStringUTFChars(text, nullptr) : nullptr;
        std::string pair = utf ? utf : "";
        if (utf) env->ReleaseStringUTFChars(text, utf);
        if (text) env->DeleteLocalRef(text);
        const size_t newline = pair.find('\n');
        PickedFile file;
        file.uri = pair.substr(0, newline);
        file.displayName = newline == std::string::npos ? pair : pair.substr(newline + 1);
        file.size = i < sizeCount ? (uint64_t)sizeBuffer[i] : 0;
        picked.push_back(file);
    }
    // the prefix read needs the ContentResolver: a Java call per file, on the game thread, because
    // this runs once per pick and a phone's pick is a handful of files
    readPickedPrefixes(picked);           // openInputStream + read 8 KiB, see below
    importQueue.push(std::move(picked));   // the frame loop drains it
}

// the frame loop, once the queue is not empty: preflight, then a copy plan per file
void NativeEngine::importPicked() {
    std::vector<PickedFile> picked;
    while (importQueue.tryPop(picked)) {
        std::vector<android::PreflightInput> inputs;
        for (const PickedFile& file : picked) {
            android::PreflightInput input;
            input.fileName = file.displayName;
            input.prefix = file.prefix;
            input.fileBytes = file.size;
            inputs.push_back(input);
        }
        android::PreflightOptions options;
        options.androidBitmapFactory = true;   // Android's own decoder is there
        options.androidMediaCodec = true;
        options.takenNames = &importFolderListing;   // what the folder already holds
        const android::PreflightReport report = android::preflightImports(inputs, options);
        // the reasons, to the editor: a message per file, "" when nothing is worth saying
        for (const android::PreflightFinding& finding : report.findings) {
            if (!finding.message.empty()) importMessages.push_back(finding.message);
            if (finding.severity == android::PreflightSeverity::Broken) continue;   // do not copy it
            const android::TransferPlan plan = android::planTransfer(
                android::sanitizeFileName(finding.assetName), picked[&finding - report.findings.data()].size, 0);
            transferQueue.push({ picked[&finding - report.findings.data()].uri, plan });
        }
    }
}

// one chunk per frame: a 200 MB video must not block the frame the user is watching
void NativeEngine::pumpTransfers() {
    if (transferQueue.empty()) return;
    TransferJob& job = transferQueue.front();
    const std::string folder = mInternalDataPath + "/projects/" + currentProject + "/" + assetsDir;
    android::SpaceInfo space = querySpace(folder);            // integration 2
    const android::TransferStep step = android::transferStep(
        job.plan, job.copiedBytes, space.writableBytes(), 8ull << 20, 1ull << 20);
    if (step.outcome == android::TransferOutcome::OutOfSpace) {
        importMessages.push_back(step.message);
        transferQueue.pop();                                  // the temporary stays, resumable
        return;
    }
    if (step.copyBytes == 0) {
        // the stream ended: settle decides whether the file is complete
        // `streamEnded`: the provider's stream returned EOF, which is what a copy's end looks like
        const android::TransferOutcome outcome = android::settleTransfer(
            job.plan, job.copiedBytes, job.cancelled, true);
        if (outcome == android::TransferOutcome::Complete)
            FileUtils::renameFile(job.plan.temporary, job.plan.target, error);
        transferQueue.pop();
        return;
    }
    job.copiedBytes += readFromUriInto(job.uri, job.plan.temporary, job.copiedBytes, step.skipBytes, step.copyBytes);
}
```

**Expected behaviour**, from the demo (`tools/android/integration/import-demo.cpp`, run by
`verify.py`):

```
preflight: 3 files, largest decode 9.9 MB, resident 9.9 MB
  DCIM/Camera/IMG_20240101_120000.jpg      -> img_20240101_120000.jpg
  Screenshot_2024-01-01-12-00-00.png       -> screenshot_2024_01_01_12_00_00.png
  CON.png                                  -> _con.png
transfer: img_20240101_120000.jpg      -> .img_20240101_120000.jpg.jeimport step: copy 65536, skip 0, complete
transfer: _con.png                     -> ._con.png.jeimport           step: copy 65536, skip 0, complete
out of space: outcome=OutOfSpace "not enough space to finish the copy: free some space and try again, 3 MB are already there"
```

So: the phone's DCIM folder name becomes an asset name the engine can open, Windows' device names are
escaped (`CON` -> `_con`), and the copy goes to a hidden `.jeimport` beside the target so a kill
mid-copy never leaves a half file the asset scanner would try to load.

---

## 2. Free space before a write

**What the user does**: saves a scene, or imports a 40 MB texture, on a phone whose storage is nearly
full.

**Today**: nothing asks. `FileUtils::writeFileAtomic` writes the temporary, hits `ENOSPC` on the flush,
and leaves the target untouched but the folder carrying temporaries. On a phone that is the normal
end of a session, not an edge case.

| | |
|---|---|
| File | `engine/platform/android/NativeEngine.cpp` (the reading), and the save path that asks |
| Add | `querySpace()`, and the `allowWrite` check before the atomic write starts |
| Called from | before `AtomicFileWriter::open`, and before each import chunk |
| Depends on | `AndroidStorageSpace.h`; `StatFs` in Java, or `statvfs` on the folder's path (no JNI hop: the app's own folders are real paths) |

`statvfs` is the cheaper half and needs no Java at all — an app always owns its private folder:

```cpp
// NativeEngine.cpp: the volume that holds a folder, read without a JNI hop
#include <sys/statvfs.h>

static android::SpaceInfo querySpace(const std::string& folder) {
    struct statvfs stats;
    if (statvfs(folder.c_str(), &stats) != 0) return android::unknownSpace();
    android::SpaceInfo space;
    space.known = true;
    space.totalBytes = (uint64_t)stats.f_blocks * stats.f_frsize;
    space.availableBytes = (uint64_t)stats.f_bavail * stats.f_frsize;   // bavail: what an app may use
    space.reservedBytes = (uint64_t)(stats.f_bfree - stats.f_bavail) * stats.f_frsize;
    return space;
}
```

A folder reached through a SAF tree URI has no path, so it needs the provider's own answer; that is
`JasperPlatform`'s half, and it is only needed once integration 1's picker exists:

```java
// JasperPlatform.java: the provider's view of a picked folder's volume
public static long getAvailableBytes(Activity activity, String treeUri) {
    try (android.os.ParcelFileDescriptor fd = activity.getContentResolver()
            .openFileDescriptor(android.net.Uri.parse(treeUri), "r")) {
        if (fd == null) return -1;
        android.os.storage.StorageManager sm = activity.getSystemService(android.os.storage.StorageManager.class);
        java.util.UUID uuid = sm.getUuidForPath(new java.io.File("/storage/emulated/0"));
        return sm.getAllocatableBytes(uuid);
    } catch (Exception e) {
        return -1;   // -1: unknown, and the policy then never refuses
    }
}
```

**Expected behaviour** (`space-demo.cpp`, run by `verify.py`, on a 32 GB phone with 6 MB free):

```
1 MB save:  allowed=0 critical=1 budget=0.0 MB
40 MB copy: allowed=0 critical=1 (the import is refused before it starts)
unknown reading, 4 GB write: allowed=1 unknown=1
```

The 1 MB save is refused because the policy keeps 8 MiB free past every write: a phone left with 0
bytes free does not boot its launcher, so "the save succeeded" is not the only thing at stake. An
unknown reading never refuses, which is the rule that keeps a platform without the call working.

---

## 3. Where the project is, and what may be done with it

**What the user does**: opens a project folder from the phone's Files app (a `content://` tree) or
from the app's own storage, and starts editing.

**Today**: the engine has one internal path (`NativeEngine::getInternalDataPath`) and no notion of a
picked folder; an editor on a phone could only offer the app's private storage.

| | |
|---|---|
| File | `engine/platform/android/NativeEngine.cpp`, and the editor's project-open path |
| Add | `classifyLocation` on the path the project came from, then `accessFor` before anything is offered |
| Called from | when the project opens, and before every save |
| Depends on | `AndroidProjectLayout.h`; integration 1's picker for the tree URI |

```cpp
// NativeEngine.cpp: once, when the project is chosen
const android::ProjectLocation location = android::classifyLocation(
    systemPath, mInternalDataPath, externalFilesPath, treeUri, isRemovable);
const android::ProjectAccess access = android::accessFor(location);
if (!access.canOpen) {
    // "this folder holds no project.yaml" / "read-only": said before the user spends an hour in it
    showProjectProblem(access.warning);
    return false;
}
if (!access.atomicRename) {
    // a SAF folder cannot rename over a target, which is what the atomic save needs: the save
    // writes a new document and deletes the old, and the editor says so once
    saveStrategy = SaveStrategy::WriteThenDelete;
}
// every path an import or a save builds is checked against the project's root
if (!android::isInsideProject(relativePath)) {
    // a provider's display name is not trusted: "../../project.yaml" is a name, not a path
    return false;
}
```

**Expected behaviour** (`space-demo.cpp`):

```
private: place=1 canOpen=1 canWrite=1 atomicRename=1
SAF tree: place=3 canOpen=1 canWrite=1 canRename=0
  assets/../project.yaml   inside=0
  /etc/passwd              inside=0
  assets/CON.png           inside=1
  assets/ok.png            inside=1
  assets\ok.png            inside=0
  C:/ok.png                inside=0
  assets/                  inside=1
  folder Library    cache=1
  folder library    cache=1
  folder Temp       cache=1
  folder assets     cache=0
```

The SAF folder opens and writes but cannot atomic-rename (`canRename=0`), which is the fact that
decides the save strategy; `..`, an absolute path, a backslash and a drive letter are refused before
anything is written, and `Library`/`Temp` are recognised as caches at any case.

---

## 4. Gestures: pinch, drag, long press, tap

**What the user does**: pinches to zoom a scene view, drags to orbit it, holds a panel to open its
context menu, double taps a node to focus it.

**Today**: `NativeEngine::handleGameActivityInput` translates motion events into
`Engine::systemTouchStart/Move/End/Cancel` and stops there. `Input` reports raw touches;
`InputActions` maps them to a mapped action; a UI panel gets nothing but a press and a release. There
is no long press and no double tap anywhere in the engine, and `Input::multiTapTime` only counts taps
close together after the fact.

This is the integration that needs no JNI and no picker: it is the header reading `Input`'s own
touches.

| | |
|---|---|
| File | `engine/platform/android/NativeEngine.cpp` |
| Function | `NativeEngine::gameLoop`, the line `handleGameActivityInput();` (line 836 at `fbca5ed4`) |
| Add | one call after it, and a `RecognizerState` member |
| Called from | once per frame, after the frame's touches are latched |
| Depends on | `AndroidTouchGestures.h`, `Input.h`; nothing else |

```cpp
// NativeEngine.h: one recogniser per engine, next to the other input state
#include "AndroidTouchGestures.h"
...
    jasperengine::android::RecognizerState gestureState;
    int gestureFrame = 0;   // the pinch's frame counter: any increasing number, its own is what it reads

// NativeEngine.cpp, in gameLoop(), right after handleGameActivityInput():
    handleGameActivityInput();
    // the frame's touches, read once the platform has filled them in: the four readers are pure
    // and cheap (a handful of fingers), and each is a no-op on a frame with no gesture in it
    const std::vector<jasperengine::Touch> frameTouches = jasperengine::Input::getTouches();
    jasperengine::android::updatePinch(gestureState, frameTouches, gestureFrame++);
    jasperengine::android::updateDrag(gestureState.drag, frameTouches);
    jasperengine::android::updateLongPress(gestureState, frameTouches, (float)jasperengine::Input::getInputTime());
    jasperengine::android::updateTap(gestureState, frameTouches);
```

Then the readers go where the gestures are consumed — the editor's scene view and the UI's panel
handler, both of which are cross-platform and therefore not this branch's files:

```cpp
// the scene view, once per frame: a pinch zooms, a drag orbits, a tap selects
const android::PinchState& pinch = gestureState.pinch;
if (pinch.active && pinch.deltaDistance != 0.0f)
    camera.zoomBy(1.0f + pinch.deltaDistance / std::max(pinch.distance, 1.0f));
if (gestureState.drag.dragging)
    camera.orbitBy(gestureState.drag.frame);
if (gestureState.tap.doubleTapped)
    scene.focusOn(gestureState.tap.position);

// a panel's press-and-hold opens its context menu; the release that follows must not also click
if (gestureState.longPress.triggered)
    openContextMenuAt(gestureState.longPress.position);
if (gestureState.tap.tapped && !android::pressWasConsumed(gestureState, gestureState.tap.position, 8.0f))
    clickAt(gestureState.tap.position);
```

**Two facts that bite**, both found by compiling the demo against the real headers rather than reading
them:

- `updateLongPress` takes the held time from `Touch::time`, not from a subtraction against its
  `seconds` argument: `Touch::time` is already the unscaled seconds since that finger went down, so a
  caller cannot make it negative or drift. `Input::getInputTime()` returns a `double` and the
  parameter is a `float`, so the cast above is needed; the argument is otherwise unused.
- `updateDrag` reads `Touch::delta` for its per-frame travel, so it must be called after `Input` has
  filled the touches for the frame, which is what `getTouches()` at that point in `gameLoop` does.

**Expected behaviour** (`gestures-demo.cpp`, run by `verify.py`):

```
pinch: 100 px -> 200 px, delta 100, midpoint (150, 300), active=1
drag: dragging=1 pointer=3 travel=(40, 0)
long press: triggered=0 at frame 32 (0.64 s held), consumed=1
tap 1: tapped=1
tap 2: tapped=1 doubleTapped=1 interval=200 ms
```

(`triggered=0` is the last frame of the loop, not the press: the flag is true for exactly one frame,
frame 32 in a 60 Hz loop, which is the 500 ms `getLongPressTimeout` plus a frame of rounding — the
`=1` on `consumed` is the press having fired at all, and `updateTap` then correctly refuses to call
its release a tap.)

---

## 5. A crashed import's leftovers

**What the user does**: picks a 200 MB video, backgrounds the app because a call came in, and the
system kills the process mid-copy. Tomorrow they open the project again.

**Today**: nothing scans for it; the leftover matters only once integration 1 writes temporaries.

| | |
|---|---|
| File | `engine/platform/android/NativeEngine.cpp`, the project-open path |
| Add | a scan of the import folder for `.jeimport` files, then a resume or a drop |
| Called from | when a project opens, before the asset scan |
| Depends on | `AndroidImportTransfer.h` (both halves), `FileUtils::list` |

```cpp
// NativeEngine.cpp: on project open, before the folder is scanned for assets
std::vector<std::string> entries;
FileUtils::list(importFolder, entries, error);
for (const std::string& entry : entries) {
    if (!android::isImportTemporary(entry)) continue;     // an ordinary asset
    const std::string target = android::temporaryTarget(entry);
    const uint64_t partial = fileSize(importFolder + "/" + entry);
    // the plan asks the provider again for the size; a provider that no longer has the file (a
    // download that expired) leaves nothing to resume, and the partial is dropped
    const android::TransferPlan plan = android::planTransfer(target, providerSize(target), partial);
    if (plan.discardPartial) {
        FileUtils::remove(importFolder + "/" + entry, error);
        importMessages.push_back(target + ": the copy could not be resumed and was dropped");
    } else {
        resumeQueue.push({ target, plan });               // the same pump as integration 1
    }
}
```

**Expected behaviour**: a temporary whose partial bytes can still be the source's prefix resumes
(`resumeFrom` is the partial's size, and `transferStep` returns the `skipBytes` the provider must skip
so the copy continues rather than restarts); one that is longer than the source, or whose source is
gone, is dropped with a sentence rather than left in the folder for ever. The 80 gtest cases cover
both branches, including the resumed copy that is already longer than its source.

---

## What is missing before any of this is "working on Android"

1. A device or emulator. This sandbox has no `javac`, no Android SDK, no NDK and no emulator, so no
   APK can be built here and none of the Java above has been compiled — it is written against the
   APIs `MainActivity` already uses, but it is unverified.
2. `JNIEnv` plumbing for `nativePicked` (`RegisterNatives` or a `Java_com_jaspergameengine_...` name,
   in `NativeEngine::setupJNI`), which is the main agent's file.
3. The `ContentResolver` prefix read (`readPickedPrefixes`) and the stream read
   (`readFromUriInto`), which are Java calls the main agent has to add next to `JasperPlatform`.
4. The consumers in §4 — the scene view and the UI panel handler — which are shared with the desktop
   editor and belong to whoever owns them.

Everything on this side of the JNI line is written and verified: `tools/android/verify.py` builds and
runs all of it from a clean checkout.
