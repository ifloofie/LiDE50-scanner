// LiDE50 Scan - immediate-mode "liquid glass" widget toolkit.
#include "ui.h"

#include <algorithm>
#include <cmath>

// ----------------------------------------------------------------- themes

Theme theme_light()
{
    Theme t;
    t.base = Color::hex(0xE6EAF2);
    t.base2 = Color::hex(0xF5F6FA);
    t.blob[0] = Color::hex(0x6FB1FF);
    t.blob[1] = Color::hex(0xFF8FC0);
    t.blob[2] = Color::hex(0x7FE0C2);
    t.blob[3] = Color::hex(0xB49CFF);
    t.blob_alpha = 0.55f;
    t.glass = Color::hex(0xFFFFFF, 0.50f);
    t.glass_strong = Color::hex(0xFFFFFF, 0.80f);
    t.sheen_top = Color::hex(0xFFFFFF, 0.50f);
    t.sheen_bottom = Color::hex(0xFFFFFF, 0.0f);
    t.rim_top = Color::hex(0xFFFFFF, 0.95f);
    t.rim_bottom = Color::hex(0xFFFFFF, 0.30f);
    t.border = Color::hex(0x101828, 0.09f);
    t.shadow = Color::hex(0x1E2846, 0.20f);
    t.group = Color::hex(0xFFFFFF, 0.42f);
    t.text = Color::hex(0x1C1C1E);
    t.text2 = Color::hex(0x5E6371);
    t.text3 = Color::hex(0xA4A8B2);
    t.accent = Color::hex(0x0A84FF);
    t.accent2 = Color::hex(0x5E5CE6);
    t.on_accent = Color::hex(0xFFFFFF);
    t.success = Color::hex(0x30C25A);
    t.warning = Color::hex(0xF59E0B);
    t.danger = Color::hex(0xFF3B30);
    t.track = Color::hex(0x787880, 0.20f);
    t.knob = Color::hex(0xFFFFFF);
    t.canvas = Color::hex(0xFFFFFF, 0.0f);
    t.scanner_body = Color::hex(0x2B2E35);
    t.scanner_glass = Color::hex(0xDCE2EA);
    t.dim = Color::hex(0x0F1420, 0.22f);
    return t;
}

Theme theme_dark()
{
    Theme t;
    t.base = Color::hex(0x0B0D12);
    t.base2 = Color::hex(0x151821);
    t.blob[0] = Color::hex(0x2A4BD7);
    t.blob[1] = Color::hex(0x8B2BE2);
    t.blob[2] = Color::hex(0x0E9F9A);
    t.blob[3] = Color::hex(0xC2185B);
    t.blob_alpha = 0.42f;
    t.glass = Color::hex(0x282C36, 0.50f);
    t.glass_strong = Color::hex(0x464A56, 0.74f);
    t.sheen_top = Color::hex(0xFFFFFF, 0.12f);
    t.sheen_bottom = Color::hex(0xFFFFFF, 0.0f);
    t.rim_top = Color::hex(0xFFFFFF, 0.34f);
    t.rim_bottom = Color::hex(0xFFFFFF, 0.06f);
    t.border = Color::hex(0x000000, 0.45f);
    t.shadow = Color::hex(0x000000, 0.50f);
    t.group = Color::hex(0xFFFFFF, 0.055f);
    t.text = Color::hex(0xF5F5F7);
    t.text2 = Color::hex(0xA3A3AD);
    t.text3 = Color::hex(0x5D6069);
    t.accent = Color::hex(0x0A84FF);
    t.accent2 = Color::hex(0x7D7AFF);
    t.on_accent = Color::hex(0xFFFFFF);
    t.success = Color::hex(0x32D74B);
    t.warning = Color::hex(0xFFB340);
    t.danger = Color::hex(0xFF453A);
    t.track = Color::hex(0xFFFFFF, 0.15f);
    t.knob = Color::hex(0xF2F2F7);
    t.canvas = Color::hex(0x000000, 0.0f);
    t.scanner_body = Color::hex(0x17191E);
    t.scanner_glass = Color::hex(0x2A2F38);
    t.dim = Color::hex(0x000000, 0.45f);
    return t;
}

Theme mix(const Theme& a, const Theme& b, float f)
{
    Theme t;
    t.base = lerp(a.base, b.base, f);
    t.base2 = lerp(a.base2, b.base2, f);
    for (int i = 0; i < 4; ++i) t.blob[i] = lerp(a.blob[i], b.blob[i], f);
    t.blob_alpha = lerpf(a.blob_alpha, b.blob_alpha, f);
#define MIXC(n) t.n = lerp(a.n, b.n, f)
    MIXC(glass); MIXC(glass_strong); MIXC(sheen_top); MIXC(sheen_bottom); MIXC(rim_top); MIXC(rim_bottom);
    MIXC(border); MIXC(shadow); MIXC(group); MIXC(text); MIXC(text2); MIXC(text3); MIXC(accent);
    MIXC(accent2); MIXC(on_accent); MIXC(success); MIXC(warning); MIXC(danger); MIXC(track); MIXC(knob);
    MIXC(canvas); MIXC(scanner_body); MIXC(scanner_glass); MIXC(dim);
#undef MIXC
    return t;
}

// ------------------------------------------------------------------ frame

void Ui::begin_frame(double now)
{
    dt = last_time_ > 0 ? static_cast<float>(std::clamp(now - last_time_, 0.0, 0.1)) : 0.016f;
    last_time_ = now;
    time = now;
    want_frame_ = false;
    want_slow_frame_ = false;
    cursor_ = IDC_ARROW;
    hot_.clear();
    tip_id_.clear();
    clip_stack_.clear();
    blockers_.clear();
    if (alert_.open || pop_.open) blockers_.push_back({-1e6f, -1e6f, 2e6f, 2e6f});
}

bool Ui::blocked(float x, float y) const
{
    for (const auto& b : blockers_)
        if (b.contains(x, y)) return true;
    return false;
}

void Ui::push_clip(const RectF& r)
{
    RectF c = r;
    if (!clip_stack_.empty()) {
        const RectF& p = clip_stack_.back();
        float x0 = std::max(c.x, p.x), y0 = std::max(c.y, p.y);
        float x1 = std::min(c.r(), p.r()), y1 = std::min(c.b(), p.b());
        c = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    clip_stack_.push_back(c);
    g.push_clip(r);
}

void Ui::pop_clip()
{
    if (!clip_stack_.empty()) clip_stack_.pop_back();
    g.pop_clip();
}

bool Ui::hover(const std::string& id, const RectF& r)
{
    if (!mouse_inside || !r.contains(mx, my)) return false;
    if (!clip_stack_.empty() && !clip_stack_.back().contains(mx, my)) return false;
    if (blocked(mx, my)) return false;
    if (!active_.empty() && active_ != id) return false;
    hot_ = id;
    return true;
}

bool Ui::press(const std::string& id, const RectF& r)
{
    bool h = hover(id, r);
    if (h && pressed) {
        active_ = id;
        return true;
    }
    return false;
}

float Ui::anim(const std::string& key, float target, float speed)
{
    auto it = anims_.find(key);
    if (it == anims_.end()) {
        anims_[key] = target;
        return target;
    }
    float& v = it->second;
    if (reduce_motion) speed *= 3.0f;
    float k = 1.0f - std::exp(-dt * speed);
    v += (target - v) * k;
    if (std::fabs(target - v) > 0.002f) want_frame_ = true;
    else v = target;
    return v;
}

void Ui::set_anim(const std::string& key, float v) { anims_[key] = v; }

void Ui::set_spring(const std::string& key, float v)
{
    Spring& s = springs_[key];
    s.x = v;
    s.v = 0;
    s.init = true;
    want_frame_ = true;
}

float Ui::spring(const std::string& key, float target, float stiffness, float damping)
{
    Spring& s = springs_[key];
    if (!s.init || reduce_motion) {
        s.x = target;
        s.v = 0;
        s.init = true;
        return target;
    }
    float remaining = dt;
    while (remaining > 0) {
        float h = std::min(remaining, 1.0f / 240.0f);
        float a = stiffness * (target - s.x) - damping * s.v;
        s.v += a * h;
        s.x += s.v * h;
        remaining -= h;
    }
    if (std::fabs(target - s.x) > 0.01f || std::fabs(s.v) > 0.01f) want_frame_ = true;
    else { s.x = target; s.v = 0; }
    return s.x;
}

std::vector<std::function<void()>> Ui::take_deferred()
{
    std::vector<std::function<void()>> out;
    out.swap(deferred_);
    return out;
}

// --------------------------------------------------------------- drawing

void Ui::background(const RectF& r, bool motion)
{
    g.fill_rrect_vgrad(r, 0, t.base2, t.base);
    const float tt = motion && !reduce_motion ? static_cast<float>(time) : 0.0f;
    struct B { float x, y, rx, ry, sp, ph; };
    const B blobs[4] = {
        {0.18f, 0.20f, 0.55f, 0.50f, 0.050f, 0.0f},
        {0.85f, 0.18f, 0.45f, 0.45f, 0.043f, 1.7f},
        {0.30f, 0.92f, 0.55f, 0.40f, 0.037f, 3.1f},
        {0.78f, 0.80f, 0.50f, 0.50f, 0.047f, 4.4f},
    };
    const float span = std::max(r.w, r.h);
    for (int i = 0; i < 4; ++i) {
        const B& b = blobs[i];
        float cx = r.x + r.w * (b.x + 0.07f * std::sin(tt * b.sp * 6.28f + b.ph));
        float cy = r.y + r.h * (b.y + 0.06f * std::cos(tt * b.sp * 5.1f + b.ph * 1.3f));
        g.fill_radial(cx, cy, span * b.rx, span * b.ry, t.blob[i].with_a(t.blob_alpha), t.blob[i].with_a(0.0f));
    }
    if (motion && !reduce_motion) want_slow_frame_ = true;
}

void Ui::glass(const RectF& r, float radius, float elevation, bool strong, float pointer_sheen)
{
    if (elevation > 0) g.shadow(r, radius, 16.0f * elevation, t.shadow, 7.0f * elevation);
    g.fill_rrect(r, radius, strong ? t.glass_strong : t.glass);
    // top-lit gloss across the upper part of the panel
    RectF sheen = r;
    sheen.h = std::min(r.h, std::max(40.0f, r.h * 0.55f));
    g.fill_rrect_vgrad(sheen, radius, t.sheen_top, t.sheen_bottom);
    if (pointer_sheen > 0 && r.contains(mx, my) && mouse_inside) {
        g.fill_rrect_radial(r, radius, mx, my, std::max(60.0f, std::min(r.w, r.h) * 0.9f),
                            t.rim_top.alpha(0.18f * pointer_sheen), t.rim_top.with_a(0.0f));
    }
    g.stroke_rrect(r.inset(-0.5f), radius + 0.5f, t.border, 1.0f);
    g.stroke_rrect_vgrad(r.inset(0.75f), std::max(0.0f, radius - 0.75f), t.rim_top, t.rim_bottom, 1.2f);
}

void Ui::glass_overlay(const RectF& r, float radius, float elevation)
{
    // without a real backdrop blur, a mostly opaque frosted base keeps text legible
    g.shadow(r, radius, 18.0f * elevation, t.shadow, 8.0f * elevation);
    g.fill_rrect(r, radius, t.base2.with_a(0.90f));
    g.fill_rrect(r, radius, t.glass_strong.alpha(0.55f));
    RectF sheen = r;
    sheen.h = std::min(r.h, std::max(40.0f, r.h * 0.45f));
    g.fill_rrect_vgrad(sheen, radius, t.sheen_top.alpha(0.8f), t.sheen_bottom);
    g.stroke_rrect(r.inset(-0.5f), radius + 0.5f, t.border, 1.0f);
    g.stroke_rrect_vgrad(r.inset(0.75f), std::max(0.0f, radius - 0.75f), t.rim_top, t.rim_bottom, 1.2f);
}

void Ui::group(const RectF& r, float radius)
{
    g.fill_rrect(r, radius, t.group);
    g.stroke_rrect(r.inset(0.5f), radius, t.rim_bottom.alpha(0.8f), 1.0f);
}

void Ui::label(const std::wstring& s, const RectF& r, float size, int weight, int tone, Align h, Align v, bool wrap)
{
    const Color& c = tone == 0 ? t.text : tone == 1 ? t.text2 : tone == 2 ? t.text3 : t.accent;
    g.text(s, r, size, weight, c, h, v, wrap);
}

void Ui::tip(const std::string& id, const std::wstring& text, const RectF& r)
{
    if (text.empty() || hot_ != id) return;
    tip_id_ = id;
    tip_text_ = text;
    tip_rect_ = r;
}

// ----------------------------------------------------------------- widgets

bool Ui::button(const std::string& id, const RectF& r, const std::wstring& label, Icon icon, bool has_icon,
                ButtonStyle style, bool enabled, const std::wstring& tiptext)
{
    bool h = enabled && hover(id, r);
    if (h && pressed) active_ = id;
    bool held = active_ == id && down && h;
    bool clicked = enabled && active_ == id && released && h;
    if (h) cursor_ = IDC_HAND;
    tip(id, tiptext, r);

    float hv = anim(id + ".h", h ? 1.0f : 0.0f, 16.0f);
    float pr = spring(id + ".p", held ? 1.0f : 0.0f, 500.0f, 28.0f);
    float en = anim(id + ".e", enabled ? 1.0f : 0.0f, 12.0f);
    RectF rr = r.scaled_about_center(1.0f - 0.04f * pr + 0.012f * hv);
    float radius = rr.h * 0.5f;

    g.push_opacity(0.45f + 0.55f * en);
    Color fg = t.text;
    if (style == ButtonStyle::Primary) {
        g.shadow(rr, radius, 14.0f, t.accent.alpha(0.30f + 0.15f * hv), 5.0f);
        g.fill_rrect_hgrad(rr, radius, t.accent, t.accent2);
        g.fill_rrect_vgrad(RectF{rr.x, rr.y, rr.w, rr.h * 0.6f}, radius, Color::hex(0xFFFFFF, 0.30f + 0.1f * hv),
                           Color::hex(0xFFFFFF, 0.0f));
        if (hv > 0.01f)
            g.fill_rrect_radial(rr, radius, mx, my, rr.w * 0.6f, Color::hex(0xFFFFFF, 0.22f * hv),
                                Color::hex(0xFFFFFF, 0.0f));
        g.stroke_rrect_vgrad(rr.inset(0.75f), radius, Color::hex(0xFFFFFF, 0.65f), Color::hex(0xFFFFFF, 0.10f), 1.2f);
        fg = t.on_accent;
    } else if (style == ButtonStyle::Plain) {
        if (hv > 0.01f) g.fill_rrect(rr, radius, t.track.alpha(hv));
        fg = t.accent;
    } else {
        glass(rr, radius, 0.35f + 0.25f * hv, true, hv);
        if (style == ButtonStyle::Danger) fg = t.danger;
    }
    if (pr > 0.01f) g.fill_rrect(rr, radius, Color::hex(0x000000, 0.06f * pr));

    float tw = label.empty() ? 0.0f : g.text_width(label, 13.0f, 600);
    float iw = has_icon ? 18.0f : 0.0f;
    float gap = (has_icon && !label.empty()) ? 7.0f : 0.0f;
    float total = tw + iw + gap;
    float x0 = rr.cx() - total * 0.5f;
    if (has_icon) g.icon(icon, RectF{x0, rr.cy() - 9.0f, 18.0f, 18.0f}, fg, 1.7f);
    if (!label.empty())
        g.text(label, RectF{x0 + iw + gap, rr.y, tw + 4.0f, rr.h}, 13.0f, 600, fg, Align::Start, Align::Center);
    g.pop_opacity();
    return clicked;
}

bool Ui::icon_button(const std::string& id, const RectF& r, Icon icon, const std::wstring& tiptext, bool enabled,
                     bool glassy)
{
    bool h = enabled && hover(id, r);
    if (h && pressed) active_ = id;
    bool held = active_ == id && down && h;
    bool clicked = enabled && active_ == id && released && h;
    if (h) cursor_ = IDC_HAND;
    tip(id, tiptext, r);
    float hv = anim(id + ".h", h ? 1.0f : 0.0f, 16.0f);
    float pr = spring(id + ".p", held ? 1.0f : 0.0f, 500.0f, 28.0f);
    float en = anim(id + ".e", enabled ? 1.0f : 0.0f, 12.0f);
    RectF rr = r.scaled_about_center(1.0f - 0.08f * pr + 0.03f * hv);
    float radius = rr.h * 0.5f;
    g.push_opacity(0.38f + 0.62f * en);
    if (glassy) glass(rr, radius, 0.3f + 0.3f * hv, true, hv);
    else if (hv > 0.01f) g.fill_rrect(rr, radius, t.track.alpha(hv));
    float is = std::min(rr.w, rr.h) * 0.56f;
    g.icon(icon, RectF{rr.cx() - is * 0.5f, rr.cy() - is * 0.5f, is, is}, t.text, 1.7f);
    g.pop_opacity();
    return clicked;
}

bool Ui::segmented(const std::string& id, const RectF& r, const std::vector<std::wstring>& items, int& sel,
                   bool enabled)
{
    const int n = static_cast<int>(items.size());
    if (n == 0) return false;
    float radius = r.h * 0.5f;
    g.fill_rrect(r, radius, t.track);
    float seg_w = (r.w - 4.0f) / n;
    float pos = spring(id + ".pos", static_cast<float>(std::clamp(sel, 0, n - 1)), 380.0f, 30.0f);
    float stretch = std::fabs(pos - std::round(pos)) * 0.18f; // liquid stretch while moving
    RectF pill{r.x + 2.0f + pos * seg_w - stretch * seg_w * 0.5f, r.y + 2.0f, seg_w * (1.0f + stretch), r.h - 4.0f};
    g.shadow(pill, pill.h * 0.5f, 6.0f, t.shadow.alpha(0.7f), 2.0f);
    g.fill_rrect(pill, pill.h * 0.5f, t.glass_strong);
    g.fill_rrect_vgrad(pill, pill.h * 0.5f, t.sheen_top, t.sheen_bottom);
    g.stroke_rrect_vgrad(pill.inset(0.6f), pill.h * 0.5f, t.rim_top, t.rim_bottom, 1.0f);
    bool changed = false;
    for (int i = 0; i < n; ++i) {
        RectF cell{r.x + 2.0f + i * seg_w, r.y, seg_w, r.h};
        std::string cid = id + "." + std::to_string(i);
        bool h = enabled && hover(cid, cell);
        if (h && pressed) active_ = cid;
        if (h && i != sel) cursor_ = IDC_HAND;
        if (enabled && active_ == cid && released && h && sel != i) {
            sel = i;
            changed = true;
        }
        float hv = anim(cid + ".h", (h && i != sel) ? 1.0f : 0.0f, 16.0f);
        float on = anim(cid + ".on", i == sel ? 1.0f : 0.0f, 14.0f);
        Color c = lerp(lerp(t.text2, t.text, hv), t.text, on);
        if (!enabled) c = t.text3;
        g.text(items[i], cell, 12.5f, i == sel ? 600 : 500, c, Align::Center, Align::Center);
    }
    return changed;
}

bool Ui::slider(const std::string& id, const RectF& r, const std::wstring& label, int& value, int mn, int mx_,
                int def, const std::wstring& value_text, bool enabled, bool bipolar)
{
    // layout: label row (18) + track row (22)
    Color lc = enabled ? t.text : t.text3;
    g.text(label, RectF{r.x, r.y, r.w * 0.7f, 18.0f}, 13.0f, 400, lc, Align::Start, Align::Center);
    g.text(value_text, RectF{r.x + r.w * 0.5f, r.y, r.w * 0.5f, 18.0f}, 12.5f, 500,
           enabled ? t.text2 : t.text3, Align::End, Align::Center);
    RectF track{r.x + 2.0f, r.y + 29.0f, r.w - 4.0f, 4.0f};
    RectF hit{r.x - 4.0f, r.y + 18.0f, r.w + 8.0f, 26.0f};
    bool h = enabled && hover(id, hit);
    bool changed = false;
    if (h && pressed) {
        active_ = id;
        if (dbl) {  // double-click resets to the default
            if (value != def) { value = def; changed = true; }
        }
    }
    bool dragging = enabled && active_ == id && down;
    if (h || dragging) cursor_ = IDC_HAND;
    if (dragging && !dbl) {
        float f = std::clamp((mx - track.x) / track.w, 0.0f, 1.0f);
        int nv = static_cast<int>(std::lround(mn + f * (mx_ - mn)));
        if (nv != value) { value = nv; changed = true; }
    }
    float f = static_cast<float>(value - mn) / std::max(1, mx_ - mn);
    float fa = anim(id + ".f", f, dragging ? 40.0f : 18.0f);
    float hv = anim(id + ".h", (h || dragging) ? 1.0f : 0.0f, 14.0f);
    float dr = spring(id + ".d", dragging ? 1.0f : 0.0f, 420.0f, 26.0f);

    g.fill_rrect(track, 2.0f, t.track);
    float zero = bipolar ? std::clamp(static_cast<float>(0 - mn) / std::max(1, mx_ - mn), 0.0f, 1.0f) : 0.0f;
    float a = std::min(zero, fa), b = std::max(zero, fa);
    if (enabled && b - a > 0.001f)
        g.fill_rrect_hgrad(RectF{track.x + track.w * a, track.y, track.w * (b - a), track.h}, 2.0f, t.accent,
                           t.accent2);
    if (bipolar) g.fill_ellipse(track.x + track.w * zero, track.cy(), 1.6f, 1.6f, t.text3);

    // glass knob that swells like a lens while dragged
    float kw = 26.0f + 10.0f * dr + 2.0f * hv, kh = 18.0f + 6.0f * dr;
    RectF knob{track.x + track.w * fa - kw * 0.5f, track.cy() - kh * 0.5f, kw, kh};
    g.push_opacity(enabled ? 1.0f : 0.5f);
    g.shadow(knob, kh * 0.5f, 6.0f + 4.0f * dr, t.shadow.alpha(0.9f), 2.5f);
    g.fill_rrect(knob, kh * 0.5f, t.knob.alpha(1.0f - 0.35f * dr));
    if (dr > 0.01f) {
        // refraction: show the track through the lens
        g.fill_rrect(RectF{knob.x + 4, track.y - 0.5f, knob.w - 8, track.h + 1}, 2.0f, t.accent.alpha(0.55f * dr));
    }
    g.fill_rrect_vgrad(knob, kh * 0.5f, Color::hex(0xFFFFFF, 0.55f), Color::hex(0xFFFFFF, 0.0f));
    g.stroke_rrect_vgrad(knob.inset(0.5f), kh * 0.5f, Color::hex(0xFFFFFF, 0.9f), t.border, 1.0f);
    g.pop_opacity();
    return changed;
}

bool Ui::toggle(const std::string& id, const RectF& r, const std::wstring& label, bool& value, bool enabled)
{
    RectF sw{r.r() - 46.0f, r.cy() - 14.0f, 46.0f, 28.0f};
    g.text(label, RectF{r.x, r.y, r.w - 56.0f, r.h}, 13.0f, 400, enabled ? t.text : t.text3, Align::Start,
           Align::Center);
    bool h = enabled && hover(id, r);
    if (h && pressed) active_ = id;
    bool held = active_ == id && down && h;
    bool changed = false;
    if (enabled && active_ == id && released && h) {
        value = !value;
        changed = true;
    }
    if (h) cursor_ = IDC_HAND;
    float on = spring(id + ".on", value ? 1.0f : 0.0f, 300.0f, 24.0f);
    float pr = anim(id + ".p", held ? 1.0f : 0.0f, 18.0f);
    g.push_opacity(enabled ? 1.0f : 0.45f);
    g.fill_rrect(sw, 14.0f, lerp(t.track, t.success, std::clamp(on, 0.0f, 1.0f)));
    float kw = 24.0f + 6.0f * pr;  // knob stretches while pressed
    float kx = sw.x + 2.0f + (sw.w - 4.0f - kw) * std::clamp(on, -0.1f, 1.1f);
    RectF knob{kx, sw.y + 2.0f, kw, 24.0f};
    g.shadow(knob, 12.0f, 4.0f, t.shadow.alpha(0.9f), 2.0f);
    g.fill_rrect(knob, 12.0f, Color::hex(0xFFFFFF));
    g.stroke_rrect(knob.inset(0.5f), 12.0f, t.border, 1.0f);
    g.pop_opacity();
    return changed;
}

void Ui::dropdown(const std::string& id, const RectF& r, const std::vector<std::wstring>& items, int sel,
                  std::function<void(int)> on_pick, bool enabled)
{
    bool h = enabled && hover(id, r);
    if (h && pressed) active_ = id;
    bool clicked = enabled && active_ == id && released && h;
    if (h) cursor_ = IDC_HAND;
    float hv = anim(id + ".h", h ? 1.0f : 0.0f, 16.0f);
    bool open = pop_.open && pop_.id == id;
    g.push_opacity(enabled ? 1.0f : 0.45f);
    glass(r, 10.0f, 0.25f + 0.2f * hv, true, hv * 0.6f);
    std::wstring text = (sel >= 0 && sel < static_cast<int>(items.size())) ? items[sel] : L"";
    g.text(text, RectF{r.x + 12.0f, r.y, r.w - 40.0f, r.h}, 13.0f, 500, t.text, Align::Start, Align::Center);
    float rot = anim(id + ".rot", open ? 1.0f : 0.0f, 18.0f);
    RectF ic{r.r() - 28.0f, r.cy() - 9.0f, 18.0f, 18.0f};
    if (rot > 0.5f) g.icon(Icon::Up, ic, t.text2, 1.7f);
    else g.icon(Icon::Chevron, ic, t.text2, 1.7f);
    g.pop_opacity();
    if (clicked) {
        pop_.open = true;
        pop_.id = id;
        pop_.anchor = r;
        pop_.is_menu = false;
        pop_.selected = sel;
        pop_.items.clear();
        for (auto& s : items) pop_.items.push_back({s, Icon::Check, false, true, nullptr});
        pop_.on_pick = std::move(on_pick);
        pop_.opened_at = time;
        pop_anim_ = 0.0f;
        active_.clear();
    }
}

void Ui::menu(const std::string& id, const RectF& anchor, std::vector<MenuItem> items)
{
    pop_.open = true;
    pop_.id = id;
    pop_.anchor = anchor;
    pop_.is_menu = true;
    pop_.selected = -1;
    pop_.items = std::move(items);
    pop_.on_pick = nullptr;
    pop_.opened_at = time;
    pop_anim_ = 0.0f;
    active_.clear();
}

void Ui::toast(const std::wstring& text, int kind)
{
    if (!toasts_.empty() && toasts_.back().text == text) {
        toasts_.back().born = time;
        return;
    }
    toasts_.push_back({text, kind, time});
    if (toasts_.size() > 3) toasts_.erase(toasts_.begin());
    want_frame_ = true;
}

void Ui::alert(const std::wstring& title, const std::wstring& message, std::vector<std::wstring> buttons,
               std::function<void(int)> on_close, int default_button, int cancel_button)
{
    alert_.open = true;
    alert_.title = title;
    alert_.message = message;
    alert_.buttons = buttons.empty() ? std::vector<std::wstring>{L"OK"} : std::move(buttons);
    alert_.on_close = std::move(on_close);
    alert_.def = default_button;
    alert_.cancel = cancel_button;
    want_frame_ = true;
}

// --------------------------------------------------------------- overlays

void Ui::draw_popover()
{
    float target = pop_.open ? 1.0f : 0.0f;
    pop_anim_ = anim("__pop", target, 20.0f);
    if (!pop_.open && pop_anim_ < 0.01f) return;
    const float item_h = 32.0f, pad = 6.0f;
    float w = pop_.anchor.w;
    for (auto& it : pop_.items)
        if (!it.separator) w = std::max(w, g.text_width(it.text, 13.0f, 500) + (pop_.is_menu ? 64.0f : 52.0f));
    float h = pad * 2;
    for (auto& it : pop_.items) h += it.separator ? 9.0f : item_h;
    float x = pop_.is_menu ? pop_.anchor.r() - w : pop_.anchor.x;
    float y = pop_.anchor.b() + 6.0f;
    if (y + h > g.height() - 8.0f) y = std::max(8.0f, pop_.anchor.y - 6.0f - h);
    x = std::clamp(x, 8.0f, std::max(8.0f, g.width() - w - 8.0f));
    pop_.rect = {x, y, w, h};

    float a = pop_anim_;
    g.push_opacity(a);
    float sc = 0.94f + 0.06f * a;
    float ox = pop_.is_menu ? pop_.rect.r() : pop_.rect.x;
    float oy = y > pop_.anchor.y ? pop_.rect.y : pop_.rect.b();
    g.set_transform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(ox, oy)));
    glass_overlay(pop_.rect, 14.0f, 1.4f);

    float iy = y + pad;
    int clicked = -1;
    for (size_t i = 0; i < pop_.items.size(); ++i) {
        auto& it = pop_.items[i];
        if (it.separator) {
            g.line(x + 14, iy + 4.5f, x + w - 14, iy + 4.5f, t.border, 1.0f);
            iy += 9.0f;
            continue;
        }
        RectF row{x + pad, iy, w - 2 * pad, item_h};
        bool h = pop_.open && it.enabled && mouse_inside && row.contains(mx, my);
        std::string rid = "__pop." + std::to_string(i);
        float hv = anim(rid, h ? 1.0f : 0.0f, 22.0f);
        if (hv > 0.01f) g.fill_rrect(row, 8.0f, t.accent.alpha(0.85f * hv));
        Color c = !it.enabled ? t.text3 : lerp(t.text, t.on_accent, hv);
        float tx = row.x + 12.0f;
        if (pop_.is_menu) {
            if (it.has_icon) g.icon(it.icon, RectF{row.x + 10.0f, row.cy() - 8.5f, 17.0f, 17.0f}, c, 1.6f);
            tx = row.x + 36.0f;
        } else {
            if (static_cast<int>(i) == pop_.selected)
                g.icon(Icon::Check, RectF{row.x + 8.0f, row.cy() - 8.0f, 16.0f, 16.0f}, c, 1.8f);
            tx = row.x + 30.0f;
        }
        g.text(it.text, RectF{tx, row.y, row.r() - tx - 8.0f, row.h}, 13.0f, 500, c, Align::Start, Align::Center);
        if (h) cursor_ = IDC_HAND;
        if (h && released && pop_.open && time - pop_.opened_at > 0.05) clicked = static_cast<int>(i);
        iy += item_h;
    }
    g.reset_transform();
    g.pop_opacity();

    if (!pop_.open) return;
    if (clicked >= 0) {
        if (pop_.is_menu) {
            auto action = pop_.items[static_cast<size_t>(clicked)].action;
            if (action) defer(action);
        } else if (pop_.on_pick) {
            auto pick = pop_.on_pick;
            defer([pick, clicked] { pick(clicked); });
        }
        pop_.open = false;
    } else if ((pressed && !pop_.rect.contains(mx, my) && time - pop_.opened_at > 0.05) || key == VK_ESCAPE) {
        pop_.open = false;
        key = 0;
    }
}

void Ui::draw_toasts()
{
    const float life = 3.6f;
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(),
                                 [&](const Toast& x) { return time - x.born > life + 0.5; }),
                  toasts_.end());
    // toasts rise from the bottom centre, above the canvas info capsule
    float y = g.height() - 116.0f;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        float age = static_cast<float>(time - it->born);
        float in = std::clamp(age / 0.25f, 0.0f, 1.0f);
        float out = std::clamp((life + 0.5f - age) / 0.4f, 0.0f, 1.0f);
        float a = std::min(in, out);
        a = a * a * (3 - 2 * a);
        float tw = std::min(g.text_width(it->text, 13.0f, 500), g.width() - 140.0f);
        float w = tw + 56.0f, h = 40.0f;
        float cx = toast_cx > 0 ? toast_cx : g.width() * 0.5f;
        RectF r{cx - w * 0.5f, y - h + 14.0f * (1.0f - a), w, h};
        g.push_opacity(a);
        glass_overlay(r, h * 0.5f, 1.2f);
        Color dot = it->kind == 1 ? t.success : it->kind == 2 ? t.warning : t.accent;
        g.fill_ellipse(r.x + 22.0f, r.cy(), 4.5f, 4.5f, dot);
        g.text(it->text, RectF{r.x + 36.0f, r.y, tw + 8.0f, h}, 13.0f, 500, t.text, Align::Start, Align::Center);
        g.pop_opacity();
        y -= (h + 8.0f) * a;
        want_frame_ = true;
    }
}

void Ui::draw_alert()
{
    if (alert_.open) alert_draw_ = alert_;
    alert_anim_ = anim("__alert", alert_.open ? 1.0f : 0.0f, 16.0f);
    if (alert_anim_ < 0.01f) return;
    const Alert& al = alert_draw_;
    float a = alert_anim_;
    g.push_opacity(a);
    g.fill_rect(RectF{0, 0, g.width(), g.height()}, t.dim);
    float w = std::min(420.0f, g.width() - 40.0f);
    float msg_h = g.text_height(al.message, 13.0f, 400, w - 48.0f);
    float h = 24.0f + 26.0f + 8.0f + msg_h + 24.0f + 36.0f + 22.0f;
    float sc = 0.92f + 0.08f * a;
    RectF r{g.width() * 0.5f - w * 0.5f, g.height() * 0.5f - h * 0.5f, w, h};
    g.set_transform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(r.cx(), r.cy())));
    glass_overlay(r, 22.0f, 2.0f);
    g.text(al.title, RectF{r.x + 24.0f, r.y + 24.0f, w - 48.0f, 26.0f}, 17.0f, 600, t.text, Align::Start, Align::Center);
    g.text(al.message, RectF{r.x + 24.0f, r.y + 58.0f, w - 48.0f, msg_h + 4.0f}, 13.0f, 400, t.text2, Align::Start,
           Align::Start, true);
    g.reset_transform();

    int n = static_cast<int>(al.buttons.size());
    float bw = (w - 48.0f - 10.0f * (n - 1)) / n;
    float by = r.b() - 22.0f - 36.0f;
    int chosen = -1;
    // buttons ignore the modal blocker
    auto saved = blockers_;
    blockers_.clear();
    for (int i = 0; i < n; ++i) {
        RectF br{r.x + 24.0f + i * (bw + 10.0f), by, bw, 36.0f};
        bool primary = i == al.def;
        if (button("__alert.b" + std::to_string(i), br, al.buttons[i], Icon::Check, false,
                   primary ? ButtonStyle::Primary : ButtonStyle::Glass, alert_.open))
            chosen = i;
    }
    blockers_ = saved;
    g.pop_opacity();

    if (alert_.open) {
        if (key == VK_RETURN) { chosen = al.def; key = 0; }
        else if (key == VK_ESCAPE) { chosen = al.cancel >= 0 ? al.cancel : al.def; key = 0; }
        if (chosen >= 0) {
            auto cb = alert_.on_close;
            alert_.open = false;
            if (cb) defer([cb, chosen] { cb(chosen); });
        }
    }
}

void Ui::draw_tooltip()
{
    if (hot_ != last_hot_) {
        last_hot_ = hot_;
        hot_since_ = time;
    }
    bool show = !tip_id_.empty() && tip_id_ == hot_ && !down && time - hot_since_ > 0.55;
    if (!tip_id_.empty() && !show) want_frame_ = true;
    float a = anim("__tip", show ? 1.0f : 0.0f, 18.0f);
    if (a < 0.01f || tip_text_.empty()) return;
    float tw = g.text_width(tip_text_, 12.0f, 500);
    RectF r{tip_rect_.cx() - tw * 0.5f - 12.0f, tip_rect_.b() + 8.0f, tw + 24.0f, 28.0f};
    if (r.b() > g.height() - 6.0f) r.y = tip_rect_.y - 8.0f - r.h;
    r.x = std::clamp(r.x, 6.0f, std::max(6.0f, g.width() - r.w - 6.0f));
    g.push_opacity(a);
    glass_overlay(r, 14.0f, 0.8f);
    g.text(tip_text_, r, 12.0f, 500, t.text, Align::Center, Align::Center);
    g.pop_opacity();
}

void Ui::end_frame()
{
    draw_toasts();
    draw_popover();
    draw_tooltip();
    draw_alert();
    if (released || !down) {
        if (released) active_.clear();
    }
    pressed = released = dbl = false;
    wheel = 0;
    key = 0;
}
