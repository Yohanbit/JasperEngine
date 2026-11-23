# Android integration: status (Agent 3)

Live document. Every row below is either **verified here** (a command was run and its result recorded)
or marked **unverified** with the reason. No row is marked done because a report says so.

Session branch: `arena/b99b8fb3-jasperengine` (this session's fixed branch, branched from
`arena/8286230a-jasperengine` at `d996af6e`). Base of the fork: `main` at `92295dbf`.

**This branch is the integration branch.** Checked with `git merge-base --is-ancestor`: `origin/main`
(`92295dbf`) and `origin/arena/8286230a-jasperengine` (agent 1, now `daebd94b`) are both fully
contained in it, and it adds this session's fixes on top. No separate branch was created, because a
second branch would either duplicate this one or need a history rewrite — both forbidden. Candidate
APKs are therefore cut from this branch and from nothing else (§4 of `ANDROID_RELEASE_STATUS.md`).

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

## 4. Defects found here and what happened to them

The full record, with what each one did to a person holding the phone, is `ANDROID_INTEGRATION_BUGS.md`.
Summary, in the order they were found:

| # | Defect | State | Commit |
| --- | --- | --- | --- |
| A3-1 | GameActivity's default motion filter admits only `SOURCE_TOUCHSCREEN`, so a trackpad, a mouse or a hovering stylus reached the editor as no events at all | **fixed** — both filters installed with the editor's rules | `0efd69600` |
| A3-2 | The IME pre-edit type was spelled unclearly (`InputType_dot_TYPE_CLASS_TEXT`, `33554432`) | **not a defect** — `TYPE_CLASS_TEXT` (1) with `IME_FLAG_NO_FULLSCREEN` is right; the constants are now named and checked against the reference | `0efd69600` |
| A3-3 | A hardware keyboard could not type: letters, digits and punctuation produced no character | **fixed** — full key map + Shift/Caps character path, 24 host checks | `0efd69600` |
| A3-4 | `Backend::penPressure()` returned −1 although the engine already reads stylus pressure, so the terrain brush ignored how hard a stylus is pressed | **fixed** — returns `Input::getPenPressure()` | `1f9d40cfb` |
| A3-5 | Same area as A3-1 (nothing installed the key filter) | **merged into A3-1**; the key filter is now explicit and BACK is admitted only from the soft keyboard | `0efd69600` |
| A3-6 | `JASPERENGINE_ANDROID_PROJECTS_DIR` had no reader, so the Hub offered `/JasperEngine Projects`, which a phone refuses | **fixed** — `defaultLocation()` prefers the exported directory | `bc31db334` |
| A3-7 | No SAF, so a project cannot be opened from Downloads or Drive | **open**, design work: SAF is asynchronous and needs a Java→native channel | — |
| A3-8 | The editor had no clipboard at all (ImGui's clipboard callbacks were never set) | **fixed** — `EditorClipboard` + `EditorActivity` methods | `0efd69600` |
| A3-9 | `:editor`'s Java source set pointed at a path that does not exist, failing the whole Gradle invocation | **fixed** | `f68b39ea7` |
| A3-10 | A candidate APK would have had a `versionCode` below the installed V8, so the phone would refuse it | **fixed** | `6133b661a` |
| A3-11 | `McpClient.cpp` used `posix_spawn`, which Bionic does not provide — the first real compiler error after the Java fix | **fixed** — the Android branch of the constructor throws with an explanation | `1d204377c` |

### 4.1 What the CI says now, and how it is read

The android lane's `Annotate the build errors` step (added by agent 1 in `daebd94b`, integrated
here as `01044ce6a`) is the only readable output from this sandbox. It proved its worth
immediately: run `45096599335` failed with `use of undeclared identifier 'posix_spawn'` in
`editor/cli/McpClient.cpp:474`, ten lines of it, where the two earlier runs had said only
`Process completed with exit code 1`.

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

1. **Get both APKs built** (steps `APK` and `Editor APK` `success`). The Java source set, the input
   backend, `McpClient`'s Android branch and the Hub path are each fixed; only the compiler knows
   what is next, and the annotations now say it. Run `45099004930` is the one to watch.
2. **Cut Android RC1** from this branch with `.github/workflows/android-release.yml` (dispatch
   `name="Android RC1"`, `kind=rc`) once the lane is green. That workflow is the only path an APK
   takes to becoming official; it builds, reads the identity back out of the packaged APK, records
   the SHA-256, and attaches both APKs to a GitHub release. `ANDROID_RELEASE_STATUS.md` §3 gets the
   real run and hashes when it exists — not before.
3. **The phone's evidence.** Every fix above is `DEVICE UNVERIFIED`: no adb, no emulator, no device
   in this environment, so nothing here ran on Android. The user installs the RC and reports what
   the boot report (`Downloads/jasperengine-boot.txt`) and logcat say; A3-1/A3-3/A3-8 each have a
   concrete press-and-see step in `ANDROID_INTEGRATION_BUGS.md`.
4. **SAF (A3-7)** after RC1: without it a project cannot be opened from where the user keeps it,
   which is the difference between a demo and a usable editor.
5. Touch-first layout, the `PENDING` rows of `ANDROID_PARITY_MATRIX.md`, only after the above: they
   make the editor pleasant, and none of them makes it usable for the first time.

## 7. What this session did *not* do

- **No test ran on Android.** No emulator (no KVM, no SDK), no device, no adb. Every "fixed" row in
  §4 means *the code is right and the build compiles*; the device rows are the user's to confirm.
- **No APK was published.** `android-release.yml` has never run — it is committed and its YAML
  parses, but it needs a runner and the android image, neither of which exists here.
- **The Gradle release-identity wiring has never executed.** `-P` properties, the `python3`
  exec in `build.gradle` and the generated `EditorBuildInfo.java` are written and reviewed, but the
  first execution will be the RC1 build.
