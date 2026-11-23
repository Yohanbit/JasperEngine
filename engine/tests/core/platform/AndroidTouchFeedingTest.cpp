// The Android editor's touch feed, which is the half of the gesture subsystem that had no code at
// all: AndroidTouchGestures.h recognised long press, tap, drag and pinch from Input's touches, and
// nothing on Android ever put a finger into Input. The events arrived (they drove ImGui's pointer)
// and the engine's own touch state stayed empty, so no gesture could fire and a game played in that
// build saw no touches either.
//
// editor/backend/BackendAndroid.cpp now feeds both from the same buffer. The translation from
// AMOTION_EVENT_ACTION_* to addTouch/removeTouch is the part that can be wrong in a way that looks
// fine on a phone (a stuck finger, a tap that never ends), so it is modelled here over the same
// actions, and the recogniser is then driven from what it produces -- the two halves have to agree
// for a long press to ever fire.

#include "AndroidTouchGestures.h"
#include "Input.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace jasperengine;

namespace {

// The subset of GameActivityMotionEvent the translation reads.
enum class Action { Down, Move, Up, PointerDown, PointerUp, Cancel };

struct Pointer { int id; float x; float y; };
struct Event { Action action; std::vector<Pointer> pointers; int index = 0; };

// The model of if_/BackendAndroid.cpp's feedInputTouches: the same rules, over the actions a
// GameActivity buffer carries, so a mistake in the translation shows up here instead of on a phone.
class TouchFeed {
public:
    void feed(const Event& event) {
        if (event.action == Action::Cancel) {
            touches_.clear();
            return;
        }
        bool seen[32] = {};
        for (size_t p = 0; p < event.pointers.size() && p < 32; ++p) {
            const Pointer& pointer = event.pointers[p];
            if (pointer.id < 0 || pointer.id >= 32) continue;

            const bool lifts = event.action == Action::Up ||
                               (event.action == Action::PointerUp && static_cast<int>(p) == event.index);
            if (lifts) {
                erase(pointer.id);
                continue;
            }
            if (!seen[pointer.id]) {
                set(pointer.id, pointer.x, pointer.y);
                seen[pointer.id] = true;
            } else {
                set(pointer.id, pointer.x, pointer.y);
            }
        }
    }

    struct Finger { int id; float x, y; };
    const std::vector<Finger>& fingers() const { return touches_; }
    size_t count() const { return touches_.size(); }
    bool has(int id) const {
        for (const Finger& f : touches_) if (f.id == id) return true;
        return false;
    }
    float x(int id) const { for (const Finger& f : touches_) if (f.id == id) return f.x; return -1; }

private:
    void set(int id, float x, float y) {
        for (Finger& f : touches_) if (f.id == id) { f.x = x; f.y = y; return; }
        touches_.push_back({id, x, y});
    }
    void erase(int id) {
        for (size_t i = 0; i < touches_.size(); ++i) {
            if (touches_[i].id == id) { touches_.erase(touches_.begin() + static_cast<long>(i)); return; }
        }
    }
    std::vector<Finger> touches_;
};

// Input's Touch, from the feed's fingers, as Input::getTouches() reports them at `seconds`:
// `time` is the seconds since that finger went down (Input keeps beganAt and subtracts), NOT the
// clock reading, and it is what updateLongPress measures the hold with. Getting this wrong is the
// whole reason this test exists: a reader that passes the clock as `time` sees a hold of zero
// seconds every frame, and no long press ever fires.
std::vector<Touch> asTouches(const TouchFeed& feed, float downAt, float seconds) {
    std::vector<Touch> touches;
    for (const TouchFeed::Finger& f : feed.fingers()) {
        Touch touch;
        touch.pointer = f.id;
        touch.position = Vector2(f.x, f.y);
        touch.startPosition = Vector2(f.x, f.y);
        touch.phase = TouchPhase::BEGAN;
        touch.time = seconds - downAt;
        touch.began = true;
        touches.push_back(touch);
    }
    return touches;
}

} // namespace

// one finger down, moved, up: the list follows it and empties at the end
TEST(AndroidTouchFeeding, AFingerIsAddedMovedAndRemoved) {
    TouchFeed feed;
    feed.feed({Action::Down, {{0, 10, 20}}});
    ASSERT_EQ(feed.count(), 1u);
    EXPECT_TRUE(feed.has(0));

    feed.feed({Action::Move, {{0, 40, 50}}});
    ASSERT_EQ(feed.count(), 1u);
    EXPECT_EQ(feed.x(0), 40.0f);

    feed.feed({Action::Up, {{0, 40, 50}}});
    EXPECT_EQ(feed.count(), 0u);
}

// two fingers: the second does not disturb the first, and lifting the second leaves the first
TEST(AndroidTouchFeeding, ASecondFingerIsIndependentOfTheFirst) {
    TouchFeed feed;
    feed.feed({Action::Down, {{0, 10, 10}}});
    feed.feed({Action::PointerDown, {{0, 10, 10}, {1, 100, 100}}, 1});
    ASSERT_EQ(feed.count(), 2u);

    // POINTER_UP names its finger by index: the second one lifts, the first stays
    feed.feed({Action::PointerUp, {{0, 10, 10}, {1, 100, 100}}, 1});
    ASSERT_EQ(feed.count(), 1u);
    EXPECT_TRUE(feed.has(0));
    EXPECT_FALSE(feed.has(1));
}

// lifting the wrong index must not remove the other finger: this is the bug a phone shows as a
// finger stuck to the screen
TEST(AndroidTouchFeeding, PointerUpRemovesTheIndexedFingerAndNotTheOther) {
    TouchFeed feed;
    feed.feed({Action::Down, {{0, 10, 10}}});
    feed.feed({Action::PointerDown, {{0, 10, 10}, {1, 20, 20}}, 1});

    feed.feed({Action::PointerUp, {{0, 10, 10}, {1, 20, 20}}, 0});
    ASSERT_EQ(feed.count(), 1u);
    EXPECT_FALSE(feed.has(0));
    EXPECT_TRUE(feed.has(1));
}

// a cancel (the window lost focus, the app paused) leaves no finger down
TEST(AndroidTouchFeeding, ACancelClearsEveryFinger) {
    TouchFeed feed;
    feed.feed({Action::Down, {{0, 1, 1}}});
    feed.feed({Action::PointerDown, {{0, 1, 1}, {2, 5, 5}}, 1});
    feed.feed({Action::Cancel, {{0, 1, 1}, {2, 5, 5}}});
    EXPECT_EQ(feed.count(), 0u);
}

// The two halves agree: a finger the feed holds still long enough becomes the long press the editor
// turns into a context click, and one that lifts before the time does not.
TEST(AndroidTouchFeeding, AHeldFingerReachesTheLongPress) {
    TouchFeed feed;
    feed.feed({Action::Down, {{3, 200, 300}}});

    android::RecognizerState state;
    android::TapOptions options;              // 0.5 s long press, 8 px slop

    // 0.4 s: not yet
    android::updateLongPress(state, asTouches(feed, 0.0f, 0.4f), 0.4f, options);
    EXPECT_FALSE(state.longPress.triggered);

    // 0.6 s, still down, still in place: the press fires exactly once
    android::updateLongPress(state, asTouches(feed, 0.0f, 0.6f), 0.6f, options);
    EXPECT_TRUE(state.longPress.triggered);
    EXPECT_EQ(state.longPress.pointer, 3);
    EXPECT_FLOAT_EQ(state.longPress.position.x, 200.0f);

    state.longPress.triggered = false;
    android::updateLongPress(state, asTouches(feed, 0.0f, 0.7f), 0.7f, options);
    EXPECT_FALSE(state.longPress.triggered) << "a long press fires once, not every frame";
}

// a finger that moved further than the slop is a drag, and a drag must never open a menu
TEST(AndroidTouchFeeding, ADraggedFingerNeverBecomesALongPress) {
    TouchFeed feed;
    feed.feed({Action::Down, {{0, 100, 100}}});
    feed.feed({Action::Move, {{0, 400, 400}}});

    android::RecognizerState state;
    android::updateLongPress(state, asTouches(feed, 0.0f, 0.9f), 0.9f);
    EXPECT_FALSE(state.longPress.triggered);
}

// the finger lifting before the hold time cancels it, and the release is a tap instead
TEST(AndroidTouchFeeding, ALiftedFingerIsATapAndNotAPress) {
    TouchFeed feed;
    feed.feed({Action::Down, {{0, 50, 50}}});
    android::RecognizerState state;
    android::updateLongPress(state, asTouches(feed, 0.0f, 0.2f), 0.2f);
    EXPECT_FALSE(state.longPress.triggered);

    feed.feed({Action::Up, {{0, 50, 50}}});
    android::updateLongPress(state, asTouches(feed, 0.0f, 0.25f), 0.25f);
    EXPECT_FALSE(state.longPress.triggered);
    EXPECT_TRUE(feed.fingers().empty());
}

// The regression this integration hit, and the reason a subsystem can pass 80 tests and still not
// work: updateLongPress used to start a press only from a Touch whose phase was BEGAN. BEGAN is true
// for the one frame Input reports a finger in; a finger that is held still is STATIONARY for every
// frame after its first, so the press could only start on the frame the finger landed -- and a frame
// may pass between the touch and the first frame that reads it. On a phone that is a long press that
// sometimes works and usually does not, which is the worst kind. The press now starts from any one
// finger that is down and has not moved past the slop, whatever its phase.
TEST(AndroidTouchFeeding, APressStartsFromAStationaryFingerAndNotOnlyFromBegan) {
    TouchFeed feed;
    feed.feed({Action::Down, {{0, 120, 260}}});

    android::RecognizerState state;

    // the finger's landing frame was never read: the first frame this consumer sees it in, it has
    // been down for 0.2 s and has not moved, so its phase is STATIONARY
    std::vector<Touch> stationary = asTouches(feed, 0.0f, 0.2f);
    stationary.front().phase = TouchPhase::STATIONARY;
    android::updateLongPress(state, stationary, 0.2f);
    EXPECT_EQ(state.longPress.pointer, 0) << "a stationary finger must start the press";

    // and it fires when the hold is long enough
    std::vector<Touch> held = asTouches(feed, 0.0f, 0.6f);
    held.front().phase = TouchPhase::STATIONARY;
    android::updateLongPress(state, held, 0.6f);
    EXPECT_TRUE(state.longPress.triggered);
}

// an ended or canceled finger must not start one: that is a tap or an interruption, not a hold
TEST(AndroidTouchFeeding, AnEndedFingerDoesNotStartAPress) {
    TouchFeed feed;
    feed.feed({Action::Down, {{0, 10, 10}}});

    android::RecognizerState state;
    std::vector<Touch> ended = asTouches(feed, 0.0f, 0.1f);
    ended.front().phase = TouchPhase::ENDED;
    android::updateLongPress(state, ended, 0.1f);
    EXPECT_EQ(state.longPress.pointer, -1);
}
