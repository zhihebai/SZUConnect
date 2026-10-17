// Retained widget list rebuilt from the view model on every change, plus a layout builder.
// Pages describe content; paint(), hit testing, focus order and UI Automation read the same list.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "canvas.hpp"
#include "theme.hpp"

namespace szu::app {

enum class Kind { panel, text, icon, button, toggle, radio, nav, chip, field, divider, spinner };
enum class Role { none, text, heading, button, link, toggle, radio, tab, field, status };
enum class ButtonStyle { primary, secondary, plain, link, tile };

struct Widget {
    Kind kind = Kind::panel;
    D2D1_RECT_F rect{};
    int layer = 0;
    bool scrolls = true;
    std::wstring text, detail;
    TextStyle style = TextStyle::body;
    std::string color = "textPrimary";
    std::string fill, border;
    float radius = 0;
    std::string icon;
    std::string tone;
    ButtonStyle button = ButtonStyle::secondary;
    bool center = false;
    bool compact = false;     // compact segmented option, menu or checkbox
    std::string id;
    Role role = Role::none;
    std::wstring name;        // accessible name
    bool focusable = false;
    bool enabled = true;
    bool checked = false;
    std::string group;
    std::function<void()> action;
    std::function<void(int)> step;  // arrow keys inside a radio group (-1 / +1)
    bool live = false;        // UIA live region (status headline)
    bool secret = false;      // password field
};

struct Layer {
    float scroll = 0;
    float content_height = 0;
    D2D1_RECT_F viewport{};   // visible area of scrolled widgets in this layer
    bool modal = false;
};

struct Interaction {
    std::string hovered, pressed, focused;
};

class Scene {
public:
    float chrome_height = 0;
    std::vector<Widget> widgets;
    std::vector<Layer> layers{Layer{}};
    int top_layer() const { return static_cast<int>(layers.size()) - 1; }
    const Widget* find(const std::string& id) const;  // highest layer, then last in paint order; includes disabled widgets
    D2D1_RECT_F visible_rect(const Widget& w) const;  // client DIPs, scroll applied
    const Widget* hit(float x, float y) const;        // topmost enabled interactive widget of the top layer
    std::vector<const Widget*> focus_order() const;   // focusable widgets of the top layer, in reading order
    void clamp_scroll(int layer);
};

struct ButtonSpec {
    std::string id;
    std::wstring label;
    ButtonStyle style = ButtonStyle::secondary;
    std::function<void()> action;
    std::string icon;
    bool enabled = true;
};

struct ChipSpec {
    std::wstring text;
    std::string tone;
    std::string icon;   // tone icon: state is never carried by colour alone
    std::wstring name;  // accessible name (defaults to text)
    std::string id;
    std::function<void()> action;
    std::wstring label;  // result chip: quiet surface, semibold endpoint/site name
};

struct ResultChipLayout {
    D2D1_RECT_F visual, label, detail;
};
// Painting and offline checks share the status-first allocation inside result chips.
ResultChipLayout result_chip_layout(Canvas& canvas, const Widget& widget, const D2D1_RECT_F& rect);

struct RibbonCell {
    std::string id;
    std::string tone;   // success | warning
    std::wstring name;
    bool selected = false;
    std::function<void()> action;
};

struct Choice {
    std::wstring title, detail;
    std::string icon;
};

struct Tile {
    std::string id;
    std::string icon;
    std::wstring title, value, detail;
    std::wstring chip;
    std::string chip_tone;
    std::function<void()> action;
};

class Builder {
public:
    Builder(Scene& scene, Canvas& canvas, int layer, float left, float width, float top, bool scrolls = true);
    float y() const { return y_; }
    float left() const { return x_; }
    float width() const { return w_; }
    void gap(float dy) { y_ += dy; }
    void set_y(float y) { y_ = y; }
    Widget& add(Widget w);

    float text(std::wstring_view text, TextStyle style, std::string color = "textSecondary", Role role = Role::text, std::string id = {});
    float text_at(std::wstring_view text, TextStyle style, std::string color, float x, float width, float y);
    void heading(std::wstring_view text, TextStyle style = TextStyle::title);
    void section(std::wstring_view title);
    void button(std::string id, std::wstring label, ButtonStyle style, std::function<void()> action, std::string icon = {},
                bool full = false, bool enabled = true);
    void icon_button(std::string id, std::wstring name, std::string icon, std::function<void()> action, bool enabled = true);
    void menu_button(std::string id, std::wstring label, std::wstring name, std::function<void()> action, bool enabled = true);
    void buttons(std::vector<ButtonSpec> specs, bool stretch = false);
    void toggle(std::string id, std::wstring label, std::wstring detail, bool checked, std::function<void()> action, bool enabled = true,
                bool checkbox = false, TextStyle label_style = TextStyle::body);
    void choices(std::string group, std::vector<Choice> options, int selected, std::function<void(int)> select, bool enabled = true);
    void segmented(std::string group, std::wstring label, std::vector<std::wstring> options, int selected, std::function<void(int)> select, bool enabled = true, float max_width = 0);
    // Row/columns share the same retained widgets, hit targets and keyboard order.
    // Fixed widths reserve trailing controls; zero-width cells share remaining space.
    void row(std::vector<std::function<void(Builder&)>> cells, std::vector<float> widths, float gap = 8, float min_fill_width = 160);
    void columns(std::vector<std::function<void(Builder&)>> cells, float min_column_width = 240, float gap = 12);
    void status_header(std::string icon, std::string tone, std::wstring title, std::wstring detail,
                       std::wstring info_label = {}, std::function<void()> info_action = {});
    void selector_row(std::string group, std::wstring leading, std::vector<std::wstring> options, int selected,
                      std::function<void(int)> select, std::wstring info_label, std::function<void()> info_action,
                      bool enabled = true, float selector_width = 190);
    void field(std::string id, std::wstring label, std::wstring hint, bool secret, bool show_label = true);
    void chip(std::wstring text, std::string tone, std::wstring name = {});
    // Flowing row of tone chips (wraps); each chip carries an icon.
    void chips(std::vector<ChipSpec> specs);
    // Title line with an optional ⓘ button at the right edge and an optional state badge before it.
    void title_row(std::string id, std::string icon, std::wstring title, TextStyle style, std::string text_color,
                   std::wstring info_label, std::function<void()> info_action, std::wstring badge = {},
                   std::string badge_tone = {}, std::string badge_icon = {}, bool compact = false);
    // One clickable cell per sample; the whole row shares the width.
    void ribbon(std::vector<RibbonCell> cells);
    void divider();
    void banner(std::string id, std::string tone, std::wstring title, std::wstring detail, std::wstring action_label,
                std::function<void()> action);
    void tiles(std::vector<Tile> tiles);
    void link(std::string id, std::wstring label, std::function<void()> action);
    void card_begin(std::string fill = "surface", std::string border = "border", float padding = 20, float radius = 14);
    void card_end();
    void icon_row(std::string icon, std::string color, std::wstring text, TextStyle style, std::string text_color, float icon_size = 20);
    float measure_button(const std::wstring& label, const std::string& icon);
    D2D1_SIZE_F measure_text(std::wstring_view text, TextStyle style, float width) { return canvas_.measure(text, style, width); }

private:
    Scene& scene_;
    Canvas& canvas_;
    int layer_;
    bool scrolls_;
    float x_, w_, y_;
    struct Card { std::size_t index; float x, w, padding; };
    std::vector<Card> cards_;
};

void paint(Canvas& canvas, const Palette& palette, const Scene& scene, const Interaction& state, float width, float height, float spin);
D2D1_RECT_F inflate(const D2D1_RECT_F& r, float d);

}  // namespace szu::app
