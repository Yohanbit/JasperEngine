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
//  - a one-finger drag past a slop, which is not a tap (VNC, a scene view's orbit).
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

    // The state a Recognizer keeps between frames.
    struct RecognizerState {
        PinchState pinch;
        DragState drag;
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
