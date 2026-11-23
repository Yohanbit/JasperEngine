# ANDROID_TEST_MATRIX.md

What is tested for the subsystems this agent owns, how it was run, and what the result was.
Every row is a test that exists in the branch; none is aspirational.

Legend: **pass** = ran, green. **red control** = the same test was run against a build without the
change (or with the bug still in it) and failed, which is what proves it tests something.

## Suites

| Suite | File | Layers | Cases |
| --- | --- | --- | --- |
| `AndroidAssetImport` | `engine/tests/core/platform/AndroidSubsystemsTest.cpp` | URI → name → legal name → unique name → asset type | 10 |
| `AndroidTouchGestures` | same | pinch (distance, midpoint, angle, continuity, end), one-finger drag | 7 |
| `AndroidStorageSpace` | same | write allowance, reserve, low/critical, app quota, cache release plan | 5 |
| `AndroidProjectLayout` | same | path containment, project place, access flags, warnings, caches | 4 |

Total: **25 cases, 25 pass, 0 skipped, 0 disabled.**

`AndroidTextInputTest.cpp` (7 cases, the soft keyboard's text conversions) already existed and is
not repeated here.

## Per case

| Suite.Case | What it pins | Result |
| --- | --- | --- |
| `AndroidAssetImport.PercentDecodesAndKeepsPlus` | `%20`, `%3A`, `+` is not a space, malformed `%` left alone | pass |
| `AndroidAssetImport.FileNameComesFromTheLastSegment` | SAF document id, query/fragment cut, `file://`, folder grant | pass |
| `AndroidAssetImport.SanitizesWhatThePathRulesRefuse` | `:`, `/`, `\`, `*?"<>|`, `..`, `.`, blanks, trailing dot | pass |
| `AndroidAssetImport.SanitizeKeepsUnicodeAndDropsControls` | `Foto Año Nuevo 🎉.png` unchanged; NUL/control/NL dropped; truncated sequence dropped | pass |
| `AndroidAssetImport.SanitizeKeepsTheExtensionWhenItCuts` | 200-byte name → ≤32 bytes ending `.png`; no UTF-8 split | pass |
| `AndroidAssetImport.UniqueNameCountsLikeTheProjectWindow` | `sprite (1).png`, no-extension `README (1)`, sanitize-then-count | pass |
| `AndroidAssetImport.MagicBeatsExtension` | PNG named `photo.txt`; Ogg/GLB/TTF magic; RIFF WAVE vs WEBP | pass (**red**: found the RIFF offset bug) |
| `AndroidAssetImport.ExtensionDecidesWhenBytesSayNothing` | `.obj`, `.gltf`, `.lua`, `.html`, `.css`, `.json`, `.aseprite`, unknown; folder ≠ extension | pass |
| `AndroidAssetImport.ImportedAssetNameIsAnAssetName` | `Photo 12:30.PNG` → `photo_12_30.png`; `1234` → `_1234.png`; Ogg stays `.ogg`; `...` → `asset` | pass (**red**: found `.wav` for Ogg) |
| `AndroidTouchGestures.PinchReadsDistanceAndItsChange` | 200 px, +40 spread, −100 close, midpoint | pass |
| `AndroidTouchGestures.PinchReadsRotationAndPan` | +π/2 line, pan without pinch, quarter-turn delta | pass |
| `AndroidTouchGestures.PinchEndsWhenAFingerLeaves` | `ended` for exactly one frame | pass |
| `AndroidTouchGestures.AnotherPairIsANewPinch` | a swapped finger reports no jump; a skipped frame restarts | pass |
| `AndroidTouchGestures.PinchIgnoresAThirdFinger` | fingers 0/1 keep the gesture | pass |
| `AndroidTouchGestures.DragStartsPastTheSlop` | 4 px no, 20 px yes, all travel on the first frame | pass |
| `AndroidTouchGestures.DragEndsOnReleaseAndKeepsTheFling` | release clears `dragging`, `previousFrame` survives | pass |
| `AndroidStorageSpace.RefusesAWriteThatWouldFillTheVolume` | 1 MiB ok; 100 MiB refused; budget = writable − reserve; budget+1 refused | pass |
| `AndroidStorageSpace.UnknownVolumeNeverRefuses` | no reading ⇒ never blocks a save | pass |
| `AndroidStorageSpace.WarnsByShareOfTheVolume` | 30% none, 9% low, 3% critical only | pass |
| `AndroidStorageSpace.AnAppQuotaIsTheSmallerBowl` | quota rest < volume free | pass |
| `AndroidStorageSpace.PlansWhichCachesToDrop` | largest first; smallest that covers the need; all when not enough; nothing when not critical | pass |
| `AndroidProjectLayout.RefusesAPathThatLeavesTheProject` | `..` anywhere, `c:`, `\` | pass |
| `AndroidProjectLayout.ClassifiesWhereTheProjectIs` | internal / external (and prefix-lookalikes) / tree grant / shared read-only | pass |
| `AndroidProjectLayout.WarnsAboutARemovableVolume` | SD card warning is appended, not lost | pass |
| `AndroidProjectLayout.RecognizesProjectCachesAndTheImportFolder` | `Library`/`Temp`/`.git` vs `assets`; assets dir | pass |

## How it was run (verified)

Host build of the same file the CI target globs, GCC 12.2, `-std=c++20 -Wall -Wextra -Werror`:

```
[==========] 25 tests from 4 test suites ran. (0 ms total)
[  PASSED  ] 25 tests.
```

The command is in `ANDROID_AGENT2_STATUS.md` §6. In the repository's own target the same file is
picked up by `file(GLOB_RECURSE CORE_TEST_SOURCES ... *Test.cpp)` and compiled with
`engine/platform/android` already on the include path, so:
`UNIT_SUITE=core UNIT_TEST_ARGS='--gtest_filter=Android*' tools/build.sh unit-tests` runs it plus
`AndroidTextInputTest` (**compiled here; not yet run inside the image** — the sandbox has no
Docker, so that lane is CI's).

## Red controls

| Change | Test that failed without it | How |
| --- | --- | --- |
| the whole suite | all 25 | the header set did not exist before this branch |
| RIFF form offset (bug 1) | `MagicBeatsExtension`, `ExtensionDecides…` | run before the fix: `AssetType::Unknown` for WAVE/WEBP |
| hidden-file extension (bug 2) | `ExtensionDecidesWhenBytesSayNothing` | `a.folder/photo` / `.bashrc` read as extensions |
| audio extension (bug 3) | `ImportedAssetNameIsAnAssetName` | `clip.wav` for an Ogg |
| control byte in a cut (bug 4) | `SanitizeKeepsUnicodeAndDropsControls` | `abc.tx` for an 8-byte name |
| missing `<cstddef>` (bug 5) | compiling `AndroidStorageSpace.h` alone | `size_t has not been declared` |

## Not covered (and why)

| Area | Why | Where it is tracked |
| --- | --- | --- |
| Anything on a real device | no device or emulator in this session | `ANDROID_HANDOFF.md` |
| The Java side of the picker | needs Android SDK; not JVM-testable | `ANDROID_AGENT2_STATUS.md` §5.1 |
| `StatFs` numbers from a real volume | needs the platform seam (`System.h`, main agent) | §5.2 |
| Decoding each of the 20 magic kinds on Android | needs an import run per kind on a device | §5.4 |
