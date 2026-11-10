// LiDE50 Scan - tests for the file encoders and the image adjustments.
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
//
// Runs on the build machine ("make test"), built with AddressSanitizer and UBSan.
// Every format is written and then read back with a small reader written here, so a
// change that breaks a file format or touches memory it shouldn't fails the build.

#include "encoders.h"
#include "process.h"

#include "../third_party/miniz.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<unsigned char>;
int g_failed = 0, g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failed;                                                                \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

unsigned g_seed = 20261110;
unsigned rnd()
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 16) & 0x7fff;
}

// kind 0: noise, 1: smooth gradient, 2: black & white pattern, 3: mixed
Image make_image(int w, int h, int c, int kind, int dpi = 300)
{
    Image im;
    im.alloc(w, h, c);
    im.dpi = dpi;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w * c; ++x) {
            std::uint8_t v;
            switch (kind) {
                case 0: v = static_cast<std::uint8_t>(rnd()); break;
                case 1: v = static_cast<std::uint8_t>((x * 3 + y * 5) / 7); break;
                case 2: v = (((x / 5) + (y / 3)) % 2) ? 255 : 0; break;
                default: v = (y / 16) % 2 ? static_cast<std::uint8_t>(rnd()) : static_cast<std::uint8_t>(x + y); break;
            }
            im.row(y)[x] = v;
        }
    if (kind == 2 && c == 1) im.bilevel = true;
    return im;
}

std::uint32_t be32(const unsigned char* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
std::uint32_t le32(const unsigned char* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
std::uint16_t le16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

Bytes inflate(const unsigned char* data, size_t len, size_t expect)
{
    Bytes out(expect + 16);
    mz_ulong n = static_cast<mz_ulong>(out.size());
    if (mz_uncompress(out.data(), &n, data, static_cast<mz_ulong>(len)) != MZ_OK) return {};
    out.resize(n);
    return out;
}

// 8-bit pixels of a bilevel image after packing to 1 bit and back
Bytes unpack_bits(const Bytes& packed, int w, int h)
{
    const size_t rb = (static_cast<size_t>(w) + 7) / 8;
    Bytes out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) out[static_cast<size_t>(y) * w + x] = (packed[rb * y + (x >> 3)] & (0x80 >> (x & 7))) ? 255 : 0;
    return out;
}

// ------------------------------------------------------------------- PNG

bool decode_png(const Bytes& f, Image& out)
{
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (f.size() < 8 || std::memcmp(f.data(), sig, 8) != 0) return false;
    size_t at = 8;
    int w = 0, h = 0, depth = 0, type = 0;
    Bytes idat;
    bool end = false;
    while (at + 12 <= f.size() && !end) {
        std::uint32_t len = be32(&f[at]);
        if (at + 12 + len > f.size()) return false;
        const unsigned char* body = &f[at + 8];
        if (mz_crc32(MZ_CRC32_INIT, &f[at + 4], len + 4) != be32(body + len)) return false;
        if (!std::memcmp(&f[at + 4], "IHDR", 4)) {
            w = static_cast<int>(be32(body)); h = static_cast<int>(be32(body + 4));
            depth = body[8]; type = body[9];
        } else if (!std::memcmp(&f[at + 4], "pHYs", 4)) {
            out.dpi = static_cast<int>(be32(body) * 0.0254 + 0.5);
        } else if (!std::memcmp(&f[at + 4], "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(&f[at + 4], "IEND", 4)) {
            end = true;
        }
        at += 12 + len;
    }
    if (!end || at != f.size() || w <= 0 || h <= 0 || idat.empty()) return false;
    const int ch = type == 2 ? 3 : 1;
    const size_t stride = depth == 1 ? (static_cast<size_t>(w) + 7) / 8 : static_cast<size_t>(w) * ch;
    const int bpp = depth == 1 ? 1 : ch;
    Bytes raw = inflate(idat.data(), idat.size(), (stride + 1) * h);
    if (raw.size() != (stride + 1) * h) return false;
    Bytes px(stride * h);
    for (int y = 0; y < h; ++y) {
        const unsigned char* in = &raw[(stride + 1) * y];
        unsigned char* cur = &px[stride * y];
        const unsigned char* prev = y ? &px[stride * (y - 1)] : nullptr;
        for (size_t i = 0; i < stride; ++i) {
            int a = i >= static_cast<size_t>(bpp) ? cur[i - bpp] : 0, b = prev ? prev[i] : 0;
            int c = (prev && i >= static_cast<size_t>(bpp)) ? prev[i - bpp] : 0, pred = 0;
            switch (in[0]) {
                case 0: break;
                case 1: pred = a; break;
                case 2: pred = b; break;
                case 3: pred = (a + b) / 2; break;
                case 4: {
                    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    break;
                }
                default: return false;
            }
            cur[i] = static_cast<unsigned char>(in[1 + i] + pred);
        }
    }
    out.width = w; out.height = h; out.channels = ch;
    out.data = depth == 1 ? unpack_bits(px, w, h) : px;
    return true;
}

// ------------------------------------------------------------------ TIFF

bool lzw_decode(const unsigned char* in, size_t len, Bytes& out)
{
    struct Entry { int prefix; unsigned char last; };
    std::vector<Entry> table(4096);
    std::vector<unsigned char> stack;
    int next = 258, nbits = 9, prev = -1;
    std::uint32_t buf = 0;
    int cnt = 0;
    size_t at = 0;
    auto first_of = [&](int code) { while (code >= 258) code = table[code].prefix; return static_cast<unsigned char>(code); };
    for (;;) {
        while (cnt < nbits) {
            if (at >= len) return false;
            buf = (buf << 8) | in[at++];
            cnt += 8;
        }
        int code = static_cast<int>((buf >> (cnt - nbits)) & ((1u << nbits) - 1));
        cnt -= nbits;
        if (code == 257) return true;
        if (code == 256) { next = 258; nbits = 9; prev = -1; continue; }
        if (prev < 0) {
            if (code > 255) return false;
            out.push_back(static_cast<unsigned char>(code));
        } else {
            unsigned char first;
            int walk = code;
            if (code > next) return false;
            if (code == next) { first = first_of(prev); stack.push_back(first); walk = prev; }
            else first = first_of(code);
            for (; walk >= 258; walk = table[walk].prefix) stack.push_back(table[walk].last);
            stack.push_back(static_cast<unsigned char>(walk));
            while (!stack.empty()) { out.push_back(stack.back()); stack.pop_back(); }
            if (next < 4096) { table[next] = {prev, first}; ++next; }
        }
        prev = code;
        // TIFF switches one code early
        if (next + 1 > (1 << nbits) - 1 && nbits < 12) ++nbits;
    }
}

bool decode_tiff(const Bytes& f, std::vector<Image>& pages)
{
    if (f.size() < 8 || f[0] != 'I' || f[1] != 'I' || le16(&f[2]) != 42) return false;
    std::uint32_t ifd = le32(&f[4]);
    while (ifd) {
        if (ifd + 2 > f.size()) return false;
        int n = le16(&f[ifd]);
        if (ifd + 2 + 12ull * n + 4 > f.size()) return false;
        std::uint32_t w = 0, h = 0, comp = 0, spp = 1, rps = 0, pred = 1, bits = 8, strips = 0, off_val = 0, cnt_val = 0,
                      res_at = 0, last_tag = 0;
        for (int i = 0; i < n; ++i) {
            const unsigned char* e = &f[ifd + 2 + 12 * i];
            std::uint32_t tag = le16(e), type = le16(e + 2), count = le32(e + 4);
            std::uint32_t val = (type == 3 && count == 1) ? le16(e + 8) : le32(e + 8);
            if (tag <= last_tag) return false;   // tags must be in ascending order
            last_tag = tag;
            switch (tag) {
                case 256: w = val; break;
                case 257: h = val; break;
                case 258: bits = count == 1 ? val : le16(&f[val]); break;
                case 259: comp = val; break;
                case 273: strips = count; off_val = val; break;
                case 277: spp = val; break;
                case 278: rps = val; break;
                case 279: cnt_val = val; break;
                case 282: res_at = val; break;
                case 317: pred = val; break;
            }
        }
        if (comp != 5 || !w || !h || !rps || !strips) return false;
        Image im;
        im.width = static_cast<int>(w); im.height = static_cast<int>(h); im.channels = static_cast<int>(spp);
        im.dpi = res_at ? static_cast<int>(le32(&f[res_at]) / le32(&f[res_at + 4])) : 0;
        const size_t row = bits == 1 ? (w + 7) / 8 : static_cast<size_t>(w) * spp;
        Bytes px;
        for (std::uint32_t s = 0; s < strips; ++s) {
            std::uint32_t off = strips == 1 ? off_val : le32(&f[off_val + 4 * s]);
            std::uint32_t cnt = strips == 1 ? cnt_val : le32(&f[cnt_val + 4 * s]);
            if (static_cast<size_t>(off) + cnt > f.size()) return false;
            Bytes strip;
            if (!lzw_decode(&f[off], cnt, strip)) return false;
            if (strip.size() % row) return false;
            if (pred == 2)
                for (size_t y = 0; y < strip.size() / row; ++y)
                    for (size_t i = spp; i < row; ++i) strip[y * row + i] = static_cast<unsigned char>(strip[y * row + i] + strip[y * row + i - spp]);
            px.insert(px.end(), strip.begin(), strip.end());
        }
        if (px.size() != row * h) return false;
        im.data = bits == 1 ? unpack_bits(px, im.width, im.height) : px;
        pages.push_back(std::move(im));
        ifd = le32(&f[ifd + 2 + 12 * n]);
    }
    return true;
}

// ------------------------------------------------------------------- PDF

size_t find(const Bytes& f, const std::string& s, size_t from = 0)
{
    auto it = std::search(f.begin() + static_cast<long>(std::min(from, f.size())), f.end(), s.begin(), s.end());
    return it == f.end() ? std::string::npos : static_cast<size_t>(it - f.begin());
}

// checks the cross-reference table: every entry must point at "<n> 0 obj"
bool check_pdf_xref(const Bytes& f, size_t expect_objects)
{
    if (find(f, "%PDF-1.4") != 0) return false;
    size_t sx = find(f, "startxref\n");
    if (sx == std::string::npos) return false;
    size_t xref = std::strtoull(reinterpret_cast<const char*>(&f[sx + 10]), nullptr, 10);
    if (find(f, "xref\n0 ", xref) != xref) return false;
    size_t count = std::strtoull(reinterpret_cast<const char*>(&f[xref + 7]), nullptr, 10);
    if (count != expect_objects + 1) return false;
    size_t line = find(f, "\n", xref + 5) + 1;
    for (size_t i = 0; i < count; ++i, line += 20) {
        if (line + 20 > f.size()) return false;
        if (i == 0) continue;
        size_t off = std::strtoull(reinterpret_cast<const char*>(&f[line]), nullptr, 10);
        std::string head = std::to_string(i) + " 0 obj\n";
        if (find(f, head, off) != off) return false;
    }
    return find(f, "%%EOF") != std::string::npos;
}

// ---------------------------------------------------------------- checks

void test_png()
{
    for (int kind = 0; kind < 4; ++kind)
        for (int c : {1, 3}) {
            Image im = make_image(37 + kind * 61, 23 + kind * 40, c, kind, 600);
            Bytes f = encode_png(im);
            Image back;
            CHECK(decode_png(f, back));
            CHECK(back.width == im.width && back.height == im.height && back.channels == im.channels);
            CHECK(back.data == im.data);
            CHECK(back.dpi == 600);
        }
    Image one = make_image(1, 1, 3, 0);
    Image back;
    CHECK(decode_png(encode_png(one), back) && back.data == one.data);
}

void test_tiff()
{
    Image a = make_image(700, 300, 3, 3, 300), b = make_image(1237, 133, 1, 1, 600), c = make_image(1001, 201, 1, 2, 150),
          d = make_image(900, 700, 3, 0, 1200);   // noise: the LZW table fills up and is cleared many times
    std::vector<const Image*> all = {&a, &b, &c, &d};
    std::vector<Image> back;
    CHECK(decode_tiff(encode_tiff(all), back));
    CHECK(back.size() == all.size());
    for (size_t i = 0; i < back.size() && i < all.size(); ++i) {
        CHECK(back[i].width == all[i]->width && back[i].height == all[i]->height && back[i].channels == all[i]->channels);
        CHECK(back[i].data == all[i]->data);
        CHECK(back[i].dpi == all[i]->dpi);
    }
    std::vector<Image> single;
    CHECK(decode_tiff(encode_tiff({&b}), single) && single.size() == 1 && single[0].data == b.data);
}

void test_pdf()
{
    Image a = make_image(300, 200, 3, 1, 300), b = make_image(211, 97, 1, 2, 300), c = make_image(120, 80, 1, 1, 150);
    SaveOptions lossless;
    lossless.pdf_lossless = true;
    Bytes f = encode_pdf({&a, &b, &c}, lossless);
    CHECK(check_pdf_xref(f, 2 + 3 * 3 + 1));
    // the first image is stored with Flate: it must unpack to exactly the pixels
    size_t img = find(f, "/Subtype /Image /Width 300 /Height 200");
    CHECK(img != std::string::npos);
    if (img != std::string::npos) {
        size_t len_at = find(f, "/Length ", img), stream = find(f, "stream\n", img);
        size_t len = std::strtoull(reinterpret_cast<const char*>(&f[len_at + 8]), nullptr, 10);
        CHECK(len > 0);
        CHECK(inflate(&f[stream + 7], len, a.data.size()) == a.data);
    }
    SaveOptions jpeg;
    Bytes g = encode_pdf({&a, &c}, jpeg);
    CHECK(check_pdf_xref(g, 2 + 2 * 3 + 1));
    CHECK(find(g, "/DCTDecode") != std::string::npos);
}

void test_docx()
{
    Image a = make_image(300, 200, 3, 1, 300), b = make_image(211, 97, 1, 2, 300);
    SaveOptions opt;
    for (bool letter : {false, true}) {
        Bytes f = encode_docx({&a, &b}, opt, letter);
        mz_zip_archive zip;
        std::memset(&zip, 0, sizeof(zip));
        CHECK(mz_zip_reader_init_mem(&zip, f.data(), f.size(), 0));
        CHECK(mz_zip_reader_get_num_files(&zip) == 7);
        CHECK(mz_zip_validate_archive(&zip, 0));
        for (const char* name : {"[Content_Types].xml", "_rels/.rels", "docProps/app.xml", "word/_rels/document.xml.rels",
                                 "word/document.xml", "word/media/image1.jpeg", "word/media/image2.png"})
            CHECK(mz_zip_reader_locate_file(&zip, name, nullptr, 0) >= 0);
        size_t n = 0;
        void* p = mz_zip_reader_extract_file_to_heap(&zip, "word/media/image2.png", &n, 0);
        CHECK(p != nullptr);
        if (p) {
            Image back;
            CHECK(decode_png(Bytes(static_cast<unsigned char*>(p), static_cast<unsigned char*>(p) + n), back) &&
                  back.data == b.data);
            mz_free(p);
        }
        p = mz_zip_reader_extract_file_to_heap(&zip, "word/document.xml", &n, 0);
        CHECK(p != nullptr);
        if (p) {
            std::string xml(static_cast<char*>(p), n);
            CHECK(xml.find(letter ? "w:w=\"12240\"" : "w:w=\"11906\"") != std::string::npos);
            mz_free(p);
        }
        mz_zip_reader_end(&zip);
    }
}

void test_jpeg_bmp()
{
    Image a = make_image(123, 77, 3, 1, 1200), g = make_image(50, 31, 1, 1, 75);
    Bytes j = encode_jpeg(a, 90);
    CHECK(j.size() > 100 && j[0] == 0xFF && j[1] == 0xD8 && j[j.size() - 2] == 0xFF && j[j.size() - 1] == 0xD9);
    CHECK(j[13] == 1 && ((j[14] << 8) | j[15]) == 1200 && ((j[16] << 8) | j[17]) == 1200);
    CHECK(encode_jpeg(g, 30).size() > 100);
    Bytes b = encode_bmp(a);
    CHECK(b.size() > 54 && b[0] == 'B' && b[1] == 'M' && le32(&b[2]) == b.size());
    CHECK(le32(&b[18]) == 123 && le32(&b[22]) == 77 && le32(&b[38]) == 47244);
    const size_t off = le32(&b[10]), row = (123 * 3 + 3) / 4 * 4;
    bool same = b.size() == off + row * 77;
    for (int y = 0; same && y < 77; ++y)
        for (int x = 0; x < 123; ++x) {
            const unsigned char* p = &b[off + row * (76 - y) + x * 3];   // bottom-up, BGR
            const unsigned char* s = a.row(y) + x * 3;
            if (p[0] != s[2] || p[1] != s[1] || p[2] != s[0]) { same = false; break; }
        }
    CHECK(same);
}

// the blur the adjustments use, written the slow and obvious way
Image reference_blur(const Image& src, int r, int passes)
{
    Image a = src, b = src;
    const int w = src.width, h = src.height, c = src.channels, win = 2 * r + 1;
    for (int p = 0; p < passes; ++p) {
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int k = 0; k < c; ++k) {
                    int s = 0;
                    for (int i = -r; i <= r; ++i) s += a.row(y)[std::clamp(x + i, 0, w - 1) * c + k];
                    b.row(y)[x * c + k] = static_cast<std::uint8_t>(s / win);
                }
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w * c; ++x) {
                int s = 0;
                for (int i = -r; i <= r; ++i) s += b.row(std::clamp(y + i, 0, h - 1))[x];
                a.row(y)[x] = static_cast<std::uint8_t>(s / win);
            }
    }
    return a;
}

void test_process()
{
    // full noise reduction with everything else neutral is exactly the blur
    for (int c : {1, 3})
        for (double hint : {0.4, 1.0, 2.6}) {
            Image im = make_image(61, 47, c, 3);
            Adjustments a;
            a.denoise = 100;
            int r = std::max(1, static_cast<int>(2.0 * hint + 0.5));
            CHECK(apply_adjustments(im, a, hint).data == reference_blur(im, r, 2).data);
        }
    // neutral settings change nothing; four quarter turns and two mirrors come back to the start
    Image im = make_image(53, 31, 3, 0);
    Adjustments none;
    CHECK(apply_adjustments(im, none).data == im.data);
    Adjustments turn;
    turn.rotation = 90;
    Image t = im;
    for (int i = 0; i < 4; ++i) t = apply_adjustments(t, turn);
    CHECK(t.width == im.width && t.data == im.data);
    Image q = apply_adjustments(im, turn);
    CHECK(q.width == im.height && q.height == im.width);
    Adjustments mirror;
    mirror.flip_h = true;
    CHECK(apply_adjustments(apply_adjustments(im, mirror), mirror).data == im.data);
    Adjustments bw;
    bw.black_white = true;
    Image k = apply_adjustments(im, bw);
    CHECK(k.channels == 1 && k.bilevel);
    for (auto v : k.data) if (v != 0 && v != 255) { CHECK(false); break; }

    // random settings and sizes (including 1-pixel images): sizes must be consistent and the
    // sanitizers must stay quiet
    for (int it = 0; it < 300; ++it) {
        int w = 1 + rnd() % (it % 7 == 0 ? 4 : 220), h = 1 + rnd() % (it % 5 == 0 ? 4 : 220), c = (rnd() & 1) ? 3 : 1;
        Image src = make_image(w, h, c, static_cast<int>(rnd() % 4), 75 + static_cast<int>(rnd() % 2400));
        src.bilevel = false;
        Adjustments a;
        a.brightness = static_cast<int>(rnd() % 201) - 100; a.contrast = static_cast<int>(rnd() % 201) - 100;
        a.saturation = static_cast<int>(rnd() % 201) - 100; a.gamma = 30 + static_cast<int>(rnd() % 271);
        a.sharpness = static_cast<int>(rnd() % 101); a.threshold = 1 + static_cast<int>(rnd() % 254);
        a.denoise = static_cast<int>(rnd() % 101); a.auto_levels = rnd() & 1; a.invert = rnd() & 1;
        a.black_white = rnd() % 4 == 0; a.grayscale = rnd() % 3 == 0; a.rotation = static_cast<int>(rnd() % 4) * 90;
        a.flip_h = rnd() & 1;
        Image out = apply_adjustments(src, a, (rnd() % 300) / 100.0 + 0.01);
        CHECK(out.data.size() == out.stride() * static_cast<size_t>(out.height));
        Image small = downscale(out, 1 + static_cast<int>(rnd() % 150));
        CHECK(!small.empty() && small.data.size() == small.stride() * static_cast<size_t>(small.height));
        int x, y, ww, hh;
        if (detect_content(src, x, y, ww, hh)) CHECK(x >= 0 && y >= 0 && x + ww <= src.width && y + hh <= src.height);
        Image back;
        CHECK(decode_png(encode_png(out), back) && back.data == out.data);
    }
}

} // namespace

int main()
{
    test_png();
    test_tiff();
    test_pdf();
    test_docx();
    test_jpeg_bmp();
    test_process();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
