// LiDE50 Scan - file format encoders (memory to memory).
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
#pragma once
#include "image.h"

#include <vector>

struct SaveOptions {
    int jpeg_quality = 90;      // JPEG and PDF/DOCX color pages
    bool pdf_lossless = false;  // PDF: Flate instead of JPEG for color/gray
};

// Thrown when the result would not fit the file format (for example a TIFF over 4 GB).
struct EncodeTooLarge {};

// Every encoder returns the complete file. They throw std::bad_alloc when memory runs out
// (including a failed compression step) and never return a partly written file.
std::vector<unsigned char> encode_png(const Image& img);
std::vector<unsigned char> encode_jpeg(const Image& img, int quality);
std::vector<unsigned char> encode_bmp(const Image& img);
std::vector<unsigned char> encode_tiff(const std::vector<const Image*>& pages);
std::vector<unsigned char> encode_pdf(const std::vector<const Image*>& pages, const SaveOptions& opt);
// `us_letter`: page size Letter instead of A4
std::vector<unsigned char> encode_docx(const std::vector<const Image*>& pages, const SaveOptions& opt,
                                       bool us_letter);
