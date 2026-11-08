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
// The file is written under a temporary name first and only replaces an existing file
// once it is complete, so a failed save never damages the old file.
// On failure `error` describes it and `win32_error` (if given) holds the Windows error code;
// `needed_mb`/`free_mb` are set when the disk is too full.
bool write_document(const std::wstring& path, OutFormat f, const std::vector<const Image*>& pages,
                    const SaveOptions& opt, std::wstring& error, unsigned long* win32_error = nullptr,
                    double* needed_mb = nullptr, double* free_mb = nullptr);

// Encoders exposed for tests.
std::vector<unsigned char> encode_png(const Image& img);
std::vector<unsigned char> encode_jpeg(const Image& img, int quality);
