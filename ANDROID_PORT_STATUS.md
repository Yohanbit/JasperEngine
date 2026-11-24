# Android port — where it really stands

Updated: 2026-11-23. Branch `arena/8286230a-jasperengine`, PR [#1](https://github.com/Yohanbit/JasperEngine/pull/1) (open, not to be merged by an agent).

## One line

The engine already runs as a game on the phone. The **editor** now builds its own APK, and the
last thing between the build and a first run is the game sample's C++ (fixed, verifying).

## The four-way distinction (read this before believing anything)

| | Status |
|---|---|
| **Written** | the Android editor backend, the editor's paths, its APK module, its boot report, the bionic guards |
| **Compiles** | every C++ file was checked with `g++ -fsyntax-only` (desktop and `-D__ANDROID__` where it applies); the game sample's APK is what CI is building now |
| **Tested** | nothing has been run on hardware. There is no device, emulator or `/dev/kvm` in this environment |
| **Verified on Android** | only what a previous session saw on the phone: the engine's own game APK starting and drawing its "J" |

## Where the pipeline is

`tools/build.sh android` (the game sample, `engine/project`) → then the editor module
(`engine/workspaces/androidstudio/editor`, package `com.jaspergameengine.editor`) → two APKs.

The CI run's logs and artifacts cannot be downloaded from this environment. The one channel that
works, and which the workflow now maintains, is described in `ANDROID_TEST_MATRIX.md`; it is how
each of the failures below was found.

## The failures, in the order they were peeled off

1. `jasperengineRoot` pointed four directories too shallow → CMake found no `CMakeLists.txt`.
2. `EditorActivity.java` imported `JasperBootLog`, which was not in the module's source set.
3. Gradle 9 refused the Kotlin compile reading `build/shared-java`; the Sync's output was a
   generated directory. **Resolved by removing the shared source entirely**: the editor has its own
   `EditorBootLog`, which also reports the editor's own paths and version.
4. The failure-reporting step died itself, so a red run explained nothing. It now runs under
   `set +e` and emits `::error::` annotations as well as a PR comment.
5. **The game APK, C++**: `editor/cli/McpClient.cpp` used `posix_spawn`, which bionic does not
   have. Fixed with an `__ANDROID__` guard, alongside the title library's batch runner.

## What is next, concretely

1. Green APK run; both artifacts (`jasperengine-project` and `jasperengine-editor`) present.
2. Install the editor APK on the phone and read `Downloads/editor-boot.txt` — the report names
   device, ABI, both directories, each step, and the app's own logcat, and it is written even if
   the start hangs (20 s and 60 s dumps).
3. From that report: the first frame, the ImGui scale, whether the touch dock is usable.
4. Then the acceptance path: Project Hub → create/open → scene → Play/Stop → save and reopen.

## Honest blockers

- No device and no adb: every statement about the phone comes from a boot report the user shares.
- Reading another app's screen (the eyedropper) needs MediaProjection.
- No external compiler on Android: exporting an APK from the phone, and Aseprite, cannot work.
- Multi-viewport ImGui is impossible there; the editor is a single canvas.
