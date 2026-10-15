#include "scene.hpp"

#include <algorithm>
#include <cmath>

#include "shared_tokens.hpp"

namespace szu::app {

D2D1_RECT_F inflate(const D2D1_RECT_F& r, float d) { return D2D1::RectF(r.left - d, r.top - d, r.right + d, r.bottom + d); }

namespace {
bool contains(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
constexpr float kTarget = tokens::kSizeMinTarget;  // 44 DIP minimum hit target
}  // namespace

// ---------------- Scene ----------------
const Widget* Scene::find(const std::string& id) const {
    if (id.empty()) return nullptr;
    for (const Widget& w : widgets)
        if (w.id == id) return &w;
    return nullptr;
}

D2D1_RECT_F Scene::visible_rect(const Widget& w) const {
    if (!w.scrolls || w.layer >= static_cast<int>(layers.size())) return w.rect;
    const float dy = layers[static_cast<std::size_t>(w.layer)].scroll;
    return D2D1::RectF(w.rect.left, w.rect.top - dy, w.rect.right, w.rect.bottom - dy);
}

const Widget* Scene::hit(float x, float y) const {
    const int top = top_layer();
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) {
        if (it->layer != top || !it->action || !it->enabled) continue;
        if (it->scrolls && !contains(layers[static_cast<std::size_t>(top)].viewport, x, y)) continue;
        if (contains(visible_rect(*it), x, y)) return &*it;
    }
    return nullptr;
}

std::vector<const Widget*> Scene::focus_order() const {
    std::vector<const Widget*> out;
    const int top = top_layer();
    for (const Widget& w : widgets)
        if (w.layer == top && w.focusable && w.enabled) out.push_back(&w);
    return out;
}

void Scene::clamp_scroll(int layer) {
    Layer& l = layers[static_cast<std::size_t>(layer)];
    const float visible = l.viewport.bottom - l.viewport.top;
    const float max_scroll = std::max(0.0f, l.content_height - visible);
    l.scroll = std::clamp(l.scroll, 0.0f, max_scroll);
}

// ---------------- Builder ----------------
Builder::Builder(Scene& scene, Canvas& canvas, int layer, float left, float width, float top, bool scrolls)
    : scene_(scene), canvas_(canvas), layer_(layer), scrolls_(scrolls), x_(left), w_(width), y_(top) {}

Widget& Builder::add(Widget w) {
    w.layer = layer_;
    w.scrolls = scrolls_;
    scene_.widgets.push_back(std::move(w));
    return scene_.widgets.back();
}

float Builder::text(std::wstring_view text, TextStyle style, std::string color, Role role, std::string id) {
    if (text.empty()) return 0;
    const float h = canvas_.measure(text, style, w_).height;
    Widget w;
    w.kind = Kind::text;
    w.rect = D2D1::RectF(x_, y_, x_ + w_, y_ + h);
    w.text = std::wstring(text);
    w.style = style;
    w.color = std::move(color);
    w.role = role;
    w.name = w.text;
    w.id = std::move(id);
    add(std::move(w));
    y_ += h;
    return h;
}

float Builder::text_at(std::wstring_view text, TextStyle style, std::string color, float x, float width, float y) {
    const float h = canvas_.measure(text, style, width).height;
    Widget w;
    w.kind = Kind::text;
    w.rect = D2D1::RectF(x, y, x + width, y + h);
    w.text = std::wstring(text);
    w.style = style;
    w.color = std::move(color);
    w.role = Role::text;
    w.name = w.text;
    add(std::move(w));
    return h;
}

void Builder::heading(std::wstring_view text, TextStyle style) {
    this->text(text, style, "textPrimary", Role::heading);
    y_ += 12;
}

void Builder::section(std::wstring_view title) {
    y_ += 8;
    text(title, TextStyle::headline, "textPrimary", Role::heading);
    y_ += 8;
}

void Builder::icon_row(std::string icon, std::string color, std::wstring text, TextStyle style, std::string text_color, float icon_size) {
    const float text_x = x_ + icon_size + 10;
    const float h = std::max(icon_size, canvas_.measure(text, style, w_ - icon_size - 10).height);
    Widget i;
    i.kind = Kind::icon;
    i.icon = std::move(icon);
    i.color = std::move(color);
    const float line = canvas_.line_height(style);
    const float icon_top = y_ + std::max(0.0f, (std::min(h, line) - icon_size) / 2);
    i.rect = D2D1::RectF(x_, icon_top, x_ + icon_size, icon_top + icon_size);
    add(std::move(i));
    if (!text.empty()) text_at(text, style, std::move(text_color), text_x, std::max(0.0f, w_ - icon_size - 10), y_);
    y_ += h;
}

float Builder::measure_button(const std::wstring& label, const std::string& icon) {
    const float text_w = canvas_.measure(label, TextStyle::body_strong, 10000).width;
    return text_w + 32 + (icon.empty() ? 0 : 26);
}

void Builder::button(std::string id, std::wstring label, ButtonStyle style, std::function<void()> action, std::string icon, bool full,
                     bool enabled) {
    const float width = full ? w_ : std::min(w_, measure_button(label, icon));
    const float height = std::max(kTarget, canvas_.measure(label, TextStyle::body_strong, std::max(8.0f, width - 32 - (icon.empty() ? 0 : 26))).height + 20);
    Widget w;
    w.kind = Kind::button;
    w.rect = D2D1::RectF(x_, y_, x_ + width, y_ + height);
    w.text = label;
    w.name = label;
    w.button = style;
    w.icon = std::move(icon);
    w.radius = style == ButtonStyle::link ? 0.0f : 10.0f;
    w.id = std::move(id);
    w.role = style == ButtonStyle::link ? Role::link : Role::button;
    w.focusable = true;
    w.enabled = enabled;
    w.action = std::move(action);
    add(std::move(w));
    y_ += height;
}

void Builder::icon_button(std::string id, std::wstring name, std::string icon, std::function<void()> action, bool enabled) {
    Widget w; w.kind = Kind::button; w.button = ButtonStyle::plain; w.icon = std::move(icon);
    w.rect = D2D1::RectF(x_, y_, x_ + std::min(w_, kTarget), y_ + kTarget);
    w.id = std::move(id); w.name = std::move(name); w.role = Role::button;
    w.focusable = true; w.enabled = enabled; w.action = std::move(action);
    add(std::move(w)); y_ += kTarget;
}

void Builder::menu_button(std::string id, std::wstring label, std::wstring name, std::function<void()> action, bool enabled) {
    const float height = std::max(kTarget, canvas_.measure(label, TextStyle::callout, std::max(8.0f, w_ - 30)).height + 12);
    Widget control; control.kind = Kind::button; control.button = ButtonStyle::secondary;
    control.rect = D2D1::RectF(x_, y_, x_ + w_, y_ + height);
    control.id = std::move(id); control.role = Role::button; control.compact = true; control.radius = tokens::kRadiusSmall;
    control.text = std::move(label); control.name = std::move(name);
    control.focusable = true; control.enabled = enabled; control.action = std::move(action);
    add(std::move(control)); y_ += height;
}

void Builder::buttons(std::vector<ButtonSpec> specs, bool stretch) {
    if (specs.empty()) return;
    const float gap = tokens::kSpaceS;
    float x = x_, top = y_, row_h = 0;
    for (auto& spec : specs) {
        const float bw = stretch ? w_ : std::min(w_, measure_button(spec.label, spec.icon));
        const float inner = std::max(8.0f, bw - 32 - (spec.icon.empty() ? 0 : 26));
        const float bh = std::max(kTarget, canvas_.measure(spec.label, TextStyle::body_strong, inner).height + 20);
        if (x > x_ && (stretch || x + bw > x_ + w_ + 0.5f)) {
            x = x_; top += row_h + gap; row_h = 0;
        }
        Widget w;
        w.kind = Kind::button; w.rect = D2D1::RectF(x, top, x + bw, top + bh);
        w.text = spec.label; w.name = spec.label; w.button = spec.style; w.icon = spec.icon;
        w.radius = spec.style == ButtonStyle::link ? 0 : tokens::kRadiusMedium;
        w.id = spec.id; w.role = spec.style == ButtonStyle::link ? Role::link : Role::button;
        w.focusable = true; w.enabled = spec.enabled; w.action = spec.action;
        add(std::move(w)); x += bw + gap; row_h = std::max(row_h, bh);
    }
    y_ = top + row_h;
}

void Builder::row(std::vector<std::function<void(Builder&)>> cells, std::vector<float> widths, float gap, float min_fill_width) {
    if (cells.empty() || cells.size() != widths.size()) return;
    float fixed = gap * static_cast<float>(cells.size() - 1); std::size_t flexible = 0;
    for (float width : widths) { if (width > 0) fixed += width; else ++flexible; }
    const float remaining = flexible ? (w_ - fixed) / static_cast<float>(flexible) : 0;
    if (fixed > w_ || (flexible && remaining < min_fill_width)) {
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (i) y_ += gap;
            Builder child(scene_, canvas_, layer_, x_, w_, y_, scrolls_);
            cells[i](child); y_ = child.y();
        }
        return;
    }
    float x = x_, bottom = y_;
    for (std::size_t i = 0; i < cells.size(); ++i) {
        const float width = widths[i] > 0 ? widths[i] : remaining;
        Builder child(scene_, canvas_, layer_, x, width, y_, scrolls_);
        cells[i](child); bottom = std::max(bottom, child.y()); x += width + gap;
    }
    y_ = bottom;
}

void Builder::columns(std::vector<std::function<void(Builder&)>> cells, float min_column_width, float gap) {
    if (cells.empty()) return;
    if ((w_ - gap * static_cast<float>(cells.size() - 1)) / static_cast<float>(cells.size()) < min_column_width) {
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (i) y_ += gap;
            Builder child(scene_, canvas_, layer_, x_, w_, y_, scrolls_);
            cells[i](child); y_ = child.y();
        }
    } else { const std::size_t count = cells.size(); row(std::move(cells), std::vector<float>(count, 0), gap); }
}

void Builder::toggle(std::string id, std::wstring label, std::wstring detail, bool checked, std::function<void()> action, bool enabled, bool checkbox,
                     TextStyle label_style) {
    const TextStyle style = checkbox ? TextStyle::callout : label_style;
    const float text_w = w_ - (checkbox ? 28 : 64);
    const float label_h = canvas_.measure(label, style, text_w).height;
    const float detail_h = detail.empty() ? 0 : canvas_.measure(detail, TextStyle::caption, text_w).height + 2;
    const float height = std::max(kTarget, label_h + detail_h + 16);
    Widget w;
    w.kind = Kind::toggle;
    w.compact = checkbox;
    w.style = style;
    w.rect = D2D1::RectF(x_, y_, x_ + w_, y_ + height);
    w.text = label;
    w.detail = detail;
    w.name = label;
    w.checked = checked;
    w.radius = 8;
    w.id = std::move(id);
    w.role = Role::toggle;
    w.focusable = true;
    w.enabled = enabled;
    w.action = std::move(action);
    add(std::move(w));
    y_ += height;
}

void Builder::choices(std::string group, std::vector<Choice> options, int selected, std::function<void(int)> select, bool enabled) {
    for (std::size_t i = 0; i < options.size(); ++i) {
        const Choice& c = options[i];
        const float text_w = w_ - 56;
        const float title_h = canvas_.measure(c.title, TextStyle::body_strong, text_w).height;
        const float detail_h = c.detail.empty() ? 0 : canvas_.measure(c.detail, TextStyle::caption, text_w).height + 2;
        const float height = std::max(56.0f, title_h + detail_h + 20);
        Widget w;
        w.kind = Kind::radio;
        w.rect = D2D1::RectF(x_, y_, x_ + w_, y_ + height);
        w.text = c.title;
        w.detail = c.detail;
        w.icon = c.icon;
        w.name = c.title;
        w.checked = static_cast<int>(i) == selected;
        w.radius = 10;
        w.id = group + "." + std::to_string(i);
        w.group = group;
        w.role = Role::radio;
        w.focusable = true;
        w.enabled = enabled;
        const int index = static_cast<int>(i);
        const int count = static_cast<int>(options.size());
        w.action = [select, index] { select(index); };
        w.step = [select, index, count](int delta) { select((index + delta + count) % count); };
        add(std::move(w));
        y_ += height + 8;
    }
    if (!options.empty()) y_ -= 8;
}

void Builder::segmented(std::string group, std::wstring label, std::vector<std::wstring> options, int selected,
                        std::function<void(int)> select, bool enabled, float max_width) {
    if (options.empty()) return;
    if (!label.empty()) { text(label, TextStyle::callout, "textSecondary"); y_ += tokens::kSpaceXs; }
    float option_width = kTarget;
    for (const auto& label_text : options) option_width = std::max(option_width, canvas_.measure(label_text, TextStyle::body_strong, 10000).width + 24);
    const float desired = max_width > 0 ? max_width : option_width * static_cast<float>(options.size()) + 8;
    const float width = std::min(w_, desired), cell = (width - 8) / static_cast<float>(options.size());
    float height = kTarget;
    for (const auto& label_text : options) height = std::max(height, canvas_.measure(label_text, TextStyle::body_strong, std::max(8.0f, cell - 8)).height + 16);
    Widget frame; frame.kind = Kind::panel; frame.rect = D2D1::RectF(x_, y_, x_ + width, y_ + height);
    const float visual_inset = std::min(8.0f, (height - tokens::kSizeDesktopPickerVisualHeight) / 2);
    frame.rect.top += visual_inset; frame.rect.bottom -= visual_inset;
    frame.fill = "surfacePressed"; frame.radius = tokens::kRadiusSmall;
    add(std::move(frame));
    for (std::size_t i = 0; i < options.size(); ++i) {
        Widget w; w.kind = Kind::radio; w.compact = true;
        w.rect = D2D1::RectF(x_ + 4 + cell * static_cast<float>(i), y_, x_ + 4 + cell * static_cast<float>(i + 1), y_ + height);
        w.text = options[i]; w.name = label.empty() ? options[i] : label + L"：" + options[i];
        w.checked = static_cast<int>(i) == selected; w.radius = tokens::kRadiusSmall;
        w.id = group + "." + std::to_string(i); w.group = group; w.role = Role::radio;
        w.focusable = true; w.enabled = enabled;
        const int index = static_cast<int>(i), count = static_cast<int>(options.size());
        w.action = [select, index] { select(index); };
        w.step = [select, index, count](int delta) { select((index + delta + count) % count); };
        add(std::move(w));
    }
    y_ += height;
}

void Builder::status_header(std::string icon, std::string tone, std::wstring title, std::wstring detail,
                            std::wstring info_label, std::function<void()> info_action) {
    constexpr float diameter = 56;
    const float text_x = x_ + diameter + tokens::kSpaceL;
    const float text_width = std::max(40.0f, w_ - diameter - tokens::kSpaceL - (info_action ? kTarget + 4 : 0));
    const float title_h = canvas_.measure(title, TextStyle::hero, text_width).height;
    const float detail_width = std::max(40.0f, w_ - diameter - tokens::kSpaceL);
    const float detail_h = detail.empty() ? 0 : canvas_.measure(detail, TextStyle::callout, detail_width).height;
    const float height = std::max(diameter, title_h + (detail.empty() ? 0 : 4 + detail_h));
    Widget circle; circle.kind = Kind::panel; circle.fill = tone + "Surface"; circle.radius = diameter / 2;
    circle.rect = D2D1::RectF(x_, y_ + (height - diameter) / 2, x_ + diameter, y_ + (height + diameter) / 2); add(std::move(circle));
    Widget glyph; glyph.kind = Kind::icon; glyph.icon = std::move(icon); glyph.color = tone;
    glyph.rect = D2D1::RectF(x_ + 14, y_ + height / 2 - 14, x_ + 42, y_ + height / 2 + 14); add(std::move(glyph));
    const float top = y_ + (height - title_h - (detail.empty() ? 0 : detail_h + 4)) / 2;
    const std::size_t title_index = scene_.widgets.size();
    text_at(title, TextStyle::hero, "textPrimary", text_x, text_width, top);
    scene_.widgets[title_index].role = Role::heading; scene_.widgets[title_index].id = "status.headline";
    if (!detail.empty()) text_at(detail, TextStyle::callout, "textSecondary", text_x, detail_width, top + title_h + 4);
    if (info_action) {
        Widget info; info.kind = Kind::button; info.button = ButtonStyle::plain; info.icon = "info";
        const float info_x = std::min(x_ + w_ - kTarget, text_x + std::min(text_width, canvas_.measure(title, TextStyle::hero, 10000).width) - tokens::kSpaceXxs);
        const float info_y = top + (title_h - kTarget) / 2;
        info.rect = D2D1::RectF(info_x, info_y, info_x + kTarget, info_y + kTarget);
        info.id = "status.meta.info"; info.name = std::move(info_label); info.role = Role::button;
        info.focusable = true; info.action = std::move(info_action); add(std::move(info));
    }
    y_ += height;
}

void Builder::selector_row(std::string group, std::wstring leading, std::vector<std::wstring> options, int selected,
                           std::function<void(int)> select, std::wstring info_label, std::function<void()> info_action,
                           bool enabled, float selector_width) {
    // Keep the Mac-sized picker at ordinary text size; large Windows text must
    // widen it before moving the whole selector onto its own row.
    float option_width = 0;
    for (const auto& label : options) option_width = std::max(option_width, canvas_.measure(label, TextStyle::body_strong, 10000).width + 16);
    selector_width = std::max(selector_width, option_width * static_cast<float>(options.size()) + 8);
    const float picker_width = std::min(selector_width, w_ - kTarget - 8);
    const float cell_width = (picker_width - 8) / static_cast<float>(std::max<std::size_t>(1, options.size()));
    float picker_height = kTarget;
    for (const auto& label : options)
        picker_height = std::max(picker_height, canvas_.measure(label, TextStyle::body_strong, std::max(8.0f, cell_width - 8)).height + 16);
    const auto info = [group, info_label, info_action, picker_height](Builder& b) {
        b.gap((picker_height - kTarget) / 2);
        Widget w; w.kind = Kind::button; w.button = ButtonStyle::plain; w.icon = "info";
        w.rect = D2D1::RectF(b.x_, b.y_, b.x_ + kTarget, b.y_ + kTarget);
        w.id = group + ".row.info"; w.name = info_label; w.role = Role::button;
        w.focusable = true; w.action = info_action; b.add(std::move(w)); b.y_ += kTarget;
    };
    const auto selector = [group, options, selected, select, enabled, selector_width](Builder& b) {
        b.segmented(group, L"", options, selected, select, enabled, selector_width);
    };
    const float trailing = picker_width + kTarget + 8;
    const float leading_width = canvas_.measure(leading, TextStyle::caption, 10000).width;
    if (!leading.empty() && w_ - trailing - 12 < leading_width) {
        text(leading, TextStyle::caption, "textSecondary");
        gap(tokens::kSpaceS);
        row({selector, info}, {picker_width, kTarget}, 8);
    } else if (leading.empty()) row({selector, info}, {picker_width, kTarget}, 8);
    else row({[leading, picker_height](Builder& b) {
                 const float label_height = b.measure_text(leading, TextStyle::caption, b.width()).height;
                 b.gap(std::max(0.0f, (picker_height - label_height) / 2));
                 b.text(leading, TextStyle::caption, "textSecondary");
             }, selector, info},
             {0, selector_width, kTarget}, 8);
}

void Builder::field(std::string id, std::wstring label, std::wstring hint, bool secret, bool show_label) {
    if (show_label && !label.empty()) { text(label, TextStyle::body_strong, "textPrimary"); y_ += 6; }
    Widget w;
    w.kind = Kind::field;
    w.rect = D2D1::RectF(x_, y_, x_ + w_, y_ + kTarget);
    w.radius = 8;
    w.id = std::move(id);
    w.role = Role::field;
    w.name = label;
    w.secret = secret;
    w.focusable = true;
    add(std::move(w));
    y_ += kTarget;
    if (!hint.empty()) {
        y_ += 4;
        text(hint, TextStyle::caption, "textTertiary");
    }
}

void Builder::chip(std::wstring text, std::string tone, std::wstring name) {
    const float tw = canvas_.measure(text, TextStyle::caption, 10000).width;
    Widget w;
    w.kind = Kind::chip;
    w.rect = D2D1::RectF(x_, y_, x_ + tw + 20, y_ + 24);
    w.text = std::move(text);
    w.tone = std::move(tone);
    w.role = Role::text;
    w.name = name.empty() ? w.text : name;
    add(std::move(w));
    y_ += 24;
}

void Builder::chips(std::vector<ChipSpec> specs) {
    if (specs.empty()) return;
    const float gap = tokens::kSpaceS;
    float x = x_, top = y_, row_h = 0;
    for (auto& spec : specs) {
        const float leading = spec.icon.empty() ? 20.0f : 34.0f;
        const float trailing = spec.action ? 27.0f : 6.0f;
        const auto value = spec.label.empty() ? L"" : spec.text.substr(spec.label.size() + 1);
        const float natural = spec.label.empty() ? canvas_.measure(spec.text, TextStyle::caption, 10000).width + leading + trailing :
            canvas_.measure(spec.label, TextStyle::caption_strong, 10000).width + 6 + canvas_.measure(value, TextStyle::caption, 10000).width + leading + trailing;
        const float cw = std::min(w_, std::min(300.0f, natural));
        const float text_height = spec.label.empty()
            ? canvas_.measure(spec.text, TextStyle::caption, std::max(8.0f, cw - leading - trailing)).height + 12
            : std::max(canvas_.line_height(TextStyle::caption), canvas_.line_height(TextStyle::caption_strong)) + 8;
        const float height = std::max(spec.action ? kTarget : tokens::kSizeChipHeight, text_height);
        if (x > x_ && x + cw > x_ + w_ + 0.5f) { x = x_; top += row_h + gap; row_h = 0; }
        Widget w; w.kind = Kind::chip; w.rect = D2D1::RectF(x, top, x + cw, top + height);
        w.text = std::move(spec.text); w.tone = std::move(spec.tone); w.icon = std::move(spec.icon);
        if (!spec.label.empty()) { w.compact = true; w.text = std::move(spec.label); w.detail = value; }
        w.role = spec.action ? Role::button : Role::text; w.id = std::move(spec.id);
        w.focusable = !!spec.action; w.action = std::move(spec.action); w.radius = height / 2;
        w.name = spec.name.empty() ? w.text : spec.name; add(std::move(w));
        x += cw + gap; row_h = std::max(row_h, height);
    }
    y_ = top + row_h;
}

void Builder::title_row(std::string id, std::string icon, std::wstring title, TextStyle style, std::string text_color,
                        std::wstring info_label, std::function<void()> info_action, std::wstring badge, std::string badge_tone,
                        std::string badge_icon, bool compact) {
    const float icon_size = 18, gap = 8;
    const float title_x = x_ + (icon.empty() ? 0 : icon_size + gap);
    const float right = x_ + w_ - (!compact && info_action ? kTarget + 4 : 0);
    const float info_gap = compact ? -tokens::kSpaceXxs : tokens::kSpaceXs;
    const float title_available = right - title_x - (compact && info_action ? kTarget + info_gap : 0);
    const float badge_extra = badge_icon.empty() ? 20.0f : 42.0f;
    const float badge_natural = badge.empty() ? 0 : canvas_.measure(badge, TextStyle::caption, 10000).width + badge_extra;
    const float natural_title = canvas_.measure(title, style, 10000).width;
    const bool separate_badge = !badge.empty() && title_available < natural_title + badge_natural + (compact ? gap : 16);
    const float badge_width = std::min(std::max(40.0f, compact ? right - title_x : title_available), badge_natural);
    const float text_width = std::min(natural_title, std::max(40.0f, title_available - (!badge.empty() && !separate_badge ? badge_width + gap : 0)));
    const float title_h = canvas_.measure(title, style, text_width).height;
    const float badge_h = badge.empty() ? 0 : std::max(28.0f, canvas_.measure(badge, TextStyle::caption, std::max(8.0f, badge_width - badge_extra)).height + 10);
    // The 44-DIP information target can extend into the card's padding; it does
    // not need to inflate the title's visual line. Callers leave 8 DIPs before
    // the next interactive row, so those targets remain disjoint.
    const float row_h = std::max(compact ? tokens::kSizeWindowsSectionTitleVisualHeight : kTarget, std::max(title_h, separate_badge ? 0.0f : badge_h));
    if (!icon.empty()) {
        Widget i; i.kind = Kind::icon; i.icon = std::move(icon); i.color = "accent";
        i.rect = D2D1::RectF(x_, y_ + (row_h - icon_size) / 2, x_ + icon_size, y_ + (row_h + icon_size) / 2); add(std::move(i));
    }
    Widget t; t.kind = Kind::text; t.rect = D2D1::RectF(title_x, y_ + (row_h - title_h) / 2, title_x + text_width, y_ + (row_h + title_h) / 2);
    t.text = title; t.style = style; t.color = std::move(text_color); t.role = Role::heading; t.name = title; t.id = id; add(std::move(t));
    if (info_action) {
        Widget w; w.kind = Kind::button; w.button = ButtonStyle::plain; w.icon = "info"; w.radius = 10;
        const float info_x = id == "home.network" ? x_ + w_ - kTarget : std::min(x_ + w_ - kTarget, title_x + text_width + info_gap);
        w.rect = D2D1::RectF(info_x, y_ + (row_h - kTarget) / 2, info_x + kTarget, y_ + (row_h + kTarget) / 2);
        w.id = id.empty() ? "info" : id + ".info"; w.name = std::move(info_label); w.role = Role::button; w.focusable = true;
        w.action = std::move(info_action); add(std::move(w));
    }
    if (!badge.empty()) {
        const float badge_x = separate_badge ? title_x : right - badge_width;
        const float badge_y = separate_badge ? y_ + row_h + 4 : y_ + (row_h - badge_h) / 2;
        Widget w; w.kind = Kind::chip; w.rect = D2D1::RectF(badge_x, badge_y, badge_x + badge_width, badge_y + badge_h);
        w.text = std::move(badge); w.name = w.text; w.tone = std::move(badge_tone); w.icon = std::move(badge_icon); w.role = Role::text; add(std::move(w));
    }
    y_ += row_h + (separate_badge ? badge_h + 4 : 0);
}

void Builder::ribbon(std::vector<RibbonCell> cells) {
    if (cells.empty() || !std::isfinite(w_) || w_ <= 0) return;
    const float height = 28;
    const float n = static_cast<float>(cells.size());
    // Keep the normal spacing, reducing it only when a very narrow viewport
    // needs more room for the samples themselves. Every sample shares the row.
    const float gap = std::min(3.0f, w_ / (2 * n));
    const float cw = (w_ - gap * (n - 1)) / n;
    for (std::size_t index = 0; index < cells.size(); ++index) {
        auto& cell = cells[index];
        const float x = x_ + static_cast<float>(index) * (cw + gap);
        Widget w;
        w.kind = Kind::panel;
        w.rect = D2D1::RectF(x, y_, index + 1 == cells.size() ? x_ + w_ : x + cw, y_ + height);
        w.fill = cell.tone;  // tone names are palette roles
        w.border = cell.selected ? "selectionFill" : "";
        w.checked = cell.selected;
        w.radius = 4;
        w.id = std::move(cell.id);
        w.name = std::move(cell.name);
        w.role = Role::button;
        w.focusable = true;
        w.action = std::move(cell.action);
        add(std::move(w));
    }
    y_ += height;
}

void Builder::divider() {
    Widget w;
    w.kind = Kind::divider;
    w.rect = D2D1::RectF(x_, y_ + 8, x_ + w_, y_ + 9);
    add(std::move(w));
    y_ += 17;
}

void Builder::banner(std::string id, std::string tone, std::wstring title, std::wstring detail, std::wstring action_label,
                     std::function<void()> action) {
    card_begin(tone + "Surface", "", 16, 12);
    const float start = y_;
    const float text_x = x_ + 30;
    const float text_w = w_ - 30;
    Widget icon;
    icon.kind = Kind::icon;
    icon.icon = std::string(tone == "danger" ? "alert-triangle" : tone == "warning" ? "alert-triangle" : "info");
    icon.color = tone;
    icon.rect = D2D1::RectF(x_, y_ + 1, x_ + 20, y_ + 21);
    add(std::move(icon));
    y_ += text_at(title, TextStyle::body_strong, "textPrimary", text_x, text_w, y_);
    if (!detail.empty()) y_ += 2 + text_at(detail, TextStyle::callout, "textSecondary", text_x, text_w, y_ + 2);
    if (action && !action_label.empty()) {
        y_ += 10;
        const float save_x = x_, save_w = w_;
        x_ = text_x;
        w_ = text_w;
        button(std::move(id), std::move(action_label), ButtonStyle::secondary, std::move(action));
        x_ = save_x;
        w_ = save_w;
    }
    (void)start;
    card_end();
}

void Builder::tiles(std::vector<Tile> tiles) {
    const float gap = 12;
    const int columns = std::clamp(static_cast<int>((w_ + gap) / (tokens::kSizeTileMinWidth + gap)), 1, 3);
    const float tile_w = (w_ - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    std::size_t index = 0;
    while (index < tiles.size()) {
        const std::size_t row_end = std::min(tiles.size(), index + static_cast<std::size_t>(columns));
        float row_h = 0;
        std::vector<float> heights;
        for (std::size_t i = index; i < row_end; ++i) {
            const Tile& t = tiles[i];
            const float inner = tile_w - 32;
            float h = 16 + 22 + 8 + canvas_.measure(t.title, TextStyle::headline, inner).height;
            if (!t.chip.empty()) h += 8 + 24;
            if (!t.value.empty()) h += 6 + canvas_.measure(t.value, TextStyle::body, inner).height;
            if (!t.detail.empty()) h += 4 + canvas_.measure(t.detail, TextStyle::caption, inner).height;
            h += 16;
            heights.push_back(h);
            row_h = std::max(row_h, h);
        }
        for (std::size_t i = index; i < row_end; ++i) {
            Tile& t = tiles[i];
            const float tx = x_ + static_cast<float>(i - index) * (tile_w + gap);
            Widget card;
            card.kind = Kind::button;
            card.button = ButtonStyle::tile;
            card.rect = D2D1::RectF(tx, y_, tx + tile_w, y_ + row_h);
            card.radius = 14;
            card.id = t.id;
            card.role = Role::button;
            card.name = t.title + L"：" + (t.chip.empty() ? L"" : t.chip + L"，") + t.value + (t.detail.empty() ? L"" : L"，" + t.detail);
            card.focusable = true;
            card.action = t.action;
            add(std::move(card));
            float cy = y_ + 16;
            const float inner_x = tx + 16, inner = tile_w - 32;
            Widget icon;
            icon.kind = Kind::icon;
            icon.icon = t.icon;
            icon.color = "accent";
            icon.rect = D2D1::RectF(inner_x, cy, inner_x + 22, cy + 22);
            add(std::move(icon));
            Widget chevron;
            chevron.kind = Kind::icon;
            chevron.icon = "chevron-right";
            chevron.color = "textTertiary";
            chevron.rect = D2D1::RectF(tx + tile_w - 16 - 18, cy + 2, tx + tile_w - 16, cy + 20);
            add(std::move(chevron));
            cy += 22 + 8;
            cy += text_at(t.title, TextStyle::headline, "textPrimary", inner_x, inner, cy);
            if (!t.chip.empty()) {
                cy += 8;
                const float save_x = x_, save_y = y_;
                x_ = inner_x;
                y_ = cy;
                chip(t.chip, t.chip_tone);
                x_ = save_x;
                y_ = save_y;
                cy += 24;
            }
            if (!t.value.empty()) cy += 6 + text_at(t.value, TextStyle::body, "textPrimary", inner_x, inner, cy + 6);
            if (!t.detail.empty()) cy += 4 + text_at(t.detail, TextStyle::caption, "textSecondary", inner_x, inner, cy + 4);
        }
        y_ += row_h;
        index = row_end;
        if (index < tiles.size()) y_ += gap;
    }
}

void Builder::link(std::string id, std::wstring label, std::function<void()> action) {
    const float bw = std::min(w_, canvas_.measure(label, TextStyle::body, 10000).width + 26);
    const float height = std::max(kTarget, std::ceil(canvas_.measure(label, TextStyle::body, std::max(10.0f, bw - 26)).height) + 12.0f);
    Widget w;
    w.kind = Kind::button;
    w.button = ButtonStyle::link;
    w.rect = D2D1::RectF(x_, y_, x_ + bw, y_ + height);
    w.text = label;
    w.name = label;
    w.icon = "external-link";
    w.id = std::move(id);
    w.role = Role::link;
    w.focusable = true;
    w.action = std::move(action);
    add(std::move(w));
    y_ += height;
}

void Builder::card_begin(std::string fill, std::string border, float padding, float radius) {
    Widget w;
    w.kind = Kind::panel;
    w.rect = D2D1::RectF(x_, y_, x_ + w_, y_);
    w.fill = std::move(fill);
    w.border = std::move(border);
    w.radius = radius;
    add(std::move(w));
    cards_.push_back(Card{scene_.widgets.size() - 1, x_, w_, padding});
    x_ += padding;
    w_ -= 2 * padding;
    y_ += padding;
}

void Builder::card_end() {
    if (cards_.empty()) return;
    const Card card = cards_.back();
    cards_.pop_back();
    y_ += card.padding;
    scene_.widgets[card.index].rect.bottom = y_;
    x_ = card.x;
    w_ = card.w;
}

ResultChipLayout result_chip_layout(Canvas& canvas, const Widget& widget, const D2D1_RECT_F& rect) {
    const float visual_height = std::min(rect.bottom - rect.top, std::max(tokens::kSizeChipHeight,
        std::max(canvas.line_height(TextStyle::caption), canvas.line_height(TextStyle::caption_strong)) + 8));
    const float cy = (rect.top + rect.bottom) / 2;
    const auto visual = D2D1::RectF(rect.left, cy - visual_height / 2, rect.right, cy + visual_height / 2);
    const float left = std::min(visual.right, visual.left + (widget.icon.empty() ? 10.0f : 28.0f));
    const float right = std::max(left, visual.right - (widget.action ? 27.0f : 6.0f));
    const float available = right - left;
    // Preserve the result before allocating space to the arbitrary website name.
    const float detail_width = std::min(available, canvas.measure(widget.detail, TextStyle::caption, 10000).width);
    const float gap = widget.detail.empty() ? 0.0f : std::min(6.0f, std::max(0.0f, available - detail_width));
    const float label_width = std::min(std::max(0.0f, available - detail_width - gap),
        canvas.measure(widget.text, TextStyle::caption_strong, 10000).width);
    const float detail_left = left + label_width + gap;
    return {visual, D2D1::RectF(left, visual.top, left + label_width, visual.bottom),
        D2D1::RectF(detail_left, visual.top, right, visual.bottom)};
}

// ---------------- painting ----------------
namespace {

D2D1_COLOR_F with_alpha(D2D1_COLOR_F c, float a) {
    c.a *= a;
    return c;
}

bool info_button(const Widget& w) {
    return w.kind == Kind::button && w.button == ButtonStyle::plain && !w.compact && w.text.empty() && w.icon == "info";
}

D2D1_RECT_F centered_square(const D2D1_RECT_F& r, float size) {
    const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    return D2D1::RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2);
}

void draw(Canvas& c, const Palette& p, const Widget& w, const D2D1_RECT_F& r, const Interaction& s, float spin) {
    const bool hover = !w.id.empty() && s.hovered == w.id && w.enabled;
    const bool pressed = hover && s.pressed == w.id;
    const float alpha = w.enabled ? 1.0f : 0.45f;
    switch (w.kind) {
        case Kind::panel:
            if (!w.fill.empty()) c.fill_round(r, w.radius, p.role(w.fill));
            if (!w.border.empty()) c.stroke_round(r, w.radius, p.role(w.border), w.checked ? 2.0f : 1.0f);
            break;
        case Kind::text:
            c.text(w.text, w.style, r, p.role(w.color), w.center);
            break;
        case Kind::icon:
            if (w.icon == "appBrand") c.app_icon(r);
            else c.icon(w.icon, r, with_alpha(p.role(w.color), alpha));  // tone names are palette roles too
            break;
        case Kind::spinner:
            c.icon("loader", r, p.role(w.color), spin);
            break;
        case Kind::divider:
            c.line(r.left, r.top, r.right, r.top, p.border, 1.0f);
            break;
        case Kind::chip: {
            if (w.compact) {
                const auto layout = result_chip_layout(c, w, r);
                const float cy = (layout.visual.top + layout.visual.bottom) / 2;
                c.fill_round(layout.visual, (layout.visual.bottom - layout.visual.top) / 2, p.surface_muted);
                if (w.action && hover) c.stroke_round(layout.visual, (layout.visual.bottom - layout.visual.top) / 2, p.border_strong, 1.0f);
                if (!w.icon.empty()) c.icon(w.icon, D2D1::RectF(r.left + 9, cy - 7, r.left + 23, cy + 7), p.tone(w.tone));
                c.text_single_line(w.text, TextStyle::caption_strong, layout.label, p.text_primary);
                c.text_single_line(w.detail, TextStyle::caption, layout.detail, p.text_secondary);
                if (w.action) c.icon("info", D2D1::RectF(r.right - 22, cy - 7, r.right - 8, cy + 7), p.text_secondary);
                break;
            }
            const float h = r.bottom - r.top;
            c.fill_round(r, h / 2, p.tone_surface(w.tone));
            if (w.action && hover) c.stroke_round(r, h / 2, p.border_strong, 1.0f);
            const float text_right = r.right - (w.action ? 27.0f : 6.0f);
            if (!w.icon.empty()) {
                const float cy = (r.top + r.bottom) / 2;
                c.icon(w.icon, D2D1::RectF(r.left + 9, cy - 7, r.left + 23, cy + 7), p.tone(w.tone));
                const auto style = TextStyle::caption;
                const float name_w = std::min(text_right - r.left - 28, c.measure(w.text, style, 10000).width);
                const float th = c.measure(w.text, style, std::max(8.0f, name_w)).height;
                c.text(w.text, style, D2D1::RectF(r.left + 28, cy - th / 2, text_right, cy + th / 2), p.text_primary, false);
            } else {
                c.text(w.text, TextStyle::caption, D2D1::RectF(r.left, r.top + 4, r.right, r.bottom), p.tone(w.tone), true);
            }
            if (w.action) {
                const float cy = (r.top + r.bottom) / 2;
                c.icon("info", D2D1::RectF(r.right - 22, cy - 7, r.right - 8, cy + 7), p.text_secondary);
            }
            break;
        }
        case Kind::nav: {
            const auto style = w.checked ? TextStyle::body_strong : TextStyle::body;
            const float padding = tokens::kSizeDesktopNavigationHorizontalPadding;
            const float icon_size = w.icon.empty() ? 0.0f : tokens::kSizeDesktopNavigationIconSize;
            const float icon_gap = w.icon.empty() ? 0.0f : tokens::kSizeDesktopNavigationIconGap;
            const float label_width = std::max(1.0f, r.right - r.left - padding * 2 - icon_size - icon_gap);
            const auto label = c.measure(w.text, style, label_width);
            const float visual_height = std::min(r.bottom - r.top, std::max(tokens::kSizeDesktopNavigationVisualHeight, label.height + 8));
            const float inset = (r.bottom - r.top - visual_height) / 2;
            const auto visual = D2D1::RectF(r.left, r.top + inset, r.right, r.bottom - inset);
            if (w.checked) c.fill_round(visual, 8, p.surface);
            else if (hover) c.fill_round(visual, 8, p.surface_pressed);
            const float cy = (r.top + r.bottom) / 2;
            const auto fg = !w.enabled ? p.text_tertiary : w.checked ? p.accent : p.text_secondary;
            if (w.icon.empty()) {
                c.text(w.text, style, D2D1::RectF(r.left + padding, cy - label.height / 2, r.right - padding, cy + label.height / 2), fg, true);
            } else {
                // Centre the icon/label pair as one unit, with the same six-point gap as macOS.
                const float content_width = icon_size + icon_gap + std::min(label_width, label.width);
                const float icon_left = r.left + std::max(padding, (r.right - r.left - content_width) / 2);
                c.icon(w.icon, D2D1::RectF(icon_left, cy - icon_size / 2, icon_left + icon_size, cy + icon_size / 2), fg);
                const float text_left = icon_left + icon_size + icon_gap;
                c.text(w.text, style, D2D1::RectF(text_left, cy - label.height / 2,
                    std::min(r.right - padding, text_left + label_width), cy + label.height / 2), fg);
            }
            break;
        }
        case Kind::button: {
            if (w.compact) {
                const float cy = (r.top + r.bottom) / 2;
                const auto style = TextStyle::callout;
                const float height = c.measure(w.text, style, r.right - r.left - 30).height;
                const float visual_height = std::max(tokens::kSizeDesktopInputVisualHeight, height + 10);
                const auto box = D2D1::RectF(r.left, cy - visual_height / 2, r.right, cy + visual_height / 2);
                c.fill_round(box, w.radius, pressed ? p.surface_pressed : p.surface);
                c.stroke_round(box, w.radius, p.border, 1);
                c.text(w.text, style, D2D1::RectF(r.left + 8, cy - height / 2, r.right - 22, cy + height / 2), w.enabled ? p.text_primary : p.text_tertiary);
                c.icon("chevron-down", D2D1::RectF(r.right - 20, cy - 7, r.right - 6, cy + 7), p.text_secondary);
                break;
            }
            if (info_button(w)) {
                // Keep the full pointer/UIA target while matching the quiet macOS info glyph.
                const auto fg = !w.enabled ? p.text_tertiary : pressed ? p.accent_pressed : hover ? p.accent_hover : p.text_secondary;
                c.icon(w.icon, centered_square(r, tokens::kSizeIconSmall), fg);
                break;
            }
            D2D1_COLOR_F bg{0, 0, 0, 0}, fg = p.text_primary, border{0, 0, 0, 0};
            switch (w.button) {
                case ButtonStyle::primary:
                    bg = pressed ? p.accent_pressed : hover ? p.accent_hover : p.accent;
                    fg = p.on_accent;
                    break;
                case ButtonStyle::secondary:
                    bg = pressed ? p.surface_pressed : hover ? p.surface_muted : p.surface;
                    border = p.border_strong;
                    break;
                case ButtonStyle::plain:
                    bg = pressed ? p.surface_pressed : hover ? p.surface_muted : D2D1_COLOR_F{0, 0, 0, 0};
                    fg = w.text.empty() ? p.text_secondary : p.accent;
                    break;
                case ButtonStyle::link:
                    fg = hover ? p.accent_hover : p.accent;
                    break;
                case ButtonStyle::tile:
                    bg = pressed ? p.surface_pressed : hover ? p.surface_muted : p.surface;
                    border = hover ? p.border_strong : p.border;
                    break;
            }
            if (!w.enabled) {
                fg = p.text_tertiary;
                if (w.button == ButtonStyle::primary) bg = p.surface_pressed;
            }
            if (bg.a > 0) c.fill_round(r, w.radius, bg);
            if (border.a > 0) c.stroke_round(r, w.radius, border, 1.0f);
            if (w.button == ButtonStyle::tile) break;
            if (w.text.empty() && !w.icon.empty()) {  // icon-only button (ⓘ): centred glyph
                const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
                c.icon(w.icon, D2D1::RectF(cx - 9, cy - 9, cx + 9, cy + 9), w.enabled ? p.text_secondary : fg);
                break;
            }
            const TextStyle style = w.button == ButtonStyle::link ? TextStyle::body : TextStyle::body_strong;
            // Links are left-aligned without the padding used by ordinary buttons.
            // Builder::link reserves only the external-link icon's width.
            const float inner_w = r.right - r.left - (w.button == ButtonStyle::link ? 0.0f : 32.0f) - (w.icon.empty() ? 0 : 26);
            const D2D1_SIZE_F size = c.measure(w.text, style, std::max(10.0f, inner_w));
            const float content_w = size.width + (w.icon.empty() ? 0 : 26);
            float x = w.button == ButtonStyle::link ? r.left : r.left + std::max(16.0f, (r.right - r.left - content_w) / 2);
            const float ty = (r.top + r.bottom) / 2 - size.height / 2;
            if (!w.icon.empty() && w.button != ButtonStyle::link) {
                const float cy = (r.top + r.bottom) / 2;
                c.icon(w.icon, D2D1::RectF(x, cy - 9, x + 18, cy + 9), fg);
                x += 26;
            }
            c.text(w.text, style, D2D1::RectF(x, ty, x + size.width + 2, ty + size.height), fg, false, w.button == ButtonStyle::link);
            if (!w.icon.empty() && w.button == ButtonStyle::link) {
                const float cy = (r.top + r.bottom) / 2;
                c.icon(w.icon, D2D1::RectF(x + size.width + 6, cy - 8, x + size.width + 22, cy + 8), fg);
            }
            break;
        }
        case Kind::toggle: {
            if (w.compact) {
                const float cy = (r.top + r.bottom) / 2;
                const auto box = D2D1::RectF(r.left + 2, cy - 8, r.left + 18, cy + 8);
                c.fill_round(box, 4, w.checked ? p.selection_fill : p.surface);
                c.stroke_round(box, 4, w.checked ? p.selection_fill : p.border_strong, 1);
                if (w.checked) c.icon("check", D2D1::RectF(r.left + 3, cy - 7, r.left + 17, cy + 7), p.on_selection);
                const float height = c.measure(w.text, w.style, r.right - r.left - 28).height;
                c.text(w.text, w.style, D2D1::RectF(r.left + 28, cy - height / 2, r.right, cy + height / 2), with_alpha(p.text_primary, alpha));
                break;
            }
            if (hover) c.fill_round(r, w.radius, p.surface_muted);
            const float tw = 40, th = 22;
            const float cy = (r.top + r.bottom) / 2;
            const D2D1_RECT_F track = D2D1::RectF(r.right - tw - 8, cy - th / 2, r.right - 8, cy + th / 2);
            c.fill_round(track, th / 2, with_alpha(w.checked ? p.accent : p.border_strong, alpha));
            const float knob_x = w.checked ? track.right - th + 3 : track.left + 3;
            c.fill_round(D2D1::RectF(knob_x, track.top + 3, knob_x + th - 6, track.bottom - 3), (th - 6) / 2,
                         w.checked ? p.on_accent : p.surface);
            const float text_w = r.right - r.left - 64;
            const float lh = c.measure(w.text, w.style, text_w).height;
            const float dh = w.detail.empty() ? 0 : c.measure(w.detail, TextStyle::caption, text_w).height + 2;
            const float top = cy - (lh + dh) / 2;
            c.text(w.text, w.style, D2D1::RectF(r.left + 8, top, r.left + 8 + text_w, top + lh), with_alpha(p.text_primary, alpha));
            if (dh > 0) c.text(w.detail, TextStyle::caption, D2D1::RectF(r.left + 8, top + lh + 2, r.left + 8 + text_w, top + lh + dh), p.text_secondary);
            break;
        }
        case Kind::radio: {
            if (w.compact) {
                const float inset = std::min(8.0f, (r.bottom - r.top - tokens::kSizeDesktopPickerVisualHeight) / 2);
                const auto visual = D2D1::RectF(r.left, r.top + inset + 1, r.right, r.bottom - inset - 1);
                if (w.checked) {
                    c.fill_round(visual, w.radius, p.selection_fill);
                } else if (hover) {
                    c.fill_round(visual, w.radius, p.surface_muted);
                }
                const float th = c.measure(w.text, w.checked ? TextStyle::body_strong : TextStyle::body, std::max(8.0f, r.right - r.left - 8)).height;
                const float cy = (r.top + r.bottom) / 2;
                c.text(w.text, w.checked ? TextStyle::body_strong : TextStyle::body, D2D1::RectF(r.left + 4, cy - th / 2, r.right - 4, cy + th / 2),
                       w.checked ? p.on_selection : p.text_secondary, true);
                break;
            }
            c.fill_round(r, w.radius, w.checked ? p.accent_surface : hover ? p.surface_muted : p.surface);
            c.stroke_round(r, w.radius, w.checked ? p.accent : p.border, w.checked ? 2.0f : 1.0f);
            const float cx = r.left + 24, top = r.top + 12;
            const float title_h = c.line_height(TextStyle::body_strong);
            const float dot_cy = top + title_h / 2;
            c.stroke_round(D2D1::RectF(cx - 9, dot_cy - 9, cx + 9, dot_cy + 9), 9, w.checked ? p.accent : p.border_strong, 1.75f);
            if (w.checked) c.fill_round(D2D1::RectF(cx - 4.5f, dot_cy - 4.5f, cx + 4.5f, dot_cy + 4.5f), 4.5f, p.accent);
            const float text_x = r.left + 44, text_w = r.right - r.left - 56;
            const float h1 = c.measure(w.text, TextStyle::body_strong, text_w).height;
            c.text(w.text, TextStyle::body_strong, D2D1::RectF(text_x, top, text_x + text_w, top + h1), with_alpha(p.text_primary, alpha));
            if (!w.detail.empty())
                c.text(w.detail, TextStyle::caption, D2D1::RectF(text_x, top + h1 + 2, text_x + text_w, r.bottom), p.text_secondary);
            break;
        }
        case Kind::field: {
            const bool focused = s.focused == w.id;
            const float inset = std::max(0.0f, (r.bottom - r.top - std::max(tokens::kSizeDesktopInputVisualHeight, c.line_height(TextStyle::body) + 8)) / 2);
            const auto visual = D2D1::RectF(r.left, r.top + inset, r.right, r.bottom - inset);
            c.fill_round(visual, tokens::kRadiusSmall, p.surface);
            c.stroke_round(visual, tokens::kRadiusSmall, focused ? p.accent : p.border, focused ? 2.0f : 1.0f);
            break;
        }
    }
}

}  // namespace

void paint(Canvas& canvas, const Palette& palette, const Scene& scene, const Interaction& state, float width, float height, float spin) {
    canvas.clear(palette.background);
    for (int layer = 0; layer <= scene.top_layer(); ++layer) {
        const Layer& l = scene.layers[static_cast<std::size_t>(layer)];
        if (layer > 0) {
            canvas.set_scroll(0);
            canvas.fill_rect(D2D1::RectF(0, scene.chrome_height, width, height), D2D1::ColorF(0, 0, 0, palette.dark ? 0.55f : 0.32f));
        }
        for (int pass = 0; pass < 2; ++pass) {  // fixed chrome first, then the scrolled content clipped to its viewport
            const bool scrolled = pass == 1;
            if (scrolled) {
                canvas.set_scroll(0);
                canvas.push_clip(l.viewport);
                canvas.set_scroll(l.scroll);
            } else {
                canvas.set_scroll(0);
            }
            for (const Widget& w : scene.widgets) {
                if (w.layer != layer || w.scrolls != scrolled) continue;
                if (scrolled) {
                    // Clipping prevents pixels outside the viewport, but does not
                    // avoid DirectWrite layout work. Keep focus strokes and glyph
                    // overhang near the edge, and retain intersecting large panels.
                    const auto visible = inflate(scene.visible_rect(w), 4.0f);
                    if (visible.right < l.viewport.left || visible.left > l.viewport.right ||
                        visible.bottom < l.viewport.top || visible.top > l.viewport.bottom) continue;
                }
                draw(canvas, palette, w, w.rect, state, spin);
                if (!state.focused.empty() && w.id == state.focused && w.kind != Kind::field && layer == scene.top_layer()) {
                    if (info_button(w)) {
                        const float size = tokens::kSizeIconSmall + 2 * tokens::kSpaceXs;
                        canvas.stroke_round(centered_square(w.rect, size), size / 2, palette.focus_ring, tokens::kSizeFocusRingWidth);
                    } else {
                        canvas.stroke_round(inflate(w.rect, 3), w.radius + 3, palette.focus_ring, tokens::kSizeFocusRingWidth);
                    }
                }
            }
            if (scrolled) {
                canvas.set_scroll(0);
                canvas.pop_clip();
            }
        }
        // Thin scroll indicator when the layer content overflows.
        const float visible = l.viewport.bottom - l.viewport.top;
        if (l.content_height > visible + 1 && visible > 0) {
            const float ratio = visible / l.content_height;
            const float bar_h = std::min(visible, std::max(32.0f, visible * ratio));
            const float top = l.viewport.top + (visible - bar_h) * (l.scroll / (l.content_height - visible));
            canvas.fill_round(D2D1::RectF(l.viewport.right - 6, top, l.viewport.right - 2, top + bar_h), 2, palette.border_strong);
        }
    }
}

}  // namespace szu::app
