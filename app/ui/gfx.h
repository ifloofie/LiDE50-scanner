// LiDE50 Scan - Direct2D / DirectWrite drawing layer. All coordinates are in
// device-independent pixels (1/96 inch).
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct Image;

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
    static Color hex(std::uint32_t rgb, float a = 1.0f)
    {
        return {((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, a};
    }
    Color alpha(float m) const { return {r, g, b, a * m}; }
    Color with_a(float na) const { return {r, g, b, na}; }
};
Color lerp(const Color& x, const Color& y, float t);
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

struct RectF {
    float x = 0, y = 0, w = 0, h = 0;
    float r() const { return x + w; }
    float b() const { return y + h; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    RectF inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    RectF inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    RectF offset(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    RectF scaled_about_center(float s) const
    {
        return {cx() - w * s * 0.5f, cy() - h * s * 0.5f, w * s, h * s};
    }
    D2D1_RECT_F d2d() const { return D2D1::RectF(x, y, x + w, y + h); }
};

enum class Align { Start, Center, End };

enum class Icon {
    Preview, Scan, Cancel, Sun, Moon, Fit, Glass, More, RotateLeft, RotateRight, Mirror, Trash,
    Up, Down, Save, Wand, Check, Chevron, Plug, Shield, Doc, Reset, Layers
};

// Holds a GPU bitmap made from an Image; recreated automatically after a
// device loss.
struct GpuImage {
    ID2D1Bitmap* bmp = nullptr;
    int w = 0, h = 0;
    std::uint64_t key = 0;     // identifies the source content
    unsigned device_gen = 0;
    void release();
    ~GpuImage() { release(); }
};

class Gfx {
public:
    bool init(HWND hwnd);
    void shutdown();
    void resize(UINT w, UINT h);
    void set_dpi(float dpi);
    float dpi() const { return dpi_; }
    float width() const { return w_dip_; }
    float height() const { return h_dip_; }

    bool begin();
    void end();
    unsigned device_generation() const { return device_gen_; }

    // global opacity multiplier (stack)
    void push_opacity(float o);
    void pop_opacity();
    void set_transform(const D2D1_MATRIX_3X2_F& m);
    void reset_transform();
    void push_clip(const RectF& r);
    void pop_clip();

    void clear(const Color& c);
    void fill_rect(const RectF& r, const Color& c);
    void fill_rrect(const RectF& r, float radius, const Color& c);
    void stroke_rrect(const RectF& r, float radius, const Color& c, float width = 1.0f);
    void fill_rrect_vgrad(const RectF& r, float radius, const Color& top, const Color& bottom);
    void fill_rrect_hgrad(const RectF& r, float radius, const Color& left, const Color& right);
    void stroke_rrect_vgrad(const RectF& r, float radius, const Color& top, const Color& bottom, float width = 1.0f);
    void fill_ellipse(float cx, float cy, float rx, float ry, const Color& c);
    void stroke_ellipse(float cx, float cy, float rx, float ry, const Color& c, float width = 1.0f);
    void fill_radial(float cx, float cy, float rx, float ry, const Color& center, const Color& edge);
    // rounded rect filled with a radial gradient (used for pointer-following highlights)
    void fill_rrect_radial(const RectF& r, float radius, float cx, float cy, float rad, const Color& center,
                           const Color& edge);
    void line(float x1, float y1, float x2, float y2, const Color& c, float width = 1.0f, bool round = true);
    // soft drop shadow made of layered translucent rounded rects
    void shadow(const RectF& r, float radius, float spread, const Color& c, float dy = 0);

    void text(const std::wstring& s, const RectF& r, float size, int weight, const Color& c,
              Align h = Align::Start, Align v = Align::Center, bool wrap = false);
    float text_width(const std::wstring& s, float size, int weight);
    float text_height(const std::wstring& s, float size, int weight, float max_width);

    // image drawing (rounded corners use a bitmap brush)
    void update_image(GpuImage& gi, const Image& img, std::uint64_t key);
    void update_image_rows(GpuImage& gi, const Image& img, int row0, int row1);
    void draw_image(GpuImage& gi, const RectF& dst, float opacity = 1.0f, float radius = 0.0f,
                    const RectF* src = nullptr, bool smooth = true);

    void icon(Icon ic, const RectF& r, const Color& c, float stroke = 1.6f);

private:
    ID2D1SolidColorBrush* brush(const Color& c);
    IDWriteTextFormat* format(float size, int weight, Align h, Align v, bool wrap);
    ID2D1GradientStopCollection* stops(const Color& a, const Color& b);
    void discard_device();
    bool create_device();

    HWND hwnd_ = nullptr;
    ID2D1Factory* factory_ = nullptr;
    IDWriteFactory* dwrite_ = nullptr;
    ID2D1HwndRenderTarget* rt_ = nullptr;
    ID2D1SolidColorBrush* solid_ = nullptr;
    ID2D1StrokeStyle* round_cap_ = nullptr;
    std::map<std::uint64_t, IDWriteTextFormat*> formats_;
    std::map<std::uint64_t, ID2D1GradientStopCollection*> stops_;
    std::vector<float> opacity_stack_;
    float opacity_ = 1.0f;
    float dpi_ = 96.0f;
    UINT w_px_ = 1, h_px_ = 1;
    float w_dip_ = 1, h_dip_ = 1;
    unsigned device_gen_ = 1;
    bool wine_ = false;   // Wine's Direct2D needs premultiplied gradient stops
    std::wstring font_family_ = L"Segoe UI";
    std::vector<GpuImage*> images_;
    friend struct GpuImage;
};
