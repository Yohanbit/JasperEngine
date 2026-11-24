// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// The invariants of engine/platform/android/*.h that are easy to break and expensive to notice, in
// one program a gate can run without CMake or googletest:
//
//   g++ -std=c++20 -Wall -Wextra -Werror -I engine/platform/android -I engine/core
//       -I engine/libs/json -I engine/libs/sokol tools/android/selfcheck.cpp -o /tmp/sc && /tmp/sc
//
// Every check below is a defect this branch actually shipped and then fixed (ANDROID_BUGS 10d,
// 10g-10o, 10b-ii). The gtest suite covers the behaviour; this covers the four properties a
// refactor silently loses: a name never escapes its folder, a read never leaves its buffer, a
// recovery name always maps back, and an include guard closes at the end of the file.
//
// Exit 0 when every invariant holds; 1 naming each violation.
//
// Verified by mutation, not just by passing: reintroducing bug 10j (a name that is already a
// temporary getting a second suffix) and bug "isInsideProject("/etc/passwd") is true" makes this
// fail with the right line, so it is a gate and not a decoration. A check that cannot fail is not
// a check -- the first version of this file had exactly that problem with bug 10j, because the
// mutation it was tried against was too shallow to reproduce it.

#include "AndroidAssetImport.h"
#include "AndroidImageLimits.h"
#include "AndroidImageOrientation.h"
#include "AndroidImportPreflight.h"
#include "AndroidImportTransfer.h"
#include "AndroidModelSidecars.h"
#include "AndroidProjectLayout.h"
#include "AndroidStorageSpace.h"

// included twice on purpose: an `#endif` that closes before a later block is invisible to one
#include "AndroidImageLimits.h"
#include "AndroidImportTransfer.h"

#include <cstdio>
#include <string>

namespace {

    int failures = 0;

    void check(bool ok, const char* what) {
        if (!ok) {
            std::printf("SELFCHECK FAIL: %s\n", what);
            ++failures;
        }
    }

    using namespace jasperengine::android;

}

int main() {
    // 1. a name from a provider never escapes the project folder (bug: "/etc/passwd" was "inside")
    for (const char* hostile : {"", ".", "..", "/etc/passwd", "//srv/share", "./a", "a/./b", "../a", "a/../../b", "c:/a"}) {
        check(!isInsideProject(hostile), "isInsideProject accepted a path that leaves the folder");
    }
    check(isInsideProject("textures/a.png"), "isInsideProject refused an ordinary relative path");

    // 2. a sanitized name is legal on every volume (bug: "con.png" broke Windows, ".PNG" broke the type table)
    for (const char* wanted : {"con.png", "NUL.PNG", "com1.png", "lpt9.png", "../../etc/passwd", "a:b.png",
                               "..", ".", "", "Photo 12:30.PNG"}) {
        const std::string name = sanitizeFileName(wanted);
        check(!name.empty(), "sanitizeFileName produced an empty name");
        check(name != "." && name != "..", "sanitizeFileName produced a navigation name");
        check(name.find('/') == std::string::npos && name.find('\\') == std::string::npos &&
                  name.find(':') == std::string::npos,
              "sanitizeFileName kept a separator or a colon");
        check(!isWindowsDeviceName(name.substr(0, name.find_last_of('.'))),
              "sanitizeFileName kept a Windows device name");
    }

    // 3. an image decision never asks for more than the format allows, and never says 0 MB
    {
        ImageSize size;
        size.known = true;
        size.width = 3;
        size.height = 3;
        size.channels = 4;
        ImageBudget budget;
        budget.maxTextureSize = 1;
        const ImageDecision decision = decideImageImport(size, budget);
        check(decision.message.find("0 MB") == std::string::npos, "a downscale message said 0 MB");
        check(decision.width >= 1 && decision.height >= 1, "a downscale produced a zero side");
        check(decision.scale > 0.0 && decision.scale <= 1.0, "a downscale produced a scale outside (0, 1]");
    }

    // 4. every transfer plan writes somewhere the scanner can recover, and maps back (bugs 10j-10l)
    // the last two are names that are *already* temporaries (bug 10j): a plan for them must keep one
    // suffix, or the scanner cannot recognise what it just wrote
    for (const char* wanted : {"photo.png", "assets/photo.png", "dir/", "", "a/.x.jeimport", "a/.photo.png.jeimport"}) {
        for (uint64_t expected : {0ull, 100ull, 4096ull}) {
            const TransferPlan plan = planTransfer(wanted, expected, 0);
            check(isImportTemporary(plan.temporary), "planTransfer wrote a name the scanner cannot recover");
            check(temporaryTarget(plan.temporary) == plan.target, "a temporary does not map back to its target");
        }
    }
    {
        // a resumed copy asks for no more than is left, and a sizeless one never resumes a stranger
        const TransferPlan sized = planTransfer("a.bin", 1000, 600);
        check(transferStep(sized, 600, 1u << 30, 0, 4096).copyBytes <= 400, "a step asked for more than is left");
        const TransferPlan sizeless = planTransfer("b.bin", 0, 5000);
        check(sizeless.discardPartial, "a sizeless source resumed from a temporary it cannot trust");
    }

    // 5. an orientation never needs a transform when the picture is already upright
    for (uint32_t value = 0; value <= 9; value++) {
        const Orientation orientation = orientationFromValue(value);
        const OrientationTransform turn = transformFor(orientation);
        if (isUpright(orientation)) check(!turn.needed, "an upright picture was reported as needing a turn");
        check(!turn.needed || turn.rotateClockwise != 0 || turn.flipHorizontal || turn.flipVertical,
              "a turn was required but nothing was set");
    }

    // 6. a model's sidecar names never collide with one another (bug 10m)
    {
        const std::string text = R"({"asset":{"version":"2.0"},"images":[{"uri":"a/common.png"},{"uri":"b/common.png"}]})";
        const ModelImportPlan plan = planModelImport(text, "Body.gltf");
        for (size_t i = 0; i < plan.sidecars.size(); i++) {
            check(!plan.sidecars[i].assetName.empty(), "a sidecar got an empty name");
            for (size_t k = i + 1; k < plan.sidecars.size(); k++)
                check(plan.sidecars[i].assetName != plan.sidecars[k].assetName, "two sidecars share a name");
        }
        check(plan.parsed && plan.gltfText.find("a/common.png") == std::string::npos,
              "the rewritten document still names a path the import does not create");
    }

    // 7. an unknown volume never refuses a write, and a known one refuses what does not fit
    {
        SpaceInfo unknown;
        check(allowWrite(unknown, UINT64_MAX).allowed, "an unknown volume refused a write");
        SpaceInfo known;
        known.known = true;
        known.availableBytes = 1000;
        known.totalBytes = 1000;
        SpacePolicy policy;
        policy.reserveBytes = 500;
        check(!allowWrite(known, 600, policy).allowed, "a write that would eat the reserve was allowed");
        check(allowWrite(known, 500, policy).allowed, "a write inside the budget was refused");
    }

    if (failures == 0) std::printf("selfcheck: every invariant holds\n");
    return failures == 0 ? 0 : 1;
}
