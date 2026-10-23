#pragma once

namespace szu::app {

// All coordinates and dimensions use the same units, normally physical screen pixels.
struct PopoverRect {
    float left = 0, top = 0, right = 0, bottom = 0;
};

struct InfoPopoverPlacement {
    PopoverRect rect;
    bool avoids_context = false;
};

// Prefer space beside the explained context. Nearby side placements may trade up
// to 10% of area for a position to its left/right. A constrained fallback may be
// smaller than the minimum but never intersects the anchor. If the anchor fills
// the work area (or inputs are invalid), return an empty rectangle: do not show it.
// A caller converting float coordinates to integer pixels should round inward.
InfoPopoverPlacement place_info_popover(PopoverRect work, PopoverRect anchor, PopoverRect protected_rect,
    float desired_width, float desired_height, float min_width, float min_height, float gap = 8.0f);

}  // namespace szu::app
