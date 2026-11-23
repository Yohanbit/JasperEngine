# Tests, commands, results and evidence

Updated: 2026-11-23. Every row states what was actually run and where.

## What this environment can and cannot do

| Capability | Available? |
|---|---|
| Compile a single C++ translation unit (`g++ -std=c++20 -fsyntax-only`) | yes |
| Compile `BackendAndroid.cpp` (needs GameActivity/EGL/GLES headers) | **yes, with NDK-shaped stubs** — see below |
| CMake configure | no (`cmake` is not installed) |
| Ninja build, Gradle, JDK, Android SDK/NDK | no |
| Emulator, `/dev/kvm`, adb | no |
| GitHub Actions runs, their job steps and conclusions | yes |
| GitHub Actions run **logs** and **artifacts** | **no** — every download ends in `EOF` |

## The local compile harness

The include set and defines were recovered from `CMakeLists.txt` and are reproducible:

```bash
INC="$(cat /tmp/inc.txt)"; DEFS="$(cat /tmp/defs.txt)"
g++ -std=c++20 -fsyntax-only $DEFS $INC <file>
```

`/tmp/inc.txt` holds the engine's `-I` list (core/, core/ecs, core/render, libs/imgui,
libs/json, libs/sokol, engine/renders, editor/backend, ...). `/tmp/defs.txt` holds
`JASPERENGINE_EDITOR`, `JASPERENGINE_CXX_STANDARD_LIST=20`, `SOKOL_GLCORE`,
`IMGUI_ENABLE_TEST_ENGINE`, `JASPERENGINE_PROFILER=1` and the physics switches.

For the Android-only file, minimal headers stand in for what the NDK ships
(`/tmp/stub/{EGL,GLES3,android,game-activity}`), and the file compiles with
`-D__ANDROID__ -DSOKOL_GLES3 -DIMGUI_IMPL_OPENGL_ES3`. The stubs declare only what the file
names, so a signature mismatch is still caught — that is how two real defects were found
(`RendererPlatform::GLProc`, and `constexpr` shadowing GameActivity's `IME_*` macros).

## Results

| # | Target | Command | Result |
|---|---|---|---|
| 1 | `editor/EditorFrame.cpp` (desktop) | harness | **pass** |
| 2 | `editor/backend/renderer/RendererGL.cpp` (desktop glcore) | harness | **pass** — the GLSL-version change compiles for both backends |
| 3 | `editor/App.cpp` | harness | **pass** |
| 4 | `editor/Project.cpp` | harness | **pass** |
| 5 | `editor/window/profiler/ProfilerWindow.cpp` | harness | **pass** — its NFD calls became FileDialogs calls |
| 6 | `editor/AppSettings.cpp`, `editor/Platform.cpp` | harness | **pass** |
| 7 | `editor/platform/ScreenColorAndroid.cpp`, `LaunchKeysAndroid.cpp` | harness | **pass** |
| 8 | `editor/backend/BackendAndroid.cpp` | harness + stubs, `-D__ANDROID__` | **pass** (after fixing the two defects) |
| 9 | `.github/workflows/android.yml` | `yaml.safe_load`, step list | **pass** |
| 10 | editor APK build | GitHub Actions, dispatch | **not yet green** — see the run history below |
| 11 | behaviour on a device | — | **not run; impossible from here** |

## How a failing CI run is diagnosed from here

Run logs and artifacts cannot be downloaded. The working channels, in order:

1. **The PR comment the workflow posts on failure** (marker `android-build-errors`):
   `gh pr view 1 --json comments --jq '.comments[]|select(.body|contains("android-build-errors"))|.body'`
   — it carries the compiler's lines and the tail of each log, including the CMake configure error.
2. **Check-run annotations**: `gh api repos/Yohanbit/JasperEngine/check-runs/<id>/annotations`.
3. **The CI report comment** (`jasper-ci-report`) that `ci.yml` posts to the PR, which lists each
   lane's verdict.

## Run history of the editor APK

| Commit | What failed | Fix |
|---|---|---|
| `539623b1` | `jasperengineRoot` was two levels short, so CMake was pointed at a directory with no `CMakeLists.txt` | four levels |
| `9e30be4e` | Java: `cannot find symbol` (JasperBootLog not in the build) | `Sync` it in |
| `a3f3978d` | Gradle configuration failure; the log the error step looked for did not exist | absolute log path |
| `3f386121` | `pull-requests: write` missing, so the error comment could not be posted | permission added |
| `9427320c` | Gradle 9: Kotlin compile read `build/shared-java` without declaring the dependency | `dependsOn` for `KotlinCompile` too |
| `57d00e22` | *in progress* | |
