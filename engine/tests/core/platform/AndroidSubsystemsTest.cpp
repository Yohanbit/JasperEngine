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
