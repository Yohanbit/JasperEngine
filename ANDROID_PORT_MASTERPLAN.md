# Android port — master plan

Branch: `arena/8286230a-jasperengine` · PR: [#1](https://github.com/Yohanbit/JasperEngine/pull/1)
Last updated: 2026-11-23

## Goal

A JasperEngine editor that runs on Android from the phone alone: create and open projects, edit
scenes, press Play, save, and reopen without loss — with desktop parity wherever Android permits
it, and an honest record everywhere it does not.

## Where the work stands

The single most important correction is already made: **the V8 APK never hung on the J logo.**
It is the game runtime, and its whole game is one image (`engine/project/assets/jasperengine.png`,
413x260), drawn centred by `FITWIDTH`. See `ANDROID_PORT_AUDIT.md` for the evidence.

So the work was never "fix the hang"; it was "the editor was never built for Android at all".
`CMakeLists.txt` refused any `API_BACKEND` other than linux/win/mac, at configure time.

## Phases, with real state

| Phase | What it means | State |
|---|---|---|
| A | Audit the repository and the V8 | **done** — `ANDROID_PORT_AUDIT.md` |
| B | Desktop/Android parity map per subsystem | **started** — `ANDROID_PARITY_MATRIX.md` |
| C | Diagnose the V8 | **done** — not a hang; the tier bug is fixed (`479f72b9`) |
| D | Android architecture | **done** — `ANDROID_PORT_ARCHITECTURE.md` |
| E | Core port | **done for the editor's host model** — the engine builds as a shared library; the editor supplies the `System`, as on desktop |
| F | Graphics | **done in code** — EGL ES 3 context, `SOKOL_GLES3` + `IMGUI_IMPL_OPENGL_ES3`, GLSL `300 es`; **unverified on hardware** |
| G | Subsystems | **partial** — see the matrix; input, files, settings and dialogs adapted; audio/video/physics are the engine's own Android backends, already in the runtime |
| H | Editor port | **code complete for the vertical slice** — opens, draws the real UI at touch scale, hosts the engine, Play/save wired; **not yet seen on a device** |
| I | Dev tools | **blocked in part** — external compilers cannot run inside an APK; SAF import not written |
| J | Optimisation | not started (ccache + Gradle caches already in the workflow) |
| K | Validation | **local only**: desktop sources compile; nothing ran on Android |
| L | Independent audit | pending |
| M | Final fixes | pending |

## The next executable steps, in priority order

1. **Make the editor APK build.** Watch run `45095440780`; read compile errors from the run's
   `::error::` annotations (`gh api repos/Yohanbit/JasperEngine/check-runs/<id>/annotations`),
   which is the only error channel that works from here.
2. **Get it onto the phone.** Install `com.jaspergameengine.editor`, let it write
   `Downloads/jasperengine-boot.txt`, and read the report. Without this nothing can be called
   verified.
3. **SAF import.** A project that exists on the phone's storage must be openable. Java side:
   `ACTION_OPEN_DOCUMENT_TREE` → pass the tree URI to native → `ContentResolver` reads. Until
   this exists, projects live in the app's private `files/projects`.
4. **Touch re-layout.** The dock is scaled for a finger but is still the desktop arrangement.
   A compact mode for one-hand use is the difference between "works" and "usable".
5. **Build a game from the phone.** Needs reasoning about what an APK can host; the desktop
   export shells out to a full toolchain.

## Dependencies between pieces

- The editor cannot be validated before step 1 succeeds.
- Step 3 depends on a Java→native channel; `EditorPaths.java` is the existing precedent for
  exporting what only Java can ask for.
- Step 4 depends on step 2: the compact layout must be judged on a real screen, not guessed.

## Rules this work holds itself to

- Never claim a device test that did not happen here. This environment has no Android SDK, NDK,
  JDK, emulator or `/dev/kvm`.
- A desktop compile regression is not acceptable: every shared file changed for Android is
  re-checked with `g++ -std=c++20 -fsyntax-only`.
- No empty function may stand in for a feature: `ScreenColorAndroid.cpp` says "not supported"
  because Android genuinely cannot read another app's screen without a MediaProjection session,
  and it says so in its own comment.
