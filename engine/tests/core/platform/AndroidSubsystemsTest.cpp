// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The Android import, gesture, storage and project subsystems, which are plain C++ so they run
// here: AndroidAssetImport.h, AndroidTouchGestures.h, AndroidStorageSpace.h,
// AndroidProjectLayout.h.

#include "AndroidAssetImport.h"
#include "AndroidProjectLayout.h"
#include "AndroidStorageSpace.h"
#include "AndroidTouchGestures.h"
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

#include "AndroidImageLimits.h"

namespace {

    // a PNG header with the given size and colour type (6: RGBA)
    std::vector<uint8_t> pngHeader(uint32_t width, uint32_t height, int colorType, int bitDepth = 8) {
        std::vector<uint8_t> bytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A,
                                      0, 0, 0, 13, 'I', 'H', 'D', 'R',
                                      (uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
                                      (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height,
                                      (uint8_t)bitDepth, (uint8_t)colorType, 0, 0, 0, 0};
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
