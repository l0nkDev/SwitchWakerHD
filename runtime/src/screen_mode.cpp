// The GamePad without asking, and play in Off-TV Play. With the controller acting as the GamePad the window shows
// the GamePad picture (input_switch.cpp): the game itself in Off-TV Play, the GamePad's own screen (items, map,
// bottles; touch) in TV play. On the Wii U the file select first asks which controller will be used (GamePad or
// Pro Controller), then play starts in TV play; here the GamePad is taken as chosen and play starts in Off-TV
// Play, as if Minus had been pressed before play began, with no switch to see.
//
// File select (its manager's states: ControllerSelect 1049EA2C, SaveDataSelect 1049EA58, NameInput, Cancel,
// CheckData, ...; the names are in the game's data). ControllerSelect's execute (0270AB10), on its first frame
// once the title's transition is done, opens the picker widget (this+0x130; its states Wait 1049AA44,
// Display 1049AA74, InAll, OutAll, ...) with changeState(widget+0x18, Display) at 0270AC04; on later frames it
// waits for the widget's answer: +0x50 cancelled (back to the title), +0x51 decided, +0x54 the controller
// (0 GamePad: 026D5120), then sets the mode and goes to SaveDataSelect. The hook leaves the widget in its
// current (hidden) state and writes the answer "decided, GamePad": the next frame takes the game's own GamePad
// branch (the save list's transition, SaveDataSelect), and the picker is never shown.
//
// The game's screen mode: the HD controller manager (*101F5088) +0x1D0, set by 02618094(mgr, mode):
//   0 TV only (no GamePad), 1 TV play (the GamePad shows its own screen), 2 Off-TV Play, 3 title screen
//   (the picture on both).
// When a file is chosen (stage Name), 0270AB10 sets mode 1 if the GamePad is in use (0 otherwise), before the
// first scene of play is set up. A scene's setup takes the mode as it finds it (02713368: the GamePad's sound
// mix in mode 2, its layouts and flags), which is how a door keeps Off-TV Play; with mode 2 set here the first
// scene is set up the same way. The game's own Minus (0270D668, in the GamePad menu manager's StateID_Play:
// ChangeToTvMode / ChangeToDrcMode) then works as on the Wii U.
#include <cstdint>

#include "input.h"
#include "runtime.h"

namespace {
bool gamepad_mode() {
#ifdef __SWITCH__  // the desktop builds show the GamePad screen beside the TV picture and keep the game's choice
    return !input::pro_controller();
#else
    return false;
#endif
}
}  // namespace

// file select: changeState(picker widget machine (widget+0x18), r4 = StateID_Display)
extern "C" void site_0270AC04(Cpu* c) {
    constexpr uint32_t kStateDisplay = 0x1049AA74;
    if (c->r[4] != kStateDisplay || !gamepad_mode()) return;
    const uint32_t machine = c->r[3], widget = machine - 0x18;
    c->r[4] = ld32(machine + 4);  // the state it is in (hidden): entered again instead of Display
    st8(widget + 0x50, 0);        // not cancelled
    st8(widget + 0x51, 1);        // decided
    st32(widget + 0x54, 0);       // the GamePad
    LOG("[screen] file select: the GamePad taken as chosen (no controller picker)");
}

// play start: 02618094(mgr, r4) with r4 = 1 (TV play)
extern "C" void site_0270ACC8(Cpu* c) {
    if (c->r[4] == 1 && gamepad_mode()) {
        c->r[4] = 2;
        LOG("[screen] play starts in Off-TV Play");
    }
}
