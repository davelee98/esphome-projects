#pragma once

// The panel's inks, defined once for the driver and for page components that
// draw through it (such as aqi_dashboard).
//
// The values are the BWRY logical colour codes (the index order of a
// display_panel code_map): black 0, white 1, yellow 2, red 3. The driver
// writes them into its frame buffer as they are, and maps them to the
// label's own panel codes only for the upload. Pixels are drawn in exactly
// these inks; there is no colour matching or dithering.

#include <cstdint>

namespace esphome::opendisplay {

enum class Ink : uint8_t { BLACK = 0, WHITE = 1, YELLOW = 2, RED = 3 };

}  // namespace esphome::opendisplay
