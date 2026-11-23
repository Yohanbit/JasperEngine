# ANDROID_CHANGELOG.md

The commits of the secondary agent's stream (`arena/593187bf-jasperengine`, PR #2), newest last
within each group. Each entry says what changed, why, and what evidence backs it. "Verified" means a
host build and a test run in this session; "inferred" means read from the code and the platform docs,
with no device to run it on.

## Unreleased — PR #2, open

### `AndroidAssetImport.h` + `AndroidTouchGestures.h` + `AndroidStorageSpace.h` + `AndroidProjectLayout.h` (`236dea81`)

- `uriFileName`, `percentDecode`, `sanitizeFileName`, `uniqueFileName`, `kindFromMagic` (20
  containers), `assetTypeForImport`, `importedAssetName`: a `content://` pick turns into a legal
  project file name of the right type.
- `updatePinch` / `updateDrag`: the derived gesture `Input` does not compute.
- `allowWrite` / `planCacheRelease`: refuse a write *before* it eats the reserve; which caches to drop.
- `isInsideProject`, `classifyLocation`, `accessFor`: app-private vs `Android/data` vs SAF grant vs
  shared read-only, with `atomicRename` / `survivesUninstall` / `userVisible`.
- 25 gtest cases. Red controls: a RIFF form type read at byte 10 (every WAV/WebP was `Unknown`), a
  dotfile read as an extension, an extension-less Ogg named `.wav`, a control byte that shortened a
  cut name, a missing `<cstddef>`. Verified: 25/25, `check-cxx-subset.py`, `check_raw_writes.py`.

### `AndroidImageLimits.h` (`ecbdac8b`)

- `readImageSize` and `decideImageImport`: Refused / Downscale-by-halves / Allowed against a budget,
  because a 120 MP photo is a 432 MB allocation before the texture.
- Red control: saturation assumed in `decodedBytes()`. 31/31.

### `decodeSupportOf` + `playbackSupportOf` + `ImportKind::IsoBmff` (`306c4fda`)

- `.webp` is in the editor's Texture table and **no decoder in the tree reads it** (bug 6a) → the
  import asks first: WebP → PNG, HEIC/AVIF → BitmapFactory, M4A/AAC → MediaCodec → WAV.
- Red control: AVIF/HEIC/M4A indistinguishable from any file (all `Unknown`). 34/34, 400,209-input
  ASan/UBSan fuzz.

### `AndroidImageOrientation.h` (`c0329a01`)

- `readOrientation` from a JPEG's APP1 or a PNG's `eXIf`, both endiannesses, bounded IFD walk;
  `transformFor` for all eight values; `orientedSize`; `orientationName`.
- The engine read no EXIF anywhere, so a portrait phone photo imported sideways with its width and
  height swapped (bug 6c). 38/38.
- The five fixture mistakes this cost (an IHDR CRC one byte short, twice; a big-endian tag encoded
  little-endian; an IHDR data length) were in the test data, not the parser.

### `AndroidImportPreflight.h` (`21ee91e5`)

- The four checks composed into per-file findings and a project report; `summary` names the first
  thing to fix.
- Real defects found: a WebP called "converted" on a device with no converter; the test fixture
  `pngHeader()`; **`AndroidImageLimits.h` closed its include guard before `decodeSupportOf`**
  (bug 10d) — the suite now includes four headers twice on purpose. 44/44.

### `AndroidModelSidecars.h` (`5a8b49b1`)

- A `.gltf` names its buffers and textures as relative URIs a phone cannot follow; the report says
  which files to bring and which are inline. The engine *suppresses* those warnings for a cooked
  model, so a user importing a broken `.gltf` was told nothing (bug 10e).
- Percent-decoding now agrees with `uriFileName`; fuzzing found `{"uri": ""}` listing a nameless
  file. 51/51.

### `tools/android-asset-report.py` (`b6daae0f`)

- The pre-flight a person can run: a Python driver plus a generated builder over the same headers,
  exit 1 = an asset is broken, 2 = the report could not be built.
- Its first run found `.m4a`/`.aac`/`.opus` classified `Unknown` (bug 10g), making
  `playbackSupportOf` unreachable for exactly its own formats. 52/52.

### Aggregate memory in `AndroidImportPreflight.h` (`e7555e74`)

- 20 × 2048² atlases are each a Note and 320 MiB together; no per-file check can see that
  (bug 10h). `residentTextureBytes`, `overResidentBudget`, `suggestDownscale` (largest first,
  deterministic on ties).
- Red controls: the test's decimal/MiB arithmetic; advice that changed between runs. 54/54.

### `AndroidTouchGestures.h` v2 + `tools/check_headers.py` (`084b24f3`, `9b9d12d1`)

- `updateLongPress` (Android's 500 ms, once, cancelled by movement) and `updateTap` (tap, double tap,
  and a long-press press is never a tap): the one-finger gestures a touch UI has no other source for.
  6 cases, 60 total.
- `check_headers.py` now covers `engine/platform/` and includes every header **twice** — the class of
  defect (bug 10d) that no other gate could see.

### `AndroidImportTransfer.h` (`1b7c9db7`)

- A copy from a `content://` stream that ends early: hidden `.jeimport` temporary, resume, refusal
  before a write that would eat the reserve, and Complete / Interrupted / Cancelled / Mismatched.
  10 cases, 70 total. Three defects found by the fuzzer (a doubled suffix, `..jeimport`, and a target
  that disagreed with the temporary's mapping).

### `planModelImport` + the report's sidecar names (`73a03fec`, `3852700b`)

- A `.gltf` and its files landing in one flat folder: `<model>_<leaf>` names, numbered collisions,
  and the document itself rewritten so it names what the import creates. Two models' `common.png`
  stopped overwriting each other. 5 cases, 75 total.
- The report prints the pairs: `Body.bin -> body_body.bin, textures/common.png -> body_common.png`.

### `absentSidecars` (`3dccd9c1`)

- A `.gltf` picked with only some of its files now names the ones that are not there
  (`PreflightInput::companionNames` → `absentSidecars`, matched on the leaf). 2 cases, 77 total.

### An audit of this branch's own code: four defects, two gates (`0f252bfa`, `f3cb5205`, `ba659cc9`)

- `isInsideProject` accepted `"/etc/passwd"`, `""`, `"."` and `"a/./b"` (bug 10p).
- `sanitizeFileName` accepted Windows device names, which break the project on a Windows machine and
  never on the phone (10q).
- `planTransfer` resumed a **sizeless** provider's copy from a temporary that might be another file's,
  and reported the truncated result Complete (10r) -- the worst of the four.
- A downscale message said "0 MB decoded" (10s).
- `tools/android/selfcheck.cpp`: seven invariants in one program, no CMake. Verified by mutation --
  and its first version did *not* catch bug 10j, which is recorded in the file.
- `tools/android/data-io-overflow-repro.cpp`: the core `Data::seek`/`read` defects with the patch
  beside them, runnable (`A` aborts under ASan). The audit found a **second** core defect the earlier
  report missed: `read()` at the end returns the buffer's first byte instead of 0 (10b-ii).

### The five integrations, written out; and one command to check everything (`615a45f2` and after)

- `ANDROID_INTEGRATIONS.md`: the five connections — the import pipeline (picker, preflight, copy,
  recovery), free space before a write, where the project is, and the gestures — each with the file,
  the function and line, the call, its dependencies, the expected behaviour and the code, Java and
  C++ both. Integration 4 (gestures) needs no JNI and is the one to do first.
- `tools/android/integration/{gestures,import,space}-demo.cpp`: the code that document gives,
  compiled and run against the real headers, so the quoted output is a run and not a sketch.
- `tools/android/verify.py`: from a bare checkout with a C++20 compiler and nothing else — the suite,
  the selfcheck, every header twice under `-Werror`, the three demos, the 400k-input fuzz run, and
  then the two `Data` defects, reported without failing the run because `Data.cpp` is not ours.
- `tools/android/data-overflow-real.py`: the same defects, but with `seek` and `read` **lifted out of
  the real `Data.cpp`** instead of retyped, so the ASan trace is about the shipped file. It exits 1
  while the defect is present and 0 once the handoff's patch is applied; both states were checked.
- `ANDROID_CLEAN_CLONE.md`: the clean-clone run, its real output, and the by-hand commands.
- `ANDROID_HANDOFF.md` §4: the verdict asked for — the SIGSEGV is an **unfixed defect** in the core,
  not a known issue and not a deliberate reproduction, with the trace and the full chain.

## Still not done

- Nothing here has run on a phone, an emulator or an APK: no SDK, no NDK, no `javac`, no Docker in
  this session. `ANDROID_TEST_MATRIX.md` names the first device verification each subsystem needs.
- The wiring is the main agent's: `System::getSpaceInfo` (handoff §1), `preflightImports` in the
  Project panel (handoff §2b), `readOrientation` + `BitmapFactory` `Matrix` in the import path
  (handoff §2c). Bugs 10c/10f/10i.
- The core `Data::seek`/`read` unsigned overflow (a `memcpy` SEGV reachable from Lua) is reproduced
  with ASan and **not** fixed: it is the main agent's file. A three-line patch is in the handoff §4.
