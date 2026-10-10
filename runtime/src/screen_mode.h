// GamePad mode on the Switch: no controller picker, play in Off-TV Play (see screen_mode.cpp).
#pragma once
#include <cstdint>

namespace screen_mode {
// a layout pane not to draw (with its children): the GamePad's touch guides over the game in Off-TV Play
bool hidden_pane(uint32_t pane);
}
