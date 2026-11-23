# ANDROID_AGENT2_STATUS.md

Secondary agent (subsystem porting) status for the JasperEngine Android port.
Companion files written by this agent: `ANDROID_TEST_MATRIX.md`, `ANDROID_BUGS.md`,
`ANDROID_HANDOFF.md`. The main agent's audit lives in `ANDROID-AUDIT.md`; this file does not
repeat it.

Branch: `arena/593187bf-jasperengine` (from `8adc8a93`). Nothing here is merged; the PR stays open
for agent 3.

---

## 0. How to read the status labels

Every claim below carries one of:

| Label | Meaning |
| --- | --- |
| **verified** | I ran it, and the command and result are written down here |
| **compiled** | It builds warning-free, but nothing ran it |
| **inferred** | Read from the code; no test covers it |
| **pending** | Not done. It is on the list below, not forgotten |

A subsystem that compiles is not a subsystem that works. Of the four landed here, all four are
**verified** at the SDK-free layer (the tests in `engine/tests/core/platform/AndroidSubsystemsTest.cpp`
pass), and none is yet **verified on a device** — that needs the JNI + Java side described in §4,
which belongs to the integration agent's files.

## 1. Scope and why it is shaped this way

The main agent owns the Android start-up: the activity, JNI initialisation, the engine main loop,
the central renderer, and the root Gradle/CMake configuration. This agent therefore does not touch
`engine/workspaces/androidstudio/**`, `CMakeLists.txt`, `engine/CMakeLists.txt`,
`NativeEngine.*`, `AndroidMain.cpp`, `MainActivity.java` or `JasperPlatform.java`.

What it can do independently, and did, follows a pattern the repository already uses:
`engine/platform/android/AndroidTextInput.h` is plain C++ (no Android headers) with its test in
`engine/tests/core/input/AndroidTextInputTest.cpp`, so the rules run on a Linux host. The four
headers added here follow it exactly, which also means:

* no CMake edit is needed (the core test target globs `*Test.cpp`, and the target already has
  `engine/platform/android` on its include path — `engine/tests/core/CMakeLists.txt:203`);
* no new build dependency, no JNI, no Gradle;
* the integration agent only has to *call* them (see §4) to make them live on a device.

## 2. Investigations

| # | Question | Answer (evidence) | Label |
| --- | --- | --- | --- |
| 0c | Could the existing checks answer "will my project work on this phone"? | No: each answers one file and one question. Nothing composed them, and the failure modes the composition finds are exactly the ones a desktop never shows (accepted-then-undecodable WebP, a 183 MB decode, a sideways photo, a silently renamed duplicate) | verified (the report's first run, 4 tests red) |
| 0b | Does anything read EXIF? | No: `grep -ri exif engine/core/texture` is empty and `TextureData` offers `flipVertical` only, so a portrait phone photo lands sideways with its width and height swapped | verified (grep + `TextureData.h:149`) |
| 0 | What does the engine decode, really? | stb_image (PNG, JPEG, BMP, TGA, GIF, PSD, PNM, HDR, PIC; **no WebP, no HEIC/AVIF**), own KTX2/Basis path, SVG, tinyexr; SoLoud's dr_wav/dr_mp3/dr_flac/stb_vorbis. A phone shares photos as WebP and iPhone photos as HEIC | verified (grep + `stb_image.h` feature list + `soloud/src/audiosource/wav/`) |
| 1 | Does the runtime have an Android file system layer already? | Yes. `JasperEngineAndroid.cpp:206-235` maps `data://` to `getFilesDir()` and `asset://`/`lua://` through `AAssetManager` via `funopen`; `FileUtils.cpp:128` already refuses `asset://` writes on Android and knows only `data://` is on the file system. Saves work. | inferred (code read) |
| 2 | What does an Android import lack? | A picked file is a `content://` URI, not a path. There is no URI → name → legal project name → free name → asset type path anywhere in `engine/` or `editor/` (`grep` for `content://`, percent-decoding, sanitizing, unique naming: nothing in the runtime; the editor's table `editor/asset/AssetTypes.cpp` maps extensions only). | inferred |
| 3 | Does the engine report multi-finger gestures? | Per finger only (`Input.h:228-245` `Touch`, `getTouches`, phases, deltas). Nothing computes two-finger distance/midpoint/angle; `grep -i pinch engine/core` is empty. Android's `NativeEngine.cpp:1630-1660` feeds each pointer correctly, so the *data* is there and only the derived gesture is missing. | inferred |
| 4 | Is storage exhaustion handled? | `io::writeFileAtomic` (S03) refuses to truncate and fails cleanly, and `reasonOf()` already says "no space left on the device" (`FileUtils.cpp:80`). There is no free-space reading and no policy that refuses a write *before* it starts, so a 2 GB video capture fills the phone and every later save fails. `grep statvfs`: nothing. | inferred |
| 5 | Where can a project live on a phone, and what survives? | `getUserDataPath()` is the app-private `internalDataPath` (`JasperEngineAndroid.cpp:218`). Uninstalling deletes it; nothing on the device can see it without adb; `/Android/data/<other package>` is unreadable from Android 11; a SAF tree grant has no path and cannot do an atomic rename. No code distinguishes these today. | inferred |
| 6 | Which existing tests must not be duplicated? | `AndroidTextInputTest.cpp` (soft keyboard), `tools/android-two-finger-tap.sh` (an emulator injects two fingers to test the *runtime's* touch path), `tools/android-screen-check.py` (goldens). New tests here cover the *derived* layer only. | verified (read) |

## 3. Implemented (this branch)

All eight (seven headers) are free of Android headers, no new build wiring, C++20,
`-Wall -Wextra -Werror` clean on GCC 12, and **44 gtest cases passing**. Every header is included
twice by the suite on purpose (see bug 10d). See `ANDROID_TEST_MATRIX.md` for the evidence of every
case and the red control.

| File | What it provides | State |
| --- | --- | --- |
| `engine/platform/android/AndroidAssetImport.h` | `percentDecode`, `uriFileName`, `isDocumentUri`; `sanitizeFileName` (every rule `FileUtils::resolve` enforces plus the three platforms' reserved characters, Unicode kept, UTF-8-safe cut, extension preserved); `uniqueFileName` (Unity's `"sprite (1).png"`); `kindFromMagic` over 20 container signatures; `assetTypeForImport` (bytes first, extension second); `importedAssetName` | **verified** (10 cases) |
| `engine/platform/android/AndroidTouchGestures.h` | `PinchState` + `updatePinch` (distance, delta, midpoint, delta, angle, wrapped delta, pair continuity across frames, `ended`), `DragState` + `updateDrag` (slop, total, per-frame travel and the previous frame for a fling) | **verified** (7 cases) |
| `engine/platform/android/AndroidStorageSpace.h` | `SpaceInfo` (available / reserved / app quota), `allowWrite` (refuses a write that would leave less than the reserve, low/critical by share of the volume, unknown never refuses), `planCacheRelease` (which caches to drop, largest first, and "everything, even so" reported rather than deleted) | **verified** (5 cases) |
| `engine/platform/android/AndroidProjectLayout.h` | `isInsideProject` (untrusted name from a provider), `ProjectPlace`/`ProjectLocation`/`classifyLocation`/`accessFor` (app-private vs app-external vs SAF grant vs shared read-only, with the user-facing warning and `survivesUninstall`/`atomicRename`/`userVisible`), `isProjectCacheFolder`, `importFolderOf` | **verified** (4 cases, incl. the removable-volume warning) |

### Bugs the tests found in my own first draft (fixed, kept as evidence that the tests bite)

1. `riffForm` read `"WAVE"`/`"WEBP"` from byte 10 instead of byte 8 — every RIFF file (WAV, WebP)
   came back `Unknown`. Found by `AndroidAssetImport.MagicBeatsExtension`.
2. `.hidden` was read as an extension (`"hidden"`), so a dotfile import became `"hidden"`-typed.
   Fixed: a dot with nothing before it names a hidden file.
3. An extension-less Ogg import was named `.wav`; the container now picks the extension
   (`audioExtensionFor`). Found by `AndroidAssetImport.ImportedNameIsAnAssetName`.
4. `sanitizeFileName`'s byte cut could keep a control character (`"abc.tx"` from a name with an
   embedded NUL). Found by `SanitizeKeepsUnicodeAndDropsControls`.
5. `AndroidStorageSpace.h` was not self-sufficient (`<cstddef>` missing) — found by compiling each
   header alone, which `tools/check_headers.py` does not do for `engine/platform/`.
6. WebP was classified, named and accepted as a Texture although **no decoder in this tree reads
   WebP** (`grep stbi__webp` is empty; `.webp` is nevertheless in `editor/asset/AssetTypes.cpp`). Not a
   bug in my code — a gap in the port that the import can now report instead of failing at decode.
7. AVIF/HEIC and M4A/AAC were indistinguishable from any other file (all are ISO-BMFF, `ftyp` at
   byte 4) and were classified `Unknown`. Added `ImportKind::IsoBmff`.
8. `decodedBytes()` was assumed to saturate on the largest possible header (0x7FFFFFFF squared times
   4 is just *inside* 64 bits, so it does not saturate there); the test now checks the real bound.

## 4. Dependencies on the integration agent (no file of theirs is modified)

Each of these is a *call*, not an edit, and each is the same on a phone whatever the rest of the
port does. They are listed so agent 3 can wire them without reading this agent's code.

| Need | Where it goes | What it needs from the platform |
| --- | --- | --- |
| Free space before a save/capture/import | `System` (a virtual `getSpaceInfo()` next to `platformFopen`) or a direct call before `FileUtils::writeText` | `StatFs(dir).getAvailableBytes()/getTotalBytes()` on `getUserDataPath()` |
| An import from a picker | `MainActivity` registers `ActivityResultContracts.OpenDocument`/`GetContent`, hands the URI to JNI | `ContentResolver.openInputStream(uri)`, `DISPLAY_NAME` and `SIZE` from `OpenableColumns` |
| The project's own place | the editor's project open path (Android editor backend) | `getFilesDir()`, `getExternalFilesDir(null)`, and the tree URI of a SAF grant |
| Gestures for a game | nothing to wire: a game reads `PinchState` after `updatePinch(state, Input::getTouches(), frame)` from its update | — |

Anything that changes `System.h`, `MainActivity.java` or the Gradle files is the main agent's; the
proposal in `ANDROID_HANDOFF.md` says exactly what to add and changes nothing today.

## 5. Next tasks (in priority order)

1. **Java side of the import flow** (`JasperImport.java`): `OpenDocument` result → `DISPLAY_NAME` →
   read into the `data://` folder through the C++ helpers here. Blocked on nothing but a device; a
   JVM-free unit test is not possible, so it ships with an instrumented test or not at all.
2. **`StatFs` seam** behind `SpaceQuery` and a `System::getSpaceInfo()` override on Android — needs
   one line in `System.h` (main agent's file), so it is a proposal until then.
3. **A project picker** (SAF tree) and `ProjectLocation` from a real grant: needs the editor on
   Android to exist at all (main agent's backend).
4. **Texture/media decode report** for imports: which of the 20 magic kinds the Android path can
   actually decode today (the engine uses stb_image / ufbx / tinygltf for those, so it should be
   all of them but ASTC/KTX2 paths differ); measure with a real import per kind.
5. Re-run `AndroidSubsystemsTest` inside the `unit-tests` image once this branch is merged, to
   confirm the host-gcc build matches the image's clang.

## 6. How to reproduce everything in this document

```sh
# the suite, exactly as CI's jasperengine-core-tests builds it (host g++ here, no Docker needed)
g++ -std=c++20 -Wall -Wextra -Werror \
  -I engine/platform/android -I engine/core -I engine/core/math -I engine/core/util \
  -I engine/core/asset -I engine/libs -I engine/libs/sokol -I engine/libs/json \
  -I engine/libs/lua -I engine/libs/stb \
  -I libs/googletest/googletest/include -I libs/googletest/googletest \
  engine/tests/core/platform/AndroidSubsystemsTest.cpp \
  engine/core/math/Vector2.cpp engine/core/math/Vector3.cpp engine/core/math/Vector4.cpp \
  libs/googletest/googletest/src/gtest-all.cc libs/googletest/googletest/src/gtest_main.cc \
  -lpthread -o /tmp/android-subsystems && /tmp/android-subsystems

# in the repository: the real target, which globs this file automatically
UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=Android*' tools/build.sh unit-tests
```
