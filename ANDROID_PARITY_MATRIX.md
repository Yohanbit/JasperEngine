# Desktop ↔ Android parity matrix

Every row was established by reading the code, not by assumption. Where a claim depends on
something that could not be run here (an APK, a device), the row says so.

Legend: **PORTED** (implemented, and verified in this environment as far as possible) ·
**PORTED, UNVERIFIED** (implemented, needs a device) · **PARTIAL** · **PENDING** ·
**BLOCKED** (Android genuinely forbids it).

## Engine runtime

| Subsystem | Desktop | Android | State |
|---|---|---|---|
| Renderer | Sokol, GL core 4.1 / Vulkan / Metal | Sokol GLES3 (`SOKOL_GLES3`) | PORTED — already the runtime's path, verified in the V8's boot log (GLES 3.2, Mali-G76) |
| Context / presentation | GLX, WGL, Metal | EGL ES 3, `eglSwapBuffers` | PORTED — `BackendAndroid.cpp`; the runtime's own EGL path is the existing, working precedent |
| Audio | miniaudio (WASAPI/ALSA) | miniaudio (AAudio/OpenSL ES) | PORTED — in the engine's Android build already |
| Video | Media Foundation / FFmpeg | MediaCodec (`MediaCodecBackend.cpp`) | PORTED — engine's own Android backend |
| Physics 2D/3D | Box2D / Jolt | Box2D / Jolt | PORTED — no platform dependency |
| Scripting (Lua) | Lua 5.4 + luabridge3 | same | PORTED — no platform dependency |
| Scripting (C++) | compiles with the host toolchain | cannot compile an APK at runtime | BLOCKED — see "Tooling" |
| HTTP | WinHTTP / libcurl | `HttpBackendAndroid.cpp` | PORTED — engine's own Android backend |
| Native bridge (JNI) | — | `NativePlatformAndroid.cpp` | PORTED — `JNI_OnLoad` present, verified in boot log |
| Scenes, ECS, components | — | — | PORTED — pure C++, no platform layer |
| Serialization / save | filesystem | filesystem | PORTED — `std::filesystem` works; paths are project-relative |
| Device tiers | detection + `configure()` | same | PORTED — the sample project's missing `configure()` is fixed (`479f72b9`) |

## Editor

| Piece | Desktop | Android | State |
|---|---|---|---|
| Frame loop | `EditorFrame.cpp` | same file, unchanged | PORTED |
| ImGui renderer | `RendererGL.cpp` (GLSL 410) | same file (GLSL 300 es) | PORTED, UNVERIFIED |
| Platform backend | X11 / Win32 / AppKit (~3.2k lines each) | `BackendAndroid.cpp` (~670 lines) | PORTED, UNVERIFIED |
| Input | mouse, keyboard, gamepad | touch (one-finger pointer), IME keys, soft keyboard | PORTED, UNVERIFIED |
| Multi-viewport (detachable windows) | yes | no — one OS window | BLOCKED by Android's window model; ImGui is told not to offer it |
| Clipboard | X11/Win32/Cocoa | — | PENDING (JNI `ClipboardManager`) |
| File dialogs | NativeFileDialog | the editor's own picker (`EditorFilePicker`) | PARTIAL — no SAF tree access yet |
| Project storage | any directory | app-private `files/projects` | PARTIAL — needs SAF to reach Downloads/Drive |
| Eyedropper screen reads | X11/Win32/Cocoa | none | BLOCKED — needs a MediaProjection permission flow |
| Shift-at-launch opens the Hub | yes | no (no Shift before the first event) | BLOCKED, with the Hub still reachable from the menu |
| Menu bar | macOS native, ImGui elsewhere | ImGui fallback | PORTED |
| Profiler save/load | NFD | editor picker | PORTED (compiles; the NFD calls were the blocker) |
| Console, Inspector, Hierarchy, Project panel | ImGui | ImGui, scaled ×2.6 | PORTED, UNVERIFIED — layout is still the desktop dock |
| Touch-first compact layout | n/a | not written | PENDING |
| Export / build a game | shells out to compilers | cannot exec | BLOCKED in its current design |

## Tooling

| Tool | Desktop | Android | Why |
|---|---|---|---|
| Open a project folder | any path | app storage only | SAF is asynchronous and answered by an activity; needs a Java→native channel |
| Compile C++ scripts | host toolchain via `launchDetached` | no exec | Android forbids an app from executing anything it did not ship (`Platform::launchDetached` now says so instead of pretending) |
| Export an APK | Gradle + NDK on the host | would need a toolchain inside the APK | BLOCKED in its current design |
| Aseprite integration | launches the binary | — | BLOCKED, same exec restriction |
| Crash diagnostics | files next to the editor | `Downloads/jasperengine-boot.txt` | PORTED — beside the runtime, debug builds only |

## What this table does not claim

Nothing in the "editor" section has run on Android. It compiles as far as this environment can
check, which for `BackendAndroid.cpp` is not at all (it needs GameActivity's headers). The first
real evidence will be the APK's own boot report on the user's phone.
