// The four gesture readers driven the way NativeEngine's input path would drive them: frames of
// Input::getTouches(), one finger for tap/hold/drag and two for the pinch.
#include "AndroidTouchGestures.h"
#include <cstdio>
using namespace jasperengine;
using namespace jasperengine::android;

static Touch finger(int id, Vector2 at, Vector2 from, TouchPhase phase, float held, Vector2 delta = {}) {
    Touch touch;
    touch.pointer = id; touch.fingerId = id;
    touch.position = at; touch.startPosition = from;
    touch.phase = phase; touch.time = held; touch.delta = delta;
    return touch;
}

int main() {
    RecognizerState state;
    TapOptions options;

    // --- a pinch: two fingers, 100 px apart, then 200 px apart
    updatePinch(state, {finger(0, Vector2(100, 300), Vector2(100, 300), TouchPhase::MOVED, 0.1f),
                        finger(1, Vector2(200, 300), Vector2(200, 300), TouchPhase::MOVED, 0.1f)}, 1);
    const float before = state.pinch.distance;
    updatePinch(state, {finger(0, Vector2(50, 300), Vector2(50, 300), TouchPhase::MOVED, 0.2f),
                        finger(1, Vector2(250, 300), Vector2(250, 300), TouchPhase::MOVED, 0.2f)}, 2);
    std::printf("pinch: %.0f px -> %.0f px, delta %.0f, midpoint (%.0f, %.0f), active=%d\n",
                before, state.pinch.distance, state.pinch.deltaDistance,
                state.pinch.midpoint.x, state.pinch.midpoint.y, state.pinch.active);

    // --- a drag: one finger, 60 px to the right, past the 8 px slop
    updateDrag(state.drag, {finger(3, Vector2(140, 400), Vector2(100, 400), TouchPhase::MOVED, 0.1f, Vector2(40, 0))});
    std::printf("drag: dragging=%d pointer=%d travel=(%.0f, %.0f)\n",
                state.drag.dragging, state.drag.pointer, state.drag.total.x, state.drag.total.y);

    // --- a long press: the same finger, still, Touch::time passing the 500 ms threshold
    updateLongPress(state, {finger(4, Vector2(300, 500), Vector2(300, 500), TouchPhase::BEGAN, 0.0f)}, 0.0f, options);
    int firedAt = -1;
    for (int frame = 1; frame <= 40; frame++) {
        const float held = frame * 0.016f;
        updateLongPress(state, {finger(4, Vector2(300, 500), Vector2(300, 500), TouchPhase::STATIONARY, held)},
                        held, options);
        if (state.longPress.triggered) firedAt = frame;
    }
    std::printf("long press: triggered=%d at frame %d (%.2f s held), consumed=%d\n",
                state.longPress.triggered ? 1 : 0, firedAt, state.longPress.heldSeconds, state.longPress.consumed);

    // --- a tap, then a second one 200 ms later: the double tap
    updateTap(state, {finger(5, Vector2(600, 200), Vector2(600, 200), TouchPhase::ENDED, 0.1f)}, options);
    std::printf("tap 1: tapped=%d\n", state.tap.tapped ? 1 : 0);
    updateTap(state, {finger(6, Vector2(602, 201), Vector2(602, 201), TouchPhase::ENDED, 0.3f)}, options);
    std::printf("tap 2: tapped=%d doubleTapped=%d interval=%.0f ms\n",
                state.tap.tapped ? 1 : 0, state.tap.doubleTapped ? 1 : 0, state.tap.interval * 1000.0f);
    return 0;
}
