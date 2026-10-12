// LiDE50 Scan - output file writers.
#pragma once
#include "image.h"

#include <string>
#include <vector>

enum class OutFormat { PDF, DOCX, PNG, JPEG, TIFF, BMP };

struct SaveOptions {
    int jpeg_quality = 90;      // JPEG and PDF/DOCX color pages
    bool pdf_lossless = false;  // PDF: Flate instead of JPEG for color/gray
};

const wchar_t* format_extension(OutFormat f);          // e.g. L"pdf"
const wchar_t* format_filter(OutFormat f);             // file dialog filter (double-NUL terminated)
bool format_multipage(OutFormat f);                    // PDF, DOCX, TIFF hold several pages

// Writes all pages into one file (multi-page formats) or the first page.
bool write_document(const std::wstring& path, OutFormat f, const std::vector<const Image*>& pages,
                    const SaveOptions& opt, std::wstring& error);

// Encoders exposed for tests.
std::vector<unsigned char> encode_png(const Image& img);
std::vector<unsigned char> encode_jpeg(const Image& img, int quality);
