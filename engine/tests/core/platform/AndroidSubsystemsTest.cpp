// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Android import, gesture, storage and project subsystems, which are plain C++ so they run
// here: AndroidAssetImport.h, AndroidTouchGestures.h, AndroidStorageSpace.h,
// AndroidProjectLayout.h.

#include "AndroidAssetImport.h"
#include "AndroidProjectLayout.h"
#include "AndroidStorageSpace.h"
#include "AndroidTouchGestures.h"
#include "AndroidImageOrientation.h"
#include "AndroidImportPreflight.h"
#include "AndroidModelSidecars.h"
#include "AndroidImportTransfer.h"
#include "asset/AssetType.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::android;

namespace {

    std::vector<uint8_t> bytesOf(std::initializer_list<int> values) {
        std::vector<uint8_t> bytes;
        for (int value : values) bytes.push_back(static_cast<uint8_t>(value));
        return bytes;
    }

    std::vector<uint8_t> textBytes(const std::string& text) {
        return std::vector<uint8_t>(text.begin(), text.end());
    }

    // the name is the one the engine's own path rules accept: no ':' and no ".." segment
    void expectLegalName(const std::string& name) {
        EXPECT_FALSE(name.empty());
        EXPECT_EQ(name.find(':'), std::string::npos) << name;
        EXPECT_EQ(name.find('/'), std::string::npos) << name;
        EXPECT_EQ(name.find('\\'), std::string::npos) << name;
        EXPECT_NE(name, ".") << name;
        EXPECT_NE(name, "..") << name;
    }

    // a finger, as Input reports one
    Touch finger(int pointer, float x, float y, TouchPhase phase = TouchPhase::MOVED) {
        Touch touch;
        touch.pointer = pointer;
        touch.fingerId = pointer;
        touch.position = Vector2(x, y);
        touch.startPosition = Vector2(x, y);
        touch.phase = phase;
        return touch;
    }

}

// ---- names and URIs

TEST(AndroidAssetImport, PercentDecodesAndKeepsPlus) {
    std::string decoded;
    percentDecode("My%20Photo%2012%3A30.png", decoded);
    EXPECT_EQ(decoded, "My Photo 12:30.png");
    // '+' is a legal file name character, never a space in a path
    percentDecode("a+b.png", decoded);
    EXPECT_EQ(decoded, "a+b.png");
    // a '%' that starts nothing valid stays, and a trailing one too
    percentDecode("100%d%2", decoded);
    EXPECT_EQ(decoded, "100%d%2");
}

TEST(AndroidAssetImport, FileNameComesFromTheLastSegment) {
    EXPECT_EQ(uriFileName("content://com.android.providers.media.documents/document/image%3A1000000034"),
              "image:1000000034");
    EXPECT_EQ(uriFileName("content://x/y/My%20Photo%2012%3A30.png?width=10#frag"), "My Photo 12:30.png");
    EXPECT_EQ(uriFileName("file:///storage/emulated/0/Pictures/a.png"), "a.png");
    // a folder grant's last segment is the volume and folder, decoded: the caller sanitizes it
    EXPECT_EQ(uriFileName("content://com.android.externalstorage.documents/tree/primary%3AProjects"),
              "primary:Projects");
    // a document id ("document/1234") is what the last segment holds when the provider names none
    EXPECT_EQ(uriFileName("content://com.example.cloud/document/1234"), "1234");
    EXPECT_EQ(uriFileName(""), "");
}

TEST(AndroidAssetImport, SanitizesWhatThePathRulesRefuse) {
    // a SAF id's colon, and the separators of three platforms
    EXPECT_EQ(sanitizeFileName("image:1000000034"), "image_1000000034");
    EXPECT_EQ(sanitizeFileName("a/b\\c:d*e?f\"g<h>i|j.png"), "a_b_c_d_e_f_g_h_i_j.png");
    EXPECT_EQ(sanitizeFileName("../../../etc/passwd"), "_.._.._etc_passwd");
    // the names a file system refuses
    EXPECT_EQ(sanitizeFileName(".."), "file");
    EXPECT_EQ(sanitizeFileName("."), "file");
    EXPECT_EQ(sanitizeFileName("   "), "file");
    EXPECT_EQ(sanitizeFileName(""), "file");
    // Windows drops a trailing dot itself: the name must not keep one
    EXPECT_EQ(sanitizeFileName("photo.png."), "photo.png");
    EXPECT_EQ(sanitizeFileName("  photo.png  "), "photo.png");
}

TEST(AndroidAssetImport, SanitizeKeepsUnicodeAndDropsControls) {
    // a phone names photos in the user's own script
    EXPECT_EQ(sanitizeFileName("Foto Año Nuevo 🎉.png"), "Foto Año Nuevo 🎉.png");
    // a NUL or a control byte a provider sent never reaches a path
    EXPECT_EQ(sanitizeFileName(std::string("a\0b\nc.txt", 9)), "abc.txt");
    expectLegalName(sanitizeFileName("café:té.png"));
    // a sequence cut short is dropped, not half-copied
    std::string truncated = "abc";
    truncated.push_back(static_cast<char>(0xE2));
    truncated.push_back(static_cast<char>(0x82));
    EXPECT_EQ(sanitizeFileName(truncated), "abc");
}

TEST(AndroidAssetImport, SanitizeKeepsTheExtensionWhenItCuts) {
    const std::string longName(200, 'a');
    const std::string cut = sanitizeFileName(longName + ".png", SanitizeOptions{32, true});
    EXPECT_LE(cut.size(), 32u);
    EXPECT_EQ(cut.substr(cut.size() - 4), ".png") << cut;
    expectLegalName(cut);

    // a cut never splits a UTF-8 sequence: every multi-byte character is whole
    const std::string accents = "a\xC3\xB1onucle\xC3\xB1onucle\xC3\xB1onucle\xC3\xB1onucle\xC3\xB1onucle.png";
    const std::string cutUtf8 = sanitizeFileName(accents, SanitizeOptions{24, true});
    for (size_t i = 0; i < cutUtf8.size();) {
        const unsigned char lead = static_cast<unsigned char>(cutUtf8[i]);
        size_t length = 1;
        if (lead >= 0xC2 && lead <= 0xDF) length = 2;
        else if (lead >= 0xE0 && lead <= 0xEF) length = 3;
        else if (lead >= 0xF0 && lead <= 0xF4) length = 4;
        ASSERT_GE(i + length, i) << "no wrap";
        EXPECT_LE(i + length, cutUtf8.size()) << "a character cut in two at " << i;
        for (size_t k = 1; k < length && i + k < cutUtf8.size(); k++) {
            EXPECT_EQ(static_cast<unsigned char>(cutUtf8[i + k]) & 0xC0, 0x80) << "split at " << i;
        }
        i += length;
    }
    EXPECT_EQ(cutUtf8.substr(cutUtf8.size() - 4), ".png") << cutUtf8;
}

TEST(AndroidAssetImport, UniqueNameCountsLikeTheProjectWindow) {
    std::vector<std::string> taken;
    auto isTaken = [&taken](const std::string& name) {
        for (const std::string& other : taken) {
            if (other == name) return true;
        }
        return false;
    };
    EXPECT_EQ(uniqueFileName("sprite.png", isTaken), "sprite.png");
    taken.push_back("sprite.png");
    EXPECT_EQ(uniqueFileName("sprite.png", isTaken), "sprite (1).png");
    taken.push_back("sprite (1).png");
    EXPECT_EQ(uniqueFileName("sprite.png", isTaken), "sprite (2).png");
    // a name with no extension keeps the count at its end
    taken.clear();
    taken.push_back("README");
    EXPECT_EQ(uniqueFileName("README", isTaken), "README (1)");
    // a name the rules refuse is sanitized first, then counted
    taken.clear();
    EXPECT_EQ(uniqueFileName("a:b.png", isTaken), "a_b.png");
    // the predicate is optional
    EXPECT_EQ(uniqueFileName("x.png", nullptr), "x.png");
}

// ---- what a picked file is

TEST(AndroidAssetImport, MagicBeatsExtension) {
    const auto png = bytesOf({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0});
    // a provider that handed over the file with a wrong or no extension
    EXPECT_EQ(assetTypeForImport("photo", png), AssetType::Texture);
    EXPECT_EQ(assetTypeForImport("photo.txt", png), AssetType::Texture);
    EXPECT_EQ(kindFromMagic(png), ImportKind::Png);

    EXPECT_EQ(assetTypeForImport("song.txt", bytesOf({0x4F, 0x67, 0x67, 0x53, 0, 0, 0, 0})), AssetType::Audio);
    EXPECT_EQ(assetTypeForImport("mesh.bin", bytesOf({0x67, 0x6C, 0x54, 0x46, 0x02, 0, 0, 0})), AssetType::Model);
    EXPECT_EQ(assetTypeForImport("font.bin", bytesOf({0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0})), AssetType::Font);
    // RIFF is three things; the form type decides
    EXPECT_EQ(assetTypeForImport("a.bin", textBytes(std::string("RIFF\0\0\0\0WAVEfmt ", 16))), AssetType::Audio);
    EXPECT_EQ(assetTypeForImport("a.bin", textBytes(std::string("RIFF\0\0\0\0WEBPVP8 ", 16))), AssetType::Texture);
    EXPECT_EQ(kindFromMagic(textBytes(std::string("RIFF\0\0\0\0WEBPVP8 ", 16))), ImportKind::Webp);
}

TEST(AndroidAssetImport, ExtensionDecidesWhenBytesSayNothing) {
    // containers with no header of their own, and text formats
    EXPECT_EQ(assetTypeForImport("mesh.obj", textBytes("mtllib scene.mtl\n")), AssetType::Model);
    EXPECT_EQ(assetTypeForImport("mesh.gltf", textBytes("{\"asset\":{}}")), AssetType::Model);
    EXPECT_EQ(assetTypeForImport("level.lua", textBytes("-- script")), AssetType::Script);
    EXPECT_EQ(assetTypeForImport("ui.html", textBytes("<html>")), AssetType::UIDocument);
    EXPECT_EQ(assetTypeForImport("style.css", textBytes("a{}")), AssetType::Stylesheet);
    EXPECT_EQ(assetTypeForImport("data.json", textBytes("{}")), AssetType::Data);
    EXPECT_EQ(assetTypeForImport("sprite.aseprite", bytesOf({0x20, 0xA5, 0xE0, 0xA1})), AssetType::AsepriteSprite);
    EXPECT_EQ(assetTypeForImport("notes.xyz", textBytes("hello")), AssetType::Unknown);
    // the extension is lowercased, and the folder is not the extension
    EXPECT_EQ(assetTypeForImport("Textures/Photo.PNG", {}), AssetType::Texture);
    EXPECT_EQ(assetTypeForImport("a.folder/photo", {}), AssetType::Unknown);
    EXPECT_EQ(extensionOf("Textures/Photo.PNG"), "png");
    EXPECT_EQ(extensionOf("Textures/photo"), "");
    EXPECT_EQ(extensionOf("a.folder/photo"), "");
}

TEST(AndroidAssetImport, ImportedNameIsAnAssetName) {
    const auto png = bytesOf({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A});
    // "Photo 12:30.PNG" from a phone: a lowercase identifier plus the extension
    EXPECT_EQ(importedAssetName("Photo 12:30.PNG", png), "photo_12_30.png");
    // a cloud provider's "document/1234" carries no extension: the type's usual one is added
    EXPECT_EQ(importedAssetName("1234", png), "_1234.png");
    EXPECT_EQ(importedAssetName("clip", textBytes("OggS\0\0\0\0")), "clip.ogg");
    EXPECT_EQ(importedAssetName("font.tff", bytesOf({0x00, 0x01, 0x00, 0x00})), "font.tff");
    // a name of nothing becomes something an asset can be called
    EXPECT_EQ(importedAssetName("...", {}), "asset");
    const std::string name = importedAssetName("Photo 12:30.png", png);
    expectLegalName(name);
}

// ---- gestures

TEST(AndroidTouchGestures, PinchReadsDistanceAndItsChange) {
    RecognizerState state;
    std::vector<Touch> touches = {finger(0, 100, 200), finger(1, 300, 200)};
    updatePinch(state, touches, 0);
    EXPECT_TRUE(state.pinch.active);
    EXPECT_FALSE(state.pinch.ended);
    EXPECT_EQ(state.pinch.frames, 1);
    EXPECT_FLOAT_EQ(state.pinch.distance, 200.0f);
    // the first frame of a pinch has no delta: it began here
    EXPECT_FLOAT_EQ(state.pinch.deltaDistance, 0.0f);
    EXPECT_FLOAT_EQ(state.pinch.midpoint.x, 200.0f);
    EXPECT_FLOAT_EQ(state.pinch.midpoint.y, 200.0f);
    EXPECT_EQ(state.pinch.pointerA, 0);
    EXPECT_EQ(state.pinch.pointerB, 1);

    // fingers 20 px further apart each: +40 px, a spread (zoom in)
    touches = {finger(0, 80, 200), finger(1, 320, 200)};
    updatePinch(state, touches, 1);
    EXPECT_TRUE(state.pinch.active);
    EXPECT_FLOAT_EQ(state.pinch.frames, 2);
    EXPECT_FLOAT_EQ(state.pinch.distance, 240.0f);
    EXPECT_FLOAT_EQ(state.pinch.deltaDistance, 40.0f);
    EXPECT_FLOAT_EQ(state.pinch.deltaMidpoint.x, 0.0f);

    // and closing again is negative
    touches = {finger(0, 130, 200), finger(1, 270, 200)};
    updatePinch(state, touches, 2);
    EXPECT_FLOAT_EQ(state.pinch.distance, 140.0f);
    EXPECT_FLOAT_EQ(state.pinch.deltaDistance, -100.0f);
}

TEST(AndroidTouchGestures, PinchReadsRotationAndPan) {
    RecognizerState state;
    std::vector<Touch> touches = {finger(0, 200, 100), finger(1, 200, 300)};
    updatePinch(state, touches, 0);
    // straight down the screen: atan2(dy, dx) with dy = 200 is +pi/2 in screen space
    EXPECT_NEAR(state.pinch.angle, 1.5707964f, 1e-5f);

    // both fingers moved right by 50: a pan with no pinch and no rotation
    touches = {finger(0, 250, 100), finger(1, 250, 300)};
    updatePinch(state, touches, 1);
    EXPECT_FLOAT_EQ(state.pinch.deltaMidpoint.x, 50.0f);
    EXPECT_FLOAT_EQ(state.pinch.deltaDistance, 0.0f);
    EXPECT_NEAR(state.pinch.deltaAngle, 0.0f, 1e-5f);

    // now turned a quarter: the line is across the screen
    touches = {finger(0, 100, 200), finger(1, 300, 200)};
    updatePinch(state, touches, 2);
    EXPECT_NEAR(state.pinch.deltaAngle, -1.5707964f, 1e-5f) << "a quarter turn the short way round";
}

TEST(AndroidTouchGestures, PinchEndsWhenAFingerLeaves) {
    RecognizerState state;
    updatePinch(state, {finger(0, 100, 100), finger(1, 200, 100)}, 0);
    ASSERT_TRUE(state.pinch.active);

    updatePinch(state, {finger(0, 100, 100)}, 1);
    EXPECT_FALSE(state.pinch.active);
    EXPECT_TRUE(state.pinch.ended);
    EXPECT_EQ(state.pinch.frames, 0);
    EXPECT_EQ(state.pinch.pointerA, -1);

    // and stays ended once, not every frame after
    updatePinch(state, {}, 2);
    EXPECT_FALSE(state.pinch.ended);
}

TEST(AndroidTouchGestures, AnotherPairIsANewPinch) {
    RecognizerState state;
    updatePinch(state, {finger(0, 100, 100), finger(1, 200, 100)}, 0);
    ASSERT_FLOAT_EQ(state.pinch.distance, 100.0f);

    // one finger lifted, another put down far away: not a 300 px pinch
    updatePinch(state, {finger(0, 100, 100), finger(2, 500, 100)}, 1);
    EXPECT_TRUE(state.pinch.active);
    EXPECT_FLOAT_EQ(state.pinch.distance, 400.0f);
    EXPECT_FLOAT_EQ(state.pinch.deltaDistance, 0.0f) << "a new pair reports no jump";
    EXPECT_EQ(state.pinch.frames, 1);
    EXPECT_EQ(state.pinch.pointerB, 2);

    // a frame skipped between the two: the pair did not stay down continuously
    updatePinch(state, {finger(0, 100, 100), finger(2, 500, 100)}, 5);
    EXPECT_EQ(state.pinch.frames, 1);
    EXPECT_FLOAT_EQ(state.pinch.deltaDistance, 0.0f);
}

TEST(AndroidTouchGestures, PinchIgnoresAThirdFinger) {
    RecognizerState state;
    updatePinch(state, {finger(0, 100, 100), finger(1, 200, 100)}, 0);
    std::vector<Touch> three = {finger(0, 100, 100), finger(1, 200, 100), finger(2, 400, 400)};
    updatePinch(state, three, 1);
    EXPECT_TRUE(state.pinch.active);
    EXPECT_EQ(state.pinch.pointerA, 0);
    EXPECT_EQ(state.pinch.pointerB, 1);
    EXPECT_FLOAT_EQ(state.pinch.distance, 100.0f);
    EXPECT_FLOAT_EQ(state.pinch.deltaDistance, 0.0f);
}

TEST(AndroidTouchGestures, DragStartsPastTheSlop) {
    DragState drag;
    Touch touch = finger(0, 100, 100, TouchPhase::BEGAN);
    updateDrag(drag, {touch}, 8.0f);
    EXPECT_FALSE(drag.dragging) << "a finger that has not moved is a tap";

    touch.position = Vector2(103, 103);
    touch.delta = Vector2(3, 3);
    updateDrag(drag, {touch}, 8.0f);
    EXPECT_FALSE(drag.dragging) << "4 px is inside the slop";

    touch.position = Vector2(120, 100);
    touch.delta = Vector2(17, 0);
    updateDrag(drag, {touch}, 8.0f);
    EXPECT_TRUE(drag.dragging);
    EXPECT_EQ(drag.pointer, 0);
    EXPECT_FLOAT_EQ(drag.total.x, 20.0f);
    EXPECT_FLOAT_EQ(drag.frame.x, 20.0f) << "the frame it began reports all of its travel";

    touch.position = Vector2(130, 100);
    touch.delta = Vector2(10, 0);
    updateDrag(drag, {touch}, 8.0f);
    EXPECT_TRUE(drag.dragging);
    EXPECT_FLOAT_EQ(drag.total.x, 30.0f);
    EXPECT_FLOAT_EQ(drag.frame.x, 10.0f);
    EXPECT_FLOAT_EQ(drag.previousFrame.x, 20.0f);
}

TEST(AndroidTouchGestures, DragEndsOnReleaseAndKeepsTheFling) {
    DragState drag;
    Touch touch = finger(0, 0, 0, TouchPhase::BEGAN);
    updateDrag(drag, {touch}, 8.0f);
    touch.position = Vector2(50, 0);
    touch.delta = Vector2(50, 0);
    updateDrag(drag, {touch}, 8.0f);
    ASSERT_TRUE(drag.dragging);

    touch.position = Vector2(80, 0);
    touch.delta = Vector2(30, 0);
    touch.phase = TouchPhase::ENDED;
    updateDrag(drag, {touch}, 8.0f);
    EXPECT_FALSE(drag.dragging);
    EXPECT_EQ(drag.pointer, -1);
    EXPECT_FLOAT_EQ(drag.total.x, 80.0f);
    EXPECT_FLOAT_EQ(drag.previousFrame.x, 50.0f) << "a fling reads the last movement at release";
}

// ---- storage

TEST(AndroidStorageSpace, RefusesAWriteThatWouldFillTheVolume) {
    SpaceInfo space;
    space.known = true;
    space.totalBytes = 64ull * 1024 * 1024 * 1024;
    space.availableBytes = 100ull * 1024 * 1024;

    // a save of 1 MiB with 100 MiB free: fine
    EXPECT_TRUE(allowWrite(space, 1024 * 1024).allowed);

    // a save that leaves less than the reserve: refused before it starts (and 100 MiB of 64 GiB
    // is 0.15% free, so the volume is critical too)
    const WriteAllowance tight = allowWrite(space, 100ull * 1024 * 1024);
    EXPECT_FALSE(tight.allowed);
    EXPECT_TRUE(tight.critical);
    EXPECT_EQ(tight.budget, 92ull * 1024 * 1024) << "what it may write with the reserve kept";

    // exactly the budget is allowed
    EXPECT_TRUE(allowWrite(space, tight.budget).allowed);
    EXPECT_FALSE(allowWrite(space, tight.budget + 1).allowed);
}

TEST(AndroidStorageSpace, UnknownVolumeNeverRefuses) {
    const WriteAllowance allowance = allowWrite(unknownSpace(), UINT64_MAX);
    EXPECT_TRUE(allowance.allowed) << "a platform without the call must not stop a save";
    EXPECT_TRUE(allowance.unknown);
    EXPECT_FALSE(allowance.low);
}

TEST(AndroidStorageSpace, WarnsByShareOfTheVolume) {
    SpaceInfo space;
    space.known = true;
    space.totalBytes = 1000;
    space.availableBytes = 300;   // 30%: nothing
    SpacePolicy policy;
    policy.reserveBytes = 10;
    EXPECT_FALSE(allowWrite(space, 1, policy).low);

    space.availableBytes = 90;    // 9%: low
    EXPECT_TRUE(allowWrite(space, 1, policy).low);
    EXPECT_FALSE(allowWrite(space, 1, policy).critical);

    space.availableBytes = 30;    // 3%: critical
    EXPECT_TRUE(allowWrite(space, 1, policy).critical);
    EXPECT_FALSE(allowWrite(space, 1, policy).low) << "critical implies low, not both";
}

TEST(AndroidStorageSpace, AnAppQuotaIsTheSmallerBowl) {
    SpaceInfo space;
    space.known = true;
    space.totalBytes = 64ull * 1024 * 1024;   // the volume has room
    space.availableBytes = 32ull * 1024 * 1024;
    space.appQuotaBytes = 100ull * 1024 * 1024;
    space.appUsedBytes = 95ull * 1024 * 1024;  // the app has 5 MiB left of its quota
    EXPECT_EQ(space.writableBytes(), 5ull * 1024 * 1024);
    EXPECT_FALSE(allowWrite(space, 1ull * 1024 * 1024).allowed) << "the reserve is not inside a quota";

    space.appUsedBytes = 10ull * 1024 * 1024;
    EXPECT_EQ(space.writableBytes(), 32ull * 1024 * 1024) << "the volume's free bytes are fewer now";
}

TEST(AndroidStorageSpace, PlansWhichCachesToDrop) {
    SpaceInfo space;
    space.known = true;
    space.totalBytes = 1000;
    space.availableBytes = 20;    // critical (2%)
    SpacePolicy policy;
    policy.reserveBytes = 100;
    const CacheCandidate caches[] = {{"thumbnails", 30}, {"library", 200}, {"temp", 5}};

    // 20 free, 100 reserved: 80 must come back, and the largest (library, 200) alone is enough
    EXPECT_EQ(planCacheRelease(space, caches, policy), 1u);
    // a smaller need takes the smallest candidate that covers it: 20 free and a reserve of 35
    // needs 15, and temp (5) is not enough on its own where thumbnails (30) is
    policy.reserveBytes = 35;
    EXPECT_EQ(planCacheRelease(space, caches, policy), 1u);

    // nothing to free when the volume is not critical
    space.availableBytes = 900;
    EXPECT_EQ(planCacheRelease(space, caches, policy), 0u);

    // everything listed when even all of it is not enough (235 bytes against a 1000 reserve),
    // so the caller reports a full volume rather than deleting all of it and still failing
    space.availableBytes = 1;
    policy.reserveBytes = 1000;
    EXPECT_EQ(planCacheRelease(space, caches, policy), 3u);

    // an unknown volume: nothing is deleted
    EXPECT_EQ(planCacheRelease(unknownSpace(), caches, policy), 0u);
}

// ---- project layout

TEST(AndroidProjectLayout, RefusesAPathThatLeavesTheProject) {
    EXPECT_TRUE(isInsideProject("textures/a.png"));
    EXPECT_TRUE(isInsideProject("a.png"));
    EXPECT_FALSE(isInsideProject("../a.png"));
    EXPECT_FALSE(isInsideProject("textures/../../a.png")) << "a provider's name is not trusted";
    EXPECT_FALSE(isInsideProject("c:/a.png"));
    EXPECT_FALSE(isInsideProject("a\\b.png"));
    // An absolute path is not inside anything, and on Linux/macOS a leading '/' makes a join
    // drop the project folder entirely: a provider's name must never take that route.
    EXPECT_FALSE(isInsideProject("/etc/passwd"));
    EXPECT_FALSE(isInsideProject("//server/share"));
    // the empty name joins to the folder itself rather than to a file in it
    EXPECT_FALSE(isInsideProject(""));
    // "." is navigation, not a file: it is refused wherever it appears as a whole segment
    EXPECT_FALSE(isInsideProject("."));
    EXPECT_FALSE(isInsideProject("./a.png"));
    EXPECT_FALSE(isInsideProject("a/./b.png"));
    EXPECT_FALSE(isInsideProject("a/."));
    // a trailing separator names the folder the file would be in, not a file
    EXPECT_TRUE(isInsideProject("textures/"));
}

TEST(AndroidProjectLayout, ClassifiesWhereTheProjectIs) {
    const std::string internal = "/data/user/0/com.yourcompany.project/files";
    const std::string external = "/storage/emulated/0/Android/data/com.yourcompany.project/files";

    ProjectLocation inside = classifyLocation(internal + "/Projects/Game", internal, external);
    EXPECT_EQ(inside.place, ProjectPlace::AppPrivate);
    const ProjectAccess insideAccess = accessFor(inside);
    EXPECT_TRUE(insideAccess.canWrite);
    EXPECT_TRUE(insideAccess.atomicRename);
    EXPECT_FALSE(insideAccess.survivesUninstall) << "uninstalling deletes it";
    EXPECT_FALSE(insideAccess.warning.empty());

    ProjectLocation externalProject = classifyLocation(external + "/Game", internal, external);
    EXPECT_EQ(externalProject.place, ProjectPlace::AppExternal);
    EXPECT_TRUE(accessFor(externalProject).userVisible);
    // the external root itself, and not a folder whose name only starts the same
    EXPECT_EQ(classifyLocation(external, internal, external).place, ProjectPlace::AppExternal);
    EXPECT_NE(classifyLocation("/storage/emulated/0/Android/data/other.app/files/Game", internal, external).place,
              ProjectPlace::AppExternal);

    // a picked tree has no path of its own: the grant is what it has
    ProjectLocation picked = classifyLocation("", internal, external, "content://com.android.externalstorage.documents/tree/primary%3AGames");
    EXPECT_EQ(picked.place, ProjectPlace::UserPicked);
    const ProjectAccess pickedAccess = accessFor(picked);
    EXPECT_TRUE(pickedAccess.canOpen);
    EXPECT_TRUE(pickedAccess.canWrite);
    EXPECT_TRUE(pickedAccess.survivesUninstall);
    EXPECT_FALSE(pickedAccess.atomicRename) << "a provider replaces a file by copying over it";

    // a phone's shared storage without a grant
    ProjectLocation shared = classifyLocation("/storage/emulated/0/Documents/Game", internal, external);
    EXPECT_EQ(shared.place, ProjectPlace::SharedReadOnly);
    EXPECT_TRUE(accessFor(shared).canOpen);
    EXPECT_FALSE(accessFor(shared).canWrite);
}

TEST(AndroidProjectLayout, WarnsAboutARemovableVolume) {
    ProjectLocation location;
    location.place = ProjectPlace::UserPicked;
    location.removable = true;
    const ProjectAccess access = accessFor(location);
    EXPECT_TRUE(access.canWrite);
    EXPECT_NE(access.warning.find("removable"), std::string::npos) << access.warning;
}

TEST(AndroidProjectLayout, RecognizesProjectCachesAndTheImportFolder) {
    EXPECT_TRUE(isProjectCacheFolder("Library"));
    EXPECT_TRUE(isProjectCacheFolder("library"));
    EXPECT_TRUE(isProjectCacheFolder("Temp"));
    EXPECT_TRUE(isProjectCacheFolder(".git"));
    EXPECT_FALSE(isProjectCacheFolder("assets"));
    EXPECT_FALSE(isProjectCacheFolder("scripts"));

    ProjectLayout layout;
    EXPECT_EQ(importFolderOf(layout), "") << "the default layout is the root";
    layout.assetsDir = "assets";
    EXPECT_EQ(importFolderOf(layout), "assets");
    EXPECT_TRUE(isOpenableProject(true));
    EXPECT_FALSE(isOpenableProject(false));
}

// ---- image limits

namespace {

    // a PNG header with the given size and colour type (6: RGBA)
    // a PNG: the signature and a whole IHDR chunk (length, type, 13 bytes of data, CRC). The CRC
    // is not checked by the orientation reader, but its four bytes must be there or every chunk
    // after it is read four bytes early.
    std::vector<uint8_t> pngHeader(uint32_t width, uint32_t height, int colorType, int bitDepth = 8) {
        std::vector<uint8_t> bytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        const uint8_t chunk[25] = {0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                   (uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
                                   (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
                                   (uint8_t)bitDepth, (uint8_t)colorType, 0, 0, 0, 0, 0, 0, 0};
        bytes.insert(bytes.end(), chunk, chunk + sizeof(chunk));
        return bytes;
    }

    // a JPEG with an APP0 segment, then SOF0 of the given size
    std::vector<uint8_t> jpegHeader(uint16_t width, uint16_t height, int channels) {
        std::vector<uint8_t> bytes = {0xFF, 0xD8,
                                      // APP0, length 4 (this is not a valid JFIF body; the walk only reads lengths)
                                      0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00,
                                      // SOF0, length 11 + 3 * channels
                                      0xFF, 0xC0, 0x00, (uint8_t)(8 + 3 * channels), 8,
                                      (uint8_t)(height >> 8), (uint8_t)height,
                                      (uint8_t)(width >> 8), (uint8_t)width,
                                      (uint8_t)channels};
        for (int i = 0; i < channels; i++) {
            bytes.push_back(1);
            bytes.push_back(0x11);
            bytes.push_back(0);
        }
        return bytes;
    }

    // a BMP with a 40-byte DIB header
    std::vector<uint8_t> bmpHeader(int32_t width, int32_t height) {
        std::vector<uint8_t> bytes(54, 0);
        bytes[0] = 'B';
        bytes[1] = 'M';
        const uint32_t dib = 40;
        bytes[14] = (uint8_t)dib;
        bytes[18] = (uint8_t)width;
        bytes[19] = (uint8_t)(width >> 8);
        bytes[20] = (uint8_t)(width >> 16);
        bytes[21] = (uint8_t)(width >> 24);
        const uint32_t h = (uint32_t)height;
        bytes[22] = (uint8_t)h;
        bytes[23] = (uint8_t)(h >> 8);
        bytes[24] = (uint8_t)(h >> 16);
        bytes[25] = (uint8_t)(h >> 24);
        bytes[28] = 24;
        return bytes;
    }

}

TEST(AndroidImageLimits, ReadsPngJpegBmpGifPsdAndWebp) {
    ImageSize png = readImageSize(pngHeader(1080, 1920, 6));
    ASSERT_TRUE(png.known);
    EXPECT_EQ(png.format, ImageFormat::Png);
    EXPECT_EQ(png.width, 1080);
    EXPECT_EQ(png.height, 1920);
    EXPECT_EQ(png.channels, 4);
    EXPECT_EQ(png.decodedBytes(), 1080ull * 1920ull * 4ull);

    // colour type 2 has no alpha before a decoder expands it: three channels, not four
    EXPECT_EQ(readImageSize(pngHeader(64, 64, 2)).channels, 3);
    // a 16-bit PNG takes twice the bytes
    const ImageSize deep = readImageSize(pngHeader(64, 64, 6, 16));
    EXPECT_EQ(deep.bitDepth, 16);
    EXPECT_EQ(deep.decodedBytes(), 64ull * 64ull * 4ull * 2ull);

    ImageSize jpeg = readImageSize(jpegHeader(4000, 3000, 3));
    ASSERT_TRUE(jpeg.known);
    EXPECT_EQ(jpeg.format, ImageFormat::Jpeg);
    EXPECT_EQ(jpeg.width, 4000);
    EXPECT_EQ(jpeg.height, 3000);
    EXPECT_EQ(jpeg.channels, 3);

    // an EXIF segment before the SOF is walked over, not parsed
    std::vector<uint8_t> withExif = {0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x10};
    withExif.resize(2 + 2 + 0x10, 0);
    const std::vector<uint8_t> sof = jpegHeader(20, 10, 3);
    withExif.insert(withExif.end(), sof.begin() + 2, sof.end());
    EXPECT_EQ(readImageSize(withExif).width, 20);

    // a top-down BMP has a negative height
    const ImageSize down = readImageSize(bmpHeader(100, -50));
    ASSERT_TRUE(down.known);
    EXPECT_EQ(down.format, ImageFormat::Bmp);
    EXPECT_EQ(down.width, 100);
    EXPECT_EQ(down.height, 50) << "the sign says the rows are stored top down, not a negative size";

    std::vector<uint8_t> gif = {'G', 'I', 'F', '8', '9', 'a', 0x20, 0x03, 0x58, 0x02};
    const ImageSize gifSize = readImageSize(gif);
    ASSERT_TRUE(gifSize.known);
    EXPECT_EQ(gifSize.width, 0x0320);
    EXPECT_EQ(gifSize.height, 0x0258);

    std::vector<uint8_t> psd(26, 0);
    psd[0] = '8'; psd[1] = 'B'; psd[2] = 'P'; psd[3] = 'S';
    // height then width, big endian
    psd[14] = 0; psd[15] = 0; psd[16] = 0x07; psd[17] = 0x08;
    psd[18] = 0; psd[19] = 0; psd[20] = 0x0F; psd[21] = 0x00;
    const ImageSize psdSize = readImageSize(psd);
    ASSERT_TRUE(psdSize.known);
    EXPECT_EQ(psdSize.width, 3840);
    EXPECT_EQ(psdSize.height, 1800);
}

TEST(AndroidImageLimits, UnreadableHeadersAreUnknownNotGuessed) {
    EXPECT_FALSE(readImageSize({}).known);
    EXPECT_FALSE(readImageSize(textBytes("not an image at all, really not")).known);
    // a PNG whose IHDR is cut off
    std::vector<uint8_t> cut = pngHeader(100, 100, 6);
    cut.resize(20);
    EXPECT_FALSE(readImageSize(cut).known) << "a truncated header is not a size";
    // a zero size, and one past int
    EXPECT_FALSE(readImageSize(pngHeader(0, 100, 6)).known);
    // a JPEG with no SOF before the scan
    EXPECT_FALSE(readImageSize(bytesOf({0xFF, 0xD8, 0xFF, 0xDA, 0x00, 0x04, 0, 0})).known);
    // a random file whose byte 2 happens to be a TGA image type but with no size is not a TGA
    std::vector<uint8_t> random(18, 0);
    random[2] = 2;
    EXPECT_FALSE(readImageSize(random).known);
}

TEST(AndroidImageLimits, AHundredMegapixelPhotoIsDownscaledNotFatal) {
    // a 12000x9000 camera mode: 432 MB as RGBA8, against a 256 MB budget
    const ImageSize size = readImageSize(pngHeader(12000, 9000, 6));
    ASSERT_TRUE(size.known);
    EXPECT_EQ(size.decodedBytes(), 432ull * 1000ull * 1000ull);

    ImageBudget budget;
    budget.decodedBytesLimit = 256ull * 1024ull * 1024ull;
    // the texture limit is 4096, which halves the 12000 px side to 3000 anyway
    const ImageDecision decision = decideImageImport(size, budget);
    EXPECT_EQ(decision.verdict, ImageVerdict::Downscale);
    EXPECT_EQ(decision.scale, 0.25) << "two halvings fit both limits";
    EXPECT_EQ(decision.width, 3000);
    EXPECT_EQ(decision.height, 2250);
    EXPECT_LE((uint64_t)decision.width * decision.height * 4, budget.decodedBytesLimit);
    EXPECT_FALSE(decision.message.empty());

    // a size that fits both limits is taken as it is
    const ImageDecision ok = decideImageImport(readImageSize(pngHeader(2048, 1024, 4)), budget);
    EXPECT_EQ(ok.verdict, ImageVerdict::Allowed);
    EXPECT_EQ(ok.scale, 1.0);
    EXPECT_EQ(ok.width, 2048);
    EXPECT_TRUE(ok.message.empty()) << "nothing to tell the user";
}

TEST(AndroidImageLimits, RefusesWhatNoDownscaleSaves) {
    ImageBudget budget;
    budget.decodedBytesLimit = 64ull * 1024ull * 1024ull;
    // 300 MP is past the engine's own ceiling, whatever the memory
    const ImageSize huge = readImageSize(pngHeader(20000, 15000, 6));
    EXPECT_EQ(decideImageImport(huge, budget).verdict, ImageVerdict::Refused);

    // 8000x8000 fits the pixel ceiling but a 64 MB budget needs a 1/4 (16 MB) and a texture
    // limit of 2048 needs a 1/4 too: a downscale, not a refusal
    ImageBudget small;
    small.decodedBytesLimit = 16ull * 1024ull * 1024ull;
    small.maxTextureSize = 2048;
    const ImageDecision decision = decideImageImport(readImageSize(pngHeader(8000, 8000, 6)), small);
    EXPECT_EQ(decision.verdict, ImageVerdict::Downscale);
    EXPECT_EQ(decision.width, 2000);

    // a 16-bit pixel takes twice the memory for the same dimensions
    ImageBudget tight;
    tight.decodedBytesLimit = 8ull * 1024ull * 1024ull;
    tight.maxTextureSize = 8192;
    EXPECT_EQ(decideImageImport(readImageSize(pngHeader(2048, 2048, 6, 8)), tight).verdict, ImageVerdict::Downscale);
    EXPECT_EQ(decideImageImport(readImageSize(pngHeader(2048, 2048, 6, 16)), tight).scale, 0.5);
}

TEST(AndroidImageLimits, NoBudgetMeansNoRefusal) {
    ImageBudget none;   // a platform that did not say how much memory it has
    const ImageDecision decision = decideImageImport(readImageSize(pngHeader(4096, 4096, 6)), none);
    EXPECT_EQ(decision.verdict, ImageVerdict::Allowed) << "20 MB of 4096x4096 fits the default texture limit";

    // an unknown size is the decoder's business, as before
    EXPECT_EQ(decideImageImport(ImageSize(), none).verdict, ImageVerdict::Allowed);

    // but the texture limit alone still applies when there is no byte limit
    ImageDecision limited = decideImageImport(readImageSize(pngHeader(6000, 6000, 6)), ImageBudget{0, 2048, 0});
    EXPECT_EQ(limited.verdict, ImageVerdict::Downscale);
    EXPECT_EQ(limited.width, 1500);
}

TEST(AndroidImageLimits, ExtremesDoNotOverflow) {
    // a header claiming the largest values INT allows: the product must not wrap
    ImageSize absurd;
    absurd.known = true;
    absurd.width = 0x7FFFFFFF;
    absurd.height = 0x7FFFFFFF;
    absurd.channels = 4;
    // (2^31-1)^2 * 4 is just inside 64 bits; a wider one saturates instead of wrapping small
    EXPECT_GT(absurd.decodedBytes(), UINT64_MAX / 2) << "a wrapped product would be a small number";
    absurd.width = 0x7FFFFFFF;
    absurd.height = 0x7FFFFFFF;
    absurd.bitDepth = 16;
    EXPECT_EQ(absurd.decodedBytes(), UINT64_MAX) << "saturation, not a wrap";
    EXPECT_EQ(decideImageImport(absurd, ImageBudget{1024, 4096, 0}).verdict, ImageVerdict::Refused);
}

// ---- what the engine can decode and play

TEST(AndroidAssetImport, WebPIsAcceptedButFlaggedAsUndecodable) {
    const auto bytes = textBytes(std::string("RIFF\0\0\0\0WEBPVP8 ", 16));
    // the type is right, so the file lands in the assets folder as a texture...
    EXPECT_EQ(assetTypeForImport("photo.webp", bytes), AssetType::Texture);
    // ...but this build has no WebP decoder, and the import says so before anything is copied
    const ImageSupportAnswer answer = decodeSupportOf("photo.webp", bytes, true);
    EXPECT_EQ(answer.support, ImageSupport::NeedsConversion) << "Android's BitmapFactory decodes WebP";
    EXPECT_EQ(answer.format, ImageFormat::Webp);
    ASSERT_NE(answer.convertTo, nullptr);
    EXPECT_STREQ(answer.convertTo, "png");
    EXPECT_FALSE(answer.reason.empty());

    // on a host with no platform decoder the same file is a conversion too, not a silent accept
    EXPECT_EQ(decodeSupportOf("photo.webp", bytes, false).support, ImageSupport::NeedsConversion);
}

TEST(AndroidAssetImport, TheFormatsTheEngineReadsAreSupported) {
    EXPECT_EQ(decodeSupportOf("a.png", bytesOf({0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A}), true).support,
              ImageSupport::Supported);
    EXPECT_EQ(decodeSupportOf("a.jpg", bytesOf({0xFF, 0xD8, 0xFF}), true).support, ImageSupport::Supported);
    EXPECT_EQ(decodeSupportOf("a.gif", textBytes("GIF89a"), true).support, ImageSupport::Supported);
    EXPECT_EQ(decodeSupportOf("a.psd", textBytes("8BPS"), true).support, ImageSupport::Supported);
    EXPECT_EQ(decodeSupportOf("a.hdr", textBytes("#?RADIANCE"), true).support, ImageSupport::Supported);
    EXPECT_EQ(decodeSupportOf("a.ktx2", bytesOf({0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30}), true).support,
              ImageSupport::Supported);
    // a text file, or a container with no magic of its own: the decoder decides, as before
    EXPECT_EQ(decodeSupportOf("a.json", textBytes("{}"), true).support, ImageSupport::Supported);
    // a format nothing reads
    EXPECT_EQ(decodeSupportOf("a.avif", bytesOf({0, 0, 0, 0x1C, 'f', 't', 'y', 'p', 'a', 'v', 'i', 'f'}), true).support,
              ImageSupport::Unsupported);
}

TEST(AndroidAssetImport, AudioSupportFollowsTheDecoderSet) {
    // dr_wav, stb_vorbis, dr_mp3 and dr_flac are all linked
    EXPECT_EQ(playbackSupportOf("a.wav", textBytes(std::string("RIFF\0\0\0\0WAVEfmt ", 16)), false).support,
              MediaSupport::Supported);
    EXPECT_EQ(playbackSupportOf("a.ogg", textBytes(std::string("OggS\0\0\0\0", 8)), false).support,
              MediaSupport::Supported);
    EXPECT_EQ(playbackSupportOf("a.mp3", textBytes("ID3"), false).support, MediaSupport::Supported);
    EXPECT_EQ(playbackSupportOf("a.flac", bytesOf({0x66, 0x4C, 0x61, 0x43}), false).support,
              MediaSupport::Supported);
    // an extension-less recording whose bytes read as one of the four
    EXPECT_EQ(playbackSupportOf("recording", bytesOf({0x66, 0x4C, 0x61, 0x43}), false).support,
              MediaSupport::Supported);

    // a voice memo a phone's share sheet offers: no engine decoder
    const MediaSupportAnswer aac = playbackSupportOf("memo.m4a", bytesOf({0, 0, 0, 0x20, 'f', 't', 'y', 'p', 'M', '4', 'A'}), true);
    EXPECT_EQ(aac.support, MediaSupport::NeedsConversion);
    ASSERT_NE(aac.convertTo, nullptr);
    EXPECT_STREQ(aac.convertTo, "wav");
    // and without the platform's codecs it is refused with a sentence that says what to export
    const MediaSupportAnswer refused = playbackSupportOf("memo.m4a", bytesOf({0, 0, 0, 0x20, 'f', 't', 'y', 'p'}), false);
    EXPECT_EQ(refused.support, MediaSupport::Unsupported);
    EXPECT_NE(refused.reason.find("WAV"), std::string::npos) << refused.reason;
}

// ---- EXIF orientation

#include "AndroidImageOrientation.h"

namespace {

    // a TIFF header (little or big endian) with IFD0 holding one Orientation entry
    std::vector<uint8_t> tiffWithOrientation(uint16_t value, bool little = true, uint16_t tag = 0x0112) {
        std::vector<uint8_t> bytes;
        auto push16 = [&bytes, little](uint16_t v) {
            if (little) { bytes.push_back((uint8_t)v); bytes.push_back((uint8_t)(v >> 8)); }
            else { bytes.push_back((uint8_t)(v >> 8)); bytes.push_back((uint8_t)v); }
        };
        auto push32 = [&bytes, little](uint32_t v) {
            if (little) { bytes.push_back((uint8_t)v); bytes.push_back((uint8_t)(v >> 8)); bytes.push_back((uint8_t)(v >> 16)); bytes.push_back((uint8_t)(v >> 24)); }
            else { bytes.push_back((uint8_t)(v >> 24)); bytes.push_back((uint8_t)(v >> 16)); bytes.push_back((uint8_t)(v >> 8)); bytes.push_back((uint8_t)v); }
        };
        bytes.push_back(little ? 'I' : 'M');
        bytes.push_back(little ? 'I' : 'M');
        push16(42);
        push32(8);          // IFD0 at 8
        push16(1);          // one entry
        push16(tag);        // tag (0x0112 encodes as 12 01 little endian, 01 12 big)
        push16(3);          // SHORT
        push32(1);          // count 1
        push16(value);      // the value, in the first two bytes of the field
        push16(0);
        push32(0);          // no next IFD
        return bytes;
    }

    // a JPEG: SOI, an APP1 "Exif" segment around that TIFF, then an SOF0 and a scan
    std::vector<uint8_t> jpegWithOrientation(uint16_t value, bool little = true) {
        const std::vector<uint8_t> tiff = tiffWithOrientation(value, little);
        std::vector<uint8_t> bytes = {0xFF, 0xD8, 0xFF, 0xE1};
        const uint16_t length = (uint16_t)(2 + 6 + tiff.size());
        bytes.push_back((uint8_t)(length >> 8));
        bytes.push_back((uint8_t)length);
        bytes.insert(bytes.end(), {'E', 'x', 'i', 'f', 0, 0});
        bytes.insert(bytes.end(), tiff.begin(), tiff.end());
        const std::vector<uint8_t> sof = {0xFF, 0xC0, 0x00, 0x0B, 8, 0x00, 0x10, 0x00, 0x20, 3, 1, 0x11, 0};
        bytes.insert(bytes.end(), sof.begin(), sof.end());
        bytes.insert(bytes.end(), {0xFF, 0xDA, 0x00, 0x02});
        return bytes;
    }

    // a PNG: signature, IHDR, an eXIf chunk with the TIFF, IEND
    std::vector<uint8_t> pngWithOrientation(uint16_t value) {
        // pngHeader already ends with the IHDR's CRC, so a chunk can follow it directly
        std::vector<uint8_t> bytes = pngHeader(64, 32, 6);
        const std::vector<uint8_t> tiff = tiffWithOrientation(value);
        const uint32_t length = (uint32_t)tiff.size();
        bytes.push_back((uint8_t)(length >> 24)); bytes.push_back((uint8_t)(length >> 16));
        bytes.push_back((uint8_t)(length >> 8)); bytes.push_back((uint8_t)length);
        bytes.insert(bytes.end(), {'e', 'X', 'I', 'f'});
        bytes.insert(bytes.end(), tiff.begin(), tiff.end());
        bytes.insert(bytes.end(), {0, 0, 0, 0});     // CRC: not checked here
        bytes.insert(bytes.end(), {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82});
        return bytes;
    }

}

TEST(AndroidImageOrientation, ReadsTheTagFromJpegAndPng) {
    EXPECT_EQ(readOrientation(jpegWithOrientation(6)), 6u) << "the common portrait phone photo";
    EXPECT_EQ(readOrientation(jpegWithOrientation(3)), 3u);
    EXPECT_EQ(readOrientation(jpegWithOrientation(1)), 1u) << "a tag that says upright is still read";
    // big-endian EXIF (a camera, not a phone)
    EXPECT_EQ(readOrientation(jpegWithOrientation(8, false)), 8u);
    EXPECT_EQ(readOrientation(pngWithOrientation(7)), 7u);

    // a screenshot has no EXIF at all
    EXPECT_EQ(readOrientation(jpegHeader(100, 100, 3)), 0u);
    EXPECT_EQ(readOrientation(pngHeader(100, 100, 6)), 0u);
    EXPECT_EQ(readOrientation({}), 0u);
    EXPECT_EQ(readOrientation(textBytes("RIFF....WEBP")), 0u) << "other formats have no tag here";
}

TEST(AndroidImageOrientation, AMalformedTagIsIgnoredNotFatal) {
    // the tag with the wrong type (LONG instead of SHORT)
    std::vector<uint8_t> wrongType = tiffWithOrientation(6);
    wrongType[8 + 2 + 2 + 1] = 4;   // type = 4
    EXPECT_EQ(readOrientation(jpegWithOrientation(6).size() ? [&] {
        std::vector<uint8_t> bytes = {0xFF, 0xD8, 0xFF, 0xE1};
        const uint16_t length = (uint16_t)(2 + 6 + wrongType.size());
        bytes.push_back((uint8_t)(length >> 8));
        bytes.push_back((uint8_t)length);
        bytes.insert(bytes.end(), {'E', 'x', 'i', 'f', 0, 0});
        bytes.insert(bytes.end(), wrongType.begin(), wrongType.end());
        return bytes;
    }() : std::vector<uint8_t>()), 0u);

    // an IFD offset past the segment
    std::vector<uint8_t> badOffset = tiffWithOrientation(6);
    badOffset[4] = 0xFF; badOffset[5] = 0xFF; badOffset[6] = 0xFF; badOffset[7] = 0x7F;
    std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE1, 0, 0};
    const uint16_t length = (uint16_t)(2 + 6 + badOffset.size());
    jpeg[4] = (uint8_t)(length >> 8); jpeg[5] = (uint8_t)length;
    jpeg.insert(jpeg.end(), {'E', 'x', 'i', 'f', 0, 0});
    jpeg.insert(jpeg.end(), badOffset.begin(), badOffset.end());
    EXPECT_EQ(readOrientation(jpeg), 0u);

    // a value outside 1..8
    EXPECT_EQ(orientationFromValue(9), Orientation::Unknown);
    EXPECT_EQ(orientationFromValue(0), Orientation::Unknown);
    EXPECT_EQ(orientationFromValue(1), Orientation::Normal);
}

TEST(AndroidImageOrientation, EveryValueHasATransform) {
    // 1 and 2..4 alone need no swap; 5..8 do
    const bool swap[9] = {false, false, false, false, false, true, true, true, true};
    const int rotation[9] = {0, 0, 0, 180, 0, 90, 90, 90, 270};
    const bool needed[9] = {false, false, true, true, true, true, true, true, true};
    for (uint32_t value = 1; value <= 8; value++) {
        const OrientationTransform transform = transformFor(orientationFromValue(value));
        EXPECT_EQ(transform.swapDimensions, swap[value]) << "orientation " << value;
        EXPECT_EQ(transform.rotateClockwise, rotation[value]) << "orientation " << value;
        EXPECT_EQ(transform.needed, needed[value]) << "orientation " << value;
    }
    // 1 and an absent tag both leave the pixels alone, and isUpright says so
    EXPECT_TRUE(isUpright(Orientation::Normal));
    EXPECT_TRUE(isUpright(Orientation::Unknown));
    EXPECT_FALSE(isUpright(Orientation::Rotate90));

    // 2 is a mirror only, 4 a vertical mirror only, 5 and 7 mirror one way with a rotation
    EXPECT_TRUE(transformFor(Orientation::MirrorHorizontal).flipHorizontal);
    EXPECT_EQ(transformFor(Orientation::MirrorHorizontal).rotateClockwise, 0);
    EXPECT_TRUE(transformFor(Orientation::MirrorVertical).flipVertical);
    EXPECT_TRUE(transformFor(Orientation::Transpose).flipHorizontal);
    EXPECT_TRUE(transformFor(Orientation::Transverse).flipVertical);
}

TEST(AndroidImageOrientation, ARotatedPhotoReportsSwappedDimensions) {
    // a portrait phone photo is stored 4000x3000 with Orientation 6 and shown 3000x4000
    const OrientedSize shown = orientedSize(4000, 3000, Orientation::Rotate90);
    EXPECT_TRUE(shown.swapped);
    EXPECT_EQ(shown.width, 3000);
    EXPECT_EQ(shown.height, 4000);

    // a mirror does not change the size
    const OrientedSize mirrored = orientedSize(4000, 3000, Orientation::MirrorHorizontal);
    EXPECT_FALSE(mirrored.swapped);
    EXPECT_EQ(mirrored.width, 4000);
    EXPECT_EQ(mirrored.height, 3000);

    // and a 180 turn does not either
    const OrientedSize turned = orientedSize(4000, 3000, Orientation::Rotate180);
    EXPECT_EQ(turned.width, 4000);
    EXPECT_EQ(turned.height, 3000);
}

TEST(AndroidImportPreflight, APortraitPhotoSaysHowItIsTurnedAndHowBigADecodeIs) {
    PreflightInput photo;
    photo.fileName = "DCIM/IMG_4821.JPG";
    photo.prefix = jpegWithOrientation(6);
    const PreflightFinding finding = preflightOne(photo, PreflightOptions{});
    EXPECT_EQ(finding.type, AssetType::Texture);
    EXPECT_EQ(finding.assetName, "img_4821.jpg") << "the name is what the import calls it, not what the camera did";
    EXPECT_EQ(finding.orientation, 6u);
    EXPECT_EQ(finding.severity, PreflightSeverity::Note);
    EXPECT_NE(finding.message.find("turned 90 clockwise"), std::string::npos) << finding.message;
    EXPECT_NE(finding.message.find("turned upright on import"), std::string::npos) << finding.message;
}

TEST(AndroidImportPreflight, AWebpIsBrokenWithoutThePlatformDecoderAndANoteWithIt) {
    const std::vector<uint8_t> webp = {'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
    PreflightInput file;
    file.fileName = "UI/icon.webp";
    file.prefix = webp;

    // no platform decoder: the editor accepted the extension, the engine has no decoder
    const PreflightFinding without = preflightOne(file, PreflightOptions{});
    EXPECT_EQ(without.severity, PreflightSeverity::Broken);
    EXPECT_NE(without.message.find("WebP"), std::string::npos) << without.message;

    // BitmapFactory converts it, so it draws, at the cost of a conversion on import
    PreflightOptions bitmap;
    bitmap.androidBitmapFactory = true;
    const PreflightFinding with = preflightOne(file, bitmap);
    EXPECT_EQ(with.severity, PreflightSeverity::Note);
    EXPECT_NE(with.message.find("converted to png"), std::string::npos) << with.message;
}

TEST(AndroidImportPreflight, AHugePhotoIsReducedAndOnePastEveryLimitIsRefused) {
    PreflightOptions options;
    options.budget.maxTextureSize = 4096;
    options.budget.decodedBytesLimit = 0;

    // 8000x6000 is over 4096 on both sides: 1/2 each side is 4000x3000, which fits
    PreflightInput big;
    big.fileName = "art/backdrop.png";
    big.prefix = pngHeader(8000, 6000, 6);
    const PreflightFinding reduced = preflightOne(big, options);
    EXPECT_EQ(reduced.severity, PreflightSeverity::Note);
    EXPECT_DOUBLE_EQ(reduced.scale, 0.5);
    EXPECT_EQ(reduced.importedWidth, 4000);
    EXPECT_EQ(reduced.importedHeight, 3000);
    EXPECT_NE(reduced.message.find("reduced from 8000x6000 to 4000x3000"), std::string::npos) << reduced.message;
    // 8000x6000 at the PNG's 3 channels is 183.1 MB: a phone cannot do that next to its own
    // textures, and the report says so in the units a person acts on
    EXPECT_NE(reduced.message.find("183.1 MB"), std::string::npos) << reduced.message;

    // past the format's own ceiling: refused, and the reason says so instead of "it did not load"
    PreflightInput absurd;
    absurd.fileName = "art/scan.png";
    absurd.prefix = pngHeader(20000, 20000, 6);
    const PreflightFinding refused = preflightOne(absurd, options);
    EXPECT_EQ(refused.severity, PreflightSeverity::Broken);
    EXPECT_NE(refused.message.find("20000x20000"), std::string::npos) << refused.message;
}

TEST(AndroidImportPreflight, ASecondFileWithTheSameNameIsReportedUnderTheNameItGets) {
    std::vector<std::string> taken = {"icon.png"};
    PreflightOptions options;
    options.takenNames = &taken;

    PreflightInput second;
    second.fileName = "downloads/Icon.PNG";
    second.prefix = pngHeader(64, 64, 6);
    const PreflightFinding finding = preflightOne(second, options);
    EXPECT_EQ(finding.assetName, "icon (1).png") << "the report names the collision's outcome, not its input";
}

TEST(AndroidImportPreflight, TheWholeProjectGetsOneLineThatNamesTheFirstThingToFix) {
    std::vector<PreflightInput> files(4);
    files[0].fileName = "art/photo.jpg";
    files[0].prefix = jpegWithOrientation(1);
    files[1].fileName = "art/huge.png";
    files[1].prefix = pngHeader(8000, 6000, 6);
    files[2].fileName = "ui/broken.webp";
    files[2].prefix = {'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'E', 'B', 'P', 'V', 'P', '8', ' '};
    files[3].fileName = "audio/song.mp3";
    files[3].prefix = {'I', 'D', '3', 4, 0, 0, 0, 0, 0, 0};

    const PreflightReport report = preflightImports(files, PreflightOptions{});
    EXPECT_EQ(report.findings.size(), 4u);
    EXPECT_EQ(report.broken, 1);
    EXPECT_NE(report.summary.find("broken.webp"), std::string::npos) << report.summary;
    EXPECT_EQ(report.findings[0].severity, PreflightSeverity::Fine) << "an upright photo with no limit hit is not a note";
    EXPECT_EQ(report.findings[3].severity, PreflightSeverity::Fine) << "an mp3 is what the engine plays";
    EXPECT_GT(report.largestDecodedBytes, 0u);
}

TEST(AndroidImportPreflight, AFileThatIsNotAnAssetIsCopiedAndNotReportedAsBroken) {
    PreflightInput notes;
    notes.fileName = "docs/readme.md";
    notes.prefix = {'#', ' ', 'h', 'i'};
    const PreflightFinding finding = preflightOne(notes, PreflightOptions{});
    EXPECT_EQ(finding.type, AssetType::Unknown);
    EXPECT_EQ(finding.severity, PreflightSeverity::Fine);
    EXPECT_EQ(finding.message, "not an asset type this engine imports; it is copied as it is");
}

// ---- model sidecars

TEST(AndroidAssetImport, AWindowsDeviceNameIsMadeOneInsteadOfBreakingTheProjectOnAWindowsMachine) {
    // "con.png" imports and plays on the phone; on Windows it cannot be created, copied or
    // deleted, so the project opens with a missing asset and nothing on the phone showed it
    for (const char* name : {"con.png", "NUL.PNG", "aux.png", "PRN.txt", "com1.png", "LPT9.png"}) {
        const std::string sanitized = sanitizeFileName(name);
        EXPECT_EQ(sanitized.front(), '_') << name << " -> " << sanitized;
        expectLegalName(sanitized);
    }
    // and a name that merely starts with one is not reserved
    EXPECT_EQ(sanitizeFileName("console.png"), "console.png");
    EXPECT_EQ(sanitizeFileName("com10.png"), "com10.png") << "COM10 is an ordinary name";
    EXPECT_FALSE(isWindowsDeviceName("CONSOLE"));
}

TEST(AndroidAssetImport, APhoneFormatIsClassifiedByItsExtensionWhenTheMagicIsNotEnough) {
    // .m4a is ISO-BMFF, which many containers share: calling it Unknown said "copied as it is"
    // instead of "will not play", and made playbackSupportOf unreachable for the format it exists
    // for (found by tools/android-asset-report.py on a real folder)
    const std::vector<uint8_t> ftyp = {0, 0, 0, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' ', 0, 0, 0, 0};
    EXPECT_EQ(assetTypeForImport("clip.m4a", ftyp), AssetType::Audio);
    EXPECT_EQ(assetTypeForImport("clip.aac", {}), AssetType::Audio);
    EXPECT_EQ(assetTypeForImport("voice.opus", {}), AssetType::Audio);
    // the same for the photo formats a share sheet offers without their magic in reach
    EXPECT_EQ(assetTypeForImport("icon.webp", {}), AssetType::Texture);
    EXPECT_EQ(assetTypeForImport("IMG_0001.heic", {}), AssetType::Texture);
    EXPECT_EQ(assetTypeForImport("IMG_0001.avif", {}), AssetType::Texture);
}

TEST(AndroidModelSidecars, AGltfNamesTheFilesAPhoneCannotBringAlong) {
    const std::string text = R"({"asset":{"version":"2.0"},
        "buffers":[{"uri":"Body.bin","byteLength":1024}],
        "images":[{"uri":"textures/skin.png"},{"uri":"textures/skin.png"}],
        "meshes":[]})";
    const SidecarReport report = gltfSidecars(text);
    EXPECT_TRUE(report.parsed);
    EXPECT_EQ(report.externalBuffers, 1);
    EXPECT_EQ(report.externalImages, 2);
    EXPECT_FALSE(report.isSelfContained());

    // the same texture named twice is one file to bring
    const std::vector<std::string> required = report.requiredFiles();
    ASSERT_EQ(required.size(), 2u);
    EXPECT_EQ(required[0], "Body.bin");
    EXPECT_EQ(required[1], "textures/skin.png");
    EXPECT_NE(sidecarSentence(report).find("1 buffer file and 2 texture files"), std::string::npos)
        << sidecarSentence(report);
}

TEST(AndroidModelSidecars, AGlbOrACookedGltfNeedsNothingElse) {
    // no buffers at all: nothing is external
    EXPECT_TRUE(gltfSidecars(R"({"asset":{"version":"2.0"},"meshes":[]})").isSelfContained());

    // a buffer with no URI and an image in a bufferView are the GLB's own chunks
    const SidecarReport glb = gltfSidecars(R"({"asset":{"version":"2.0"},
        "buffers":[{"byteLength":1024}],"images":[{"bufferView":0,"mimeType":"image/png"}]})");
    EXPECT_TRUE(glb.parsed);
    EXPECT_TRUE(glb.isSelfContained());
    EXPECT_TRUE(glb.requiredFiles().empty());

    // a data: URI carries its bytes in the file, so it is not something to bring
    const SidecarReport inline_data = gltfSidecars(R"({"buffers":[{"uri":"data:application/octet-stream;base64,AA=="}]})");
    EXPECT_TRUE(inline_data.isSelfContained());
    EXPECT_EQ(inline_data.sidecars.size(), 1u);
    EXPECT_TRUE(inline_data.sidecars[0].embedded);
}

TEST(AndroidModelSidecars, AnEmptyUriIsNotAFileToBring) {
    // found by fuzzing: "uri": "" listed an empty name, which a user cannot act on
    const SidecarReport report = gltfSidecars(R"({"buffers":[{"uri":""}],"images":[{"uri":""}]})");
    EXPECT_TRUE(report.parsed);
    EXPECT_TRUE(report.requiredFiles().empty());
    EXPECT_TRUE(report.isSelfContained());
}

TEST(AndroidModelSidecars, AMalformedGltfSaysSoInsteadOfClaimingNoDependencies) {
    const SidecarReport report = gltfSidecars(R"({"buffers":[)");
    EXPECT_FALSE(report.parsed);
    EXPECT_FALSE(report.error.empty());
    EXPECT_FALSE(sidecarSentence(report).empty());
    EXPECT_NE(sidecarSentence(report).find("malformed"), std::string::npos);
}

TEST(AndroidModelSidecars, AUriIsNotAPath) {
    // percent-encoded UTF-8, a query string, and a backslash a Windows exporter left behind
    EXPECT_EQ(detail::sidecarFileName("textures/skin%20map.png"), "skin map.png")
        << "the name the report gives must be the name the file has";
    EXPECT_EQ(detail::sidecarFileName("Body.bin?v=2#frag"), "Body.bin");
    EXPECT_EQ(detail::sidecarFileName("textures\\skin.png"), "skin.png");
    EXPECT_EQ(detail::sidecarFileName("plain.png"), "plain.png");
}

TEST(AndroidImportPreflight, AGltfThatNeedsItsBuffersIsANoteThatNamesThemAndAMalformedOneIsBroken) {
    const std::string gltf = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Body.bin"}],"images":[{"uri":"Body.png"}]})";
    PreflightInput file;
    file.fileName = "Characters/Body.gltf";
    file.prefix.assign(gltf.begin(), gltf.end());
    const PreflightFinding finding = preflightOne(file, PreflightOptions{});
    EXPECT_EQ(finding.type, AssetType::Model);
    EXPECT_EQ(finding.severity, PreflightSeverity::Note);
    EXPECT_EQ(finding.missingSidecars.size(), 2u) << "the import can offer to bring them";
    EXPECT_NE(finding.message.find("1 buffer file and 1 texture file"), std::string::npos) << finding.message;

    // the same name but not JSON: the model cannot load at all, and "no dependencies" would be a lie
    PreflightInput broken;
    broken.fileName = "Characters/Body.gltf";
    broken.prefix = {'{', '"', 'b', 'u', 'f'};
    const PreflightFinding brokenFinding = preflightOne(broken, PreflightOptions{});
    EXPECT_EQ(brokenFinding.severity, PreflightSeverity::Broken);
}

TEST(AndroidImportPreflight, AGlbIsOneFileAndNothingToSay) {
    // "glTF" + a version and length: the binary form is self-contained by construction
    std::vector<uint8_t> glb = {'g', 'l', 'T', 'F', 2, 0, 0, 0, 12, 0, 0, 0};
    PreflightInput file;
    file.fileName = "Characters/Body.glb";
    file.prefix = glb;
    const PreflightFinding finding = preflightOne(file, PreflightOptions{});
    EXPECT_EQ(finding.severity, PreflightSeverity::Fine);
    EXPECT_TRUE(finding.missingSidecars.empty());
    EXPECT_TRUE(finding.message.empty()) << finding.message;
}

TEST(AndroidImportPreflight, AFortyTextureProjectIsOutOfMemoryOnAPhoneAndTheReportSaysWhichToShrink) {
    // 20 atlases of 2048x2048 RGBA: each is 16.8 MB, every one of them "fine" on its own, and
    // 335 MB together -- which a mid-range phone's GPU heap does not have
    std::vector<PreflightInput> files;
    for (int i = 0; i < 20; i++) {
        PreflightInput file;
        file.fileName = "ui/atlas_" + std::to_string(i) + ".png";
        file.prefix = pngHeader(2048, 2048, 6);
        files.push_back(file);
    }
    PreflightOptions options;
    options.residentLimitBytes = 128ull << 20;
    const PreflightReport report = preflightImports(files, options);

    EXPECT_EQ(report.broken, 0) << "no single file is at fault";
    EXPECT_TRUE(report.overResidentBudget);
    EXPECT_EQ(report.residentTextureBytes, 20ull * 2048ull * 2048ull * 4ull);
    // 335.5 MB decimal is 320.0 MiB, which is how every tool on the platform reports memory
    EXPECT_NE(report.summary.find("320.0 MB"), std::string::npos) << report.summary;
    EXPECT_NE(report.summary.find("128.0 MB"), std::string::npos) << report.summary;
    EXPECT_NE(report.summary.find("past the"), std::string::npos) << report.summary;

    // and the advice is actionable: shrinking these (largest first) closes the gap
    EXPECT_FALSE(report.suggestDownscale.empty());
    EXPECT_EQ(report.suggestDownscale.front(), "ui/atlas_0.png");
}

TEST(AndroidImportPreflight, AProjectInsideItsBudgetSaysNothingAboutMemory) {
    std::vector<PreflightInput> files(2);
    files[0].fileName = "a.png";
    files[0].prefix = pngHeader(256, 256, 6);
    files[1].fileName = "b.png";
    files[1].prefix = pngHeader(256, 256, 6);
    PreflightOptions options;
    options.residentLimitBytes = 128ull << 20;
    const PreflightReport report = preflightImports(files, options);
    EXPECT_FALSE(report.overResidentBudget);
    EXPECT_TRUE(report.suggestDownscale.empty());
    EXPECT_TRUE(report.summary.empty());
    EXPECT_EQ(report.residentTextureBytes, 2ull * 256ull * 256ull * 4ull);
}

// ---- taps and long presses

namespace {

    Touch heldFinger(int pointer, float x, float y, float seconds, TouchPhase phase = TouchPhase::STATIONARY) {
        Touch touch;
        touch.pointer = pointer;
        touch.position = Vector2(x, y);
        touch.startPosition = Vector2(x, y);
        touch.phase = phase;
        touch.time = seconds;
        return touch;
    }

    Touch movedFinger(int pointer, float startX, float startY, float x, float y, float seconds) {
        Touch touch = heldFinger(pointer, x, y, seconds);
        touch.startPosition = Vector2(startX, startY);
        return touch;
    }

}

TEST(AndroidTouchGestures, AFingerHeldStillForHalfASecondIsALongPressOnce) {
    RecognizerState state;
    // down, and still down 100 ms and 400 ms later: nothing yet
    updateLongPress(state, {heldFinger(0, 100, 100, 0.0f, TouchPhase::BEGAN)}, 0.0f);
    EXPECT_FALSE(state.longPress.triggered);
    updateLongPress(state, {heldFinger(0, 100, 100, 0.4f)}, 0.4f);
    EXPECT_FALSE(state.longPress.triggered);

    // at 500 ms it fires, and only on that frame
    updateLongPress(state, {heldFinger(0, 100, 100, 0.5f)}, 0.5f);
    EXPECT_TRUE(state.longPress.triggered);
    EXPECT_EQ(state.longPress.pointer, 0);
    EXPECT_FLOAT_EQ(state.longPress.position.x, 100.0f);
    updateLongPress(state, {heldFinger(0, 100, 100, 0.6f)}, 0.6f);
    EXPECT_FALSE(state.longPress.triggered) << "a menu opens once, not every frame";

    // lifting it is not a tap: the menu it opened must not also be closed by it
    Touch lift = heldFinger(0, 100, 100, 0.7f, TouchPhase::ENDED);
    updateLongPress(state, {lift}, 0.7f);
    updateTap(state, {lift});
    EXPECT_FALSE(state.tap.tapped);
}

TEST(AndroidTouchGestures, MovingOrLiftingEarlyIsNotALongPress) {
    RecognizerState state;
    updateLongPress(state, {heldFinger(0, 100, 100, 0.0f, TouchPhase::BEGAN)}, 0.0f);
    EXPECT_EQ(state.longPress.pointer, 0);

    // the finger goes for a walk before the time: it is a drag, and the press is over
    updateLongPress(state, {movedFinger(0, 100, 100, 160, 100, 0.2f)}, 0.2f);
    EXPECT_EQ(state.longPress.pointer, -1);
    updateLongPress(state, {movedFinger(0, 100, 100, 160, 100, 0.9f)}, 0.9f);
    EXPECT_FALSE(state.longPress.triggered);

    // a second press, lifted before the time, is a tap
    RecognizerState tapped;
    updateLongPress(tapped, {heldFinger(0, 50, 50, 0.0f, TouchPhase::BEGAN)}, 0.0f);
    Touch lift = heldFinger(0, 50, 50, 0.1f, TouchPhase::ENDED);
    updateLongPress(tapped, {lift}, 0.1f);
    EXPECT_FALSE(tapped.longPress.triggered);
    updateTap(tapped, {lift});
    EXPECT_TRUE(tapped.tap.tapped);
    EXPECT_FLOAT_EQ(tapped.tap.position.x, 50.0f);
}

TEST(AndroidTouchGestures, TwoFingersAreAPinchAndNotALongPress) {
    RecognizerState state;
    updateLongPress(state, {heldFinger(0, 100, 100, 0.0f, TouchPhase::BEGAN),
                            heldFinger(1, 300, 100, 0.0f, TouchPhase::BEGAN)}, 0.0f);
    EXPECT_EQ(state.longPress.pointer, -1) << "a two-finger hold zooms nothing and opens nothing";
    updateLongPress(state, {heldFinger(0, 100, 100, 1.0f), heldFinger(1, 300, 100, 1.0f)}, 1.0f);
    EXPECT_FALSE(state.longPress.triggered);
}

TEST(AndroidTouchGestures, TwoQuickTapsInTheSamePlaceAreADoubleTapAndTheNextOneStartsOver) {
    RecognizerState state;
    TapOptions options;
    Touch first = heldFinger(0, 200, 200, 0.05f, TouchPhase::ENDED);
    updateTap(state, {first}, options);
    EXPECT_TRUE(state.tap.tapped);
    EXPECT_FALSE(state.tap.doubleTapped);
    EXPECT_FLOAT_EQ(state.tap.position.x, 200.0f);

    // 200 ms later, 10 px away: a double tap
    Touch second = heldFinger(0, 210, 205, 0.25f, TouchPhase::ENDED);
    updateTap(state, {second}, options);
    EXPECT_TRUE(state.tap.tapped) << "the second tap is still a tap";
    EXPECT_TRUE(state.tap.doubleTapped);
    EXPECT_NEAR(state.tap.interval, 0.2f, 0.001f);

    // a third tap 200 ms later is a single tap again: the pair completed
    Touch third = heldFinger(0, 210, 205, 0.45f, TouchPhase::ENDED);
    updateTap(state, {third}, options);
    EXPECT_TRUE(state.tap.tapped);
    EXPECT_FALSE(state.tap.doubleTapped);
}

TEST(AndroidTouchGestures, TooSlowOrTooFarIsTwoSingleTaps) {
    TapOptions options;
    // 400 ms apart is past the double tap window
    RecognizerState slow;
    updateTap(slow, {heldFinger(0, 100, 100, 0.05f, TouchPhase::ENDED)}, options);
    updateTap(slow, {heldFinger(0, 100, 100, 0.45f, TouchPhase::ENDED)}, options);
    EXPECT_TRUE(slow.tap.tapped);
    EXPECT_FALSE(slow.tap.doubleTapped);

    // 300 px apart is a different target
    RecognizerState far;
    updateTap(far, {heldFinger(0, 100, 100, 0.05f, TouchPhase::ENDED)}, options);
    updateTap(far, {heldFinger(0, 400, 100, 0.15f, TouchPhase::ENDED)}, options);
    EXPECT_FALSE(far.tap.doubleTapped);
}

TEST(AndroidTouchGestures, ADragIsNotATap) {
    RecognizerState state;
    Touch dragged = movedFinger(0, 100, 100, 300, 100, 0.1f);
    dragged.phase = TouchPhase::ENDED;
    updateTap(state, {dragged});
    EXPECT_FALSE(state.tap.tapped) << "a release 200 px from where it started is a drag's end";
}

// ---- an import that was cut short

using jasperengine::android::planModelImport;
using jasperengine::android::planTransfer;
using jasperengine::android::settleTransfer;
using jasperengine::android::transferStep;
using jasperengine::android::TransferOutcome;
using jasperengine::android::TransferPlan;
using jasperengine::android::TransferStep;

TEST(AndroidImportTransfer, TheCopyGoesToAHiddenTemporaryInTheSameFolder) {
    const TransferPlan plan = planTransfer("assets/photo.png", 4u << 20, 0);
    EXPECT_EQ(plan.target, "assets/photo.png");
    // hidden and suffixed: AssetTypes never sees it, and a rename within one folder is atomic
    EXPECT_EQ(plan.temporary, "assets/.photo.png.jeimport");
    EXPECT_TRUE(isImportTemporary(plan.temporary));
    EXPECT_FALSE(isImportTemporary(plan.target)) << "an asset is not a temporary";
    EXPECT_EQ(temporaryTarget(plan.temporary), "assets/photo.png");
    EXPECT_EQ(temporaryTarget("a/b/.x.jpg.jeimport"), "a/b/x.jpg");
    EXPECT_TRUE(temporaryTarget("a/b/photo.png").empty());
    EXPECT_FALSE(isImportTemporary(".jeimport")) << "a name with nothing in it is not a copy";
}

TEST(AndroidImportTransfer, ACopyThatStoppedResumesWhereItLeftOffInsteadOfStartingOver) {
    const TransferPlan plan = planTransfer("photo.png", 10u << 20, 6u << 20);
    EXPECT_EQ(plan.resumeFrom, 6u << 20);
    EXPECT_FALSE(plan.discardPartial);

    const TransferStep step = transferStep(plan, 6u << 20, 100u << 20, 32u << 20, 1u << 20);
    EXPECT_EQ(step.skipBytes, 6u << 20) << "the provider seeks past what is already on disk";
    EXPECT_EQ(step.copyBytes, 1u << 20) << "one chunk, not the rest in one gulp";
    EXPECT_EQ(step.outcome, TransferOutcome::Complete) << "the step itself is fine";

    // read the last of it: 3 MB left, asked for 4
    const TransferStep last = transferStep(plan, 7u << 20, 100u << 20, 32u << 20, 4u << 20);
    EXPECT_EQ(last.copyBytes, 3u << 20);
    EXPECT_EQ(settleTransfer(plan, 10u << 20, false, true), TransferOutcome::Complete);
}

TEST(AndroidImportTransfer, APartialThatIsAlreadyTheWholeFileNeedsNoCopy) {
    // a crash between the last write and the rename: every byte is there
    const TransferPlan plan = planTransfer("photo.png", 4u << 20, 4u << 20);
    EXPECT_EQ(plan.resumeFrom, 4u << 20);
    const TransferStep step = transferStep(plan, 4u << 20, 0, 0, 1u << 20);
    EXPECT_EQ(step.outcome, TransferOutcome::Complete);
    EXPECT_EQ(step.copyBytes, 0u) << "nothing left to read";
    EXPECT_EQ(settleTransfer(plan, 4u << 20, false, false), TransferOutcome::Complete);
}

TEST(AndroidImportTransfer, ATemporaryLongerThanItsSourceStartsOverAndSaysSo) {
    // the provider now reports a smaller file: the bytes on disk cannot be a prefix of it
    const TransferPlan plan = planTransfer("photo.png", 2u << 20, 5u << 20);
    EXPECT_TRUE(plan.discardPartial);
    EXPECT_EQ(plan.resumeFrom, 0u);
    const TransferStep step = transferStep(plan, 0, 100u << 20, 0, 1u << 20);
    EXPECT_EQ(step.skipBytes, 0u);
    EXPECT_NE(step.message.find("starts over"), std::string::npos) << step.message;
}

TEST(AndroidImportTransfer, AFullVolumeStopsTheCopyBeforeItLosesThePartial) {
    const TransferPlan plan = planTransfer("video.mp4", 800u << 20, 600u << 20);
    // 90 MB free, 200 MB left to copy: refused, and the 600 MB already there are named
    const TransferStep step = transferStep(plan, 600u << 20, 90u << 20, 32u << 20, 8u << 20);
    EXPECT_EQ(step.outcome, TransferOutcome::OutOfSpace);
    EXPECT_EQ(step.copyBytes, 0u) << "no write is attempted that would eat the reserve";
    EXPECT_NE(step.message.find("600 MB are already there"), std::string::npos) << step.message;

    // the same copy with room: fine
    const TransferStep room = transferStep(plan, 600u << 20, 400u << 20, 32u << 20, 8u << 20);
    EXPECT_EQ(room.outcome, TransferOutcome::Complete);
    EXPECT_EQ(room.copyBytes, 8u << 20);
}

TEST(AndroidImportTransfer, AnInterruptedCopyKeepsItsPartialAndACancelledOneIsItsOwnOutcome) {
    const TransferPlan plan = planTransfer("photo.png", 10u << 20, 0);
    // the provider died at 4 MB: the temporary stays, the next attempt resumes it
    EXPECT_EQ(settleTransfer(plan, 4u << 20, false, true), TransferOutcome::Interrupted);
    // the user cancelled: the caller decides, and a cancel is not a failure to report
    EXPECT_EQ(settleTransfer(plan, 4u << 20, true, false), TransferOutcome::Cancelled);
    // more bytes than the provider promised: the file is not what it said it was
    EXPECT_EQ(settleTransfer(plan, 11u << 20, false, true), TransferOutcome::Mismatched);
}

TEST(AndroidImportTransfer, AProviderThatGivesNoSizeCompletesWhenItsStreamEnds) {
    const TransferPlan plan = planTransfer("photo.png", 0, 0);
    EXPECT_EQ(plan.expectedBytes, 0u);
    const TransferStep step = transferStep(plan, 0, 0, 0, 4u << 20);
    EXPECT_EQ(step.outcome, TransferOutcome::Complete);
    EXPECT_EQ(step.copyBytes, 4u << 20);
    // the stream's end is the only completion signal there is
    EXPECT_EQ(settleTransfer(plan, 3u << 20, false, false), TransferOutcome::Interrupted);
    EXPECT_EQ(settleTransfer(plan, 3u << 20, false, true), TransferOutcome::Complete);
}

TEST(AndroidImportTransfer, ARenamedTemporaryIsNotMistakenForAnAsset) {
    // what the folder scan sees after a copy that finished its bytes but crashed before the rename
    EXPECT_TRUE(isImportTemporary("assets/.body.glb.jeimport"));
    EXPECT_EQ(temporaryTarget("assets/.body.glb.jeimport"), "assets/body.glb");
    // and what it must never treat as one
    EXPECT_FALSE(isImportTemporary("assets/body.glb"));
    EXPECT_FALSE(isImportTemporary("assets/.gitignore"));
    EXPECT_FALSE(isImportTemporary(".jeimport"));
    EXPECT_FALSE(isImportTemporary("a/.b.jeimportx"));
    EXPECT_FALSE(isImportTemporary("a/b.jeimport"));
}

TEST(AndroidImportTransfer, ARetryOfARetryKeepsOneSuffix) {
    // found by fuzzing: a name that is already a temporary planned ".<name>.jeimport.jeimport",
    // which isImportTemporary() does not recognise, so a folder scan could never recover it
    const TransferPlan plan = planTransfer("assets/.photo.png.jeimport", 100, 0);
    EXPECT_EQ(plan.target, "assets/photo.png");
    EXPECT_EQ(plan.temporary, "assets/.photo.png.jeimport");
    EXPECT_TRUE(isImportTemporary(plan.temporary));
}

TEST(AndroidImportTransfer, EveryPlanWritesSomethingTheScannerCanRecover) {
    // a name with no leaf, and no name at all: the temporary must still be the shape the import
    // folder's recovery looks for, or the bytes would sit there forever (found by fuzzing)
    for (const char* wanted : {"dir/", "", "a/b/"}) {
        const TransferPlan plan = planTransfer(wanted, 100, 0);
        EXPECT_TRUE(isImportTemporary(plan.temporary)) << wanted << " -> " << plan.temporary;
    }
    // and the target agrees with what the temporary maps back to, or a resumed copy would rename
    // the bytes onto a name the plan never mentioned
    const TransferPlan dir = planTransfer("dir/", 100, 0);
    EXPECT_EQ(dir.temporary, "dir/.import.jeimport");
    EXPECT_EQ(dir.target, "dir/import");
    EXPECT_EQ(temporaryTarget(dir.temporary), dir.target);
}

TEST(AndroidModelSidecars, ATwoModelProjectCannotLoseOneModelSCommonPngToTheOther) {
    // Body/ and Head/ both ship a "common.png". Imported flat, one overwrote the other and one of
    // the two models rendered with the wrong texture -- silently, because both files are valid.
    const std::string body = R"({"asset":{"version":"2.0"},
        "images":[{"uri":"textures/common.png"},{"uri":"textures/skin.png"}]})";
    const std::string head = R"({"asset":{"version":"2.0"},
        "images":[{"uri":"textures/common.png"},{"uri":"textures/eyes.png"}]})";
    const ModelImportPlan bodyPlan = planModelImport(body, "Characters/Body.gltf");
    const ModelImportPlan headPlan = planModelImport(head, "Characters/Head.gltf");
    std::set<std::string> names;
    for (const ModelImportPlan* plan : {&bodyPlan, &headPlan}) {
        for (const SidecarImport& sidecar : plan->sidecars) names.insert(sidecar.assetName);
    }
    EXPECT_EQ(names.size(), 4u) << "four files in, four distinct assets";
    EXPECT_TRUE(names.count("body_common.png"));
    EXPECT_TRUE(names.count("head_common.png"));
}

TEST(AndroidModelSidecars, TheDocumentIsRewrittenToNameTheFilesTheImportActuallyCreates) {
    // flattening without rewriting leaves the .gltf pointing at "textures/skin.png", a folder the
    // import never made: the model loads with no textures and nothing says why
    const std::string text = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Body.bin"}],
        "images":[{"uri":"textures/skin.png"}]})";
    const ModelImportPlan plan = planModelImport(text, "Body.gltf");
    EXPECT_TRUE(plan.parsed);
    ASSERT_EQ(plan.sidecars.size(), 2u);
    EXPECT_EQ(plan.sidecars[0].assetName, "body_body.bin");
    EXPECT_EQ(plan.sidecars[1].assetName, "body_skin.png");
    // and the document names them
    EXPECT_NE(plan.gltfText.find("\"body_body.bin\""), std::string::npos) << plan.gltfText;
    EXPECT_NE(plan.gltfText.find("\"body_skin.png\""), std::string::npos) << plan.gltfText;
    EXPECT_EQ(plan.gltfText.find("textures/skin.png"), std::string::npos) << plan.gltfText;
}

TEST(AndroidModelSidecars, TwoFilesWithTheSameLeafInDifferentFoldersAreNumbered) {
    const std::string text = R"({"asset":{"version":"2.0"},
        "images":[{"uri":"a/skin.png"},{"uri":"b/skin.png"}]})";
    const ModelImportPlan plan = planModelImport(text, "Body.gltf");
    std::set<std::string> names;
    for (const SidecarImport& sidecar : plan.sidecars) names.insert(sidecar.assetName);
    EXPECT_EQ(names.size(), 2u) << "one must not overwrite the other";
}

TEST(AndroidModelSidecars, ADataUriIsLeftAloneAndAMalformedDocumentChangesNothing) {
    const std::string inlined = R"({"asset":{"version":"2.0"},
        "images":[{"uri":"data:image/png;base64,AAAA"},{"uri":"skin.png"}]})";
    const ModelImportPlan plan = planModelImport(inlined, "Body.gltf");
    EXPECT_EQ(plan.sidecars.size(), 1u) << "the embedded image is not a file to import";
    EXPECT_NE(plan.gltfText.find("data:image/png;base64,AAAA"), std::string::npos)
        << "an inline image keeps its bytes";

    const std::string broken = R"({"images":[)";
    const ModelImportPlan none = planModelImport(broken, "Body.gltf");
    EXPECT_FALSE(none.parsed);
    EXPECT_TRUE(none.sidecars.empty());
    EXPECT_TRUE(none.gltfText.empty());
}

TEST(AndroidModelSidecars, TheModelNamePrefixIsALegalAssetName) {
    // a model with spaces, a colon and a leading digit: its sidecars must still be importable
    const std::string text = R"({"asset":{"version":"2.0"},"images":[{"uri":"My Skin (1).PNG"}]})";
    const ModelImportPlan plan = planModelImport(text, "12 Head:Body.gltf");
    ASSERT_EQ(plan.sidecars.size(), 1u);
    const std::string& name = plan.sidecars[0].assetName;
    EXPECT_EQ(name, "_12_head_body_my_skin_1.png") << name;
    expectLegalName(name);
}

TEST(AndroidImportPreflight, AGlbBroughtWithOnlyHalfItsFilesSaysWhichAreMissing) {
    const std::string gltf = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"Body.bin"}],
        "images":[{"uri":"textures/skin.png"},{"uri":"textures/eyes.png"}]})";
    PreflightInput file;
    file.fileName = "Body.gltf";
    file.prefix.assign(gltf.begin(), gltf.end());
    // the picker brought the buffer and one texture, and reports leaves while the glTF writes paths
    file.companionNames = {"Body.bin", "skin.png"};
    const PreflightFinding finding = preflightOne(file, PreflightOptions{});
    ASSERT_EQ(finding.absentSidecars.size(), 1u);
    EXPECT_EQ(finding.absentSidecars[0], "textures/eyes.png");
    EXPECT_NE(finding.message.find("not brought with it: textures/eyes.png"), std::string::npos)
        << finding.message;
    // it still imports and shows what it has: a Note, not a Broken
    EXPECT_EQ(finding.severity, PreflightSeverity::Note);

    // everything brought: no claim to make
    PreflightInput complete = file;
    complete.companionNames = {"Body.bin", "skin.png", "eyes.png"};
    EXPECT_TRUE(preflightOne(complete, PreflightOptions{}).absentSidecars.empty());

    // and a caller that passes no companions makes no claim at all (it cannot know)
    PreflightInput blind;
    blind.fileName = "Body.gltf";
    blind.prefix = file.prefix;
    EXPECT_TRUE(preflightOne(blind, PreflightOptions{}).absentSidecars.empty());
}

TEST(AndroidImportPreflight, ACompanionWithAFolderInItsNameStillMatchesItsLeaf) {
    // some pickers report "Body.bin" and some a relative path of their own
    const std::string gltf = R"({"asset":{"version":"2.0"},"buffers":[{"uri":"bin/Body.bin"}]})";
    PreflightInput file;
    file.fileName = "Body.gltf";
    file.prefix.assign(gltf.begin(), gltf.end());
    file.companionNames = {"downloads/Body.bin"};
    EXPECT_TRUE(preflightOne(file, PreflightOptions{}).absentSidecars.empty());
}

TEST(AndroidImportTransfer, ASizelessSourceFromAProviderNeverResumesFromAStrangerSTemporary) {
    // the provider does not say how big the file is, and 5000 bytes of *something* are on disk.
    // Resuming would ask it to skip past bytes it may never have sent, and the truncated result
    // would be reported Complete, because a sizeless stream's end is its only completion signal.
    const TransferPlan plan = planTransfer("song.mp3", 0, 5000);
    EXPECT_TRUE(plan.discardPartial);
    EXPECT_EQ(plan.resumeFrom, 0u);
    const TransferStep step = transferStep(plan, 5000, 1u << 30, 0, 4096);
    EXPECT_EQ(step.skipBytes, 0u) << "nothing is skipped: the bytes are not known to belong to this file";
    EXPECT_NE(step.message.find("does not say how big"), std::string::npos) << step.message;
}

TEST(AndroidImageLimits, ADownscaleMessageDoesNotSayZeroMegabytes) {
    // "(0 MB decoded)" on a 3x3 icon reads like a bug report rather than an answer
    ImageBudget budget;
    budget.maxTextureSize = 1;
    ImageSize tiny;
    tiny.known = true;
    tiny.width = 3;
    tiny.height = 3;
    tiny.channels = 4;
    const ImageDecision decision = decideImageImport(tiny, budget);
    EXPECT_EQ(decision.verdict, ImageVerdict::Downscale);
    EXPECT_EQ(decision.message.find("MB decoded"), std::string::npos) << decision.message;
    EXPECT_NE(decision.message.find("the image is 3x3"), std::string::npos) << decision.message;

    // and a big one still says its size
    ImageSize big;
    big.known = true;
    big.width = 8000;
    big.height = 6000;
    big.channels = 4;
    ImageBudget roomy;
    roomy.maxTextureSize = 4096;
    EXPECT_NE(decideImageImport(big, roomy).message.find("MB decoded"), std::string::npos);
}
