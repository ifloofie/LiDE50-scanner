// LiDE50 Scan - Direct2D / DirectWrite drawing layer.
#include "gfx.h"

#include "../image.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

template <class T>
void safe_release(T*& p)
{
    if (p) { p->Release(); p = nullptr; }
}

std::uint64_t color_key(const Color& c)
{
    auto q = [](float v) { return static_cast<std::uint64_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return (q(c.r) << 24) | (q(c.g) << 16) | (q(c.b) << 8) | q(c.a);
}

} // namespace

Color lerp(const Color& x, const Color& y, float t)
{
    return {x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t, x.a + (y.a - x.a) * t};
}

void GpuImage::release()
{
    safe_release(bmp);
    w = h = 0;
    key = 0;
}

bool Gfx::init(HWND hwnd)
{
    hwnd_ = hwnd;
    wine_ = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version") != nullptr;
    D2D1_FACTORY_OPTIONS opts{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), &opts,
                                 reinterpret_cast<void**>(&factory_))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&dwrite_))))
        return false;
    // Windows 11 ships "Segoe UI Variable"; Windows 10 has Segoe UI.
    IDWriteFontCollection* fonts = nullptr;
    if (SUCCEEDED(dwrite_->GetSystemFontCollection(&fonts, FALSE)) && fonts) {
        const wchar_t* prefs_win[] = {L"Segoe UI Variable Text", L"Segoe UI", L"Tahoma", L"Arial"};
        const wchar_t* prefs_wine[] = {L"Inter", L"Liberation Sans", L"Segoe UI", L"Tahoma"};
        auto& prefs = wine_ ? prefs_wine : prefs_win;
        for (const wchar_t* f : prefs) {
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(fonts->FindFamilyName(f, &idx, &exists)) && exists) {
                font_family_ = f;
                break;
            }
        }
        fonts->Release();
    }
    D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(&sp, nullptr, 0, &round_cap_);
    RECT rc;
    GetClientRect(hwnd, &rc);
    w_px_ = static_cast<UINT>(std::max<LONG>(1, rc.right));
    h_px_ = static_cast<UINT>(std::max<LONG>(1, rc.bottom));
    return create_device();
}

bool Gfx::create_device()
{
    if (rt_) return true;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 0, 0);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(w_px_, h_px_));
    if (FAILED(factory_->CreateHwndRenderTarget(props, hp, &rt_))) return false;
    rt_->SetDpi(dpi_, dpi_);
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &solid_);
    ++device_gen_;
    return true;
}

void Gfx::discard_device()
{
    for (auto& kv : stops_) kv.second->Release();
    stops_.clear();
    safe_release(solid_);
    safe_release(rt_);
}

void Gfx::shutdown()
{
    discard_device();
    for (auto& kv : formats_) kv.second->Release();
    formats_.clear();
    safe_release(round_cap_);
    safe_release(dwrite_);
    safe_release(factory_);
}

void Gfx::resize(UINT w, UINT h)
{
    w_px_ = std::max<UINT>(1, w);
    h_px_ = std::max<UINT>(1, h);
    w_dip_ = w_px_ * 96.0f / dpi_;
    h_dip_ = h_px_ * 96.0f / dpi_;
    if (rt_) rt_->Resize(D2D1::SizeU(w_px_, h_px_));
}

void Gfx::set_dpi(float dpi)
{
    dpi_ = dpi;
    w_dip_ = w_px_ * 96.0f / dpi_;
    h_dip_ = h_px_ * 96.0f / dpi_;
    if (rt_) rt_->SetDpi(dpi, dpi);
}

bool Gfx::begin()
{
    if (!create_device()) return false;
    w_dip_ = w_px_ * 96.0f / dpi_;
    h_dip_ = h_px_ * 96.0f / dpi_;
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
    opacity_ = 1.0f;
    opacity_stack_.clear();
    return true;
}

void Gfx::end()
{
    if (!rt_) return;
    HRESULT hr = rt_->EndDraw();
    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) discard_device();
}

void Gfx::push_opacity(float o)
{
    opacity_stack_.push_back(opacity_);
    opacity_ *= std::clamp(o, 0.0f, 1.0f);
}

void Gfx::pop_opacity()
{
    if (!opacity_stack_.empty()) {
        opacity_ = opacity_stack_.back();
        opacity_stack_.pop_back();
    }
}

void Gfx::set_transform(const D2D1_MATRIX_3X2_F& m) { rt_->SetTransform(m); }
void Gfx::reset_transform() { rt_->SetTransform(D2D1::Matrix3x2F::Identity()); }
void Gfx::push_clip(const RectF& r) { rt_->PushAxisAlignedClip(r.d2d(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE); }
void Gfx::pop_clip() { rt_->PopAxisAlignedClip(); }

ID2D1SolidColorBrush* Gfx::brush(const Color& c)
{
    solid_->SetColor(D2D1::ColorF(c.r, c.g, c.b, std::clamp(c.a * opacity_, 0.0f, 1.0f)));
    return solid_;
}

ID2D1GradientStopCollection* Gfx::stops(const Color& a, const Color& b)
{
    Color aa = a.alpha(opacity_), bb = b.alpha(opacity_);
    std::uint64_t key = (color_key(aa) << 32) | color_key(bb);
    auto it = stops_.find(key);
    if (it != stops_.end()) return it->second;
    if (stops_.size() > 4000) {
        for (auto& kv : stops_) kv.second->Release();
        stops_.clear();
    }
    if (wine_) {
        aa = {aa.r * aa.a, aa.g * aa.a, aa.b * aa.a, aa.a};
        bb = {bb.r * bb.a, bb.g * bb.a, bb.b * bb.a, bb.a};
    }
    D2D1_GRADIENT_STOP gs[2] = {{0.0f, D2D1::ColorF(aa.r, aa.g, aa.b, aa.a)},
                                {1.0f, D2D1::ColorF(bb.r, bb.g, bb.b, bb.a)}};
    ID2D1GradientStopCollection* col = nullptr;
    rt_->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &col);
    if (col) stops_[key] = col;
    return col;
}

void Gfx::clear(const Color& c) { rt_->Clear(D2D1::ColorF(c.r, c.g, c.b, c.a)); }

void Gfx::fill_rect(const RectF& r, const Color& c) { rt_->FillRectangle(r.d2d(), brush(c)); }

void Gfx::fill_rrect(const RectF& r, float radius, const Color& c)
{
    if (r.w <= 0 || r.h <= 0) return;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush(c));
}

void Gfx::stroke_rrect(const RectF& r, float radius, const Color& c, float width)
{
    if (r.w <= 0 || r.h <= 0) return;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), brush(c), width);
}

void Gfx::fill_rrect_vgrad(const RectF& r, float radius, const Color& top, const Color& bottom)
{
    if (r.w <= 0 || r.h <= 0) return;
    ID2D1LinearGradientBrush* gb = nullptr;
    rt_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.b())),
        stops(top, bottom), &gb);
    if (!gb) return;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), gb);
    gb->Release();
}

void Gfx::fill_rrect_hgrad(const RectF& r, float radius, const Color& left, const Color& right)
{
    if (r.w <= 0 || r.h <= 0) return;
    ID2D1LinearGradientBrush* gb = nullptr;
    rt_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.r(), r.y)),
        stops(left, right), &gb);
    if (!gb) return;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), gb);
    gb->Release();
}

void Gfx::stroke_rrect_vgrad(const RectF& r, float radius, const Color& top, const Color& bottom, float width)
{
    if (r.w <= 0 || r.h <= 0) return;
    ID2D1LinearGradientBrush* gb = nullptr;
    rt_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(r.x, r.y), D2D1::Point2F(r.x, r.b())),
        stops(top, bottom), &gb);
    if (!gb) return;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), gb, width);
    gb->Release();
}

void Gfx::fill_ellipse(float cx, float cy, float rx, float ry, const Color& c)
{
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush(c));
}

void Gfx::stroke_ellipse(float cx, float cy, float rx, float ry, const Color& c, float width)
{
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), brush(c), width);
}

void Gfx::fill_radial(float cx, float cy, float rx, float ry, const Color& center, const Color& edge)
{
    ID2D1RadialGradientBrush* rb = nullptr;
    rt_->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rx, ry),
        stops(center, edge), &rb);
    if (!rb) return;
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, ry), rb);
    rb->Release();
}

void Gfx::fill_rrect_radial(const RectF& r, float radius, float cx, float cy, float rad, const Color& center,
                            const Color& edge)
{
    if (r.w <= 0 || r.h <= 0) return;
    ID2D1RadialGradientBrush* rb = nullptr;
    rt_->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), rad, rad),
        stops(center, edge), &rb);
    if (!rb) return;
    radius = std::min(radius, std::min(r.w, r.h) * 0.5f);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(r.d2d(), radius, radius), rb);
    rb->Release();
}

void Gfx::line(float x1, float y1, float x2, float y2, const Color& c, float width, bool round)
{
    rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush(c), width, round ? round_cap_ : nullptr);
}

void Gfx::shadow(const RectF& r, float radius, float spread, const Color& c, float dy)
{
    const int layers = 7;
    for (int i = layers; i >= 1; --i) {
        float t = static_cast<float>(i) / layers;
        float grow = spread * t;
        RectF s{r.x - grow, r.y - grow + dy * (0.4f + 0.6f * t), r.w + 2 * grow, r.h + 2 * grow};
        float a = c.a * (1.0f - t) * (1.0f - t) * 0.55f + c.a * 0.02f;
        fill_rrect(s, radius + grow, c.with_a(a));
    }
}

IDWriteTextFormat* Gfx::format(float size, int weight, Align h, Align v, bool wrap)
{
    std::uint64_t key = (static_cast<std::uint64_t>(size * 10.0f) << 24) | (static_cast<std::uint64_t>(weight) << 8) |
                        (static_cast<std::uint64_t>(h) << 4) | (static_cast<std::uint64_t>(v) << 2) | (wrap ? 1 : 0);
    auto it = formats_.find(key);
    if (it != formats_.end()) return it->second;
    IDWriteTextFormat* f = nullptr;
    dwrite_->CreateTextFormat(font_family_.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(weight),
                              DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(h == Align::Start ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : h == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_TRAILING);
    f->SetParagraphAlignment(v == Align::Start ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                             : v == Align::Center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_FAR);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        IDWriteInlineObject* ellipsis = nullptr;
        dwrite_->CreateEllipsisTrimmingSign(f, &ellipsis);
        f->SetTrimming(&trim, ellipsis);
        if (ellipsis) ellipsis->Release();
    }
    formats_[key] = f;
    return f;
}

void Gfx::text(const std::wstring& s, const RectF& r, float size, int weight, const Color& c, Align h, Align v,
               bool wrap)
{
    if (s.empty()) return;
    IDWriteTextFormat* f = format(size, weight, h, v, wrap);
    if (!f) return;
    rt_->DrawTextW(s.c_str(), static_cast<UINT32>(s.size()), f, r.d2d(), brush(c),
                   wrap ? D2D1_DRAW_TEXT_OPTIONS_NONE : D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Gfx::text_width(const std::wstring& s, float size, int weight)
{
    if (s.empty()) return 0;
    IDWriteTextFormat* f = format(size, weight, Align::Start, Align::Start, false);
    IDWriteTextLayout* tl = nullptr;
    dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 10000.0f, 1000.0f, &tl);
    if (!tl) return 0;
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    tl->Release();
    return m.widthIncludingTrailingWhitespace;
}

float Gfx::text_height(const std::wstring& s, float size, int weight, float max_width)
{
    if (s.empty()) return 0;
    IDWriteTextFormat* f = format(size, weight, Align::Start, Align::Start, true);
    IDWriteTextLayout* tl = nullptr;
    dwrite_->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, std::max(1.0f, max_width), 10000.0f, &tl);
    if (!tl) return 0;
    DWRITE_TEXT_METRICS m{};
    tl->GetMetrics(&m);
    tl->Release();
    return m.height;
}

void Gfx::update_image(GpuImage& gi, const Image& img, std::uint64_t key)
{
    if (!rt_) return;
    if (gi.bmp && gi.key == key && gi.device_gen == device_gen_ && gi.w == img.width && gi.h == img.height) return;
    gi.release();
    if (img.empty()) return;
    const int w = img.width, h = img.height;
    std::vector<std::uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* s = img.row(y);
        std::uint8_t* d = &bgra[static_cast<size_t>(y) * w * 4];
        if (img.channels == 3) {
            for (int x = 0; x < w; ++x) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; d += 4; s += 3; }
        } else {
            for (int x = 0; x < w; ++x) { d[0] = d[1] = d[2] = s[x]; d[3] = 255; d += 4; }
        }
    }
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    rt_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), bgra.data(),
                      static_cast<UINT32>(w * 4), bp, &gi.bmp);
    if (gi.bmp) {
        gi.w = w;
        gi.h = h;
        gi.key = key;
        gi.device_gen = device_gen_;
    }
}

void Gfx::update_image_rows(GpuImage& gi, const Image& img, int row0, int row1)
{
    if (!gi.bmp || gi.device_gen != device_gen_ || gi.w != img.width || gi.h != img.height) {
        update_image(gi, img, gi.key ? gi.key : 1);
        return;
    }
    row0 = std::clamp(row0, 0, img.height);
    row1 = std::clamp(row1, row0, img.height);
    if (row1 <= row0) return;
    const int w = img.width;
    std::vector<std::uint8_t> bgra(static_cast<size_t>(w) * (row1 - row0) * 4);
    for (int y = row0; y < row1; ++y) {
        const std::uint8_t* s = img.row(y);
        std::uint8_t* d = &bgra[static_cast<size_t>(y - row0) * w * 4];
        if (img.channels == 3) {
            for (int x = 0; x < w; ++x) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; d += 4; s += 3; }
        } else {
            for (int x = 0; x < w; ++x) { d[0] = d[1] = d[2] = s[x]; d[3] = 255; d += 4; }
        }
    }
    D2D1_RECT_U dst = D2D1::RectU(0, static_cast<UINT32>(row0), static_cast<UINT32>(w), static_cast<UINT32>(row1));
    gi.bmp->CopyFromMemory(&dst, bgra.data(), static_cast<UINT32>(w * 4));
}

void Gfx::draw_image(GpuImage& gi, const RectF& dst, float opacity, float radius, const RectF* src, bool smooth)
{
    if (!gi.bmp || gi.device_gen != device_gen_ || dst.w <= 0 || dst.h <= 0) return;
    RectF s = src ? *src : RectF{0, 0, static_cast<float>(gi.w), static_cast<float>(gi.h)};
    if (s.w <= 0 || s.h <= 0) return;
    float op = std::clamp(opacity * opacity_, 0.0f, 1.0f);
    if (radius <= 0.5f) {
        rt_->DrawBitmap(gi.bmp, dst.d2d(), op,
                        smooth ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                        s.d2d());
        return;
    }
    ID2D1BitmapBrush* bb = nullptr;
    D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(
        D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
        smooth ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    rt_->CreateBitmapBrush(gi.bmp, bbp, D2D1::BrushProperties(op), &bb);
    if (!bb) return;
    // bitmap brush coordinates are in DIPs of the bitmap (96 dpi = pixels)
    float sx = dst.w / s.w, sy = dst.h / s.h;
    bb->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(dst.x - s.x * sx, dst.y - s.y * sy));
    radius = std::min(radius, std::min(dst.w, dst.h) * 0.5f);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(dst.d2d(), radius, radius), bb);
    bb->Release();
}

// ------------------------------------------------------------------ icons
// Drawn from simple strokes in a 24 x 24 design grid.

void Gfx::icon(Icon ic, const RectF& r, const Color& c, float stroke)
{
    const float s = std::min(r.w, r.h) / 24.0f;
    const float ox = r.cx() - 12 * s, oy = r.cy() - 12 * s;
    auto X = [&](float v) { return ox + v * s; };
    auto Y = [&](float v) { return oy + v * s; };
    const float sw = stroke * std::max(0.75f, s);
    auto L = [&](float x1, float y1, float x2, float y2) { line(X(x1), Y(y1), X(x2), Y(y2), c, sw); };
    // arcs are built from short line segments (works on every Direct2D version)
    auto arc_points = [&](float cx, float cy, float rad, float a0, float a1, std::vector<D2D1_POINT_2F>& pts) {
        const float d2r = 3.14159265f / 180.0f;
        int n = std::max(6, static_cast<int>(std::fabs(a1 - a0) / 8.0f));
        for (int i = 0; i <= n; ++i) {
            float a = (a0 + (a1 - a0) * i / n) * d2r;
            pts.push_back(D2D1::Point2F(X(cx + rad * std::cos(a)), Y(cy + rad * std::sin(a))));
        }
    };
    auto stroke_path = [&](const std::vector<D2D1_POINT_2F>& pts, bool closed, bool fill) {
        if (pts.size() < 2) return;
        ID2D1PathGeometry* g = nullptr;
        factory_->CreatePathGeometry(&g);
        if (!g) return;
        ID2D1GeometrySink* k = nullptr;
        g->Open(&k);
        k->BeginFigure(pts[0], fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        k->AddLines(&pts[1], static_cast<UINT32>(pts.size() - 1));
        k->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        k->Close();
        k->Release();
        if (fill) rt_->FillGeometry(g, brush(c));
        else rt_->DrawGeometry(g, brush(c), sw, round_cap_);
        g->Release();
    };
    auto arc = [&](float cx, float cy, float rad, float a0, float a1, bool arrow_end) {
        std::vector<D2D1_POINT_2F> pts;
        arc_points(cx, cy, rad, a0, a1, pts);
        stroke_path(pts, false, false);
        if (arrow_end) {
            const float d2r = 3.14159265f / 180.0f;
            float a = a1 * d2r;
            float ex = cx + rad * std::cos(a), ey = cy + rad * std::sin(a);
            float dir = a1 > a0 ? 1.0f : -1.0f;
            float tx = -std::sin(a) * dir, ty = std::cos(a) * dir;
            float nx = std::cos(a), ny = std::sin(a);
            L(ex, ey, ex - tx * 3.2f + nx * 2.6f, ey - ty * 3.2f + ny * 2.6f);
            L(ex, ey, ex - tx * 3.2f - nx * 2.6f, ey - ty * 3.2f - ny * 2.6f);
        }
    };
    auto box = [&](float x, float y, float w, float h, float rad) {
        rt_->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(X(x), Y(y), X(x + w), Y(y + h)), rad * s, rad * s),
                                  brush(c), sw);
    };

    switch (ic) {
        case Icon::Preview:  // eye
            arc(12, 19, 10, 225, 315, false);
            arc(12, 5, 10, 135, 45, false);
            stroke_ellipse(X(12), Y(12), 3.0f * s, 3.0f * s, c, sw);
            break;
        case Icon::Scan:  // page with scan line
            box(5, 3, 14, 18, 2.5f);
            L(2.5f, 12, 21.5f, 12);
            L(8.5f, 7.5f, 15.5f, 7.5f);
            L(8.5f, 16.5f, 13.5f, 16.5f);
            break;
        case Icon::Cancel:
            L(7, 7, 17, 17);
            L(17, 7, 7, 17);
            break;
        case Icon::Sun:
            stroke_ellipse(X(12), Y(12), 4.2f * s, 4.2f * s, c, sw);
            for (int i = 0; i < 8; ++i) {
                float a = i * 3.14159265f / 4.0f;
                L(12 + 7.2f * std::cos(a), 12 + 7.2f * std::sin(a), 12 + 9.4f * std::cos(a), 12 + 9.4f * std::sin(a));
            }
            break;
        case Icon::Moon: {
            // crescent = big circle minus an offset circle
            const float c1x = 12, c1y = 12, r1 = 8.0f, c2x = 16.0f, c2y = 8.0f, r2 = 6.6f;
            float dx = c2x - c1x, dy = c2y - c1y, d = std::hypot(dx, dy);
            float a = (r1 * r1 - r2 * r2 + d * d) / (2 * d), hh = std::sqrt(std::max(0.0f, r1 * r1 - a * a));
            float mx = c1x + a * dx / d, my = c1y + a * dy / d;
            float p1x = mx + hh * dy / d, p1y = my - hh * dx / d;
            float p2x = mx - hh * dy / d, p2y = my + hh * dx / d;
            auto ang = [](float cx, float cy, float x, float y) { return std::atan2(y - cy, x - cx); };
            const float tau = 6.2831853f;
            std::vector<D2D1_POINT_2F> pts;
            // outer arc on circle 1 from p1 to p2, the side away from circle 2
            float a1 = ang(c1x, c1y, p1x, p1y), a2 = ang(c1x, c1y, p2x, p2y);
            float away = ang(c1x, c1y, c1x - dx, c1y - dy);
            float span = a2 - a1;
            while (span < 0) span += tau;
            float mid = a1 + span * 0.5f;
            if (std::cos(mid - away) < 0) span -= tau;  // go the other way round
            for (int i = 0; i <= 36; ++i) {
                float t = a1 + span * i / 36.0f;
                pts.push_back(D2D1::Point2F(X(c1x + r1 * std::cos(t)), Y(c1y + r1 * std::sin(t))));
            }
            // inner arc on circle 2 from p2 back to p1, the side inside circle 1
            float b2 = ang(c2x, c2y, p2x, p2y), b1 = ang(c2x, c2y, p1x, p1y);
            float toward = ang(c2x, c2y, c1x, c1y);
            float span2 = b1 - b2;
            while (span2 < 0) span2 += tau;
            if (std::cos(b2 + span2 * 0.5f - toward) < 0) span2 -= tau;
            for (int i = 1; i <= 24; ++i) {
                float t = b2 + span2 * i / 24.0f;
                pts.push_back(D2D1::Point2F(X(c2x + r2 * std::cos(t)), Y(c2y + r2 * std::sin(t))));
            }
            stroke_path(pts, true, false);
            break;
        }
        case Icon::Fit:
            L(4, 9, 4, 4); L(4, 4, 9, 4);
            L(15, 4, 20, 4); L(20, 4, 20, 9);
            L(20, 15, 20, 20); L(20, 20, 15, 20);
            L(9, 20, 4, 20); L(4, 20, 4, 15);
            break;
        case Icon::Glass:  // scanner bed
            box(3, 6, 18, 12, 2.5f);
            L(6, 9.5f, 6, 14.5f);
            break;
        case Icon::More:
            fill_ellipse(X(6), Y(12), 1.7f * s, 1.7f * s, c);
            fill_ellipse(X(12), Y(12), 1.7f * s, 1.7f * s, c);
            fill_ellipse(X(18), Y(12), 1.7f * s, 1.7f * s, c);
            break;
        case Icon::RotateLeft:
            arc(12, 13, 7, 340, 160, true);
            break;
        case Icon::RotateRight:
            arc(12, 13, 7, 200, 380, true);
            break;
        case Icon::Mirror:
            L(12, 3, 12, 21);
            L(9, 7, 4, 17); L(4, 17, 9, 17); L(9, 17, 9, 7);
            L(15, 7, 20, 17); L(20, 17, 15, 17); L(15, 17, 15, 7);
            break;
        case Icon::Trash:
            L(4, 6.5f, 20, 6.5f);
            L(9.5f, 6.5f, 10, 3.5f); L(10, 3.5f, 14, 3.5f); L(14, 3.5f, 14.5f, 6.5f);
            L(6, 6.5f, 7, 20.5f); L(7, 20.5f, 17, 20.5f); L(17, 20.5f, 18, 6.5f);
            L(10, 10, 10, 17); L(14, 10, 14, 17);
            break;
        case Icon::Up:
            L(6, 14.5f, 12, 8.5f); L(12, 8.5f, 18, 14.5f);
            break;
        case Icon::Down:
            L(6, 9.5f, 12, 15.5f); L(12, 15.5f, 18, 9.5f);
            break;
        case Icon::Chevron:
            L(8, 10, 12, 14); L(12, 14, 16, 10);
            break;
        case Icon::Save:
            L(12, 3.5f, 12, 14.5f);
            L(7.5f, 10, 12, 14.5f); L(12, 14.5f, 16.5f, 10);
            L(4, 15, 4, 20); L(4, 20, 20, 20); L(20, 20, 20, 15);
            break;
        case Icon::Wand:
            L(4, 20, 14, 10);
            L(17, 3, 17, 7); L(15, 5, 19, 5);
            L(20, 10, 20, 13); L(18.5f, 11.5f, 21.5f, 11.5f);
            L(10, 3.5f, 10, 6.5f); L(8.5f, 5, 11.5f, 5);
            break;
        case Icon::Check:
            L(5, 12.5f, 10, 17.5f); L(10, 17.5f, 19, 7.5f);
            break;
        case Icon::Plug:
            L(9, 3, 9, 7); L(15, 3, 15, 7);
            box(6, 7, 12, 7, 3);
            L(12, 14, 12, 21);
            break;
        case Icon::Shield:
            L(12, 3, 19, 6); L(19, 6, 19, 12); L(5, 6, 12, 3); L(5, 6, 5, 12);
            arc(12, 12, 7, 0, 90, false);
            arc(12, 12, 7, 180, 90, false);
            break;
        case Icon::Doc:
            L(6, 3, 14, 3); L(14, 3, 19, 8); L(19, 8, 19, 21); L(19, 21, 6, 21); L(6, 21, 6, 3);
            L(14, 3, 14, 8); L(14, 8, 19, 8);
            L(9, 13, 16, 13); L(9, 17, 16, 17);
            break;
        case Icon::Reset:
            arc(12, 12, 7.5f, 200, 470, true);
            break;
        case Icon::Layers:
            L(12, 4, 21, 9); L(21, 9, 12, 14); L(12, 14, 3, 9); L(3, 9, 12, 4);
            L(3, 13.5f, 12, 18.5f); L(12, 18.5f, 21, 13.5f);
            break;
    }
}
