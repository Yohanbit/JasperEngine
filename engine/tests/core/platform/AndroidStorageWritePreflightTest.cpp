// The storage-space subsystem, end to end: a full volume refuses a save *before* the file is
// written, and a roomy one lets it through.
//
// This is the check the subsystem could not make on its own. AndroidStorageSpace.h decided what to
// do with a space reading; whether the engine asks at all is the wiring, and it is the difference
// between a phone that reports "no space left on the device" after writing 300 MB of temporary and
// one that refuses in the first place.
//
// FileUtils::writeText is the path a save takes (utils::Json::writeFile and the editor's document
// writes go through the same atomic writer), so the test drives it rather than the helper.

#include "io/FileUtils.h"
#include "System.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace jasperengine {
    // The test-only seam System.h declares for exactly this (ModelCpuDataGpuTest uses it too).
    struct SystemTestAccess {
        static System* swap(System* system) {
            System* previous = System::systemInstance;
            System::systemInstance = system;
            return previous;
        }
    };
}

using namespace jasperengine;

namespace {

// Installs the fake for the life of the test and puts the previous one back on the way out, including
// on a failed EXPECT: a leaked instance would make the next test read this one's answer.
struct ScopedSystem {
    System* previous;
    explicit ScopedSystem(System* system) : previous(SystemTestAccess::swap(system)) {}
    ~ScopedSystem() { SystemTestAccess::swap(previous); }
};

class FixedSpaceSystem : public System {
public:
    VolumeSpace reading;
    VolumeSpace getVolumeSpace() override { return reading; }
    // data:// resolves against this, which is the whole point on Android: a save is written into
    // the app's own internal directory, and that directory's volume is the one asked about.
    std::string root;
    std::string getUserDataPath() override { return root; }
    int getScreenWidth() override { return 1080; }
    int getScreenHeight() override { return 2400; }
};

std::string tempFolder();

std::string tempFolder() {
    const auto base = std::filesystem::temp_directory_path() / "je_storage_preflight";
    std::filesystem::create_directories(base);
    return base.string();
}

// data://<name> resolved into the fake's root, which is what FileUtils does internally
std::string dataPath(const std::string& name) {
    return "data://" + name;
}

VolumeSpace withAvailable(uint64_t bytes) {
    VolumeSpace space;
    space.known = true;
    space.availableBytes = bytes;
    space.totalBytes = 64ull * 1024 * 1024 * 1024;
    return space;
}

} // namespace

TEST(AndroidStorageWritePreflight, AFullVolumeRefusesTheSaveAndLeavesNoFile) {
    FixedSpaceSystem system;
    system.root = tempFolder();
    system.reading = withAvailable(1024);            // far below the 8 MB reserve
    ScopedSystem installed(&system);

    const std::string target = system.root + "/full.json";
    std::remove(target.c_str());

    std::string error;
    const bool written = FileUtils::writeText(dataPath("full.json"), R"({"save":true})", error);

    EXPECT_FALSE(written);
    EXPECT_NE(error.find("free space"), std::string::npos) << "error was: " << error;
    // the point of asking first: nothing was created, not even a temporary
    EXPECT_FALSE(std::filesystem::exists(target));
    EXPECT_FALSE(std::filesystem::exists(target + ".tmp"));
}

TEST(AndroidStorageWritePreflight, ARoomyVolumeWritesNormally) {
    FixedSpaceSystem system;
    system.root = tempFolder();
    system.reading = withAvailable(8ull * 1024 * 1024 * 1024);
    ScopedSystem installed(&system);

    const std::string target = system.root + "/roomy.json";
    std::remove(target.c_str());

    std::string error;
    EXPECT_TRUE(FileUtils::writeText(dataPath("roomy.json"), R"({"save":true})", error)) << error;

    std::ifstream in(target);
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(contents, R"({"save":true})");

    std::remove(target.c_str());
}

// Off Android every platform leaves the reading at its default, and that must change nothing: a
// test that runs on a desktop with an unimplemented getVolumeSpace still saves.
TEST(AndroidStorageWritePreflight, AnUnansweredReadingNeverStopsASave) {
    FixedSpaceSystem system;                          // reading left default = unknown
    system.root = tempFolder();
    ScopedSystem installed(&system);

    const std::string target = system.root + "/unknown.json";
    std::remove(target.c_str());

    std::string error;
    EXPECT_TRUE(FileUtils::writeText(dataPath("unknown.json"), "x", error)) << error;
    EXPECT_TRUE(std::filesystem::exists(target));

    std::remove(target.c_str());
}
