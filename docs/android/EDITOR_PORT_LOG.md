# Android editor port — working log

Every defect found while making the editor build, run and respond on a phone, in the order it was
found, with what proved it. Kept in the repository because the run logs are not readable from the
session that does the work (see "Reading the logs"): this file is the record that survives them.

Device the runs were checked on: **Xiaomi 2207117BPG**, Android 13 (API 33), arm64-v8a.

## Reading the logs

What does not work, and why:

| Channel | Works? | Why |
|---|---|---|
| `gh run view --job=<id> --log` while the run is going | no | GitHub answers *"still in progress; logs will be available when it is complete"* |
| the same, once the job is complete | no | it redirects to `results-receiver.actions.githubusercontent.com`, which the working session cannot reach (`EOF`) |
| the `android-build-log` artifact | no | it redirects to `*.blob.core.windows.net`, same problem |
| **check-run annotations** (`gh api repos/{o}/{r}/check-runs/{id}/annotations`) | **yes** | served by the API itself, and while the job is still running |

So the workflow publishes the logs there: the step **"Publish the build log as annotations"** in
both jobs writes the compiler's message lines and the tail of each log as chunked annotations. Read
them with:

```bash
jid=$(gh api repos/Yohanbit/JasperEngine/actions/runs/<run-id>/jobs --jq '.jobs[0].id')
gh api repos/Yohanbit/JasperEngine/check-runs/$jid/annotations --jq '.[] | "[\(.annotation_level)] \(.message)"'
```

`::notice` annotations carry a verdict that is not a failure — the editor's step emits one with the
APK's size, or an `::error` naming the missing file. `continue-on-error` makes a failure look like a
success in the job summary, and that is exactly the blindness these annotations exist to remove.

Also: `concurrency.cancel-in-progress` used to be `true`, so every dispatch killed the run that was
still building — and a killed run has no log. It is `false` now, and the game's APK is its own job
("game") so the editor's ("build") completes in ~20-50 minutes instead of waiting hours for the game.

## Defects, in the order the compiler found them

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | `use of undeclared identifier 'InputType_dot_TYPE_CLASS_TEXT'` | it is an `android.text.InputType` value (`TYPE_CLASS_TEXT`), not a GameActivity constant; `NativeEngine.cpp:544` declares it locally | declare it the same way in `BackendAndroid::showSoftKeyboard` |
| 2 | `undefined symbol` for nine `HostServices` functions | `editor/Platform.cpp` calls them; only Linux, Windows and Mac implemented them | new `engine/platform/android/HostServicesAndroid.cpp` |
| 3 | `posix_spawn_file_actions_init` etc. undeclared | bionic **declares** posix_spawn's types and implements **none** of its functions; an Android app cannot exec anyway | `#if defined(__ANDROID__)` / `#else` in `util/ShellEnv.cpp`, `vcs/ProcessExec.cpp` (two sites: `runProcess`, `ProcessStream::start`) |
| 3b | the same errors again after the fix | an early `return` inside `#if` does not remove the code from compilation — it needs `#else` | wrap the block, close the `#endif` at the end of the function |
| 4 | `use of undeclared identifier 'android_app_clear_input_buffers'` | native_app_glue has `android_app_clear_key_events` and `android_app_clear_motion_events`, not one call for both | clear each kind after processing it, as `NativeEngine.cpp:1575,1660` does |
| 5 | `ld.lld: error: undefined symbol: glGetIntegerv` (12 symbols) | the engine's sokol backend calls them; `PLATFORM_LIBS` carries GLESv3/EGL but is linked to the standalone **executable** only, never to the shared `jasperengine` | `target_link_libraries(jasperengine PUBLIC GLESv3 EGL android log)` for Android |
| 6 | the same 12 errors after #5 | the fix passed `${OPENGL_LIB}`, which is set **inside the standalone-project block** closed at line 903 — before `add_library(jasperengine)` at 2196, so it expanded to nothing | name `GLESv3 EGL android log` literally |
| 7 | `undefined symbol: AAssetManager_open`, `AAsset_close`, `AAsset_openFileDescriptor64` | same as #5, `libandroid` | added to the same `target_link_libraries` |
| 8 | configure died in 90 s: `/bin/sh: 1: ccache: not found` | the editor step passed `CMAKE_CXX_COMPILER_LAUNCHER=ccache` unconditionally; Gradle hands it to CMake, which puts it in front of its own test program | pass it only when `command -v ccache` succeeds |
| 9 | `Project Hub`'s New Project pointed at `/` | `homeDirectory()` falls back to `current_path()`, which is `/` on Android and not writable; `EditorPaths.java` exported `JASPERENGINE_ANDROID_PROJECTS_DIR` and nothing read it | read it |
| 10 | shaders could not cache | `App::getUserCacheBaseDir()` fell through the Linux branch to `temp_directory_path()` (`/data/local/tmp`, not owned by the app) | use the activity's files directory |

## Defects found on the phone (first run, build `bb4a0712`)

The boot reports (`editor-boot.txt`) from the device are clean: the activity starts, the surface is
created, no error comes from the process. Two defects are visible by using it:

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 11 | the top menu row is drawn inside the status bar (clock, battery, wifi); measured on the screenshot, content starts at **y = 0** | the window is created with `setDecorFitsSystemWindows(false)` and the bars stay visible over it, and nothing insets ImGui's viewport | **first attempt, wrong (see #13)**: `platformNewFrame()` wrote the main viewport's `WorkPos`/`WorkSize` by hand |
| 12 | the soft keyboard's backspace erased nothing | `GameTextInput` reports its **whole text**, never a keystroke: a deletion is only visible as a diff, and the backend forwarded characters only, which can grow a field but never shrink it | use the engine's own `ComponentTextInput` (`AndroidTextInput.h`) — the class its `InputField` uses — to diff the text into "one backspace per removed character", reset it when the keyboard opens on another field, and drain the backspaces into ImGui as `ImGuiKey_Backspace` before the typed characters |

## Defects found on the phone (second run, build `2a1b8cac`)

The report came back as three sentences, and each one had a different cause — two of them mine from
the commit that was supposed to fix the screen:

> "si le doy para hacer un proyecto no me sale nada, tampoco me deja exportar ni mover nada"
> "la parte de notificaciones no se quita… la línea para salir de la app tampoco"

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 13 | the bars did not move at all — #11's fix had no effect | `platformNewFrame()` runs from `EditorFrame::render` **after** `renderer->newFrame()` (EditorFrame.cpp:189-190), i.e. inside an open ImGui frame, and ImGui recomputes the work rect from `WorkInsetMin`/`WorkInsetMax` at the start of every `NewFrame` (imgui.cpp:16984-16997). The assignment was overwritten before any window read it | use ImGui's own mechanism, `Platform_GetWindowWorkAreaInsets`, which imgui.h:4284 documents as *"safeAreaInsets in iOS land, DisplayCutout in Android land"*; registered in `initImGuiPlatform()` |
| 14 | nothing could be dragged or resized; taps still worked | ImGui derives `MouseDelta` from **two positions queued in the same frame** — the one from before the frame and the last one queued. The backend queued one position per `ACTION_MOVE`, so both were equal, the delta was **0**, and every drag was a click | each MOVE queues `(previous, current)`, tracking the previous point from `ACTION_DOWN` so the step is the whole movement, however many events arrived |
| 15 | — | a change made while chasing #12 that never shipped: `GameActivity_setTextInputState(empty)` on keyboard open | it does not reset a counter, it **replaces the field**, so tapping a field that already held "New Project" emptied it. Reverted; only this side's diff counter is reset |
| 16 | every diagnosis so far was made from a screenshot: the boot reports carried only Java lines | `Log::print` writes to `stderr` (Logger.cpp:367), which on Android goes nowhere — not logcat, and there is no terminal | `Logger` writes through `__android_log_print` under `__ANDROID__`, so `EditorBootLog`'s own logcat capture contains the engine's lines |

Two of these are worth stating plainly, because they were the cost of the whole round: **#13 was a
fix that could not work where it was written**, and **#16 is why nobody could see that** — without a
line of the engine in logcat, "the bars did not move" and "the code never ran" look identical.

To remove the guessing for good, the boot report now also carries (a) the frame geometry — surface
size, insets, UI scale — logged once when the context comes up, (b) the three silent early returns in
`hubCreateProject` (`a creation is still running`, `a project is loading`, `a project change is
pending`), where a new project is being written, and why it failed if it did, and (c) whether each
system bar is visible, read from `WindowInsets` in the report rather than judged from a screenshot.

**Immersive, not inset.** #11 assumed the bars stay and the content moves around them. What the phone
needs is the opposite: `EditorActivity` called `controller.show(systemBars())` — it was *asking* for
them. It now calls `hide(systemBars())` with `BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE`, so a swipe from
the top edge brings the status bar and the shade back, one from the bottom brings the navigation bar,
and both hide themselves again. With no bars over the content the insets are 0 and the editor is
genuinely full screen; the inset plumbing stays in place for the cutout and the transient bars, and
for the day a Java-side value can be handed to it.

## Build history

| run | commit | outcome |
|---|---|---|
| 45122674982 | 3335f508 | stuck in the game's step for 45+ min, editor never reached |
| 45127390334 | 2b5d8c51 | editor step success (first time), artifact not produced |
| 45131142844 | d29bb5b4 | `ccache: not found`; `posix_spawn` in ShellEnv/ProcessExec |
| 45136972034 | 856bb17c | still `posix_spawn` (#3b) |
| 45140694582 | dd1a562b | `ProcessStream::start`, the second spawn site |
| 45146316152 | cd1c6561 | one error in 3339 objects: `android_app_clear_input_buffers` |
| 45155146583 | 26d26c1a | link: 12 GL symbols |
| 45173422938 | a5e4891a | link: the same 12 — `${OPENGL_LIB}` was empty (#6) |
| 45184673689 | 01d36ff1 | link: the same 12, then `AAssetManager_open` |
| 45215268485 | bb4a0712 | **APK produced**, 105 917 484 bytes, installed and running on the device |
| 45230610321 | 2a1b8cac | #11 and #12; **BUILD SUCCESSFUL in 47m 34s**, no errors |

## What is still open

- Storage Access Framework: importing a project from the phone's storage is a Java intent, not a path.
- A touch-first re-layout of the dock: the panels are usable but sized for a mouse.
- Building a game on the phone: `CommandRunner` and `ProcessExec` correctly refuse to exec on
  Android, so the phone edits and previews; it does not compile the APK of a project.
