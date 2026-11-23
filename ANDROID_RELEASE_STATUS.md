# JasperEngine Android: release status

The single place that says which APK is which. **Only this document and `tools/android/releases.json`
name a release**; an APK built from any other branch is a test artifact and is never called a
release.

Last updated: 2026-11-23 (build `f68b39ea7` + release tooling)

## 1. The version ladder

| Name | What it is | Package | State |
| --- | --- | --- | --- |
| **V8** | The APK that was on the phone before this port: the game runtime of `engine/project` (the J logo). | `com.yourcompany.project` | **Kept, never replaced.** The baseline everything is compared against. Its commit was not recorded when it was built. |
| **Android RC1** | First integrated candidate: the editor APK (`com.jaspergameengine.editor`) built from the integrated branch, plus the game APK when the same run builds it. | `com.jaspergameengine.editor` | **Not published yet.** Blocked on a green build (see §3). |
| **Android RC2, RC3…** | A new candidate after each correction. A candidate never reuses a name and never replaces the previous one. | as above | — |
| **Android Stable 1.0** | The first officially stable release. Only declared after the acceptance criteria in §5 pass. | as above | Not reached. |

`versionCode` is monotonic and comes from `tools/android/releases.json`: a newer APK always installs
over an older one, and a candidate's number is above every published release, so a candidate can be
installed on a phone that already has a release.

## 2. Where an APK gets its identity

1. `tools/android/release_identity.py` reads `tools/android/releases.json` and the checkout's commit.
   A commit that is in the manifest keeps its published name; any other commit is named
   `candidate-<7 hex>` — there is no way for a build to borrow a release name.
2. `engine/workspaces/androidstudio/editor/build.gradle` puts that identity into
   `versionName` ("Android RC1 build 9"), into the generated `EditorBuildInfo.java`
   (`RELEASE_NAME`, `BUILD_NUMBER`, `COMMIT`, `DIRTY`), and into `assets/jasper-build.txt` inside the
   APK. `EditorActivity` exports it to the native side as `JASPERENGINE_ANDROID_RELEASE_*` and logs
   `starting <name> build #<n> (<commit>)` on every launch.
3. `tools/android/publish.py` reads those values back **out of the APK**, refuses to publish if they
   disagree with the checkout, computes the SHA-256, and appends the release to `releases.json`.

So the chain APK → commit → release name is checkable without trusting a report.

## 3. Current state

| Field | Value |
| --- | --- |
| Latest candidate | **none yet published** |
| Current stable | **none** (V8 is the reference, not a release of this port) |
| Branch | `arena/b99b8fb3-jasperengine` (this session's branch; branched from `arena/8286230a-jasperengine` at `d996af6e`) |
| Commit of the last verified fix | `f68b39ea7` — the editor APK's Java source set |
| CI lane | `.github/workflows/android.yml` (dispatch on this branch) |
| Download | none yet |

### Why there is no RC1 yet

The lane has been red since `d996af6e`:

| Run | Result | Cause |
| --- | --- | --- |
| 45094805556 | failed (step `APK`) | the game APK's Gradle build ran `:editor`'s configure first |
| 45096041001 | failed (step `APK`) | `EditorActivity.java:15/48/50/81 cannot find symbol` — see below |
| 45096599335 | **in progress** (verifies `f68b39ea7`) | — |

The error was `:editor`'s Java source set pointing at `<root>/platform/android/java`, which does not
exist (the file is at `engine/platform/android/java`, as `app/build.gradle` names it). One broken
module failed the whole Gradle invocation — `:app`, the game APK, included.

## 4. Tests: what has actually been run

Nothing in this table is inferred; each row was executed in this session or is a CI run whose step
status is recorded.

| Test | Where | Result |
| --- | --- | --- |
| `python3 -I tools/android/test_publish.py` (release tooling: binary-manifest reader, identity rules) | this sandbox | **7 tests, pass** |
| `tools/ci/lint.sh` checks (C++ subset, pollers, event clears, raw writes, stream keys, notices, MCP suite, the tooling's own tests) | this sandbox | pass, except `test_editor_install.py` (needs `zstd`, absent here) and `test_debug_info.py`/`test_copy_editor_engine.sh` (need `cmake`, absent; all three pass once `cmake` from PyPI is on `PATH`) |
| Workflow YAML parses (all 8 files) | this sandbox | pass |
| Android APK lane (game + editor) | GitHub Actions | **red through `d996af6e`**, fix in flight |
| Unit/integration suites (`tools/build.sh unit-tests`) | needs Docker | **not run here** |
| Install on a device | needs adb + the phone | **not run** |
| Editor opens on a device | needs the phone | **not run** |

**Nothing has been validated on a physical device from this session.** Any claim otherwise would be
false.

## 5. Acceptance criteria for Android Stable 1.0

Every row must be green on the published commit, and the device rows must say *how* they were run
(emulator or the user's phone).

| # | Criterion | State |
| --- | --- | --- |
| 1 | Reproducible build of both APKs from the integration branch | not yet (lane red) |
| 2 | APK installs (update over the previous build, same package) | not run |
| 3 | Starts past the J logo (boot report shows `first drawn frame`) | not run |
| 4 | Engine initialises (Lua, jobs, device tier) | not run |
| 5 | Editor's real UI appears (panels, docking, not a placeholder) | not run |
| 6 | Create, save and reopen a project | not run |
| 7 | Create, modify, save and reload a scene | not run |
| 8 | Import and use an asset | not run |
| 9 | Edit and run a Lua script | not run |
| 10 | Play a game in the editor and stop it | not run |
| 11 | Persistence across app close/reopen | not run |
| 12 | Lifecycle: background and foreground without losing state | not run |
| 13 | No crash in a long session | not run |
| 14 | Boot report written to `Downloads/jasperengine-boot.txt` | the file exists in the tree; **not re-verified after the integration** |

## 6. Known problems (not yet fixed)

Recorded in detail, with code evidence, in `ANDROID_INTEGRATION_STATUS.md` §4: A3-1 (no GameActivity
input filters installed → no mouse/trackpad), A3-3 (hardware-keyboard characters dropped), A3-2 (IME
editor type), A3-4 (stylus pressure), A3-6 (projects not in a user-visible directory).

## 7. What the user needs to do to move from RC to Stable

1. Install the RC APK from the GitHub release named in this file.
2. Open it; wait for the editor UI to appear.
3. Press Home (or switch apps) once, which writes `Downloads/jasperengine-boot.txt`.
4. Send that file back, and say whether the editor's panels appeared.

That report is what turns the "not run" rows above into evidence. Until then the release stays an RC.
