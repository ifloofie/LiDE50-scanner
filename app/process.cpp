// LiDE50 Scan - image adjustments.
#include "process.h"

#include <algorithm>
#include <cmath>
#include <cstring>

bool Adjustments::operator==(const Adjustments& o) const
{
    return brightness == o.brightness && contrast == o.contrast && saturation == o.saturation &&
           gamma == o.gamma && sharpness == o.sharpness && threshold == o.threshold &&
           denoise == o.denoise && auto_levels == o.auto_levels && invert == o.invert &&
           black_white == o.black_white && grayscale == o.grayscale && rotation == o.rotation &&
           flip_h == o.flip_h;
}

namespace {

inline std::uint8_t clamp8(double v)
{
    return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5));
}
inline std::uint8_t clamp8i(int v) { return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Separable box blur, radius r, applied `passes` times (2 passes ~ gaussian).
Image box_blur(const Image& src, int r, int passes = 2)
{
    if (r < 1) return src;
    Image a = src, b = src;
    const int w = src.width, h = src.height, c = src.channels;
    const int win = 2 * r + 1;
    std::vector<int> acc(static_cast<size_t>(c));
    for (int p = 0; p < passes; ++p) {
        // horizontal a -> b
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* in = a.row(y);
            std::uint8_t* out = b.row(y);
            for (int k = 0; k < c; ++k) {
                int s = 0;
                for (int i = -r; i <= r; ++i) s += in[std::clamp(i, 0, w - 1) * c + k];
                for (int x = 0; x < w; ++x) {
                    out[x * c + k] = static_cast<std::uint8_t>(s / win);
                    int add = std::min(x + r + 1, w - 1), sub = std::max(x - r, 0);
                    s += in[add * c + k] - in[sub * c + k];
                }
            }
        }
        // vertical b -> a
        const size_t stride = b.stride();
        for (int x = 0; x < w * c; ++x) {
            int s = 0;
            for (int i = -r; i <= r; ++i) s += b.data[std::clamp(i, 0, h - 1) * stride + x];
            for (int y = 0; y < h; ++y) {
                a.data[y * stride + x] = static_cast<std::uint8_t>(s / win);
                int add = std::min(y + r + 1, h - 1), sub = std::max(y - r, 0);
                s += b.data[add * stride + x] - b.data[sub * stride + x];
            }
        }
    }
    return a;
}

Image to_gray(const Image& src)
{
    if (src.channels == 1) return src;
    Image g;
    g.alloc(src.width, src.height, 1);
    g.dpi = src.dpi;
    const size_t n = static_cast<size_t>(src.width) * src.height;
    const std::uint8_t* s = src.data.data();
    for (size_t i = 0; i < n; ++i, s += 3)
        g.data[i] = static_cast<std::uint8_t>((s[0] * 77 + s[1] * 150 + s[2] * 29) >> 8);
    return g;
}

Image rotate(const Image& src, int deg, bool flip)
{
    deg = ((deg % 360) + 360) % 360;
    if (deg == 0 && !flip) return src;
    const int w = src.width, h = src.height, c = src.channels;
    Image out;
    const bool swap = deg == 90 || deg == 270;
    out.alloc(swap ? h : w, swap ? w : h, c);
    out.dpi = src.dpi;
    out.bilevel = src.bilevel;
    const int ow = out.width;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* in = src.row(y);
        for (int x = 0; x < w; ++x) {
            int sx = flip ? (w - 1 - x) : x;
            int nx, ny;
            switch (deg) {
                case 90: nx = h - 1 - y; ny = sx; break;
                case 180: nx = w - 1 - sx; ny = h - 1 - y; break;
                case 270: nx = y; ny = w - 1 - sx; break;
                default: nx = sx; ny = y; break;
            }
            std::memcpy(&out.data[(static_cast<size_t>(ny) * ow + nx) * c], in + x * c, c);
        }
    }
    return out;
}

} // namespace

Image downscale(const Image& src, int max_side)
{
    if (src.empty()) return src;
    int big = std::max(src.width, src.height);
    if (big <= max_side) return src;
    double f = static_cast<double>(max_side) / big;
    int nw = std::max(1, static_cast<int>(src.width * f));
    int nh = std::max(1, static_cast<int>(src.height * f));
    Image out;
    out.alloc(nw, nh, src.channels);
    out.dpi = static_cast<int>(src.dpi * f + 0.5);
    out.bilevel = false;
    const int c = src.channels;
    std::vector<int> xs(nw + 1);
    for (int x = 0; x <= nw; ++x) xs[x] = std::min(src.width, static_cast<int>(static_cast<long long>(x) * src.width / nw));
    std::vector<unsigned> acc(static_cast<size_t>(nw) * c);
    for (int y = 0; y < nh; ++y) {
        int y0 = static_cast<int>(static_cast<long long>(y) * src.height / nh);
        int y1 = std::max(y0 + 1, static_cast<int>(static_cast<long long>(y + 1) * src.height / nh));
        std::fill(acc.begin(), acc.end(), 0u);
        for (int sy = y0; sy < y1; ++sy) {
            const std::uint8_t* in = src.row(sy);
            for (int x = 0; x < nw; ++x) {
                int x1 = std::max(xs[x] + 1, xs[x + 1]);
                for (int sx = xs[x]; sx < x1; ++sx)
                    for (int k = 0; k < c; ++k) acc[x * c + k] += in[sx * c + k];
            }
        }
        std::uint8_t* out_row = out.row(y);
        for (int x = 0; x < nw; ++x) {
            unsigned cnt = static_cast<unsigned>((y1 - y0) * (std::max(xs[x] + 1, xs[x + 1]) - xs[x]));
            for (int k = 0; k < c; ++k) out_row[x * c + k] = static_cast<std::uint8_t>(acc[x * c + k] / cnt);
        }
    }
    return out;
}

Image crop(const Image& src, int x, int y, int w, int h)
{
    x = std::clamp(x, 0, std::max(0, src.width - 1));
    y = std::clamp(y, 0, std::max(0, src.height - 1));
    w = std::clamp(w, 1, src.width - x);
    h = std::clamp(h, 1, src.height - y);
    Image out;
    out.alloc(w, h, src.channels);
    out.dpi = src.dpi;
    out.bilevel = src.bilevel;
    for (int r = 0; r < h; ++r)
        std::memcpy(out.row(r), src.row(y + r) + static_cast<size_t>(x) * src.channels, out.stride());
    return out;
}

Image apply_adjustments(const Image& src_in, const Adjustments& a, double scale_hint)
{
    if (src_in.empty()) return src_in;
    Image img = (a.grayscale || a.black_white) ? to_gray(src_in) : src_in;
    const int c = img.channels;
    const size_t n = img.data.size();

    // 1. noise reduction / descreen
    if (a.denoise > 0) {
        int r = std::max(1, static_cast<int>(std::lround(2.0 * scale_hint)));
        Image blur = box_blur(img, r, 2);
        double amt = a.denoise / 100.0;
        for (size_t i = 0; i < n; ++i)
            img.data[i] = clamp8(img.data[i] + (blur.data[i] - img.data[i]) * amt);
    }

    // 2. tone curve: levels -> gamma -> contrast -> brightness
    double lo = 0, hi = 255;
    if (a.auto_levels) {
        unsigned long long hist[256] = {};
        if (c == 3) {
            for (size_t i = 0; i + 2 < n; i += 3)
                hist[(img.data[i] * 77 + img.data[i + 1] * 150 + img.data[i + 2] * 29) >> 8]++;
        } else {
            for (size_t i = 0; i < n; ++i) hist[img.data[i]]++;
        }
        unsigned long long total = n / c, cut = total / 200, s = 0;
        int l = 0, h = 255;
        for (s = 0, l = 0; l < 255 && s + hist[l] <= cut; ++l) s += hist[l];
        for (s = 0, h = 255; h > 0 && s + hist[h] <= cut; --h) s += hist[h];
        if (h - l >= 16) { lo = l; hi = h; }
    }
    const double gamma = std::clamp(a.gamma, 30, 300) / 100.0;
    const double cf = a.contrast >= 0 ? 1.0 + a.contrast / 100.0 * 2.0 : 1.0 + a.contrast / 100.0;
    const double bo = a.brightness * 1.28;
    std::uint8_t lut[256];
    for (int v = 0; v < 256; ++v) {
        double x = (v - lo) / (hi - lo);
        x = std::clamp(x, 0.0, 1.0);
        x = std::pow(x, 1.0 / gamma) * 255.0;
        x = (x - 127.5) * cf + 127.5 + bo;
        lut[v] = clamp8(x);
    }
    for (size_t i = 0; i < n; ++i) img.data[i] = lut[img.data[i]];

    // 3. saturation
    if (c == 3 && a.saturation != 0) {
        double s = 1.0 + a.saturation / 100.0;
        for (size_t i = 0; i + 2 < n; i += 3) {
            double l = 0.299 * img.data[i] + 0.587 * img.data[i + 1] + 0.114 * img.data[i + 2];
            for (int k = 0; k < 3; ++k) img.data[i + k] = clamp8(l + (img.data[i + k] - l) * s);
        }
    }

    // 4. sharpen (unsharp mask)
    if (a.sharpness > 0) {
        int r = std::max(1, static_cast<int>(std::lround(1.5 * scale_hint)));
        Image blur = box_blur(img, r, 1);
        double amt = a.sharpness / 100.0 * 1.5;
        for (size_t i = 0; i < n; ++i)
            img.data[i] = clamp8(img.data[i] + (img.data[i] - blur.data[i]) * amt);
    }

    // 5. black & white
    if (a.black_white) {
        const int t = std::clamp(a.threshold, 1, 254);
        for (size_t i = 0; i < n; ++i) img.data[i] = img.data[i] >= t ? 255 : 0;
        img.bilevel = true;
    } else {
        img.bilevel = false;
    }

    // 6. invert
    if (a.invert)
        for (size_t i = 0; i < n; ++i) img.data[i] = static_cast<std::uint8_t>(255 - img.data[i]);

    // 7. orientation
    return rotate(img, a.rotation, a.flip_h);
}

bool detect_content(const Image& p, int& bx, int& by, int& bw, int& bh)
{
    if (p.empty()) return false;
    const int w = p.width, h = p.height, c = p.channels;
    std::vector<int> cols(w, 0), rows(h, 0);
    for (int y = 0; y < h; ++y) {
        const std::uint8_t* r = p.row(y);
        for (int x = 0; x < w; ++x) {
            int lum, chroma = 0;
            if (c == 3) {
                int R = r[x * 3], G = r[x * 3 + 1], B = r[x * 3 + 2];
                lum = (R * 77 + G * 150 + B * 29) >> 8;
                chroma = std::max({R, G, B}) - std::min({R, G, B});
            } else {
                lum = r[x];
            }
            if (lum < 215 || chroma > 40) { cols[x]++; rows[y]++; }
        }
    }
    int min_col = std::max(2, h / 100), min_row = std::max(2, w / 100);
    int x0 = 0, x1 = w - 1, y0 = 0, y1 = h - 1;
    while (x0 < w && cols[x0] < min_col) ++x0;
    while (x1 > x0 && cols[x1] < min_col) --x1;
    while (y0 < h && rows[y0] < min_row) ++y0;
    while (y1 > y0 && rows[y1] < min_row) --y1;
    if (x0 >= x1 || y0 >= y1) return false;
    int pad = std::max(1, w / 150);
    bx = std::max(0, x0 - pad);
    by = std::max(0, y0 - pad);
    bw = std::min(w - bx, x1 - x0 + 1 + 2 * pad);
    bh = std::min(h - by, y1 - y0 + 1 + 2 * pad);
    return bw > w / 20 && bh > h / 20;
}
