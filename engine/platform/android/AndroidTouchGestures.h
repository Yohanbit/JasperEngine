// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_TOUCH_GESTURES_H
#define ANDROID_TOUCH_GESTURES_H

// The multi-finger gestures a phone screen has and a mouse does not, computed from the engine's
// own touch list (Input::getTouches) so a game, a player or the editor's future Android backend
// reads the same numbers on every platform. No Android headers: the core tests run it anywhere.
//
// The engine already reports each finger (position, phase, delta, deltaTime); what it does not
// compute are the gestures that need several fingers at once, which every phone UI (and every
// editor scene view on one) is driven by:
//
//  - the two-finger distance and its change since the last frame: a pinch zoom, in pixels;
//  - the midpoint of two fingers and its movement: panning while pinching;
//  - the angle between two fingers and its change: a rotation gesture;
//  - a one-finger drag past a slop, which is not a tap (VNC, a scene view's orbit);
//  - a long press: a finger held still for half a second, which is how Android's own UI opens a
//    context menu, starts a drag and begins text selection. `InputActionAsset` has a `holdTime` for
//    a *mapped action*, but the UI does not go through InputActions, so a tap on a panel had no
//    press-and-hold at all;
//  - the tap and the double tap, decided at release rather than inferred from `Touch::tapCount`,
//    which only says how many quick taps landed and not whether the finger moved meanwhile.
//
// Recognizer holds the state between frames and answers per frame, so a consumer only forwards
// its touches each frame. It is a plain value: copyable, no allocation after the first frame's
// touches (the previous positions live in a small fixed array).

#include "Input.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jasperengine::android {

    // A two-finger gesture's reading for one frame.
    struct PinchState {
        // false when fewer (or more) than two fingers were down this frame: every field below is
        // then the last value the gesture had, and `ended` says whether it just lost a finger
        bool active = false;
        bool ended = false;
        // distance between the two fingers, and the change of it since the previous frame (a
        // closing pinch is negative): canvas pixels, like Input's touches
        float distance = 0.0f;
        float deltaDistance = 0.0f;
        // of the two fingers, and the change of it since the previous frame
        Vector2 midpoint;
        Vector2 deltaMidpoint;
        // the angle of the line between the two fingers (atan2(dy, dx), radians, screen space
        // with y down), and the change since the previous frame wrapped to (-pi, pi]
        float angle = 0.0f;
        float deltaAngle = 0.0f;
        // the two fingers, in `Input::getTouches` order
        int pointerA = -1;
        int pointerB = -1;
        // whole frames the gesture has been active (1 on the frame it began)
        int frames = 0;
        // the Input frame the gesture was last read in (continuity of the pair across frames)
        int lastFrame = -1;
    };

    // A finger that moved further than `slop` pixels from where it touched the screen: the
    // gesture a tap is not (Unity's touch drag; the editor's Scene view orbit on a phone).
    struct DragState {
        bool dragging = false;
        // the finger that owns the drag (-1: none)
        int pointer = -1;
        // where the drag started and the travel since, in canvas pixels
        Vector2 start;
        Vector2 total;
        // total travel of this frame and of the previous one (a fling reads the previous one at
        // release, when this frame's is already 0)
        Vector2 frame;
        Vector2 previousFrame;
        int frames = 0;
    };

    // A finger's press followed by a release without moving past the slop: a tap.
    struct TapState {
        // true for the one frame the release happened in a tap
        bool tapped = false;
        // true for the one frame a *second* tap landed close enough and soon enough to be a double
        // tap (the first tap's `tapped` still fired: a consumer that only wants double taps ignores
        // the first and undoes it, which is what every UI toolkit does)
        bool doubleTapped = false;
        // where it landed, and which finger
        Vector2 position;
        int pointer = -1;
        // the seconds between the two taps of a double tap (0 on a single tap)
        float interval = 0.0f;
    };

    // A finger held still: the press-and-hold every phone UI is driven by.
    struct LongPressState {
        // true for the one frame the press became a long press (not every frame it is still held)
        bool triggered = false;
        // where the finger is holding and since when
        Vector2 position;
        int pointer = -1;
        float heldSeconds = 0.0f;
        // the press that became a long press is no longer a tap: `updateTap` will not fire for it
        bool consumed = false;
    };

    // How a touch UI's taps and holds are recognised. The thresholds are Android's own
    // ViewConfiguration defaults (getLongPressTimeout 500 ms, getDoubleTapTimeout 300 ms,
    // getScaledDoubleTapSlop 100 px at mdpi), so a gesture behaves as the phone's users expect.
    struct TapOptions {
        float longPressSeconds = 0.5f;
        float doubleTapSeconds = 0.3f;
        // a double tap's second touch must land within this of the first's: Android scales it by
        // density; a caller with the canvas scale passes its own
        float doubleTapDistance = 100.0f;
        // further than this from the start and the finger is dragging, not tapping or holding
        float slop = 8.0f;
    };

    // The state a Recognizer keeps between frames.
    struct RecognizerState {
        PinchState pinch;
        DragState drag;
        TapState tap;
        LongPressState longPress;
        // the last tap's release, for the double tap's window
        bool lastTapValid = false;
        Vector2 lastTapPosition;
        float lastTapEndTime = 0.0f;
        // fingers read last frame, and the frame number last read
        size_t previousCount = 0;
        int previousFrame = -1;
    };

    namespace detail {
        // the shortest way round, in (-pi, pi]
        inline float wrapAngle(float angle) {
            constexpr float PI = 3.14159265358979323846f;
            constexpr float TWO_PI = 2.0f * PI;
            angle = std::fmod(angle + PI, TWO_PI);
            if (angle <= 0.0f) angle += TWO_PI;
            return angle - PI;
        }
    }

    // Advances the long press with this frame's touches. A finger that has been down for
    // `options.longPressSeconds` without moving further than `options.slop` triggers once; moving
    // further cancels it (the finger became a drag), and so does lifting it before the time.
    //
    // `seconds` is the engine's unscaled input time (Input::getInputTime or the touches' own
    // `time`), so a paused game does not trigger a menu.
    inline void updateLongPress(RecognizerState& state, const std::vector<Touch>& touches, float seconds, const TapOptions& options = {}) {
        LongPressState& press = state.longPress;
        press.triggered = false;
        if (press.pointer >= 0) {
            // find the finger that owns the press; it may have lifted (a tap, not a hold)
            const Touch* owner = nullptr;
            for (const Touch& touch : touches) {
                if (touch.pointer == press.pointer) {
                    owner = &touch;
                    break;
                }
            }
            if (!owner || owner->phase == TouchPhase::ENDED) {
                // lifted before the time: not a long press, and still a tap
                press.pointer = -1;
                press.heldSeconds = 0.0f;
                return;
            }
            press.position = owner->position;
            // the held time comes from the touch itself and not from a subtraction against
            // `seconds`: Touch::time is already the unscaled seconds since this finger went down,
            // so a caller that passes any monotonic clock cannot make it negative or drift
            press.heldSeconds = owner->time;
            (void)seconds;
            const float moved = (owner->position - owner->startPosition).length();
            if (moved > options.slop) {
                press.pointer = -1;   // it is a drag
                press.heldSeconds = 0.0f;
                return;
            }
            if (!press.consumed && press.heldSeconds >= options.longPressSeconds) {
                press.triggered = true;
                press.consumed = true;
            }
            return;
        }
        // a new press, when exactly one finger is down (two fingers are a pinch, not a hold)
        if (touches.size() != 1) return;
        const Touch& only = touches.front();
        // Not `phase != BEGAN`: BEGAN is true for the one frame Input reports a finger in, so a
        // finger that is held still is STATIONARY for every frame after its first -- and a press
        // that required BEGAN every frame could only ever start on the frame the finger landed,
        // which no caller can guarantee (a frame may pass between the touch and the first read).
        // The rule is what a hold is: one finger down, still near where it touched, not lifting.
        // A press that started and then moved is caught above, by the slop test on the owner.
        if (only.phase == TouchPhase::ENDED || only.phase == TouchPhase::CANCELED ||
            (only.position - only.startPosition).length() > options.slop) return;
        press.pointer = only.pointer;
        press.position = only.position;
        press.heldSeconds = only.time;
        press.consumed = false;
    }

    // Whether the press that just ended is the one a long press consumed. Kept out of the state
    // machine so both readers agree on it: the long press records the finger, the tap asks.
    inline bool pressWasConsumed(const RecognizerState& state, const Vector2& releasePosition, float slop) {
        if (!state.longPress.consumed) return false;
        return (state.longPress.position - releasePosition).length() <= slop;
    }

    // Advances the tap reading with this frame's touches. A release within `options.slop` of where
    // the finger started is a tap; two of them within `options.doubleTapDistance` and
    // `options.doubleTapSeconds` is a double tap. A press that became a long press is not a tap.
    inline void updateTap(RecognizerState& state, const std::vector<Touch>& touches, const TapOptions& options = {}) {
        TapState& tap = state.tap;
        tap.tapped = false;
        tap.doubleTapped = false;
        for (const Touch& touch : touches) {
            if (touch.phase != TouchPhase::ENDED) continue;
            // the press that opened a menu is the one whose long press already fired: lifting it
            // must not also close the menu a tap would open (the finger has not moved, so a
            // distance test is what tells them apart)
            const bool held = pressWasConsumed(state, touch.position, options.slop);
            if (held) continue;   // the context menu opened here
            if ((touch.position - touch.startPosition).length() > options.slop) continue;
            tap.tapped = true;
            tap.pointer = touch.pointer;
            tap.position = touch.position;
            if (state.lastTapValid && touch.time - state.lastTapEndTime <= options.doubleTapSeconds &&
                (touch.position - state.lastTapPosition).length() <= options.doubleTapDistance) {
                tap.doubleTapped = true;
                tap.interval = touch.time - state.lastTapEndTime;
                state.lastTapValid = false;   // the pair is complete: a third tap starts again
            } else {
                state.lastTapValid = true;
                state.lastTapPosition = touch.position;
                state.lastTapEndTime = touch.time;
            }
        }
    }

    // Advances the pinch reading with this frame's touches (Input::getTouches() or
    // Input::getFrameTouches(), both work: only positions and pointers are read).
    //
    // A pinch is the first two fingers down, taken in the order the engine lists them and kept
    // by pointer while they stay down: lifting one finger and putting another down is a new
    // pinch (deltaDistance restarts from 0), where following the new finger would report the
    // whole jump between them as one huge pinch. The gesture `ended` on the frame a finger
    // leaves; a third finger down is not part of it and does not end it.
    inline void updatePinch(RecognizerState& state, const std::vector<Touch>& touches, int frame) {
        // this frame's pointers and positions, in the order the engine lists them
        int pointers[D_INPUT_MAX_TOUCHES] = {};
        Vector2 positions[D_INPUT_MAX_TOUCHES];
        size_t count = 0;
        for (const Touch& touch : touches) {
            if (count >= D_INPUT_MAX_TOUCHES) break;
            pointers[count] = touch.pointer;
            positions[count] = touch.position;
            count++;
        }

        const int frameIndex = frame >= 0 ? frame : state.previousFrame + 1;
        PinchState& pinch = state.pinch;
        // the same two fingers, on the frame right after the one they were read in: the reading
        // continues; otherwise this is a new pinch and no delta is reported from the old one
        const bool samePair = pinch.active && count >= 2 &&
                              frameIndex == pinch.lastFrame + 1 &&
                              ((pointers[0] == pinch.pointerA && pointers[1] == pinch.pointerB) ||
                               (pointers[0] == pinch.pointerB && pointers[1] == pinch.pointerA));

        if (count >= 2) {
            const Vector2 a = positions[0];
            const Vector2 b = positions[1];
            const float dx = b.x - a.x;
            const float dy = b.y - a.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const Vector2 midpoint((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            const float angle = std::atan2(dy, dx);

            if (samePair) {
                // the same two fingers, unbroken between frames: the deltas are this frame's move
                pinch.deltaDistance = distance - pinch.distance;
                pinch.deltaMidpoint = midpoint - pinch.midpoint;
                pinch.deltaAngle = detail::wrapAngle(angle - pinch.angle);
                pinch.frames++;
            } else {
                // no delta on the frame a pinch begins: the jump from another pair is not a pinch
                pinch.deltaDistance = 0.0f;
                pinch.deltaMidpoint = Vector2(0.0f, 0.0f);
                pinch.deltaAngle = 0.0f;
                pinch.frames = 1;
            }
            pinch.distance = distance;
            pinch.midpoint = midpoint;
            pinch.angle = angle;
            pinch.pointerA = pointers[0];
            pinch.pointerB = pointers[1];
            pinch.active = true;
            pinch.ended = false;
            pinch.lastFrame = frameIndex;
        } else {
            // a finger left the gesture, or none is down: `ended` is true for the one frame the
            // gesture stops being active
            pinch.ended = pinch.active;
            pinch.active = false;
            pinch.frames = 0;
            pinch.pointerA = -1;
            pinch.pointerB = -1;
            pinch.deltaDistance = 0.0f;
            pinch.deltaMidpoint = Vector2(0.0f, 0.0f);
            pinch.deltaAngle = 0.0f;
            pinch.lastFrame = frameIndex;
        }

        state.previousCount = count;
        state.previousFrame = frameIndex;
    }

    // ---- one-finger drag

    inline void updateDrag(DragState& state, const std::vector<Touch>& touches, float slop = 8.0f) {
        const Touch* owned = nullptr;
        for (const Touch& touch : touches) {
            if (state.dragging) {
                if (touch.pointer == state.pointer) owned = &touch;
            } else {
                // the first finger down that is still on the screen
                if (touch.phase != TouchPhase::ENDED && touch.phase != TouchPhase::CANCELED) {
                    owned = &touch;
                    break;
                }
            }
        }

        state.previousFrame = state.frame;
        state.frame = Vector2(0.0f, 0.0f);

        if (!owned) {
            state.dragging = false;
            state.pointer = -1;
            state.frames = 0;
            state.total = Vector2(0.0f, 0.0f);
            return;
        }

        if (!state.dragging) {
            if (owned->phase == TouchPhase::ENDED || owned->phase == TouchPhase::CANCELED) {
                state.pointer = -1;
                return;
            }
            const Vector2 travel = owned->position - owned->startPosition;
            if (std::sqrt(travel.x * travel.x + travel.y * travel.y) > slop) {
                state.dragging = true;
                state.pointer = owned->pointer;
                state.start = owned->startPosition;
                state.total = travel;
                state.frame = travel;
                state.previousFrame = Vector2(0.0f, 0.0f);
                state.frames = 1;
            }
            return;
        }

        // the finger lifts in this frame: the drag is over, and previousFrame holds the last
        // movement for a fling
        if (owned->phase == TouchPhase::ENDED || owned->phase == TouchPhase::CANCELED) {
            state.total = owned->position - state.start;
            state.dragging = false;
            state.pointer = -1;
            state.frames = 0;
            return;
        }
        state.frames++;
        state.total = owned->position - state.start;
        state.frame = owned->delta;
    }

}

#endif // ANDROID_TOUCH_GESTURES_H
