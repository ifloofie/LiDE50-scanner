// LiDE50 Scan - output file writers.
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
#pragma once
#include "encoders.h"
#include "image.h"

#include <string>
#include <vector>

enum class OutFormat { PDF, DOCX, PNG, JPEG, TIFF, BMP };


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
