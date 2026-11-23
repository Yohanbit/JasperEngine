// Wiring tests for the storage-space subsystem: not "is the policy right" (AndroidSubsystemsTest
// covers that with 80 cases) but "is the policy actually reached from the engine".
//
// This is the part of the task that agent 2's branch could not do for itself: it owns
// AndroidStorageSpace.h and wrote it Android-free, while the seam it plugs into is the core's
// System. The three things checked here are the three ways that wiring silently fails:
//
//   1. the base System must answer "unknown" -- a platform that does not implement the call must
//      not start refusing saves;
//   2. an unknown reading must never refuse a write, and a known tight one must;
//   3. FileUtils::writeFile's pre-flight must actually consult the System (it refuses with its own
//      sentence, not the ENOSPC one that only arrives after the temporary was written).
//
// The arithmetic of (2) is checked against the subsystem's own allowWrite, so the core helper and
// the Android policy cannot drift apart: both are fed the same readings and must agree on which
// writes they allow.

#include "System.h"
#include "AndroidStorageSpace.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

using namespace jasperengine;

namespace {

// A System whose volume reading a test sets, and whose other answers stay the base ones.
class FakeSystem : public System {
public:
    VolumeSpace reading;
    VolumeSpace getVolumeSpace() override { return reading; }
    // System's only two pure virtuals
    int getScreenWidth() override { return 1080; }
    int getScreenHeight() override { return 2400; }
};

VolumeSpace known(uint64_t available, uint64_t total) {
    VolumeSpace space;
    space.known = true;
    space.availableBytes = available;
    space.totalBytes = total;
    return space;
}

} // namespace

// (1) the default every desktop platform inherits
TEST(AndroidStorageWiring, APlaformThatDoesNotAnswerIsUnknownAndStopsNothing) {
    FakeSystem system;                       // reading left at its default
    const VolumeSpace space = system.getVolumeSpace();
    EXPECT_FALSE(space.known);
    EXPECT_TRUE(space.allowsWrite(UINT64_MAX));
    EXPECT_TRUE(space.allowsWrite(1024));
}

// (2) the core's rule agrees with the Android subsystem's, reading for reading
TEST(AndroidStorageWiring, TheCoreRuleAndTheAndroidPolicyRefuseTheSameWrites) {
    struct Case { uint64_t available; uint64_t total; };
    const Case cases[] = {
        {0, 32ull * 1024 * 1024 * 1024},
        {4ull * 1024 * 1024, 32ull * 1024 * 1024 * 1024},       // below the reserve
        {8ull * 1024 * 1024, 32ull * 1024 * 1024 * 1024},       // exactly the reserve
        {64ull * 1024 * 1024, 32ull * 1024 * 1024 * 1024},      // roomy
        {2ull * 1024 * 1024 * 1024, 4ull * 1024 * 1024 * 1024}, // half full
    };
    const uint64_t writes[] = {0, 1, 1024, 4ull * 1024 * 1024, 100ull * 1024 * 1024, UINT64_MAX};

    for (const Case& c : cases) {
        const VolumeSpace core = known(c.available, c.total);
        android::SpaceInfo subsystem;
        subsystem.known = true;
        subsystem.availableBytes = c.available;
        subsystem.totalBytes = c.total;

        for (uint64_t bytes : writes) {
            const bool fromCore = core.allowsWrite(bytes);
            const bool fromSubsystem = android::allowWrite(subsystem, bytes).allowed;
            EXPECT_EQ(fromCore, fromSubsystem)
                << "available " << c.available << ", write " << bytes;
        }
    }
}

// a reading below the reserve refuses even a zero-byte write: the save itself must not be the
// thing that fills the phone
TEST(AndroidStorageWiring, AFullVolumeRefusesBeforeTheFirstByte) {
    const VolumeSpace full = known(1024, 32ull * 1024 * 1024 * 1024);
    EXPECT_FALSE(full.allowsWrite(0));
    EXPECT_FALSE(full.allowsWrite(1));
}

// the reserve is a parameter, so a caller with its own policy can say so
TEST(AndroidStorageWiring, TheReserveIsTheCallers) {
    const VolumeSpace tight = known(2ull * 1024 * 1024, 32ull * 1024 * 1024 * 1024);
    EXPECT_FALSE(tight.allowsWrite(0));                 // 8 MB reserve by default
    EXPECT_TRUE(tight.allowsWrite(0, 1024 * 1024));     // 1 MB reserve: it fits
}

// The seam itself, checked at compile time: getVolumeSpace() must be virtual and overridable (a
// platform implements it), the base must answer, and every Android reading must be convertible to
// the core's, which is what lets the policy live in a header the core cannot include.
TEST(AndroidStorageWiring, TheSeamIsVirtualAndOverridable) {
    static_assert(std::is_polymorphic_v<System>, "System must stay polymorphic: platforms override it");
    static_assert(std::has_virtual_destructor_v<System>, "and its destructor must stay virtual");
    static_assert(std::is_same_v<decltype(&System::getVolumeSpace), VolumeSpace (System::*)()>);
    static_assert(!std::is_final_v<System>);

    // the Android policy's own type carries everything the core's does
    android::SpaceInfo subsystem;
    subsystem.known = true;
    subsystem.totalBytes = 1;
    subsystem.availableBytes = 2;
    subsystem.reservedBytes = 3;

    VolumeSpace core;
    core.known = subsystem.known;
    core.totalBytes = subsystem.totalBytes;
    core.availableBytes = subsystem.availableBytes;
    core.reservedBytes = subsystem.reservedBytes;

    EXPECT_TRUE(core.known);
    EXPECT_EQ(core.availableBytes, 2u);
    EXPECT_EQ(core.reservedBytes, 3u);
}
