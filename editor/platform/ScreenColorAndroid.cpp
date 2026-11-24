// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// The eyedropper on Android reads no screen, and says so.
//
// Reading another app's screen on Android needs a MediaProjection session: a system dialog the
// user must accept, a foreground service, and a VirtualDisplay rendering into a Surface. That is
// a permission flow, not a screen read, and it would be wrong to trigger it from an eyedropper
// button. So every entry point reports "not supported" and the editor's colour picker falls back
// to its own palette, exactly as it does on a Wayland session that cannot read the screen.

#include "ScreenColor.h"

namespace jasperengine::editor::screencolor {

    bool liveSupported(){ return false; }

    bool begin(std::string& why){
        why = "The eyedropper cannot read an Android screen (it needs a MediaProjection session)";
        return false;
    }

    void end(){}

    bool active(){ return false; }

    bool cursor(int&, int&){ return false; }

    bool read9x9(int, int, uint8_t*){ return false; }

    Button pollButton(){ return Button::Idle; }

    // A touch screen has no pointer outside the editor's own window, so no point is ever inside
    // one: the caller keeps its own window-local coordinates from the touch it already has.
    bool toWindow(void*, int, int, float&, float&){ return false; }

    bool pickOnceSupported(){ return false; }

    void pickOnce(std::function<void(bool, float, float, float)> done){
        if (done)
            done(false, 0.0f, 0.0f, 0.0f);
    }

    bool waylandSession(){ return false; }

} // namespace jasperengine::editor::screencolor
