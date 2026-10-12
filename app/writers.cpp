// LiDE50 Scan - output writers: PDF, DOCX, PNG, JPEG, TIFF, BMP.
#include "writers.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "../third_party/miniz.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../third_party/stb_image_write.h"

namespace {

using Bytes = std::vector<unsigned char>;

void put_be32(Bytes& b, std::uint32_t v)
{
    b.push_back(static_cast<unsigned char>(v >> 24)); b.push_back(static_cast<unsigned char>(v >> 16));
    b.push_back(static_cast<unsigned char>(v >> 8)); b.push_back(static_cast<unsigned char>(v));
}
void put_le16(Bytes& b, std::uint16_t v)
{
    b.push_back(static_cast<unsigned char>(v)); b.push_back(static_cast<unsigned char>(v >> 8));
}
void put_le32(Bytes& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
void set_le32(Bytes& b, size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<unsigned char>(v >> (8 * i));
}
void append(Bytes& b, const std::string& s) { b.insert(b.end(), s.begin(), s.end()); }

bool write_file(const std::wstring& path, const Bytes& data, std::wstring& error)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) { error = L"Could not create the file. Is it open in another program?"; return false; }
    size_t w = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    bool ok = w == data.size() && std::fclose(f) == 0;
    if (!ok) error = L"Could not write the file (disk full?).";
    return ok;
}

Bytes zlib_compress(const unsigned char* data, size_t len, int level = 6)
{
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(len));
    Bytes out(bound);
    mz_ulong out_len = bound;
    if (mz_compress2(out.data(), &out_len, data, static_cast<mz_ulong>(len), level) != MZ_OK) return {};
    out.resize(out_len);
    return out;
}

// Packs a bilevel gray image to 1 bit per pixel, 1 = white, rows byte-aligned.
Bytes pack_bits(const Image& img)
{
    const size_t rb = (static_cast<size_t>(img.width) + 7) / 8;
    Bytes out(rb * img.height, 0);
    for (int y = 0; y < img.height; ++y) {
        const std::uint8_t* r = img.row(y);
        unsigned char* o = &out[rb * y];
        for (int x = 0; x < img.width; ++x)
            if (r[x] >= 128) o[x >> 3] |= static_cast<unsigned char>(0x80 >> (x & 7));
    }
    return out;
}

// ------------------------------------------------------------------ PNG

void png_chunk(Bytes& out, const char* type, const Bytes& data)
{
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, &out[start], data.size() + 4);
    put_be32(out, static_cast<std::uint32_t>(crc));
}

} // namespace

std::vector<unsigned char> encode_png(const Image& img)
{
    Bytes raw;
    int bit_depth = 8, color_type = img.channels == 3 ? 2 : 0;
    if (img.bilevel && img.channels == 1) {
        bit_depth = 1;
        Bytes packed = pack_bits(img);
        const size_t rb = (static_cast<size_t>(img.width) + 7) / 8;
        raw.reserve((rb + 1) * img.height);
        for (int y = 0; y < img.height; ++y) {
            raw.push_back(0);
            raw.insert(raw.end(), packed.begin() + rb * y, packed.begin() + rb * (y + 1));
        }
    } else {
        // adaptive filtering: pick the filter with the smallest sum of |residuals|
        const size_t stride = img.stride();
        const int bpp = img.channels;
        raw.reserve((stride + 1) * img.height);
        Bytes cand[5];
        for (auto& c : cand) c.resize(stride);
        Bytes zero(stride, 0);
        for (int y = 0; y < img.height; ++y) {
            const std::uint8_t* cur = img.row(y);
            const std::uint8_t* prev = y > 0 ? img.row(y - 1) : zero.data();
            size_t best = 0;
            unsigned long long best_sum = ~0ULL;
            for (int f = 0; f < 5; ++f) {
                unsigned long long sum = 0;
                for (size_t i = 0; i < stride; ++i) {
                    int a = i >= static_cast<size_t>(bpp) ? cur[i - bpp] : 0;
                    int b = prev[i];
                    int c = i >= static_cast<size_t>(bpp) ? prev[i - bpp] : 0;
                    int pred = 0;
                    switch (f) {
                        case 1: pred = a; break;
                        case 2: pred = b; break;
                        case 3: pred = (a + b) / 2; break;
                        case 4: {
                            int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                            pred = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                            break;
                        }
                    }
                    unsigned char v = static_cast<unsigned char>(cur[i] - pred);
                    cand[f][i] = v;
                    sum += v < 128 ? v : 256 - v;
                }
                if (sum < best_sum) { best_sum = sum; best = static_cast<size_t>(f); }
            }
            raw.push_back(static_cast<unsigned char>(best));
            raw.insert(raw.end(), cand[best].begin(), cand[best].end());
        }
    }

    Bytes out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    Bytes ihdr;
    put_be32(ihdr, static_cast<std::uint32_t>(img.width));
    put_be32(ihdr, static_cast<std::uint32_t>(img.height));
    ihdr.push_back(static_cast<unsigned char>(bit_depth));
    ihdr.push_back(static_cast<unsigned char>(color_type));
    ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    png_chunk(out, "IHDR", ihdr);
    if (img.dpi > 0) {
        Bytes phys;
        std::uint32_t ppm = static_cast<std::uint32_t>(img.dpi / 0.0254 + 0.5);
        put_be32(phys, ppm); put_be32(phys, ppm); phys.push_back(1);
        png_chunk(out, "pHYs", phys);
    }
    png_chunk(out, "IDAT", zlib_compress(raw.data(), raw.size(), 6));
    png_chunk(out, "IEND", {});
    return out;
}

// ----------------------------------------------------------------- JPEG

namespace {
void stb_to_vector(void* ctx, void* data, int size)
{
    auto* v = static_cast<Bytes*>(ctx);
    auto* p = static_cast<unsigned char*>(data);
    v->insert(v->end(), p, p + size);
}
} // namespace

std::vector<unsigned char> encode_jpeg(const Image& img, int quality)
{
    Bytes out;
    stbi_write_jpg_to_func(stb_to_vector, &out, img.width, img.height, img.channels, img.data.data(),
                           std::clamp(quality, 10, 100));
    // stamp the resolution into the JFIF header (units = dots per inch)
    if (out.size() > 20 && img.dpi > 0 && std::memcmp(&out[6], "JFIF", 4) == 0) {
        out[13] = 1;
        out[14] = static_cast<unsigned char>(img.dpi >> 8); out[15] = static_cast<unsigned char>(img.dpi);
        out[16] = static_cast<unsigned char>(img.dpi >> 8); out[17] = static_cast<unsigned char>(img.dpi);
    }
    return out;
}

namespace {

// ------------------------------------------------------------------ BMP

Bytes encode_bmp(const Image& img)
{
    Bytes out;
    stbi_write_bmp_to_func(stb_to_vector, &out, img.width, img.height, img.channels, img.data.data());
    // stb leaves the resolution fields at zero; fill them in (pixels per metre)
    if (out.size() > 46 && img.dpi > 0) {
        std::uint32_t ppm = static_cast<std::uint32_t>(img.dpi / 0.0254 + 0.5);
        set_le32(out, 38, ppm);
        set_le32(out, 42, ppm);
    }
    return out;
}

// ----------------------------------------------------------------- TIFF

class LzwEncoder {
public:
    Bytes encode(const unsigned char* data, size_t len)
    {
        out_.clear(); bitbuf_ = 0; bitcnt_ = 0;
        reset();
        put(kClear);
        if (len == 0) { put(kEoi); flush(); return out_; }
        int w = data[0];
        for (size_t i = 1; i < len; ++i) {
            int k = data[i];
            int code = lookup(w, k);
            if (code >= 0) { w = code; continue; }
            put(w);
            insert(w, k, next_++);
            w = k;
            if (next_ == 4094) { put(kClear); reset(); }
            else if (next_ > maxcode_) { ++nbits_; maxcode_ = (1 << nbits_) - 1; }
        }
        put(w);
        ++next_;
        if (next_ == 4094) { put(kClear); reset(); }
        else if (next_ > maxcode_) { ++nbits_; maxcode_ = (1 << nbits_) - 1; }
        put(kEoi);
        flush();
        return out_;
    }

private:
    static constexpr int kClear = 256, kEoi = 257, kHash = 8191;
    struct Slot { std::uint32_t gen = 0; int key = 0; int code = 0; };
    std::vector<Slot> table_ = std::vector<Slot>(kHash);
    std::uint32_t gen_ = 0;
    int next_ = 258, nbits_ = 9, maxcode_ = 511;
    Bytes out_;
    std::uint32_t bitbuf_ = 0;
    int bitcnt_ = 0;

    void reset() { ++gen_; next_ = 258; nbits_ = 9; maxcode_ = 511; }
    int lookup(int w, int k)
    {
        int key = (w << 8) | k;
        unsigned h = static_cast<unsigned>(key * 2654435761u) % kHash;
        while (table_[h].gen == gen_) {
            if (table_[h].key == key) return table_[h].code;
            h = (h + 1) % kHash;
        }
        return -1;
    }
    void insert(int w, int k, int code)
    {
        int key = (w << 8) | k;
        unsigned h = static_cast<unsigned>(key * 2654435761u) % kHash;
        while (table_[h].gen == gen_) h = (h + 1) % kHash;
        table_[h] = {gen_, key, code};
    }
    void put(int code)
    {
        bitbuf_ = (bitbuf_ << nbits_) | static_cast<std::uint32_t>(code);
        bitcnt_ += nbits_;
        while (bitcnt_ >= 8) {
            out_.push_back(static_cast<unsigned char>(bitbuf_ >> (bitcnt_ - 8)));
            bitcnt_ -= 8;
        }
        bitbuf_ &= (1u << bitcnt_) - 1;
    }
    void flush()
    {
        if (bitcnt_ > 0) out_.push_back(static_cast<unsigned char>(bitbuf_ << (8 - bitcnt_)));
        bitcnt_ = 0; bitbuf_ = 0;
    }
};

struct TiffEntry { std::uint16_t tag, type; std::uint32_t count, value; };

Bytes encode_tiff(const std::vector<const Image*>& pages)
{
    Bytes out = {'I', 'I', 42, 0, 0, 0, 0, 0};
    size_t next_ifd_ptr = 4;
    LzwEncoder lzw;
    for (size_t p = 0; p < pages.size(); ++p) {
        const Image& img = *pages[p];
        const bool bilevel = img.bilevel && img.channels == 1;
        const size_t row_bytes = bilevel ? (static_cast<size_t>(img.width) + 7) / 8 : img.stride();
        Bytes packed;
        if (bilevel) packed = pack_bits(img);
        const unsigned char* src = bilevel ? packed.data() : img.data.data();
        int rows_per_strip = static_cast<int>(std::max<size_t>(1, 65536 / std::max<size_t>(1, row_bytes)));
        int strips = (img.height + rows_per_strip - 1) / rows_per_strip;

        std::vector<std::uint32_t> offsets, counts;
        Bytes tmp;
        for (int s = 0; s < strips; ++s) {
            int y0 = s * rows_per_strip, y1 = std::min(img.height, y0 + rows_per_strip);
            tmp.assign(src + row_bytes * y0, src + row_bytes * y1);
            if (!bilevel) {
                // horizontal differencing predictor
                const int c = img.channels;
                for (int y = 0; y < y1 - y0; ++y) {
                    unsigned char* r = &tmp[row_bytes * y];
                    for (size_t i = row_bytes; i-- > static_cast<size_t>(c);) r[i] = static_cast<unsigned char>(r[i] - r[i - c]);
                }
            }
            Bytes enc = lzw.encode(tmp.data(), tmp.size());
            if (out.size() & 1) out.push_back(0);
            offsets.push_back(static_cast<std::uint32_t>(out.size()));
            counts.push_back(static_cast<std::uint32_t>(enc.size()));
            out.insert(out.end(), enc.begin(), enc.end());
        }
        auto put_array = [&](const std::vector<std::uint32_t>& v) -> std::uint32_t {
            if (v.size() == 1) return v[0];
            if (out.size() & 1) out.push_back(0);
            std::uint32_t at = static_cast<std::uint32_t>(out.size());
            for (auto x : v) put_le32(out, x);
            return at;
        };
        std::uint32_t off_val = put_array(offsets);
        std::uint32_t cnt_val = put_array(counts);
        if (out.size() & 1) out.push_back(0);
        std::uint32_t res_at = static_cast<std::uint32_t>(out.size());
        put_le32(out, static_cast<std::uint32_t>(std::max(1, img.dpi))); put_le32(out, 1);
        std::uint32_t bps_val = bilevel ? 1 : 8;
        if (img.channels == 3) {
            bps_val = static_cast<std::uint32_t>(out.size());
            put_le16(out, 8); put_le16(out, 8); put_le16(out, 8);
        }
        if (out.size() & 1) out.push_back(0);

        std::vector<TiffEntry> e = {
            {254, 4, 1, pages.size() > 1 ? 2u : 0u},
            {256, 4, 1, static_cast<std::uint32_t>(img.width)},
            {257, 4, 1, static_cast<std::uint32_t>(img.height)},
            {258, 3, static_cast<std::uint32_t>(img.channels), bps_val},
            {259, 3, 1, 5},                                              // LZW
            {262, 3, 1, img.channels == 3 ? 2u : 1u},                    // RGB / BlackIsZero
            {273, 4, static_cast<std::uint32_t>(strips), off_val},
            {277, 3, 1, static_cast<std::uint32_t>(img.channels)},
            {278, 4, 1, static_cast<std::uint32_t>(rows_per_strip)},
            {279, 4, static_cast<std::uint32_t>(strips), cnt_val},
            {282, 5, 1, res_at},
            {283, 5, 1, res_at},
            {284, 3, 1, 1},
            {296, 3, 1, 2},                                              // inches
            {297, 3, 2, static_cast<std::uint32_t>(p) | (static_cast<std::uint32_t>(pages.size()) << 16)},
            {317, 3, 1, bilevel ? 1u : 2u},                              // predictor
        };
        std::uint32_t ifd_at = static_cast<std::uint32_t>(out.size());
        set_le32(out, next_ifd_ptr, ifd_at);
        put_le16(out, static_cast<std::uint16_t>(e.size()));
        for (auto& x : e) {
            put_le16(out, x.tag); put_le16(out, x.type); put_le32(out, x.count);
            if (x.type == 3 && x.count == 1) { put_le16(out, static_cast<std::uint16_t>(x.value)); put_le16(out, 0); }
            else put_le32(out, x.value);
        }
        next_ifd_ptr = out.size();
        put_le32(out, 0);
    }
    return out;
}

// ------------------------------------------------------------------ PDF

Bytes encode_pdf(const std::vector<const Image*>& pages, const SaveOptions& opt)
{
    Bytes out;
    std::vector<size_t> offsets(3 + pages.size() * 3, 0);
    auto obj_begin = [&](size_t id) {
        offsets[id] = out.size();
        append(out, std::to_string(id) + " 0 obj\n");
    };
    append(out, "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n");

    // 1 = catalog, 2 = pages, then per page: page, content, image
    obj_begin(1);
    append(out, "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    obj_begin(2);
    std::string kids;
    for (size_t i = 0; i < pages.size(); ++i) kids += std::to_string(3 + i * 3) + " 0 R ";
    append(out, "<< /Type /Pages /Count " + std::to_string(pages.size()) + " /Kids [ " + kids + "] >>\nendobj\n");

    char buf[256];
    for (size_t i = 0; i < pages.size(); ++i) {
        const Image& img = *pages[i];
        const size_t page_id = 3 + i * 3, content_id = page_id + 1, image_id = page_id + 2;
        const double dpi = img.dpi > 0 ? img.dpi : 300.0;
        const double pw = img.width * 72.0 / dpi, ph = img.height * 72.0 / dpi;

        obj_begin(page_id);
        std::snprintf(buf, sizeof(buf),
                      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 %.2f %.2f] /Contents %zu 0 R "
                      "/Resources << /XObject << /Im0 %zu 0 R >> >> >>\nendobj\n",
                      pw, ph, content_id, image_id);
        append(out, buf);

        std::snprintf(buf, sizeof(buf), "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q\n", pw, ph);
        std::string content = buf;
        obj_begin(content_id);
        append(out, "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream\nendobj\n");

        Bytes data;
        std::string dict;
        if (img.bilevel && img.channels == 1) {
            Bytes packed = pack_bits(img);
            data = zlib_compress(packed.data(), packed.size(), 9);
            dict = "/ColorSpace /DeviceGray /BitsPerComponent 1 /Filter /FlateDecode";
        } else if (opt.pdf_lossless) {
            data = zlib_compress(img.data.data(), img.data.size(), 6);
            dict = std::string("/ColorSpace ") + (img.channels == 3 ? "/DeviceRGB" : "/DeviceGray") +
                   " /BitsPerComponent 8 /Filter /FlateDecode";
        } else {
            data = encode_jpeg(img, opt.jpeg_quality);
            dict = "/ColorSpace /DeviceRGB /BitsPerComponent 8 /Filter /DCTDecode";
        }
        obj_begin(image_id);
        std::snprintf(buf, sizeof(buf), "<< /Type /XObject /Subtype /Image /Width %d /Height %d ",
                      img.width, img.height);
        append(out, buf + dict + " /Length " + std::to_string(data.size()) + " >>\nstream\n");
        out.insert(out.end(), data.begin(), data.end());
        append(out, "\nendstream\nendobj\n");
    }

    size_t info_id = offsets.size();
    offsets.push_back(out.size());
    append(out, std::to_string(info_id) + " 0 obj\n<< /Producer (LiDE50 Scan) /Creator (LiDE50 Scan) >>\nendobj\n");

    size_t xref = out.size();
    append(out, "xref\n0 " + std::to_string(offsets.size()) + "\n0000000000 65535 f \n");
    for (size_t i = 1; i < offsets.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%010zu 00000 n \n", offsets[i]);
        append(out, buf);
    }
    append(out, "trailer\n<< /Size " + std::to_string(offsets.size()) + " /Root 1 0 R /Info " +
                    std::to_string(info_id) + " 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n");
    return out;
}

// ----------------------------------------------------------------- DOCX

Bytes encode_docx(const std::vector<const Image*>& pages, const SaveOptions& opt)
{
    // Page size follows the Windows measurement setting: Letter for US, else A4.
    DWORD measure = 0;
    bool letter = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE | LOCALE_RETURN_NUMBER,
                                  reinterpret_cast<LPWSTR>(&measure), sizeof(measure) / sizeof(wchar_t)) > 0 &&
                  measure == 1;
    const long page_w = letter ? 12240 : 11906, page_h = letter ? 15840 : 16838; // twips
    const long margin = 720;
    const long long max_cx = (page_w - 2 * margin) * 635LL;
    const long long max_cy = static_cast<long long>((page_h - 2 * margin) * 635LL * 0.97);

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 1 << 20)) return {};

    std::string rels, body;
    std::vector<std::pair<std::string, Bytes>> media;
    for (size_t i = 0; i < pages.size(); ++i) {
        const Image& img = *pages[i];
        bool png = img.bilevel && img.channels == 1;
        Bytes data = png ? encode_png(img) : encode_jpeg(img, opt.jpeg_quality);
        std::string name = "image" + std::to_string(i + 1) + (png ? ".png" : ".jpeg");
        media.emplace_back("word/media/" + name, std::move(data));
        std::string rid = "rIdImg" + std::to_string(i + 1);
        rels += "<Relationship Id=\"" + rid +
                "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/" +
                name + "\"/>";

        const double dpi = img.dpi > 0 ? img.dpi : 300.0;
        double cx = img.width / dpi * 914400.0, cy = img.height / dpi * 914400.0;
        double s = std::min({1.0, max_cx / cx, max_cy / cy});
        long long ecx = static_cast<long long>(cx * s), ecy = static_cast<long long>(cy * s);
        std::string id = std::to_string(i + 1);
        if (i > 0) body += "<w:p><w:r><w:br w:type=\"page\"/></w:r></w:p>";
        body += "<w:p><w:pPr><w:spacing w:before=\"0\" w:after=\"0\"/><w:jc w:val=\"center\"/></w:pPr><w:r><w:drawing>"
                "<wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                "<wp:extent cx=\"" + std::to_string(ecx) + "\" cy=\"" + std::to_string(ecy) + "\"/>"
                "<wp:docPr id=\"" + id + "\" name=\"Scan " + id + "\"/>"
                "<wp:cNvGraphicFramePr><a:graphicFrameLocks noChangeAspect=\"1\"/></wp:cNvGraphicFramePr>"
                "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"" + id + "\" name=\"" + name + "\"/><pic:cNvPicPr/></pic:nvPicPr>"
                "<pic:blipFill><a:blip r:embed=\"" + rid + "\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"" + std::to_string(ecx) + "\" cy=\"" +
                std::to_string(ecy) + "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
                "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>";
    }
    body += "<w:sectPr><w:pgSz w:w=\"" + std::to_string(page_w) + "\" w:h=\"" + std::to_string(page_h) +
            "\"/><w:pgMar w:top=\"720\" w:right=\"720\" w:bottom=\"720\" w:left=\"720\" w:header=\"0\" "
            "w:footer=\"0\" w:gutter=\"0\"/></w:sectPr>";

    const std::string content_types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Default Extension=\"png\" ContentType=\"image/png\"/>"
        "<Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
        "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
        "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
        "</Types>";
    const std::string root_rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
        "</Relationships>";
    const std::string app_xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
        "<Application>LiDE50 Scan</Application></Properties>";
    const std::string doc_rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" + rels +
        "</Relationships>";
    const std::string document =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
        "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
        "<w:body>" + body + "</w:body></w:document>";

    auto add = [&](const char* name, const std::string& s) {
        mz_zip_writer_add_mem(&zip, name, s.data(), s.size(), MZ_DEFAULT_COMPRESSION);
    };
    add("[Content_Types].xml", content_types);
    add("_rels/.rels", root_rels);
    add("docProps/app.xml", app_xml);
    add("word/_rels/document.xml.rels", doc_rels);
    add("word/document.xml", document);
    for (auto& m : media)
        mz_zip_writer_add_mem(&zip, m.first.c_str(), m.second.data(), m.second.size(), MZ_BEST_SPEED);

    void* buf = nullptr;
    size_t size = 0;
    Bytes out;
    if (mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        auto* p = static_cast<unsigned char*>(buf);
        out.assign(p, p + size);
    }
    mz_zip_writer_end(&zip);
    return out;
}

} // namespace

const wchar_t* format_extension(OutFormat f)
{
    switch (f) {
        case OutFormat::PDF: return L"pdf";
        case OutFormat::DOCX: return L"docx";
        case OutFormat::PNG: return L"png";
        case OutFormat::JPEG: return L"jpg";
        case OutFormat::TIFF: return L"tif";
        case OutFormat::BMP: return L"bmp";
    }
    return L"bin";
}

const wchar_t* format_filter(OutFormat f)
{
    switch (f) {
        case OutFormat::PDF: return L"PDF document (*.pdf)\0*.pdf\0";
        case OutFormat::DOCX: return L"Word document (*.docx)\0*.docx\0";
        case OutFormat::PNG: return L"PNG image (*.png)\0*.png\0";
        case OutFormat::JPEG: return L"JPEG image (*.jpg)\0*.jpg;*.jpeg\0";
        case OutFormat::TIFF: return L"TIFF image (*.tif)\0*.tif;*.tiff\0";
        case OutFormat::BMP: return L"Bitmap image (*.bmp)\0*.bmp\0";
    }
    return L"All files\0*.*\0";
}

bool format_multipage(OutFormat f)
{
    return f == OutFormat::PDF || f == OutFormat::DOCX || f == OutFormat::TIFF;
}

bool write_document(const std::wstring& path, OutFormat f, const std::vector<const Image*>& pages,
                    const SaveOptions& opt, std::wstring& error)
{
    if (pages.empty() || !pages[0] || pages[0]->empty()) { error = L"Nothing to save."; return false; }
    Bytes data;
    switch (f) {
        case OutFormat::PDF: data = encode_pdf(pages, opt); break;
        case OutFormat::DOCX: data = encode_docx(pages, opt); break;
        case OutFormat::TIFF: data = encode_tiff(pages); break;
        case OutFormat::PNG: data = encode_png(*pages[0]); break;
        case OutFormat::JPEG: data = encode_jpeg(*pages[0], opt.jpeg_quality); break;
        case OutFormat::BMP: data = encode_bmp(*pages[0]); break;
    }
    if (data.empty()) { error = L"Encoding failed (out of memory?)."; return false; }
    return write_file(path, data, error);
}
