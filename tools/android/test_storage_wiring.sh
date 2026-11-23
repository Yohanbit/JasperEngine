#!/usr/bin/env bash
# Builds and runs the Android storage wiring tests without CMake.
#
# Why not CMake: the core test target needs X11 here and the Android backend needs the SDK, so a
# sandbox (and a developer without either) cannot configure the project at all. These two test files
# are written to need the engine's io/System layer and nothing above it -- no renderer, no window --
# so they can be built against the sources they exercise and run anywhere a g++ and the vendored
# googletest are.
#
# In CI the same files are picked up by engine/tests/core/CMakeLists.txt's `*Test.cpp` glob and run
# by the normal core-test target against the real engine; this script is the local gate.
#
#   tools/android/test_storage_wiring.sh          # build and run
#   tools/android/test_storage_wiring.sh --quiet  # only failures
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

QUIET=0
[[ "${1:-}" == "--quiet" ]] && QUIET=1

CXX="${CXX:-g++}"
OUT="${OUT:-build/android-storage-wiring}"
mkdir -p "$OUT"

INCLUDES=(
    -I engine -I engine/renders -I engine/core -I engine/core/script -I engine/core/math -I engine/core/input
    -I engine/libs -I engine/libs/json -I engine/libs/sokol -I engine/libs/tinyxml2
    -I engine/platform/android
    -I libs/imgui -I libs/lua -I libs/lua/src
    -I libs/googletest/googletest/include -I libs/googletest/googletest
)

# The platform functions these sources reference but never need (no renderer, no window, no device):
# a packed resource is absent, no log sink is installed, no player content root. Written next to the
# binary so the script leaves the tree alone.
cat > "$OUT/stubs.cpp" <<'STUBS'
#include "Engine.h"
#include "io/ResourcePack.h"
#include "log/LogSinks.h"
#include "PlatformConfig.h"

#include <memory>
#include <string>
#include <vector>

namespace jasperengine {
    double Engine::getFixedDeltatime() { return 0.0; }
    double Engine::getFixedTime() { return 0.0; }
    uint64_t Engine::getGameFrames() { return 0; }

    bool ResourcePack::contains(const std::string&) { return false; }
    bool ResourcePack::read(const std::string&, std::vector<unsigned char>&) { return false; }
    bool ResourcePack::readOwned(const std::string&, std::unique_ptr<unsigned char[]>&, unsigned int&) { return false; }

    void installPlatformLogSinks(const PlatformLogOptions&) {}
    PlatformConfig& platformConfig() { static PlatformConfig config; return config; }
}
STUBS

SOURCES=(
    engine/tests/core/platform/AndroidStorageWiringTest.cpp
    engine/tests/core/platform/AndroidStorageWritePreflightTest.cpp
    engine/tests/core/platform/AndroidTouchFeedingTest.cpp
    "$OUT/stubs.cpp"
    engine/core/System.cpp
    engine/core/Log.cpp
    engine/core/math/Vector2.cpp
    engine/core/log/Logger.cpp
    engine/core/io/Data.cpp
    engine/core/io/FileData.cpp
    engine/core/io/File.cpp
    engine/core/io/FileUtils.cpp
    engine/core/io/AtomicFile.cpp
    engine/core/io/UserSettingsStore.cpp
    engine/core/debug/ProcessMemory.cpp
    engine/core/diag/CrashHandler.cpp
    engine/core/diag/CrashHandlerPosix.cpp
    engine/core/util/PlatformRules.cpp
    engine/core/util/Base64.cpp
    engine/core/util/StringUtils.cpp
    engine/core/util/CrashHook.cpp
    engine/core/util/SubscriptionOwnerIndex.cpp
    engine/libs/tinyxml2/tinyxml2.cpp
    libs/googletest/googletest/src/gtest-all.cc
    libs/googletest/googletest/src/gtest_main.cc
)

while IFS= read -r extra; do SOURCES+=("$extra"); done < <(
    find engine/core -name 'NumberText.cpp' -o -name 'Backtrace.cpp' -o -name 'CrashRing*.cpp'
)

echo "storage-wiring: compiling ${#SOURCES[@]} sources"
"$CXX" -std=c++20 -O1 -g -ffunction-sections -fdata-sections \
    "${INCLUDES[@]}" "${SOURCES[@]}" \
    -Wl,--gc-sections -lpthread -o "$OUT/storage-wiring"

if [[ $QUIET -eq 1 ]]; then
    "$OUT/storage-wiring" > "$OUT/run.log" 2>&1 || { tail -40 "$OUT/run.log"; exit 1; }
    grep -E '^\[  (PASSED|FAILED)' "$OUT/run.log" || true
else
    "$OUT/storage-wiring"
fi
