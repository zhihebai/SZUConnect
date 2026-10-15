#include "info_popover_placement.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace szu::app {
namespace {

float width(PopoverRect r) { return r.right - r.left; }
float height(PopoverRect r) { return r.bottom - r.top; }
bool nonempty(PopoverRect r) { return width(r) > 0 && height(r) > 0; }
bool finite(PopoverRect r) {
    return std::isfinite(r.left) && std::isfinite(r.top) && std::isfinite(r.right) && std::isfinite(r.bottom) &&
        std::isfinite(width(r)) && std::isfinite(height(r));
}
PopoverRect normalized(PopoverRect r) {
    if (r.right < r.left) std::swap(r.left, r.right);
    if (r.bottom < r.top) std::swap(r.top, r.bottom);
    return r;
}
bool intersects(PopoverRect a, PopoverRect b) {
    return nonempty(a) && nonempty(b) && std::min(a.right, b.right) > std::max(a.left, b.left) &&
        std::min(a.bottom, b.bottom) > std::max(a.top, b.top);
}
PopoverRect clipped(PopoverRect r, PopoverRect work) {
    if (!intersects(r, work)) return {work.left, work.top, work.left, work.top};
    return {std::max(r.left, work.left), std::max(r.top, work.top),
        std::min(r.right, work.right), std::min(r.bottom, work.bottom)};
}
PopoverRect united(PopoverRect a, PopoverRect b) {
    if (!nonempty(a)) return b;
    if (!nonempty(b)) return a;
    return {std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom)};
}

enum class Side { right, left, below, above };
struct Candidate {
    PopoverRect rect;
    bool lateral;
    double area, distance;
};

std::vector<Candidate> candidates(PopoverRect work, PopoverRect avoid, PopoverRect anchor,
    float desired_width, float desired_height, float min_width, float min_height, float gap) {
    std::vector<Candidate> out;
    const float anchor_x = anchor.left + width(anchor) / 2;
    const float anchor_y = anchor.top + height(anchor) / 2;
    const auto offer = [&](PopoverRect area, Side side) {
        const float w = std::min(desired_width, width(area)), h = std::min(desired_height, height(area));
        if (w <= 0 || h <= 0 || w < min_width || h < min_height) return;
        const bool lateral = side == Side::left || side == Side::right;
        const float x = lateral ? (side == Side::left ? area.right - w : area.left) :
            std::clamp(anchor_x - w / 2, area.left, area.right - w);
        const float y = lateral ? std::clamp(anchor_y - h / 2, area.top, area.bottom - h) :
            (side == Side::above ? area.bottom - h : area.top);
        const double dx = static_cast<double>(x) + w / 2 - anchor_x;
        const double dy = static_cast<double>(y) + h / 2 - anchor_y;
        out.push_back({{x, y, x + w, y + h}, lateral, static_cast<double>(w) * h, dx * dx + dy * dy});
    };
    if (!nonempty(avoid)) {
        const float w = std::min(desired_width, width(work)), h = std::min(desired_height, height(work));
        if (w >= min_width && h >= min_height) {
            const float x = std::clamp(anchor_x - w / 2, work.left, work.right - w);
            const float y = std::clamp(anchor_y - h / 2, work.top, work.bottom - h);
            out.push_back({{x, y, x + w, y + h}, false, static_cast<double>(w) * h, 0});
        }
        return out;
    }
    offer({std::min(work.right, avoid.right + gap), work.top, work.right, work.bottom}, Side::right);
    offer({work.left, work.top, std::max(work.left, avoid.left - gap), work.bottom}, Side::left);
    offer({work.left, std::min(work.bottom, avoid.bottom + gap), work.right, work.bottom}, Side::below);
    offer({work.left, work.top, work.right, std::max(work.top, avoid.top - gap)}, Side::above);
    return out;
}

const Candidate& best(const std::vector<Candidate>& values) {
    const auto better = [](const Candidate& a, const Candidate& b) {
        return a.area > b.area || (a.area == b.area && a.distance < b.distance);
    };
    const Candidate* result = &values.front();
    for (const auto& value : values) if (better(value, *result)) result = &value;
    const double near_area = result->area * 0.9;
    const Candidate* lateral = nullptr;
    for (const auto& value : values) {
        if (value.lateral && value.area >= near_area && (!lateral || better(value, *lateral))) lateral = &value;
    }
    return lateral ? *lateral : *result;
}

}  // namespace

InfoPopoverPlacement place_info_popover(PopoverRect work, PopoverRect anchor, PopoverRect protected_rect,
    float desired_width, float desired_height, float min_width, float min_height, float gap) {
    if (!finite(work) || !nonempty(work)) return {};
    const InfoPopoverPlacement empty{{work.left, work.top, work.left, work.top}, false};
    if (!finite(anchor) || !finite(protected_rect) || !std::isfinite(desired_width) || !std::isfinite(desired_height) ||
        !std::isfinite(min_width) || !std::isfinite(min_height) || !std::isfinite(gap) || desired_width <= 0 || desired_height <= 0) return empty;
    anchor = normalized(anchor); protected_rect = normalized(protected_rect);
    min_width = std::clamp(min_width, 0.0f, desired_width);
    min_height = std::clamp(min_height, 0.0f, desired_height);
    gap = std::max(0.0f, gap);
    const auto visible_anchor = clipped(anchor, work);
    const auto context = united(clipped(protected_rect, work), visible_anchor);
    auto options = candidates(work, context, anchor, desired_width, desired_height, min_width, min_height, gap);
    if (options.empty()) {
        // There may be no readable rectangle beside the full context. Preserve
        // the anchor even when a tiny work area requires a narrow scrolling body.
        options = candidates(work, visible_anchor, anchor, desired_width, desired_height, 0, 0, gap);
        if (options.empty() && gap > 0)
            options = candidates(work, visible_anchor, anchor, desired_width, desired_height, 0, 0, 0);
    }
    if (options.empty()) return empty;
    auto rect = best(options).rect;
    // Float addition can extend the far edge by an ULP at large coordinates.
    rect.right = std::min(rect.right, work.right); rect.bottom = std::min(rect.bottom, work.bottom);
    return {rect, !intersects(rect, protected_rect)};
}

}  // namespace szu::app
