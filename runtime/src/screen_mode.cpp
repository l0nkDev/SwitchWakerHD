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
// Off-TV Play draws the GamePad's touch guides over the game: with the Wind Waker out, the slide-conducting
// guide (BatonSignList_00's N_SlideInput_00: a circle with four arrows, P_BatonSlideInput_00/01, and the
// Wind Waker icon, P_BatonSlideInputIcon_00) sits in the middle of the picture. It is not drawn in Off-TV
// Play (hidden_pane, from the layout Pane::Draw hook in aspect.cpp); in TV play it stays on the GamePad
// screen with the touch controls it belongs to.
//
// The game's screen mode: the HD controller manager (*101F5088) +0x1D0, set by 02618094(mgr, mode):
//   0 TV only (no GamePad), 1 TV play (the GamePad shows its own screen), 2 Off-TV Play, 3 title screen
//   (the picture on both).
// When a file is chosen (stage Name), 0270AB10 sets mode 1 if the GamePad is in use (0 otherwise), before the
// first scene of play is set up. A scene's setup takes the mode as it finds it (02713368: the GamePad's sound
// mix in mode 2, its layouts and flags), which is how a door keeps Off-TV Play; with mode 2 set here the first
// scene is set up the same way. The game's own Minus (0270D668, in the GamePad menu manager's StateID_Play:
// ChangeToTvMode / ChangeToDrcMode) then works as on the Wii U.
#include "screen_mode.h"

#include <cstdint>
#include <cstring>

#include "guest_addr.h"
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

namespace screen_mode {
bool hidden_pane(uint32_t pane) {
    constexpr uint32_t kMode = 0x1D0, kPaneName = 0x80;
    static const char kSlideInput[] = "N_SlideInput_00";
    if (!gamepad_mode()) return false;
    if (memcmp(mem::ptr(pane + kPaneName), kSlideInput, sizeof kSlideInput)) return false;
    const uint32_t mgr = ld32(GD(0x101F5088));  // ControllerMgr*
    return mgr && ld32(mgr + kMode) == 2;  // Off-TV Play
}
}  // namespace screen_mode

// file select: changeState(picker widget machine (widget+0x18), r4 = StateID_Display)
extern "C" void site_0270AC04(Cpu* c) {
    if (c->r[4] != GD(0x1049AA74) || !gamepad_mode()) return;  // StateID_Display
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


// ---- the Controller option in the pause menu's Options (GamePad mode: the GamePad is the only controller)
// The options window (constructor 026D7828, 0x148 bytes) lists 6 items in the US game: Target Type, Camera,
// First-Person Camera, Controller, Gyroscope, UI Display (Furigana is Japan only; the Miiverse items have their
// own window, mode 1 at +0x118). Its data:
//   item descriptors, static at 1010307C, 24 bytes an item: value message ids, a message table, flags and a
//     callback (PTMF); item 3, Controller, the only one with a callback: 026D87D0, the controller change
//   +0x104..+0x109 the items' values, a byte each in item order: the loader (026D5CE4) fills them (Controller
//     from the screen mode, Gyroscope and UI Display from the save's getters 0271FC44 / 0271FC8C), the saver
//     (026D6DD4) writes back by fixed offsets (+0x108 Gyroscope, +0x109 UI Display), and everything else reads
//     and writes value[item]
//   +0x120 the item count (6, set by 026D83F0), +0x124 the row panes (5, recycled while scrolling: item % 5),
//     +0x128 the cursor item, +0x12C the first item shown
// Without Controller: the items are put in the order Target Type, Camera, First-Person, Gyroscope, UI Display,
// Controller (descriptors and values alike) and the count is 5, so Controller is past the end: never shown, never
// reached. The code that uses fixed offsets (the saver, the Controller callback) sees the game's order.
namespace {
constexpr uint32_t kOptDescSize = 0x18;
uint32_t opt_descs() { return GD(0x1010307C); }  // the item descriptors
constexpr uint32_t kOptValues = 0x104;
bool g_opt_reordered = false;  // the descriptor table is in the GamePad order

void swap_desc_order(bool reordered) {  // items 3, 4, 5: Controller, Gyroscope, UI <-> Gyroscope, UI, Controller
    if (g_opt_reordered == reordered) return;
    uint32_t d[3][6];
    for (int i = 0; i < 3; i++)
        for (int w = 0; w < 6; w++) d[i][w] = ld32(opt_descs() + (3 + i) * kOptDescSize + w * 4);
    static const int kTo[3] = {1, 2, 0}, kFrom[3] = {2, 0, 1};  // reordered[k] = game[kTo[k]]; game[k] = reordered[kFrom[k]]
    for (int i = 0; i < 3; i++)
        for (int w = 0; w < 6; w++) st32(opt_descs() + (3 + i) * kOptDescSize + w * 4, d[reordered ? kTo[i] : kFrom[i]][w]);
    g_opt_reordered = reordered;
    LOG("[screen] options: the Controller item %s", reordered ? "removed (GamePad mode)" : "back");
}
void values_to_reordered(uint32_t win) {  // game order (Controller, Gyroscope, UI) -> GamePad order
    const uint8_t ctl = ld8(win + kOptValues + 3), gyro = ld8(win + kOptValues + 4), ui = ld8(win + kOptValues + 5);
    st8(win + kOptValues + 3, gyro); st8(win + kOptValues + 4, ui); st8(win + kOptValues + 5, ctl);
}
void values_to_game(uint32_t win) {
    const uint8_t gyro = ld8(win + kOptValues + 3), ui = ld8(win + kOptValues + 4), ctl = ld8(win + kOptValues + 5);
    st8(win + kOptValues + 3, ctl); st8(win + kOptValues + 4, gyro); st8(win + kOptValues + 5, ui);
}
bool options_mode(uint32_t win) { return ld32(win + 0x118) == 0; }  // the Options window (not Miiverse's)
}  // namespace

extern "C" void f_026D7828_orig(Cpu* c);
extern "C" void hook_026D7828(Cpu* c) {  // the options window constructed (each scene): the item order for the mode
    swap_desc_order(gamepad_mode());
    f_026D7828_orig(c);
}
// options list setup, Options mode: the count (r7 = 6) stored at +0x120
extern "C" void site_026D8514(Cpu* c) {
    if (g_opt_reordered && c->r[7] == 6) c->r[7] = 5;
}
// the values loaded, the visible rows not filled yet (r31 the window)
extern "C" void site_026D5D84(Cpu* c) {
    if (g_opt_reordered && options_mode(c->r[31])) values_to_reordered(c->r[31]);
}
extern "C" void f_026D6DD4_orig(Cpu* c);
extern "C" void hook_026D6DD4(Cpu* c) {  // save: by fixed offsets
    const uint32_t win = c->r[3];
    const bool swap = g_opt_reordered && options_mode(win);
    if (swap) values_to_game(win);
    f_026D6DD4_orig(c);
    if (swap) values_to_reordered(win);
}
extern "C" void f_026D87D0_orig(Cpu* c);
extern "C" void hook_026D87D0(Cpu* c) {  // the Controller item's callback: its value at +0x107
    const uint32_t win = c->r[3];
    const bool swap = g_opt_reordered && options_mode(win);
    if (swap) values_to_game(win);
    f_026D87D0_orig(c);
    if (swap) values_to_reordered(win);
}
