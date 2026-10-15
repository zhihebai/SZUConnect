#include "info_popover_placement.hpp"

#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <limits>

using szu::app::InfoPopoverPlacement;
using szu::app::PopoverRect;
using szu::app::place_info_popover;

namespace {
int checks = 0, failures = 0;
void check(bool passed, const char* message) {
    ++checks;
    if (!passed) { ++failures; std::printf("FAIL: %s\n", message); }
}
float width(PopoverRect r) { return r.right - r.left; }
float height(PopoverRect r) { return r.bottom - r.top; }
bool nonempty(PopoverRect r) { return width(r) > 0 && height(r) > 0; }
bool intersects(PopoverRect a, PopoverRect b) {
    return nonempty(a) && nonempty(b) && a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}
bool inside(PopoverRect r, PopoverRect work) {
    return r.left >= work.left && r.top >= work.top && r.right <= work.right && r.bottom <= work.bottom;
}
void invariants(InfoPopoverPlacement p, PopoverRect work, PopoverRect anchor, PopoverRect context) {
    check(inside(p.rect, work), "placement stays in work area");
    check(!intersects(p.rect, anchor), "placement never covers the anchor");
    if (nonempty(p.rect)) check(p.avoids_context == !intersects(p.rect, context), "context flag describes actual overlap");
}
PopoverRect scaled(PopoverRect r, float scale) {
    return {r.left * scale, r.top * scale, r.right * scale, r.bottom * scale};
}
}  // namespace

int main() {
    {
        const PopoverRect work{0, 0, 1200, 800}, anchor{580, 390, 600, 410}, context{400, 300, 600, 500};
        const auto p = place_info_popover(work, anchor, context, 300, 200, 160, 80);
        invariants(p, work, anchor, context);
        check(p.avoids_context && p.rect.left == 608 && width(p.rect) == 300 && height(p.rect) == 200,
            "equal-area side nearest the info button wins");
    }
    {
        const PopoverRect work{0, 0, 1000, 800}, anchor{790, 330, 810, 350}, context{180, 300, 820, 400};
        const auto p = place_info_popover(work, anchor, context, 500, 250, 100, 80);
        invariants(p, work, anchor, context);
        check(p.avoids_context && p.rect.bottom <= context.top - 8 && width(p.rect) == 500 && height(p.rect) == 250,
            "much larger vertical placement beats narrow sides");
    }
    {
        const PopoverRect work{0, 0, 1000, 600}, anchor{690, 290, 710, 310}, context{288, 210, 712, 390};
        const auto p = place_info_popover(work, anchor, context, 300, 200, 100, 80);
        invariants(p, work, anchor, context);
        check(p.rect.left == 720 && width(p.rect) == 280, "side may trade less than ten percent of area");
        const auto narrow = place_info_popover(work, anchor, {268, 210, 732, 390}, 300, 200, 100, 80);
        check(width(narrow.rect) == 300 && (narrow.rect.bottom <= 202 || narrow.rect.top >= 398),
            "side preference does not discard substantially more readable area");
    }
    {
        const PopoverRect work{-1920, -1080, 0, 0}, anchor{-50, -40, -30, -20}, context{-500, -180, -20, -10};
        const auto p = place_info_popover(work, anchor, context, 320, 220, 160, 100);
        invariants(p, work, anchor, context);
        check(p.avoids_context && p.rect.right <= -508, "negative monitor coordinates and bottom edge placement");
        const auto p150 = place_info_popover(scaled(work, 1.5f), scaled(anchor, 1.5f), scaled(context, 1.5f), 480, 330, 240, 150, 12);
        const auto expected = scaled(p.rect, 1.5f);
        check(std::abs(p150.rect.left - expected.left) < 0.01f && std::abs(p150.rect.top - expected.top) < 0.01f &&
            std::abs(p150.rect.right - expected.right) < 0.01f && std::abs(p150.rect.bottom - expected.bottom) < 0.01f,
            "uniform DPI scaling preserves placement");
    }
    {
        const PopoverRect work{0, 0, 800, 500}, anchor{378, 228, 422, 272};
        const auto p = place_info_popover(work, anchor, work, 320, 180, 200, 120);
        invariants(p, work, anchor, work);
        check(nonempty(p.rect) && !p.avoids_context, "full-context fallback reports the compromise");
        const auto tiny = place_info_popover({0, 0, 100, 60}, {40, 20, 60, 40}, {0, 0, 100, 60}, 320, 200, 180, 100);
        invariants(tiny, {0, 0, 100, 60}, {40, 20, 60, 40}, {0, 0, 100, 60});
        check(nonempty(tiny.rect) && width(tiny.rect) < 180, "tiny work area permits a narrow scrolling fallback");
        const auto blocked = place_info_popover(work, work, work, 320, 180, 200, 120);
        invariants(blocked, work, work, work);
        check(!nonempty(blocked.rect), "no available pixels returns empty rather than covering anchor");
    }
    {
        const PopoverRect work{0, 0, 100, 100}, anchor{1, 1, 99, 99};
        const auto p = place_info_popover(work, anchor, work, 320, 200, 180, 100);
        invariants(p, work, anchor, work);
        check(nonempty(p.rect), "gap may relax only when necessary to avoid the anchor");
        const auto invalid = place_info_popover(work, anchor, work, std::numeric_limits<float>::quiet_NaN(), 200, 100, 100);
        check(!nonempty(invalid.rect) && !invalid.avoids_context, "non-finite size fails closed");
        check(!nonempty(place_info_popover({0, 0, 0, 0}, anchor, work, 320, 200, 100, 100).rect), "zero work area is empty");
    }
    // Property coverage: taskbars/work-area offsets, four screen corners, small
    // work areas and three DPI factors. No monitor or window APIs are simulated.
    for (const float scale : {1.0f, 1.5f, 2.0f}) {
        for (const float w : {200.0f, 480.0f, 1920.0f}) for (const float h : {180.0f, 600.0f, 1080.0f}) {
            const PopoverRect work{-1000 * scale, 40 * scale, (-1000 + w) * scale, (40 + h) * scale};
            for (const float x : {0.0f, 0.5f, 1.0f}) for (const float y : {0.0f, 0.5f, 1.0f}) {
                const float left = work.left + x * (w - 44) * scale, top = work.top + y * (h - 44) * scale;
                const PopoverRect anchor{left, top, left + 44 * scale, top + 44 * scale};
                const PopoverRect context{left - 56 * scale, top - 10 * scale, left + 44 * scale, top + 54 * scale};
                const auto p = place_info_popover(work, anchor, context, 320 * scale, 240 * scale, 160 * scale, 96 * scale, 8 * scale);
                invariants(p, work, anchor, context);
                check(width(p.rect) <= 320 * scale && height(p.rect) <= 240 * scale, "desired size remains an upper bound");
            }
        }
    }
    std::printf("info popover placement: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
