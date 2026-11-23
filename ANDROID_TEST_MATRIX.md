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
| `AndroidImageLimits` | same | header sizes (7 formats), truncation, downscale, refusal, no-budget, overflow | 6 |

| `AndroidAssetImport` (decode/playback support) | same | WebP gap, the formats the engine reads, the audio decoder set | 3 |

| `AndroidImageOrientation` | same | the tag in JPEG and PNG, malformed tags, all eight transforms, swapped sizes | 4 |

| `AndroidImportPreflight` | same | a portrait photo's note; WebP broken vs. converted; a huge photo reduced and an absurd one refused; the renamed duplicate; the project summary; a non-asset | 6 |

| `AndroidModelSidecars` | same | a .gltf's external files; a GLB/inline/data: URI needs nothing; a malformed .gltf; a URI is not a path | 5 |

Total: **54 cases, 54 pass, 0 skipped, 0 disabled.**

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

| `AndroidImageLimits.ReadsPngJpegBmpGifPsdAndWebp` | IHDR + colour type + bit depth; SOF after an EXIF segment; negative BMP height; GIF/PSD/WebP | pass |
| `AndroidImageLimits.UnreadableHeadersAreUnknownNotGuessed` | empty, garbage, cut IHDR, zero size, no SOF, a random file that looks like a TGA | pass |
| `AndroidImageLimits.AHundredMegapixelPhotoIsDownscaledNotFatal` | 12000x9000 RGBA = 432 MB → 1/4 (3000x2250) within 256 MB and 4096 px; a fitting image untouched | pass |
| `AndroidImageLimits.RefusesWhatNoDownscaleSaves` | 300 MP refused; 8000x8000 within a 16 MB / 2048 budget → 2000 px; 16-bit needs twice the scale | pass |
| `AndroidImageLimits.NoBudgetMeansNoRefusal` | no byte limit ⇒ allowed; unknown header ⇒ decoder's business as today; texture limit still applies | pass |
| `AndroidImageLimits.ExtremesDoNotOverflow` | 0x7FFFFFFF² does not wrap; 16-bit saturates | pass (**red**: my first assertion assumed saturation one step too early) |

| `AndroidAssetImport.WebPIsAcceptedButFlaggedAsUndecodable` | a WebP classifies as a Texture *and* the import says it must become PNG | pass (**red**: the test caught that my `std::string("RIFF\0...")` literal truncated at the NUL and WebP was `Unknown`) |
| `AndroidAssetImport.APhoneFormatIsClassifiedByItsExtensionWhenTheMagicIsNotEnough` | `.m4a` (ISO-BMFF bytes), `.aac`, `.opus` → Audio; `.webp`, `.heic`, `.avif` → Texture | pass (**red**: `.m4a` was Unknown, "copied as it is") |
| `AndroidAssetImport.TheFormatsTheEngineReadsAreSupported` | PNG/JPEG/GIF/PSD/HDR/KTX2 supported; text goes to the decoder; AVIF unsupported | pass (**red**: AVIF was `Unknown`, indistinguishable from any file, before `ImportKind::IsoBmff`) |
| `AndroidAssetImport.AudioSupportFollowsTheDecoderSet` | WAV/OGG/MP3/FLAC supported (the four decoders linked); M4A needs MediaCodec, else refused with a sentence | pass |

| `AndroidImageOrientation.ReadsTheTagFromJpegAndPng` | 6/3/1/8 (big-endian) from a JPEG, 7 from a PNG; no tag on a screenshot | pass (fixture bugs found first) |
| `AndroidImageOrientation.AMalformedTagIsIgnoredNotFatal` | wrong field type, IFD offset past the segment, a value outside 1..8 | pass |
| `AndroidImageOrientation.EveryValueHasATransform` | the eight values' rotation and swap, `isUpright` | pass (**red**: 1 was reported as "needs work") |
| `AndroidImageOrientation.ARotatedPhotoReportsSwappedDimensions` | 4000x3000 + 6 → 3000x4000; a mirror and a 180 do not swap | pass |

| `AndroidImportPreflight.APortraitPhotoSaysHowItIsTurnedAndHowBigADecodeIs` | `DCIM/IMG_4821.JPG` + EXIF 6 → asset `img_4821.jpg`, a Note naming "turned 90 clockwise" | pass (**red**: the path was used as the name) |
| `AndroidImportPreflight.AWebpIsBrokenWithoutThePlatformDecoderAndANoteWithIt` | no BitmapFactory → Broken; with it → Note "converted to png" | pass (**red**: called a note when nothing converts it) |
| `AndroidImportPreflight.AHugePhotoIsReducedAndOnePastEveryLimitIsRefused` | 8000x6000 → 4000x3000 at 50%, "183.1 MB" named; 20000x20000 → Broken with its size | pass (**red**: the message said 0.0 MB for every image) |
| `AndroidImportPreflight.ASecondFileWithTheSameNameIsReportedUnderTheNameItGets` | `downloads/Icon.PNG` over a taken `icon.png` → `icon (1).png` | pass |
| `AndroidImportPreflight.TheWholeProjectGetsOneLineThatNamesTheFirstThingToFix` | four files, one broken; summary names `broken.webp`; an upright photo and an mp3 are Fine | pass |
| `AndroidImportPreflight.AFileThatIsNotAnAssetIsCopiedAndNotReportedAsBroken` | `docs/readme.md` → Fine, "copied as it is" | pass |

| `AndroidModelSidecars.AGltfNamesTheFilesAPhoneCannotBringAlong` | 1 buffer + 2 images (one named twice) → 2 files to bring, "1 buffer file and 2 texture files" | pass |
| `AndroidModelSidecars.AGlbOrACookedGltfNeedsNothingElse` | no buffers; a URI-less buffer + `bufferView` image; a `data:` URI | pass |
| `AndroidModelSidecars.AnEmptyUriIsNotAFileToBring` | `{"uri": ""}` → nothing to bring | pass (**found by fuzzing**: it listed a nameless file) |
| `AndroidModelSidecars.AMalformedGltfSaysSoInsteadOfClaimingNoDependencies` | `{"buffers":[` → unparsed, with a reason | pass |
| `AndroidModelSidecars.AUriIsNotAPath` | `%20` decoded (agreeing with `uriFileName`), query/fragment cut, `\\` as a separator | pass (**red**: it left `skin%20map.png` encoded) |
| `AndroidImportPreflight.AGltfThatNeedsItsBuffersIsANote...` | a .gltf with a buffer and an image → Note, 2 `missingSidecars`; a non-JSON .gltf → Broken | pass |
| `AndroidImportPreflight.AGlbIsOneFileAndNothingToSay` | a GLB → Fine, no message | pass |

| `AndroidImportPreflight.AFortyTextureProjectIsOutOfMemoryOnAPhoneAndTheReportSaysWhichToShrink` | 20 × 2048² RGBA, budget 128 MiB → 0 broken, 320.0 MB resident, summary, `suggestDownscale` named | pass (**red**: advice was unstable between runs) |
| `AndroidImportPreflight.AProjectInsideItsBudgetSaysNothingAboutMemory` | 2 × 256² under the budget → no summary | pass |

### The double-include guard

The suite includes `AndroidAssetImport.h`, `AndroidImageLimits.h`, `AndroidImageOrientation.h` and
`AndroidImportPreflight.h` a second time at the top of the file. This is not redundancy: an `#endif`
written before a later block redefines the block's enums on the second include, and two appends had
left `AndroidImageLimits.h` in exactly that state (bug 10d). Without the second include nothing on the
host compiles a header twice, so nothing could see it.

### Fuzzing (verified, sanitizers)

`/tmp/fuzz.cpp` (not committed; reproduce below) feeds every truncation of eight real header
fixtures plus 400,000 random buffers — 1 in 7 behind a PNG magic, 1 in 11 behind a JPEG SOI — through
`readImageSize`, `assetTypeForImport` and `sanitizeFileName`, built with
`-fsanitize=address,undefined`:

```
ok: 400209 inputs, no out-of-bounds read and no impossible size
```

The fifth run also parses JSON-shaped noise (and raw noise) through `gltfSidecars`, aborting if a
parsed model yields an empty required file name or an unparseable one carries no reason — which is
how the `{"uri": ""}` defect above was found.

The fourth run drives `preflightOne` with a hostile file name built from the same bytes (separators,
colons, a NUL, empty, 300 bytes) and aborts if any finding cannot name itself or a broken one has no
reason. `ok: 400209 inputs`.

The second run also drove `decodeSupportOf`, `playbackSupportOf` and `decideImageImport` over the
same inputs (the ISO-BMFF path reads bytes 4..7, which is where a truncation bites); the third added
`readOrientation` (a JPEG marker walk and a PNG chunk walk, the two places a length field decides
where the next read is).

The orientation work took five rounds to get its *fixtures* right (a PNG CRC one byte short, twice;
a big-endian tag encoded in the little-endian order; an IHDR data length). Each round failed a test
that named the wrong number, which is the reason the fixture is now built from a named chunk array
rather than a counted literal.

## How it was run (verified)

Host build of the same file the CI target globs, GCC 12.2, `-std=c++20 -Wall -Wextra -Werror`:

```
[==========] 54 tests from 8 test suites ran. (0 ms total)
[  PASSED  ] 54 tests.
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
| `ExtremesDoNotOverflow` | a saturating `decodedBytes()` assumed where the product still fits |
| `WebPIsAcceptedButFlaggedAsUndecodable` | a `std::string` literal truncated at its first NUL, so the WebP fixture was 9 bytes |
| missing `<cstddef>` (bug 5) | compiling `AndroidStorageSpace.h` alone | `size_t has not been declared` |

## Not covered (and why)

| Area | Why | Where it is tracked |
| --- | --- | --- |
| Anything on a real device | no device or emulator in this session | `ANDROID_HANDOFF.md` |
| The Java side of the picker | needs Android SDK; not JVM-testable | `ANDROID_AGENT2_STATUS.md` §5.1 |
| `StatFs` numbers from a real volume | needs the platform seam (`System.h`, main agent) | §5.2 |
| Decoding each of the 20 magic kinds on Android | needs an import run per kind on a device | §5.4 |

### The runnable tool (`tools/android-asset-report.py`, verified on a folder)

A folder built for the test — `art/backdrop.png` (8000x6000), `art/icon.png` (64x64), `ui/icon.webp`,
`models/Body.gltf` naming a `.bin` and a texture, `audio/song.mp3`, `audio/clip.m4a`, `notes.txt`:

```
- backdrop.png -> backdrop.png (texture)
    reduced from 8000x6000 to 4000x3000 (50% of each side) to fit a 4096 texture; one decode peaks at 137.3 MB
! clip.m4a -> clip.m4a (audio)
    no decoder in this engine reads this audio; export it as WAV, OGG, MP3 or FLAC
- Body.gltf -> body.gltf (model)
    needs 1 buffer file and 1 texture file; a phone imports one file at a time, so they must be imported too
! icon.webp -> icon.webp (texture)
    this build has no WebP decoder; the photo must be converted to PNG before it can be used

6 assets: 2 broken, 2 to note
largest single decode: 183.1 MB
first to fix: clip.m4a: no decoder in this engine reads this audio; export it as WAV, OGG, MP3 or FLAC
```

Exit codes are the contract: `1` (the line above), `0` with `--bitmap-factory --media-codec`
("6 assets: 0 broken, 4 to note"), `0` for an empty folder, `2` for a path that is not a folder.
`notes.txt` is not scanned at all. The first run of this tool is what found the `.m4a` bug above.

### The aggregate case, on a folder (`--resident-mb 128`)

```
20 assets: 0 broken, 20 to note
largest single decode: 16.0 MB
all textures resident: 320.0 MB
shrink first: atlas_00.png, atlas_01.png, ... atlas_15.png
first to fix: the project's textures take 320.0 MB, past the 128.0 MB this device has for them
```
