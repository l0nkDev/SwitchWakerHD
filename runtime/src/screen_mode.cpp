// Play starts in Off-TV Play. With the controller acting as the GamePad the window shows the GamePad picture
// (input_switch.cpp): the game itself in Off-TV Play, the GamePad's own screen (items, map, bottles; touch) in
// TV play. The game starts every session in TV play; here it starts in Off-TV Play instead, as if Minus had
// been pressed before play began, with no switch to see.
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

// play start: 02618094(mgr, r4) with r4 = 1 (TV play)
extern "C" void site_0270ACC8(Cpu* c) {
#ifdef __SWITCH__  // the desktop builds show the GamePad screen beside the TV picture
    if (c->r[4] == 1 && !input::pro_controller()) {
        c->r[4] = 2;
        LOG("[screen] play starts in Off-TV Play");
    }
#endif
}
