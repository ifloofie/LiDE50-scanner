// LiDE50 Scan - immediate-mode "liquid glass" widget toolkit on top of Gfx.
//
// Each frame the application lays out and draws its widgets; widgets read the
// current pointer state, keep small animation states keyed by id, and report
// clicks. Overlays (popovers, tooltips, toasts, alerts) are drawn last.
#pragma once

#include "gfx.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct Theme {
    Color base, base2;               // window background gradient
    Color blob[4];                   // ambient light colours behind the glass
    float blob_alpha = 0.6f;
    Color glass, glass_strong;       // panel tints
    Color sheen_top, sheen_bottom;   // top-lit gloss
    Color rim_top, rim_bottom;       // specular edge
    Color border;                    // outer hairline
    Color shadow;
    Color group;                     // grouped rows inside panels
    Color text, text2, text3;        // primary / secondary / disabled
    Color accent, accent2, on_accent;
    Color success, warning, danger;
    Color track, knob;
    Color canvas;                    // area behind images
    Color scanner_body, scanner_glass;
    Color dim;                       // modal backdrop
};

Theme theme_light();
Theme theme_dark();
Theme mix(const Theme& a, const Theme& b, float t);

enum class ButtonStyle { Glass, Primary, Plain, Danger };

class Ui {
public:
    explicit Ui(Gfx& g) : g(g) {}

    Gfx& g;
    Theme t;
    double time = 0;      // seconds
    float dt = 0.016f;
    bool reduce_motion = false;
    float toast_cx = -1;   // horizontal centre for toasts (defaults to the window centre)

    // ---- input (set by the window procedure) ----
    float mx = -1, my = -1;
    bool down = false, pressed = false, released = false, dbl = false;
    float wheel = 0;
    int key = 0;           // virtual key pressed this frame
    bool mouse_inside = false;

    // ---- frame ----
    void begin_frame(double now);
    void end_frame();
    bool wants_frame() const { return want_frame_; }
    bool wants_slow_frame() const { return want_slow_frame_; }
    void request_frame() { want_frame_ = true; }
    LPCWSTR cursor() const { return cursor_; }
    void set_cursor(LPCWSTR c) { cursor_ = c; }
    bool modal_open() const { return alert_.open || alert_anim_ > 0.01f; }

    // ---- animation ----
    float anim(const std::string& key, float target, float speed = 14.0f);
    float spring(const std::string& key, float target, float stiffness = 320.0f, float damping = 24.0f);
    void set_anim(const std::string& key, float v);
    void set_spring(const std::string& key, float v);
    bool has_spring(const std::string& key) const { return springs_.count(key) != 0; }

    // ---- hit testing ----
    bool hover(const std::string& id, const RectF& r);
    bool is_active(const std::string& id) const { return active_ == id; }
    // starts a drag on press (returns true on the press frame)
    bool press(const std::string& id, const RectF& r);
    bool dragging(const std::string& id) const { return active_ == id && down; }
    void push_clip(const RectF& r);
    void pop_clip();

    // ---- drawing helpers ----
    void background(const RectF& r, bool motion);
    void glass(const RectF& r, float radius, float elevation = 1.0f, bool strong = false, float pointer_sheen = 0.0f);
    // frosted glass for things that float above content (menus, alerts, tooltips)
    void glass_overlay(const RectF& r, float radius, float elevation = 1.4f);
    void group(const RectF& r, float radius = 12.0f);

    // ---- widgets ----
    bool button(const std::string& id, const RectF& r, const std::wstring& label, Icon icon, bool has_icon,
                ButtonStyle style = ButtonStyle::Glass, bool enabled = true, const std::wstring& tip = L"");
    bool icon_button(const std::string& id, const RectF& r, Icon icon, const std::wstring& tip, bool enabled = true,
                     bool glassy = true);
    bool segmented(const std::string& id, const RectF& r, const std::vector<std::wstring>& items, int& sel,
                   bool enabled = true);
    // returns true while the value changes; `bipolar` fills from zero
    bool slider(const std::string& id, const RectF& r, const std::wstring& label, int& value, int mn, int mx,
                int def, const std::wstring& value_text, bool enabled = true, bool bipolar = false);
    bool toggle(const std::string& id, const RectF& r, const std::wstring& label, bool& value, bool enabled = true);
    void dropdown(const std::string& id, const RectF& r, const std::vector<std::wstring>& items, int sel,
                  std::function<void(int)> on_pick, bool enabled = true);
    void label(const std::wstring& s, const RectF& r, float size = 13.0f, int weight = 400, int tone = 0,
               Align h = Align::Start, Align v = Align::Center, bool wrap = false);

    // ---- overlays ----
    struct MenuItem { std::wstring text; Icon icon; bool has_icon; bool enabled; std::function<void()> action; bool separator = false; };
    void menu(const std::string& id, const RectF& anchor, std::vector<MenuItem> items);
    void toast(const std::wstring& text, int kind = 0); // 0 info, 1 success, 2 warning
    void alert(const std::wstring& title, const std::wstring& message, std::vector<std::wstring> buttons,
               std::function<void(int)> on_close, int default_button = 0, int cancel_button = -1);
    bool popover_open() const { return pop_.open; }
    void close_popover() { pop_.open = false; }

    // actions run after the frame (safe place for dialogs / heavy work)
    void defer(std::function<void()> fn) { deferred_.push_back(std::move(fn)); }
    std::vector<std::function<void()>> take_deferred();

private:
    void draw_popover();
    void draw_toasts();
    void draw_alert();
    void draw_tooltip();
    void tip(const std::string& id, const std::wstring& text, const RectF& r);
    bool blocked(float x, float y) const;

    struct Spring { float x = 0, v = 0; bool init = false; };
    std::unordered_map<std::string, float> anims_;
    std::unordered_map<std::string, Spring> springs_;
    std::string hot_, active_, last_hot_;
    double hot_since_ = 0;
    std::wstring tip_text_;
    RectF tip_rect_;
    std::string tip_id_;
    bool want_frame_ = false;
    bool want_slow_frame_ = false;   // only the ambient background is moving
    LPCWSTR cursor_ = IDC_ARROW;
    std::vector<RectF> clip_stack_;
    std::vector<RectF> blockers_;      // overlay areas from the previous frame
    std::vector<std::function<void()>> deferred_;
    double last_time_ = 0;

    struct Popover {
        bool open = false;
        std::string id;
        RectF anchor, rect;
        std::vector<MenuItem> items;
        int selected = -1;
        bool is_menu = false;
        std::function<void(int)> on_pick;
        double opened_at = 0;
    } pop_;
    float pop_anim_ = 0;

    struct Toast { std::wstring text; int kind; double born; };
    std::vector<Toast> toasts_;

    struct Alert {
        bool open = false;
        std::wstring title, message;
        std::vector<std::wstring> buttons;
        std::function<void(int)> on_close;
        int def = 0, cancel = -1;
    } alert_, alert_draw_;
    float alert_anim_ = 0;
};
