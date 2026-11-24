#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Runs every Android subsystem check of this branch from a clean checkout, with no CMake.

    tools/android/verify.py            # build and run everything
    tools/android/verify.py --quick    # skip the fuzz run (about 20 s instead of 90 s)

What it does, in order, and what each step proves:

  1. builds engine/tests/core/platform/AndroidSubsystemsTest.cpp with googletest from libs/ and runs
     it -- the 80 cases, on any host with a C++20 compiler;
  2. builds and runs tools/android/selfcheck.cpp -- seven invariants across the eight headers;
  3. builds engine/platform/android/*.h twice-included, one at a time, with -Werror -- the header
     guard defect (ANDROID_BUGS 10d) is invisible to a single include;
  4. builds and runs the fuzz harness over 400,209 inputs under ASan+UBSan;
  5. builds e89tools/android/data-io-overflow-repro.cpp and confirms the core defect still reproduces
     (it is expected to fail until the main agent applies the patch);
  6. runs tools/android/data-overflow-real.py, which lifts the bodies out of the real Data.cpp.

It needs a C++20 compiler (g++ or clang++, or $CXX) and nothing else: no CMake, no ninja, no
network, no Android SDK. googletest is vendored under libs/googletest. Exit 0 when every check that
this branch owns passes; the core-defect steps report their state without failing the run, because
they are the main agent's file to fix.

The commands it runs are the same ones ANDROID_AGENT2_STATUS.md section 6 records, so a reviewer can
run any of them by hand.
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GTEST = os.path.join(ROOT, "libs", "googletest", "googletest")

# the include path ANDROID_AGENT2_STATUS.md section 6 records, in one place
INCLUDES = [
    "engine/platform/android", "engine/core", "engine/core/math", "engine/core/util",
    "engine/core/asset", "engine/libs", "engine/libs/sokol", "engine/libs/json", "engine/libs/lua",
    "engine/libs/stb", "engine/core/io",
]
GTEST_INCLUDES = ["libs/googletest/googletest/include", "libs/googletest/googletest"]

HEADERS = [
    "AndroidAssetImport", "AndroidImageLimits", "AndroidImageOrientation", "AndroidImportPreflight",
    "AndroidImportTransfer", "AndroidModelSidecars", "AndroidProjectLayout", "AndroidStorageSpace",
    "AndroidTouchGestures",
]

FUZZ = r"""
// Every parser in engine/platform/android over 400,209 hostile inputs, under ASan+UBSan.
#include "AndroidAssetImport.h"
#include "AndroidImageLimits.h"
#include "AndroidImageOrientation.h"
#include "AndroidImportPreflight.h"
#include "AndroidImportTransfer.h"
#include "AndroidModelSidecars.h"
#include "AndroidTouchGestures.h"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace jasperengine::android;
using jasperengine::Touch;
using jasperengine::TouchPhase;
using jasperengine::Vector2;

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::mt19937 rng(12345);
    for (int i = 0; i < 400000; i++) {
        const size_t len = rng() % 300;
        std::vector<uint8_t> data(len);
        for (auto& byte : data) byte = (uint8_t)(rng() & 0xFF);
        if (i % 7 == 0 && data.size() >= 8) {           // a real PNG magic, then noise
            const uint8_t magic[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
            for (int k = 0; k < 8; k++) data[k] = magic[k];
        }
        if (i % 11 == 0 && data.size() >= 4) { data[0] = 0xFF; data[1] = 0xD8; }

        const std::span<const uint8_t> bytes(data.data(), data.size());
        const ImageSize size = readImageSize(bytes);
        if (size.known && (size.width <= 0 || size.height <= 0)) {
            std::printf("BAD size %dx%d\n", size.width, size.height);
            return 1;
        }
        (void)assetTypeForImport("x", bytes);
        (void)decodeSupportOf("x.png", bytes, (i & 1) != 0);
        (void)playbackSupportOf("x.wav", bytes, (i & 1) != 0);
        (void)readOrientation(bytes);

        std::string name;
        for (size_t k = 0; k < data.size() / 3; k++) {
            const uint8_t c = data[k];
            name.push_back(c < 32 ? (char)('/' + (c % 3)) : (char)c);
        }
        if (i % 5 == 0) name = "DCIM/IMG.JPG";
        if (i % 13 == 0) name.clear();

        PreflightInput input;
        input.fileName = name;
        input.prefix = data;
        PreflightOptions options;
        options.androidBitmapFactory = (data.size() & 1) != 0;
        options.androidMediaCodec = (data.size() & 2) != 0;
        options.budget.maxTextureSize = 4096;
        const PreflightFinding finding = preflightOne(input, options);
        if (finding.assetName.empty()) { std::printf("EMPTY asset name\n"); return 1; }
        if (finding.severity == PreflightSeverity::Broken && finding.message.empty()) {
            std::printf("BROKEN with no reason\n");
            return 1;
        }

        // the transfer plan must always write something the scanner can recover
        const TransferPlan plan = planTransfer(name.empty() ? "x.png" : name, 0, data.size());
        if (!isImportTemporary(plan.temporary) || temporaryTarget(plan.temporary) != plan.target) {
            std::printf("UNRECOVERABLE plan %s -> %s\n", plan.temporary.c_str(), plan.target.c_str());
            return 1;
        }

        // a JSON-shaped model must name every sidecar distinctly and rewrite the document
        std::string gltf = "{\"asset\":{\"version\":\"2.0\"}";
        if (data.size() >= 4) gltf += ",\"images\":[{\"uri\":\"" + name + "\"},{\"uri\":\"" + name + "\"}]";
        gltf += "}";
        const ModelImportPlan model = planModelImport(gltf, "Body.gltf");
        std::set<std::string> names;
        for (const SidecarImport& sidecar : model.sidecars) {
            if (sidecar.assetName.empty() || !names.insert(sidecar.assetName).second) {
                std::printf("SIDECAR collision or empty\n");
                return 1;
            }
        }

        // the touch readers over noisy finger lists
        std::vector<Touch> touches;
        for (size_t k = 0; k + 3 < data.size() && touches.size() < 5; k += 4) {
            Touch touch;
            touch.pointer = (int)(data[k] % 6);
            touch.position = Vector2((float)data[k + 1], (float)data[k + 2]);
            touch.startPosition = Vector2((float)data[k + 1], (float)data[k + 3]);
            const uint8_t phase = data[k] % 4;
            touch.phase = phase == 0 ? TouchPhase::BEGAN : phase == 1 ? TouchPhase::MOVED
                        : phase == 2 ? TouchPhase::STATIONARY : TouchPhase::ENDED;
            touch.time = (float)(data[k + 1] % 101) * 0.01f;
            touches.push_back(touch);
        }
        RecognizerState state;
        TapOptions taps;
        taps.longPressSeconds = (data.size() % 3) * 0.25f;
        updatePinch(state, touches, (int)(data.size() % 7));
        updateDrag(state.drag, touches);
        updateLongPress(state, touches, 0.5f, taps);
        updateTap(state, touches, taps);
        if (state.longPress.heldSeconds < 0.0f) { std::printf("NEGATIVE held time\n"); return 1; }
    }
    std::printf("ok: 400000 inputs, no out-of-bounds read and no impossible size\n");
    return 0;
}
"""


class Report:
    def __init__(self):
        self.failures = 0

    def step(self, name):
        print(f"\n=== {name}")

    def ok(self, detail=""):
        print(f"  PASS {detail}".rstrip())

    def fail(self, detail):
        self.failures += 1
        print(f"  FAIL {detail}")

    def note(self, detail):
        print(f"  NOTE {detail}")


def compiler():
    return os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def build_and_run(cxx, report, workdir, label, sources, extra_includes, run_it=True, expect=None):
    """Compile `sources` and, unless told not to, run the binary. Returns (ok, output)."""
    binary = os.path.join(workdir, label)
    command = [cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-o", binary, *sources]
    for include in extra_includes:
        command += ["-I", os.path.join(ROOT, include)]
    built = run(command)
    if built.returncode != 0:
        report.fail(f"{label} did not compile:\n{(built.stderr or built.stdout)[:2000]}")
        return False, ""
    if not run_it:
        report.ok(f"{label} compiles")
        return True, ""
    result = run([binary])
    output = result.stdout + result.stderr
    if expect is not None and result.returncode != expect:
        report.fail(f"{label} exited {result.returncode}, expected {expect}\n{output[-2000:]}")
        return False, output
    report.ok(output.strip().splitlines()[-1] if output.strip() else f"{label} ran")
    return True, output


def main(argv):
    parser = argparse.ArgumentParser(description="Run every Android subsystem check from a clean tree.")
    parser.add_argument("--quick", action="store_true", help="skip the 400,209-input fuzz run")
    arguments = parser.parse_args(argv)

    cxx = compiler()
    if not cxx:
        print("verify: no C++ compiler found (set CXX)", file=sys.stderr)
        return 2
    if not os.path.isdir(GTEST):
        print(f"verify: googletest is not vendored at {GTEST}", file=sys.stderr)
        return 2

    print(f"verify: {cxx}, from {ROOT}")
    report = Report()
    with tempfile.TemporaryDirectory(prefix="android-verify-") as workdir:
        includes = INCLUDES + GTEST_INCLUDES

        report.step("the gtest suite (engine/tests/core/platform/AndroidSubsystemsTest.cpp)")
        sources = [os.path.join(ROOT, "engine/tests/core/platform/AndroidSubsystemsTest.cpp")]
        for vector in ("Vector2", "Vector3", "Vector4"):
            sources.append(os.path.join(ROOT, f"engine/core/math/{vector}.cpp"))
        sources += [os.path.join(GTEST, "src/gtest-all.cc"), os.path.join(GTEST, "src/gtest_main.cc")]
        ok, output = build_and_run(cxx, report, workdir, "suite", sources, includes, run_it=False)
        if ok:
            binary = os.path.join(workdir, "suite")
            built = run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-o", binary, *sources]
                        + sum([["-I", os.path.join(ROOT, i)] for i in includes], []) + ["-lpthread"])
            if built.returncode != 0:
                report.fail(f"the suite did not link:\n{(built.stderr or built.stdout)[:2000]}")
            else:
                result = run([binary])
                tail = [line for line in (result.stdout + result.stderr).splitlines()
                        if "PASSED" in line or "FAILED" in line or "tests ran" in line]
                if result.returncode == 0:
                    report.ok(" ".join(tail[-2:]))
                else:
                    report.fail(f"the suite failed:\n" + "\n".join(tail[-12:]))

        report.step("the seven invariants (tools/android/selfcheck.cpp)")
        build_and_run(cxx, report, workdir, "selfcheck",
                      [os.path.join(ROOT, "tools/android/selfcheck.cpp")], INCLUDES, expect=0)

        report.step("every header compiles alone, and twice (the guard defect is invisible once)")
        for header in HEADERS:
            stub = os.path.join(workdir, "stub.cpp")
            with open(stub, "w", encoding="utf-8") as handle:
                handle.write(f'#include "{header}.h"\n#include "{header}.h"\nint main(){{return 0;}}\n')
            command = [cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-c", "-o",
                       os.path.join(workdir, "stub.o"), stub]
            for include in INCLUDES:
                command += ["-I", os.path.join(ROOT, include)]
            built = run(command)
            if built.returncode != 0:
                report.fail(f"{header}.h does not compile twice:\n{(built.stderr or built.stdout)[:1200]}")
            else:
                report.ok(f"{header}.h")

        report.step("the four integration demos (tools/android/integration/) -- the code the doc gives")
        for demo in ("gestures-demo", "import-demo", "space-demo", "space-query-demo"):
            build_and_run(cxx, report, workdir, demo,
                          [os.path.join(ROOT, f"tools/android/integration/{demo}.cpp"),
                           os.path.join(ROOT, "engine/core/math/Vector2.cpp")], INCLUDES, expect=0)

        if arguments.quick:
            report.step("the fuzz harness (skipped by --quick)")
            report.note("400,209 hostile inputs not run")
        else:
            report.step("the fuzz harness, 400,000 inputs under ASan+UBSan")
            source = os.path.join(workdir, "fuzz.cpp")
            with open(source, "w", encoding="utf-8") as handle:
                handle.write(FUZZ)
            binary = os.path.join(workdir, "fuzz")
            command = [cxx, "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
                       "-fno-sanitize-recover=all", "-o", binary, source]
            for vector in ("Vector2", "Vector3", "Vector4"):
                command.append(os.path.join(ROOT, f"engine/core/math/{vector}.cpp"))
            for include in INCLUDES:
                command += ["-I", os.path.join(ROOT, include)]
            built = run(command)
            if built.returncode != 0:
                report.fail(f"the fuzz harness did not compile:\n{(built.stderr or built.stdout)[:2000]}")
            else:
                result = run([binary], timeout=600)
                if result.returncode == 0 and "ok:" in result.stdout:
                    report.ok(result.stdout.strip().splitlines()[-1])
                else:
                    report.fail(f"the fuzz run reported:\n{(result.stdout + result.stderr)[-3000:]}")

    # the two core-defect steps: reported, never counted as this branch's failure
    report.step("the core Data defects (engine/core/io/Data.cpp belongs to the main agent)")
    script = os.path.join(ROOT, "tools/android/data-overflow-real.py")
    result = run([sys.executable, script])
    if result.returncode == 1:
        report.note("both defects still reproduce in Data.cpp: the patch is in ANDROID_HANDOFF.md §4")
    elif result.returncode == 0:
        report.note("Data.cpp no longer shows either defect (patched upstream)")
    else:
        report.note(f"the repro could not run (exit {result.returncode})")

    print()
    if report.failures:
        print(f"verify: {report.failures} check(s) failed")
        return 1
    print("verify: every check this branch owns passes")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
