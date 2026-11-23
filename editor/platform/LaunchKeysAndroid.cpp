// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// Whether Shift is held as the editor starts (which opens the Project Hub).
//
// Android has no Shift at launch: there is no hardware keyboard to poll and no modifier state
// before the first input event. The Project Hub is reached from the editor's own menu instead,
// which is the only way a touch screen could ask for it anyway.

#include "LaunchKeys.h"

namespace jasperengine::editor::platform {

    bool shiftHeld(){ return false; }

} // namespace jasperengine::editor::platform
