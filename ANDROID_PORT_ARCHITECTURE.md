# Android: architecture and technical decisions

Branch: `arena/8286230a-jasperengine`. Every claim below is grounded in the code; file and line
references are given so they can be re-checked.

## 1. What the V8 APK actually is

The V8 APK is the **game runtime**, not the editor. It packages `engine/project`, whose whole
game is:

```cpp
// engine/project/main.cpp
image.setTexture("jasperengine.png");
Engine::setScalingMode(Scaling::FITWIDTH);
Engine::setCanvasSize(1000,480);
```

`engine/project/assets/jasperengine.png` is 413x260 and is exactly the "J / JASPERENGINE" image
seen on screen. With `Scaling::FITWIDTH` on a 1080x2400 display the image draws 1080x680,
centred from y=860 to y=1540 — centre at y=1200, the exact centre of the screen.

**Correcting the record:** there is no hang at the J. The engine starts, creates an OpenGL ES 3.2
context on the Mali-G76, classifies the device and draws that image. The boot report says so:

```
D nativeloader : Load .../libarm64/libjasperengine-android.so ... : ok
D company.project: JNI_OnLoad success
I GameActivity : Found library libjasperengine-android.so. Loading...
I JasperEngine : Graphics: OpenGL ES 3 / WebGL2; OpenGL ES 3.2 v1.r32p1; Mali-G76
```

What was wrong was smaller and is fixed: `engine/project/main.cpp` never called
`Device::configure()`, so the detected tier (`Low`) was discarded (`Device.cpp:192` requires
`configured`), and the engine ran high-quality settings on a GPU it had itself classified as low.
Commit `479f72b9`.

## 2. Why the editor was not in any APK

The editor is Dear ImGui 1.92.8 over per-platform native backends. Before this work
`CMakeLists.txt:348` accepted exactly three:

```cmake
message(FATAL_ERROR "Unknown API_BACKEND '${API_BACKEND}' (expected linux, win, or mac)")
```

There was no `API_BACKEND=android`, so configuring the editor for Android failed at CMake
configure time, before a compiler ever ran.

## 3. What made the port feasible

Three properties of the existing code, all verified by reading it:

**The frame loop is already portable.** `editor/EditorFrame.cpp` (282 lines) is shared by the
Linux and Windows backends. It touches no window system: it takes an `EditorFrameState` (size,
focus, minimized, redraw request) and drives `App` and the renderer. Android reuses it unchanged.

**The GL renderer takes a context by callback.** `editor/backend/renderer/RendererGL.cpp`
(155 lines) never creates a context itself; it fills a `RendererPlatform` struct of function
pointers (`createContext`, `makeContext`, `swapBuffers`, `setSwapInterval`). An EGL-backed set of
those is about 60 lines.

**The engine never depends on the runtime host.** `grep -rn "NativeEngine" engine/core/` returns
nothing. `NativeEngine.cpp` is the game runtime's host (swappy pacing, its own EGL surface, its
own input routing) and the editor does not use it — the editor calls
`Engine::systemInit(argc, argv, new editor::Platform(&project))` exactly as the desktop backends
do. This is why the editor can host the engine itself without fighting the runtime for the one
window.

## 4. Design decisions

### 4.1 The editor gets its own APK and its own package

`com.jaspergameengine.editor` (`engine/workspaces/androidstudio/editor/`), separate from the
runtime's `com.yourcompany.project`. Installing the editor must not replace a game and a game must
not replace the editor. The project lives beside the runtime's Gradle module and shares its
plugin versions, NDK pin and repositories.

### 4.2 The native tree is the repository's own CMakeLists.txt

`engine/workspaces/androidstudio/editor/build.gradle` points CMake at the repository root, not at
`engine/CMakeLists.txt`. `API_BACKEND` detects Android (`CMakeLists.txt:76-88`), and the editor's
own top-level tree is what builds `libjasperengine-editor.so`.

The engine subdirectory (`add_subdirectory(${JASPERENGINE_ROOT})`) only creates its `APP_NAME`
target when `CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR` (`engine/CMakeLists.txt:2609`),
which is false here — so `AndroidMain.cpp` (which defines `android_main` for the runtime) is not
compiled and cannot collide with the editor's own `android_main`
(`editor/backend/BackendAndroid.cpp:661`).

### 4.3 GLES3 is the graphic backend, and ImGui's renderer switches too

`API_BACKEND=android` forces `GRAPHIC_BACKEND=gles3` (`CMakeLists.txt`). That adds `-DSOKOL_GLES3`
(the engine's Sokol backend) **and** `-DIMGUI_IMPL_OPENGL_ES3`, which makes ImGui's OpenGL3
backend include `<GLES3/gl3.h>` instead of its desktop gl3w loader
(`libs/imgui/backends/imgui_impl_opengl3.cpp:168-174`).

`RendererGL.cpp` used a hardcoded `"#version 410"`. GLSL ES 3.00 is what an ES 3.0 context
compiles, so the version string is now chosen by the same define. Desktop still asks for 410 and
compiles clean (verified with g++ -fsyntax-only).

### 4.4 NativeFileDialog is absent on Android, and the editor's own picker takes over

`nfd` has no Android build. `editor/util/FileDialogs.h` already routed every picker through
`EditorFilePicker::ask()` before reaching NFD, so the header now:

- keeps one filter table (`FileDialogFilter`) shared by both paths,
- returns the editor picker's answer when it has one,
- and, under `__ANDROID__`, does not include `nfd.hpp` and returns empty instead of reaching a
  dialog that cannot exist.

`nfd` is not added as a subdirectory on Android and the link list takes
`${JASPERENGINE_EDITOR_FILE_DIALOG_LIB}`, empty there.

`editor/window/profiler/ProfilerWindow.cpp` called NFD directly in two places; both now go through
`FileDialogs`, which is what makes the profiler's save/load work on Android instead of failing to
compile.

### 4.5 Two platform files, not stubs

`editor/platform/ScreenColorAndroid.cpp` — the eyedropper reports "not supported" from every entry
point. Android cannot read another app's screen without a MediaProjection session, which is a
permission flow with a system dialog, not something an eyedropper button may trigger. The editor's
colour picker already handles this case (it is what a Wayland session gets).

`editor/platform/LaunchKeysAndroid.cpp` — `shiftHeld()` is false: there is no hardware keyboard to
poll before the first input event.

These replace the X11 implementations (`EditorToolsSources.cmake`), which is why the editor links
on Android at all.

### 4.6 Touch, not a mouse

`ImGui_ImplAndroid` is not used: it is a separate platform binding with its own event handling,
and the editor's `EditorFrame` expects to be driven by a backend that submits ImGui input itself.
`BackendAndroid.cpp` submits it directly:

- `io.AddMousePosEvent` / `AddMouseButtonEvent` from GameActivity's motion buffers. One finger owns
  ImGui's single pointer for the length of a gesture (`gActivePointerId`), so a second finger
  cannot drag a slider the first one grabbed.
- `io.AddKeyEvent` for the hardware/IME keys, with `AddInputCharacter` for Enter, Tab, Backspace
  and Space, which a keyboard reports as keys but a text field needs as characters.
- The soft keyboard follows `io.WantTextInput`, shown and hidden on change (not every frame, which
  would fight the IME's animation). Its text arrives on GameActivity's thread and is queued, then
  drained into ImGui's input queue on the frame thread, because ImGui's queue is not thread safe.

The text decoder is `jasperengine::android::decodeModifiedUtf8` from
`engine/platform/android/AndroidTextInput.h` — the same tested one the engine's own InputField
uses, instead of a second copy.

### 4.7 Scale

ImGui's style is authored for a ~96 dpi desktop at 1:1; a phone is ~420 dpi. `ANDROID_UI_SCALE =
2.6` is applied with `ImGui::GetStyle().ScaleAllSizes()` and `Theme::setUiScale()` before
`App::setup()`, which lays the dock out for it. ImGui's default frame height is ~20 px; at 2.6 it
is ~52 px, at or above Android's 48 dp touch target.

### 4.8 Lifecycle

`APP_CMD_TERM_WINDOW` destroys the EGL surface but keeps the context, everything ImGui holds and
everything the engine holds; `APP_CMD_INIT_WINDOW` builds a new surface on the same context. The
process is not restarted and no editor state is lost when the user switches apps.

## 5. What is still missing (honest list)

The vertical slice delivered is: **the editor opens, draws its real ImGui UI at touch scale, hosts
the engine, and its frame loop, input, dialogs and menus are wired.** Not yet done, in order of
value:

| # | Missing | Why it matters |
|---|---|---|
| 1 | Project storage on Android (SAF) | Projects live in the app's private directory today; importing from Downloads/Drive needs a SAF flow in Java |
| 2 | Touch re-layout of panels | The dock is scaled for a finger but still the desktop layout; a phone wants a compact mode |
| 3 | Verified on a physical device | Cannot be done from here: needs the user's phone |
| 4 | Export/build of a game from the phone | Needs an Android toolchain inside the APK; the desktop export shells out to compilers |
| 5 | Undo/redo, shortcuts adapted to touch | Menus exist; a touch-first command palette does not |

## 6. Verification performed here, and its limits

Verified in this sandbox (no Android SDK, no NDK, no JDK):

- Desktop sources still compile after every shared change: `editor/EditorFrame.cpp`,
  `editor/App.cpp`, `editor/Project.cpp`, `editor/window/profiler/ProfilerWindow.cpp`,
  `editor/backend/renderer/RendererGL.cpp`, `editor/platform/*Android.cpp` — all with
  `g++ -std=c++20 -fsyntax-only` and the project's include set.
- The workflow YAML parses and lists the expected steps.
- `BackendAndroid.cpp` is the only file that cannot be checked here: it includes
  `<game-activity/GameActivity.h>`, which ships with the Gradle dependency.

**Not verified, and must not be claimed as verified:** compiling `BackendAndroid.cpp`, linking
`libjasperengine-editor.so`, producing the APK's native library, and anything on a device.
