# Android integration: status (Agent 3)

Live document. Every row below is either **verified here** (a command was run and its result recorded)
or marked **unverified** with the reason. No row is marked done because a report says so.

Session branch: `arena/b99b8fb3-jasperengine` (this session's fixed branch, branched from
`arena/8286230a-jasperengine` at `d996af6e`). Base of the fork: `main` at `92295dbf`.

## 1. What exists in the fork today (audited, not assumed)

| Thing | State | How it was checked |
| --- | --- | --- |
| `main` | 1 commit (`92295dbf`, "Add GitHub Actions workflow for Android APK build") on top of upstream `Arakuma-Studio/JasperEngine` | `git log origin/main`, `git rev-list --count origin/main` = 1 (shallow clone), then `git fetch --unshallow` → 12 909 commits |
| Branches | `main`, `arena/8286230a-jasperengine`, this session's `arena/b99b8fb3-jasperengine` | `git ls-remote --heads origin` |
| Open PRs | **one**: #1 "Android: the editor port (backend, build, APK)", branch `arena/8286230a-jasperengine` | `gh pr list --state all` |
| Agent 1 / Agent 2 branches | **do not exist** in the fork: no branch other than the three above | `git ls-remote` |
| Agent 2's own docs (`ANDROID_PORT_MASTERPLAN.md`, `ANDROID_PARITY_MATRIX.md`, `ANDROID_PORT_STATUS.md`, `ANDROID_BUGS.md`, `ANDROID_TEST_MATRIX.md`, `ANDROID_AGENT2_STATUS.md`, `ANDROID_HANDOFF.md`) | **do not exist**: the mission's list is prospective | `find . -name 'ANDROID*'` |
| Docs that do exist | `ANDROID-AUDIT.md`, `ANDROID_ARCHITECTURE.md` | `find` above |
| APK V8 (the one on the user's phone) | `engine/project` built by `app/build.gradle`: the **game runtime**, not the editor. Its whole game is `image.setTexture("jasperengine.png")` | `engine/project/main.cpp`, `ANDROID-AUDIT.md` §1 |
| "Stuck on the J" | **not a hang**: that PNG is 413×260 and `Scaling::FITWIDTH` on 1080×2400 draws it at exactly the centre (y 860–1540) | `ANDROID-AUDIT.md` §1, re-derived from the code |
| Editor on Android | `API_BACKEND=android` exists in the fork (the root `CMakeLists.txt`), a new `editor/backend/BackendAndroid.cpp` (680 lines), its own Gradle module `engine/workspaces/androidstudio/editor`, package `com.jaspergameengine.editor` | `git show d996af6e --stat`, files read |
| Testing / build here | **no Docker, no JDK, no Android SDK/NDK, no adb, 2 CPUs, 3 GB RAM**. `gcc/g++ 12`, `python3 3.11`, `node 22` only | `command -v …` for each |

## 2. Where verification actually happens

GitHub Actions is the only compiler available to this session:

- `.github/workflows/android.yml` → `tools/build.sh android` (the game sample APK, `:app`) and then
  `:editor:assembleDebug` (the editor APK). Both run in `docker/android.Dockerfile` (JDK 21, SDK 36,
  NDK 28.2.13676358).
- Artifacts and job logs are served from `productionresultssa*.blob.core.windows.net` and
  `release-assets.githubusercontent.com`, **both unreachable from this sandbox** (connection closed at
  TLS). What *is* readable: the checks API, including the run's **annotations** and the **exit status of
  every step**. The android lane's "Annotate the build errors" step publishes the compiler's first ten
  errors per log as `::error::` annotations precisely for this case.

So each fix in this branch is confirmed by a fresh run whose step 8 (`APK`) and step 9
(`Editor APK`) end `success`, cross-checked with the annotations when they fail.

## 3. Defects found and fixed in this session

### 3.1 `:editor`'s Java source set pointed outside the tree (fixed, commit `f68b39ea7`)

`engine/workspaces/androidstudio/editor/build.gradle` added
`<root>/platform/android/java` as a Java source directory. There is no `platform/` at the
repository root; the boot report is at `engine/platform/android/java`, which is the path
`app/build.gradle:115` uses for the same file. javac therefore resolved no `JasperBootLog` and
`EditorActivity.java` failed on lines 15, 48, 50, 81 with `cannot find symbol` (run 45096041001,
annotations on check-run 137435945206).

Consequence beyond the editor: `./gradlew assembleDebug` configures **every** module of
`engine/workspaces/androidstudio` before it builds one, so this single compile error failed the
whole invocation — including `:app`, the game sample APK, which is the APK `tools/build.sh android`
produces and the one the user installs. Run 45096041001 shows it: step `APK` fails and step
`Editor APK` is skipped.

## 4. Defects found and *not* yet fixed (queued, with evidence)

These are real and located; they are listed here so the next session starts from a fact, not a
memory.

| # | Defect | Evidence | Owner of the area |
| --- | --- | --- | --- |
| A3-1 | **The editor backend never installs GameActivity's input filters.** GameActivity's glue ships `default_key_filter` and `default_motion_filter`; the default motion filter admits **only** `source == SOURCE_TOUCHSCREEN` (0x00001002). `NativeEngine.cpp:533-534` sets `app->motionEventFilter`/`app->keyEventFilter` directly (that is the engine's own host), but `BackendAndroid.cpp` never calls `android_app_set_motion_event_filter` / `android_app_set_key_event_filter`. On a tablet with a trackpad or a mouse the editor should expect no pointer events at all. | GameActivity API docs: "the default motion event filter will only allow touchscreen events through"; `engine/platform/android/NativeEngine.cpp:283-288,533-534` | `editor/backend/BackendAndroid.cpp` |
| A3-2 | **The editor's soft keyboard cannot be typed into on most keyboards.** `Backend::showSoftKeyboard` calls `GameActivity_setImeEditorInfo(…, InputType_dot_TYPE_CLASS_TEXT, …)` where `InputType_dot_TYPE_CLASS_TEXT` is a *local* `constexpr int = 1` copy of the engine's (`NativeEngine.cpp:544`). Android's `TYPE_CLASS_TEXT` is `0x00000001` but its real editor type is `TYPE_CLASS_TEXT | TYPE_TEXT_FLAG_*` — the safe call is `GameActivity_setImeEditorInfo(activity, GameActivity_getImeEditorInfo(...))` or at minimum `TYPE_CLASS_TEXT | TYPE_TEXT_VARIATION_NORMAL`; worth checking against the AGDK header before changing. | `editor/backend/BackendAndroid.cpp:333-345`; `engine/platform/android/NativeEngine.cpp:544-549` | `BackendAndroid.cpp` |
| A3-3 | **The editor's key path drops every character key.** `processKeyEvents` maps only DEL/FORWARD_DEL/ENTER/ESCAPE/TAB/SPACE/DPAD/HOME/END/PAGE (via `imGuiKeyFromAndroid`) and `continue`s for everything else, so `A`…`Z`, digits and punctuation produce **no** `AddInputCharacter` — a text field fed only by a hardware keyboard gets nothing (the soft keyboard's text path through `queueText` does work). | `editor/backend/BackendAndroid.cpp` `processKeyEvents` | `BackendAndroid.cpp` |
| A3-4 | **The eyedropper and TerrainEdit pressure read `Backend::penPressure()`, which returns −1 on Android** even though the engine's `NativeEngine.cpp` already has `stylusPressureOf()` reading `GameActivityPointerAxes_getToolType/getPressure`. The backend could feed it through as the engine does. | `editor/backend/BackendAndroid.cpp:649`, `editor/window/TerrainEditWindow.cpp:1392` | `BackendAndroid.cpp` |
| A3-5 | **Nothing calls `android_app_set_key_event_filter`/`motion_event_filter`** (same as A3-1) means the editor's Back handling also depends on the default key filter's `AKEYCODE_BACK` behaviour; the engine's own `key_filter` shows the intended semantics (Back only accepted when it comes from the soft keyboard, so the system gesture still leaves the app). | `NativeEngine.cpp:283-288` | `BackendAndroid.cpp` |
| A3-6 | **Settings/projects land in `filesDir/config` and `filesDir/projects` only.** `EditorPaths.java` exports `JASPERENGINE_ANDROID_CONFIG_DIR` and `JASPERENGINE_ANDROID_PROJECTS_DIR`; only the first is read (`editor/AppSettings.cpp:67`). The second has no reader, and `ProjectHubWindow::defaultLocation()` still resolves `$HOME/JasperEngine Projects` → `/JasperEngine Projects` on a phone. Projects therefore live where the user cannot see them from a file manager. | `grep -rn JASPERENGINE_ANDROID_PROJECTS_DIR` → only the Java writer; `editor/hub/ProjectHubWindow.cpp:54,76-80` | Agent 2's area (project storage) |

Every row is a *code* fact checked in this checkout; none is a claim about the phone.

## 5. Test infrastructure usable from this session

| Suite | Command | Works here? |
| --- | --- | --- |
| Repo lint (every source lint + tooling self-tests) | `tools/ci/lint.sh` | **yes**, with `cmake`+`ninja` from PyPI and `PYTHONPATH` set; `zstd` is missing so only `tools/ci/test_editor_install.py` fails for that reason |
| Workflow YAML validity | `python3 -c "import yaml; yaml.safe_load(...)"` per file | yes (pyyaml installed to `/home/user/toolchain/pylibs`) |
| C++ syntax of shared editor sources | `g++ -std=c++20 -fsyntax-only -I…` | yes (agent 1's method, reused) |
| Android APK (game + editor) | GitHub Actions `android.yml` | only through GitHub; artifacts not downloadable from here |
| Unit/integration tests of the engine (`tools/build.sh unit-tests`) | needs Docker | **no** |

Tools installed into this sandbox for verification (not committed): `cmake 4.4.5`, `ninja` and
`pyyaml` under `/home/user/toolchain/pylibs` (from PyPI; `dl.google.com` and
`release-assets.githubusercontent.com` are unreachable).

## 6. Next steps, in priority order

1. Confirm `f68b39ea7` turns the android lane green (steps 8 and 9 `success`) and inspect the
   editor APK's artifact presence (`jasperengine-editor.apk`).
2. Fix A3-1 (input filters) — without it the editor is touch-only, and the touch-only path is the
   one a phone needs, so it must be *explicitly* set rather than inherited by luck.
3. Fix A3-3 (character keys from a hardware keyboard) and A3-2 (IME editor type) together, as one
   change to the input path, then re-run the lane.
4. A3-6 (projects location + `JASPERENGINE_ANDROID_PROJECTS_DIR` reader) once the build is green;
   this is the difference between "an editor runs" and "a project can be found again".
5. Physical-device validation is **impossible from this session** (no adb, no device). The
   deliverable for the phone is the APK artifact plus `JasperBootLog` (already in the tree) that
   writes `Downloads/jasperengine-boot.txt` without a PC.
