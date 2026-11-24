# Android port — the plan, and the checker that keeps it honest

Written after the fourth device session (build `2a7a68c1`), from its four boot reports. Everything
below is either **measured** (a number or a line that came off the phone) or **read** (a file and a
line in this repository). Nothing is guessed, and the column says which.

Previous sessions are in [EDITOR_PORT_LOG.md](EDITOR_PORT_LOG.md): the twelve defects the compiler
found, then the two the phone found. This file is forward-looking: what is wrong now, in what order
to fix it, and how to prove a fix did not break what already works.

---

## 1. What the fourth session measured

| Reading | Value | Where it comes from |
|---|---|---|
| the bars are gone | `status bar visible: false`, `navigation bar visible: false` | `editor-boot (4).txt` and `(7).txt`, `== the window ==` |
| the engine's own logs arrive | `I/JasperEngine: Scene 'Main' started`, `Shader ... generated successfully` ×10 | `editor-boot (5).txt` |
| **first drawn frame: 48 873 ms** | 48.9 s from process start to the first picture | `(5).txt`: `[start] first drawn frame: 48873 ms after the start` |
| the main thread stalled 3.3 s during load | `The editor was not responding for 3.3 s. Details saved.` | `(5).txt`; the report is on the phone under `config/reports/.../hang-001.txt` |
| the editor starts drawing at ~18:08:59 and is still hitching at 18:09:16 | `GED queueBuffer: fence N didn't signal in 1000 ms` about once a second, then `ged_boost_gpu_freq` bursts | `(6).txt`, `(7).txt` |
| Play's data folder cannot be created | `Failed to create the Play mode data directory: Read-only file system` ×2 | `(5).txt`, `Project.cpp:9863` |
| audio is up | `Audio output stage: linear`, AAudio 44100/2ch | `(5).txt` |
| audio gets **no data** | `AudioTrack: isLongTimeZeroData ... 1..13 Seconds` | `(7).txt` — 13 consecutive seconds of silence |
| a target named `/system/bin/engine` does not exist | `engine folder not found at: /system/bin/engine` | `(5).txt`, `Project.cpp:6761` |
| the assets crash is **not in these files** | no `SIGSEGV`, no `FATAL`, no `tombstone`, no `libc` line | all four logs |
| window size is logical, not physical | `decor: 2400x1080` while the panel is 1080x2400 | the two numbers, and `getWidth()` on a decor view is in pixels |

The assets crash is absent for a reason that is worth writing down: `EditorBootLog` writes on a
20 s and a 60 s timer and in `onStop`, and a native crash kills the process before any of the three.
`SIGSEGV` is not an exception, so the Java `UncaughtExceptionHandler` never runs either. `CrashHandler`
exists in this repository (`editor/util/CrashHandler.h`, installed on desktop through
`EditorDiagnostics`) and **is not installed in the Android backend**: `App.cpp:148` includes it and
no platform file calls it. Until that is fixed every crash on this phone is invisible, which is
exactly the blindness this plan exists to end.

---

## 2. The four defects, with cause and fix

### D1 — the editor takes 49 s to show its first frame

**Measured**: `[start] first drawn frame: 48873 ms after the start`.

**Read**: `App::setup()` runs before the first frame and does the work that shows up as a stall: the
hub's template sweep, the shader compile that the log shows one by one (10 shaders, from 15:04:07.9
to 15:04:15.2 — 7.3 s of it in plain sight), the Lua stubs and engine-API copy that
`Project::copyEngineApiToProject` writes into the project (`Project.cpp:6746`), and the asset
database. On a phone each is several times a desktop's cost, and they are sequential.

**Fix, in this order** (cheapest first, and each measurable on its own):
1. The engine-API copy and the Lua stub writing run *after* the first frame, not before it. They are
   files nothing draws.
2. The hub's template sweep already runs off the main thread (`projectHub.pollSweep()`); make the
   *first* frame not wait for it.
3. Ask the driver to compile shaders in parallel at start-up
   (`glProgramBinary` cache is already wired through `App::getUserCacheBaseDir()` — defect #10 in
   the port log — so the second start should already be faster; the logs are all first starts of a
   fresh install, which is why 49 s is the number every time).
4. Only then look at the asset database, with a timer around each phase and a log line per phase.

**Accepted when** `[start] first drawn frame:` reports under 10 000 ms on the same phone, and the
hub answers a tap within one second of it. The log line already exists, so this needs no new
instrumentation to be judged.

### D2 — the assets screen crashes the editor

**Measured**: not present in these logs (see §1); the report is the user's word — opening assets
crashes.

**Read**: what an asset screen does that the hub does not: it scans the project's folders, decodes
textures with `stb_image`, and builds previews. Three things on this port are known to be thin here
and each is a crash of its own kind:
- the asset scan walks a tree whose root came from `homeDirectory()`; a project opened before defect
  #9 was fixed can still have records pointing at `/`, and scanning `/` from an app is a permissions
  walk that ends in a dead end;
- texture decode allocates the full RGBA of the image; a photo from the phone's camera is 12 MP =
  48 MB in one allocation, and Android kills an app over its heap before `new` returns;
- the previews are drawn outside `ImGui::Image`'s clamp.

**Fix**: install the crash handler first (below), then reproduce with the phone in hand and a
`crash.txt` on disk. **Do not touch the asset code before a stack trace exists** — that is how the
last three regressions were made.

### D3 — Play's data folder is refused as "Read-only file system"

**Measured**: `Failed to create the Play mode data directory: Read-only file system`, twice, at the
moment the new project is loaded.

**Read**: `Project::getUserDataPath()` (`Project.cpp:9856-9866`) opens a `safewrite::EditorInternal`
on `.jasperengine/user/rundata` and then calls `FileUtils::createDirectories`. Under a strict write
scope `FileUtils` routes to `safewrite::scopedCreateDirectories` (`SafeWrite.cpp:1898`), which for a
folder that does not exist yet asks `protectedPlaceRefusal(path, false)`. That function refuses any
path *below* a trusted root whose first component is hidden — `.jasperengine` — unless
`editorInternalCovers()` already covers it. The token is alive, so it should cover it; the message
that reached the user is not this one either (this one would name the path and say "hidden"), which
makes it the plain-`std::filesystem` path: something in that chain is not what it looks like, and the
error text is `EROFS`'s.

**Measured, not guessed.** On this machine, `std::filesystem::create_directories` on a fresh,
non-existent path under a writable tree *succeeds* (`ec = Success`), and the two failures a path
under the app's own `files/` can actually produce are `ENOENT` ("No such file or directory") and
`ENOTDIR` ("Not a directory"). "Read-only file system" is `EROFS`, errno **30**, and it comes from a
**read-only mount**. Android's `/data/user/0/<pkg>` is not one.

So the path in that call is not the one `getUserDataPath()` computes from the project directory. It
is the other return value of the same function — `getAssetsPath()` — or a project whose
`projectPath` is not where the files went. Which one is settled by a single line of evidence that
does not exist yet:

```
Out::error("Play data dir: %s (project %s): %s",
           path.string().c_str(), projectPath.string().c_str(), ec.message().c_str());
```

Until that line is on the phone, D3 is a defect with a **known error code and two candidate paths**,
and the next step is to print the path, not to edit the function. It is Play mode only — the editor
itself saves fine — so it ranks below the crash.

### D4 — the phone is silent, and the GPU is waiting

**Measured**: `AudioTrack: isLongTimeZeroData ... 13 Seconds` while `Audio output stage: linear` is
up; `GED queueBuffer: fence N didn't signal in 1000 ms` about once a second.

The audio one is a real defect: a stream that is opened, started, and never fed. The AAudio backend
needs a callback that pulls from the mixer, and on this port it is pulling nothing.

The `GED`/`ged_boost_gpu_freq` bursts are Xiaomi's GPU driver complaining that it is asked for frames
faster than it can retire them — the renderer presents whenever the editor asks for a redraw, and the
redraw request is per-tap, so the phone is likely drawing at a fixed high rate with nothing changing.
**Not a defect until the frame pacing is measured**, which `FrameTimes` already collects and the
profiler overlay already draws. Judged in D1's fix, not separately.

---

## 3. Order of work

Each step is one build, and each ends with a number or a file that says whether it worked. A step
that cannot be judged from a log line is not allowed to start.

| # | Step | Judged by |
|---|---|---|
| 0 | **The crash handler on Android** — install `diag::CrashHandler` in the backend, writing to `files/config/reports/`, and make `EditorBootLog` say where it is at start | a deliberate `--debug-crash` produces a `crash.txt` the user can share |
| 1 | **D3**, with the routing log line first | the line, then no more `Read-only file system` |
| 2 | **D1**: engine-API copy and Lua stubs after the first frame | `first drawn frame:` under 10 s |
| 3 | **D2**, reproduced with step 0 in place | a stack trace, then a fix judged by the trace |
| 4 | **D4** audio | the silence line stops appearing; sound is heard |
| 5 | The rest of the port (below) | the checker |

---

## 4. What "ported completely" means, and how far along it is

The editor is ~330 000 lines and every panel is a candidate for a phone-only failure. "Completely"
is therefore defined as: **every window opens, every menu item does something, no subsystem refuses
in a way the user can trigger.** The state, from the port log and this session:

| Subsystem | State | Evidence |
|---|---|---|
| window, input, keyboard, text | **works** | taps, drags, backspace, typing all exercised on the phone |
| the hub: new / open / import / templates | new project **works**; import from storage **not ported** (needs a Java intent, no path) | log: `New project copied ... loading it` |
| scene view, play mode | opens and runs, **Play's data folder refused** (D3), **silent** (D4) | `Scene 'Main' started`, silence lines |
| shaders | **works**, cached from the second start | 10 × `Shader ... generated successfully` |
| assets | **crashes** (D2) | user report |
| export: building a game on the phone | **not ported, and cannot be**: `CommandRunner`/`ProcessExec` refuse to `exec` on Android, correctly | port log, "What is still open" |
| VCS, shell tools, LSP | refuse correctly, do not crash | `#3` in the port log |
| audio | opens, silent (D4) | AAudio lines |
| files, settings, console, profiler | not exercised on the phone yet | — |

The honest reading: **the editor is usable and the parts that are unported are the parts that need a
desktop.** What is left is not "port more code", it is "make what is there stop crashing and stop
being slow" — which is D1-D4.

---

## 5. The reverifier

The problem to design against is the one that already happened twice: a change that fixed one thing
and silently broke another (the inset that ate the taps; the IME call that emptied the field). A
green build cannot see either. So the check has three layers, and the third is the one that matters.

### Layer 1 — the compiler (`tools/android_precheck.sh`)

Runs before every push, in seconds, on any machine: the brace/parenthesis/`#if` balance of the files
the diff touches, the Android stubs' syntax (`g++ -fsyntax-only -std=c++20 -D__ANDROID__ -I
/tmp/stub`), and **the Java symbols** — a list of the `androidx`/`android` names the backend uses,
each with the class it must be qualified by, so `WindowInsetsCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE`
(a real four-minute build failure) is caught in a second. It cannot replace the build; it catches the
class of mistake that costs a build.

### Layer 2 — the interaction contract (`docs/android/INTERACTION_CONTRACT.md`)

A numbered list of the things a human does on the phone, each with what the log must show. It exists
because "it still works" is not a test. It ships as a **checklist the user runs in one pass** after
every APK, and it is the same list the code is reviewed against:

| # | Do this | the log must show | not |
|---|---|---|---|
| 1 | open the app | `status bar visible: false` | — |
| 2 | new project | `New project: '<name>' into <path>` then `Project loaded successfully` | `ignored: ...` |
| 3 | type a name, backspace it | the field shrinks by one per press | nothing |
| 4 | drag a panel by its title | the panel moves, and keeps up with the finger | it jumps, or nothing moves |
| 5 | open the assets screen | the assets list, no `crash.txt` | the app closing |
| 6 | press Play | `Scene '<name>' started`, sound | silence |
| 7 | press Stop | the scene unloads, the editor still answers | a freeze |

Any line that changes shape is a regression, and the contract is where the change is declared.

### Layer 3 — the self-check in the app

The one thing that makes the difference between "I think this is fixed" and "the phone says so": an
**Android self-test** the editor can run from the hub — `Help > Run the Android checks` — which
performs, in order, the seven rows above that can be done without a human (open the assets folder,
list it, decode one texture, create a project, write Play's data folder, start and stop the audio
stream, take a frame-time sample) and **writes one line per check to logcat and to the boot report**:

```
[selftest] assets: 412 files, 3 textures decoded, ok
[selftest] play data dir: /data/.../rundata, created, ok
[selftest] audio: 44100 Hz, 2048 frames pulled, ok
[selftest] frame: median 16.7 ms, p99 33.4 ms, ok
[selftest] RESULT: 4 of 4 ok
```

That is the reverifier: a run whose report is a set of pass/fail lines about the things that broke
before, produced by the phone, read from the same boot report that already works. It also turns the
user's feedback loop from "it crashed" into "line 3 says `play data dir: FAILED, Read-only`, here is
the log" — which is what makes the next four sessions cheap instead of expensive.

The self-test is the **last** step, not the first: it is only worth writing once D1-D4 are fixed,
because a test that encodes today's bugs is worse than no test.

---

## 6. What the next APK must be, so the user can test it

- one build, arm64-v8a, debug (so `EditorBootLog` is active), with steps 0-2 of §3 in it;
- the report must contain, besides today's sections: the crash handler's path, the Play data folder
  routing line, the first-frame timing broken into phases;
- the same checklist (§5 layer 2) in the release notes, so one test pass covers everything.
