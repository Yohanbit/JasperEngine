# Android port — defects

One row per defect that was found by reading the code or by a failing build, with what it did to
the person holding the phone, how it was found, and what state it is in. A defect leaves this list
only when it is fixed **and** the fix has been checked as far as this environment allows — which
for anything needing a GPU, a real touchscreen or the system clipboard means a device, and those
rows say so.

Severity: **S1** nobody can use it · **S2** a normal task is blocked · **S3** degraded but usable.

State: **FIXED** (change in a commit, checked as far as this box can) · **FIXED, DEVICE UNVERIFIED**
· **OPEN** · **NOT A DEFECT** (investigated, the code was right).

## Open

| # | Sev | What | Where | State |
|---|---|---|---|---|
| A3-4 | S2 | `Backend::penPressure()` returns −1 on Android although `NativeEngine.cpp` reads real stylus pressure (`stylusPressureOf`). The terrain brush in `TerrainEditWindow.cpp:1392` therefore paints at one fixed strength on a stylus and ignores how hard it is pressed. | `editor/backend/BackendAndroid.cpp` | OPEN |
| A3-6 | S2 | `JASPERENGINE_ANDROID_PROJECTS_DIR` is exported into the environment but nothing reads it; the editor's Hub opens `/JasperEngine Projects` on device storage, which Android has not allowed a write to since API 29. Creating a project at that path fails. | `EditorPaths.java`, `BackendAndroid.cpp`, Hub default | OPEN |
| A3-7 | S2 | The editor's project storage is app-private, so a project cannot be opened from Downloads or the user's Drive: Android's SAF is asynchronous and needs a Java→native channel that does not exist yet. The parity matrix records this as PARTIAL. | `editor/asset/EditorFilePicker` | OPEN (design work) |

## Fixed

| # | Sev | What it did | Fix | Commit | State |
|---|---|---|---|---|---|
| A3-1 | S1 | GameActivity's glue admits **only** `SOURCE_TOUCHSCREEN` motion by default, so a tablet's trackpad, a USB mouse or a hovering stylus reached the editor as nothing at all. | `android_app_set_motion_event_filter` / `android_app_set_key_event_filter` installed with the editor's rules (all pointer-class sources; every key ImGui can use; BACK only from the soft keyboard). | `0efd69600` | FIXED, DEVICE UNVERIFIED |
| A3-3 | S1 | A hardware keyboard could not type: the key handler knew only DEL/ENTER/ESC/TAB/SPACE/DPAD/HOME/END/PAGE, so letters, digits and punctuation produced no characters in any text field. | Full key map (A-Z, 0-9, punctuation row, keypad, modifiers) plus a character path with Shift and Caps Lock; 24 cases compile and run on the host. | `0efd69600` | FIXED, DEVICE UNVERIFIED |
| A3-8 | S2 | The editor had no clipboard on Android: `Platform_SetClipboardTextFn`/`GetClipboardTextFn` were never installed, so ImGui used its private buffer and copy/paste never left the app. | `EditorClipboard` (Java) + `EditorActivity.setEditorClipboardText/getEditorClipboardText`, wired in `initImGuiPlatform()`; set posted to the UI thread, read failure returns "". | `0efd69600` | FIXED, DEVICE UNVERIFIED |
| A3-9 | S3 | The editor's Gradle build could not find `JasperBootLog`: the editor module pointed its `java.srcDirs` at `<project>/engine/...` instead of the repository root, which Gradle silently ignores, so the whole editor failed to compile (this is what turned the APK lane red). | Absolute path derived from `jasperengineRootAbs`; checked with `git check-ignore` and a clean `assembleDebug` path. | `f68b39ea7` | FIXED (CI lane was red before, see status file) |
| A3-10 | S1 | A candidate APK would have been built with a `versionCode` **below** the installed V8 (`max(published)+1` = 2 while `buildNumber` was 8), so the phone would refuse to install it over V8. | `versionCode = max(highest published code, buildNumber) + 1`, pinned by a test. | `6133b661a` | FIXED |

## Investigated, not a defect

| # | What was reported | What the code actually does |
|---|---|---|
| A3-2 | "The IME editor type is wrong." | `TYPE_CLASS_TEXT` (value 1) with `IME_FLAG_NO_FULLSCREEN` — the right type for a text field, and the one the engine's own runtime uses. The defect was the unreadable spelling (`InputType_dot_TYPE_CLASS_TEXT`, `33554432`), fixed in `0efd69600` with no change of value. |
| A3-5 | "The J logo is a hang." | It is the engine's own splash render, logged with timings by `JasperBootLog`; the audit (`ANDROID_PORT_AUDIT.md`) established this before any of this work. |

## How a row gets closed

A `FIXED, DEVICE UNVERIFIED` row needs the user's phone: install the candidate APK from
`ANDROID_RELEASE_STATUS.md`, then press the case the row describes — a mouse or trackpad for
A3-1, a Bluetooth keyboard typing `abc` and Shift+1 for A3-3, copy/paste between a name field and
another app for A3-8. The boot report (`Downloads/jasperengine-boot.txt`) and a logcat capture are
the evidence; without them the row stays unverified no matter how good the code looks.
