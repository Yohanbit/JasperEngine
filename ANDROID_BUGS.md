# ANDROID_BUGS.md

Bugs found while porting the import, gesture, storage and project-layout subsystems, in the state
they were found. "Fixed" means the fix is in this branch with a test that failed without it.

## Fixed in this branch

| # | Symptom | Cause | Fix | Test |
| --- | --- | --- | --- | --- |
| 1 | Every WAV and WebP picked on a phone classified as `Unknown` | my `riffForm` compared the form type at byte 10 instead of byte 8 (`"RIFF" + 4-byte size + "WAVE"`) | read `subspan(8)` | `AndroidAssetImport.MagicBeatsExtension` |
| 2 | A dotfile's name was treated as an extension (`.bashrc` → type of `bashrc`) | the dot was searched without checking something precedes it | `dot <= nameStart` is not an extension | `AndroidAssetImport.ExtensionDecidesWhenBytesSayNothing` |
| 3 | An extension-less Ogg import was saved as `clip.wav` | the audio fallback was a constant `"wav"` | `audioExtensionFor(kindFromMagic(bytes))` | `AndroidAssetImport.ImportedAssetNameIsAnAssetName` |
| 4 | A provider's name with an embedded NUL produced `abc.tx` instead of `abc.txt` | the control byte was dropped *after* the cut was measured | the cut is measured on surviving bytes | `AndroidAssetImport.SanitizeKeepsUnicodeAndDropsControls` |
| 5 | `AndroidStorageSpace.h` did not compile alone | `<cstddef>` missing (`size_t`) | include added | each header compiled by itself |
| 6 | A 120 MP camera photo would be decoded as 432 MB of RGBA before any check | nothing reads an image's dimensions before the decoder allocates | `AndroidImageLimits.h`: header-only size, then downscale/refuse (this fixes the *policy*; the decode path still has to call it, bug 10) | `AndroidImageLimits.*` (6 cases) |

## Found, not fixed (belongs to another owner or needs a device)

| # | Finding | Evidence | Owner |
| --- | --- | --- | --- |
| 6 | A save on a full phone is refused only when the write fails: no free-space check precedes it, so a 2 GB capture can fill the volume and take every later save with it | `FileUtils.cpp:80` maps ENOSPC but nothing asks first; no `statvfs` in the tree | platform seam (`System.h`) — proposal in `ANDROID_HANDOFF.md` |
| 7 | The player's `data://` is app-private: uninstalling the app deletes every save, and no file manager can reach it without adb | `JasperEngineAndroid.cpp:218` returns `internalDataPath` | product decision; `accessFor(AppPrivate)` now states the warning |
| 8 | A project on a SAF grant cannot be saved atomically: a provider replaces a file by copying over it | Android's `DocumentsContract` has no rename-over; `io::writeFileAtomic` assumes rename | editor's Android backend (main agent) |
| 10 | `AndroidImageLimits` is not wired into the decode: the importer must read the first 64 KiB and honour the verdict, and stb_image cannot decode at a reduced scale, so the peak allocation is only avoided through JPEG `inSampleSize` (Android `BitmapFactory`) or a vendored libjpeg-turbo | the policy is tested, the decode path is unchanged | runtime import path (see status §5.4) |
| 9 | Nothing computes a pinch today: a game on a phone gets per-finger data only | `grep -i pinch engine/core` empty | fixed for the derived layer here; the Android editor's backend still has to use it |
