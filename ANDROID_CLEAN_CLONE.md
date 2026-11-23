# Running this branch's checks from a clean install

Everything below was run on a machine with nothing but a compiler: no CMake, no ninja, no Android
SDK, no NDK, no emulator, no network past the clone. The point is that a reviewer can reproduce the
whole branch from a fresh checkout in one command.

## The one command

```bash
git clone --depth 1 --branch arena/593187bf-jasperengine https://github.com/Yohanbit/JasperEngine.git
cd JasperEngine
python3 tools/android/verify.py
```

Needs `python3` (3.8+) and a C++20 compiler (`g++` or `clang++`, or `CXX=...`). Nothing else: the
googletest the suite links against is vendored under `libs/googletest`, and no step runs CMake.

`--quick` skips the 400,000-input fuzz run, which is 20 s instead of 90 s.

## What it does, and what each step proves

| Step | Proves | Time |
|---|---|---|
| the gtest suite, `engine/tests/core/platform/AndroidSubsystemsTest.cpp` | the 80 cases over the eight headers | ~15 s |
| `tools/android/selfcheck.cpp` | seven invariants, no framework | ~2 s |
| every header compiled alone **and twice**, `-Werror` | a missing include guard or a missing include is caught (a single include hides both) | ~10 s |
| `tools/android/integration/*.cpp` | the code `ANDROID_INTEGRATIONS.md` gives compiles and behaves as documented | ~5 s |
| the fuzz harness, ASan+UBSan | 400,000 hostile inputs through every parser: no out-of-bounds read, no impossible size | ~70 s |
| `tools/android/data-overflow-real.py` | the two `Data.cpp` defects, from the file's own bodies | ~3 s |

The last step is **reported, not counted**: `engine/core/io/Data.cpp` is the main agent's file, so the
run notes "both defects still reproduce" and still exits 0. Once the patch in `ANDROID_HANDOFF.md` §4
lands, the same step notes "no longer shows either defect".

## The real output, from a clean clone

```
$ git clone --depth 1 --branch arena/593187bf-jasperengine https://github.com/Yohanbit/JasperEngine.git repo
$ cd repo && python3 tools/android/verify.py
verify: /usr/bin/g++, from /tmp/clean/repo

=== the gtest suite (engine/tests/core/platform/AndroidSubsystemsTest.cpp)
  PASS suite compiles
  PASS [  PASSED  ] 80 tests.

=== the seven invariants (tools/android/selfcheck.cpp)
  PASS selfcheck: every invariant holds

=== every header compiles alone, and twice (the guard defect is invisible once)
  PASS AndroidAssetImport.h
  PASS AndroidImageLimits.h
  PASS AndroidImageOrientation.h
  PASS AndroidImportPreflight.h
  PASS AndroidImportTransfer.h
  PASS AndroidModelSidecars.h
  PASS AndroidProjectLayout.h
  PASS AndroidStorageSpace.h
  PASS AndroidTouchGestures.h

=== the three integration demos (tools/android/integration/) -- the code the doc gives
  PASS tap 2: tapped=1 doubleTapped=1 interval=200 ms
  PASS out of space: outcome=OutOfSpace "not enough space to finish the copy: free some space and try again, 3 MB are already there"
  PASS   folder assets     cache=0

=== the fuzz harness, 400,000 inputs under ASan+UBSan
  PASS ok: 400000 inputs, no out-of-bounds read and no impossible size

=== the core Data defects (engine/core/io/Data.cpp belongs to the main agent)
  NOTE both defects still reproduce in Data.cpp: the patch is in ANDROID_HANDOFF.md §4

verify: every check this branch owns passes
```

(The demos print their last line as the summary; their full output is in the run's own log and in
`ANDROID_INTEGRATIONS.md`, which quotes it.)

## By hand, if you prefer

The commands `verify.py` runs are ordinary ones, and each works on its own from the repository root.

The suite (the shape `ANDROID_TEST_MATRIX.md` records):

```bash
g++ -std=c++20 -O1 -Wall -Wextra -Werror -o /tmp/suite \
    engine/tests/core/platform/AndroidSubsystemsTest.cpp \
    engine/core/math/Vector2.cpp engine/core/math/Vector3.cpp engine/core/math/Vector4.cpp \
    libs/googletest/googletest/src/gtest-all.cc libs/googletest/googletest/src/gtest_main.cc \
    -I engine/platform/android -I engine/core -I engine/core/math -I engine/core/util \
    -I engine/core/asset -I engine/libs -I engine/libs/sokol -I engine/libs/json -I engine/libs/stb \
    -I engine/core/io -I libs/googletest/googletest/include -I libs/googletest/googletest \
    -lpthread
/tmp/suite              # [  PASSED  ] 80 tests.
```

The invariants:

```bash
g++ -std=c++20 -Wall -Wextra -Werror -o /tmp/selfcheck tools/android/selfcheck.cpp \
    -I engine/platform/android -I engine/core -I engine/libs/json -I engine/libs/sokol
/tmp/selfcheck          # selfcheck: every invariant holds
```

One header, twice (the pattern that catches a guard defect):

```bash
printf '#include "AndroidTouchGestures.h"\n#include "AndroidTouchGestures.h"\nint main(){}' > /tmp/twice.cpp
g++ -std=c++20 -Wall -Wextra -Werror -c -o /tmp/twice.o /tmp/twice.cpp \
    -I engine/platform/android -I engine/core -I engine/libs/sokol -I engine/libs
```

The `Data` defects, from the real file:

```bash
python3 tools/android/data-overflow-real.py; echo "exit $?"   # 1: defect present, 0: fixed
```

## What is *not* reproducible here, and why

- **An APK.** This machine has no `javac`, no Android SDK, no NDK and no emulator, so
  `tools/build.sh android` and the Gradle build cannot run. Anything about a phone running this code
  is unverified, which is the rule this branch follows everywhere.
- **`tools/check_headers.py --all`.** It wants `build/linux/editor` and ninja; only
  `--self-test` runs here, and it passes.
- **The CMake test targets.** `tools/run-sanitizer-tests.sh` and `tools/build.sh unit-tests` need a
  configured build directory. The suite above compiles the same source directly instead, which is
  what makes this work from a bare checkout.
- **The Java.** `ANDROID_INTEGRATIONS.md` §1 and §2 give Java that has never been compiled — it is
  written against the APIs `MainActivity` already uses, but a `javac` is needed to be sure.

## Two things a clean clone caught

Both were found by running this from `/tmp/clean` rather than in the working tree, and both are why
the script exists:

1. A path typo (`libs/googletest` vs `engine/libs/googletest`): googletest is under `libs/`, not
   `engine/libs/`. A working tree hides this because a stale build directory already has the object
   files.
2. `verify.py` reported `PASS` for a step that printed nothing: the selfcheck ran and exited 0, but
   the report line was only written for steps with an expected exit code, so an early version said
   nothing at all about it. A check that does not say what it saw cannot be audited; the fix prints
   the step's last line, which is what the output above shows.
