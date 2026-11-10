// LiDE50 Scan - output writers: PDF, DOCX, PNG, JPEG, TIFF, BMP.
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
#include "writers.h"
#include "encoders.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

namespace {

using Bytes = std::vector<unsigned char>;

struct WriteError {
    unsigned long code = 0;
    double needed_mb = 0, free_mb = 0;
};

// Writes `data` to `path` safely: into a temporary file in the same folder, flushed to disk,
// then moved over the target. The old file stays untouched if anything fails.
bool write_file(const std::wstring& path, const Bytes& data, std::wstring& error, WriteError& we)
{
    std::wstring folder = path.substr(0, path.find_last_of(L"\\/") + 1);
    if (folder.empty()) folder = L".\\";
    ULARGE_INTEGER free_bytes{};
    if (GetDiskFreeSpaceExW(folder.c_str(), &free_bytes, nullptr, nullptr)) {
        double need = data.size() + 1024.0 * 1024.0;
        if (static_cast<double>(free_bytes.QuadPart) < need) {
            we.code = ERROR_DISK_FULL;
            we.needed_mb = data.size() / 1048576.0;
            we.free_mb = free_bytes.QuadPart / 1048576.0;
            error = L"Not enough free space on the drive.";
            return false;
        }
    }
    // an existing read-only target can't be replaced
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        we.code = ERROR_ACCESS_DENIED;
        error = L"The existing file is read-only.";
        return false;
    }
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        we.code = ERROR_INVALID_NAME;
        error = L"A folder with this name already exists.";
        return false;
    }
    // check early that the target isn't open in another program (gives a clear message)
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        HANDLE probe = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) {
            we.code = GetLastError();
            error = L"The existing file can't be replaced.";
            return false;
        }
        CloseHandle(probe);
    }

    wchar_t suffix[64];
    swprintf(suffix, 64, L".lide50-%lu.tmp", static_cast<unsigned long>(GetCurrentProcessId() ^ GetTickCount()));
    std::wstring tmp = path + suffix;
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        we.code = GetLastError();
        // the temporary name may be what's too long; report the real target
        error = L"Could not create the file.";
        return false;
    }
    bool ok = true;
    size_t done = 0;
    while (ok && done < data.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(data.size() - done, 64u * 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(h, data.data() + done, chunk, &written, nullptr) || written != chunk) {
            we.code = GetLastError();
            if (we.code == 0) we.code = ERROR_WRITE_FAULT;
            ok = false;
            break;
        }
        done += written;
    }
    if (ok && !FlushFileBuffers(h)) {
        we.code = GetLastError();
        ok = false;
    }
    if (!CloseHandle(h) && ok) {
        we.code = GetLastError();
        ok = false;
    }
    if (ok && !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        we.code = GetLastError();
        ok = false;
    }
    if (!ok) {
        DeleteFileW(tmp.c_str());
        if (we.code == ERROR_DISK_FULL || we.code == ERROR_HANDLE_DISK_FULL) {
            we.needed_mb = data.size() / 1048576.0;
            if (GetDiskFreeSpaceExW(folder.c_str(), &free_bytes, nullptr, nullptr))
                we.free_mb = free_bytes.QuadPart / 1048576.0;
        }
        error = L"Could not write the file.";
    }
    return ok;
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
                    const SaveOptions& opt, std::wstring& error, unsigned long* win32_error, double* needed_mb,
                    double* free_mb)
{
    WriteError we;
    auto fail = [&](unsigned long code, const wchar_t* text) {
        we.code = code;
        error = text;
        if (win32_error) *win32_error = we.code;
        return false;
    };
    if (pages.empty()) return fail(ERROR_INVALID_DATA, L"Nothing to save.");
    for (const Image* p : pages)
        if (!p || p->empty() || (p->channels != 1 && p->channels != 3) ||
            p->data.size() < p->stride() * static_cast<size_t>(p->height))
            return fail(ERROR_INVALID_DATA, L"A scan to save is empty or damaged.");
    // limits of the file formats
    for (const Image* p : pages) {
        if ((f == OutFormat::JPEG || f == OutFormat::PDF || f == OutFormat::DOCX) && !p->bilevel &&
            (p->width > 65500 || p->height > 65500))
            return fail(ERROR_INVALID_DATA, L"The image is too large for JPEG (65,500 pixels per side at most).");
        double bytes = static_cast<double>(p->stride()) * p->height;
        if ((f == OutFormat::BMP || f == OutFormat::TIFF) && bytes > 3.9e9)
            return fail(ERROR_FILE_TOO_LARGE, L"The image is too large for this file type (4 GB at most).");
    }
    // limits that apply to the whole file
    if (f == OutFormat::TIFF) {
        double total = 0;
        for (const Image* p : pages) total += static_cast<double>(p->stride()) * p->height;
        if (total > 3.9e9)
            return fail(ERROR_FILE_TOO_LARGE, L"These scans are too large for one TIFF file (4 GB at most). "
                                              L"Save fewer scans per file or use PDF.");
    }
    Bytes data;
    try {
        switch (f) {
            case OutFormat::PDF: data = encode_pdf(pages, opt); break;
            case OutFormat::DOCX: {
                // Page size follows the Windows measurement setting: Letter for US, else A4.
                DWORD measure = 0;
                const bool letter =
                    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IMEASURE | LOCALE_RETURN_NUMBER,
                                    reinterpret_cast<LPWSTR>(&measure), sizeof(measure) / sizeof(wchar_t)) > 0 &&
                    measure == 1;
                data = encode_docx(pages, opt, letter);
                break;
            }
            case OutFormat::TIFF: data = encode_tiff(pages); break;
            case OutFormat::PNG: data = encode_png(*pages[0]); break;
            case OutFormat::JPEG: data = encode_jpeg(*pages[0], opt.jpeg_quality); break;
            case OutFormat::BMP: data = encode_bmp(*pages[0]); break;
        }
    } catch (const std::bad_alloc&) {
        return fail(ERROR_NOT_ENOUGH_MEMORY, L"Out of memory while creating the file.");
    } catch (const EncodeTooLarge&) {
        return fail(ERROR_FILE_TOO_LARGE, L"The result is too large for this file type (4 GB at most).");
    }
    if (data.empty()) return fail(ERROR_NOT_ENOUGH_MEMORY, L"Creating the file failed (out of memory?).");
    bool ok = write_file(path, data, error, we);
    if (win32_error) *win32_error = we.code;
    if (needed_mb) *needed_mb = we.needed_mb;
    if (free_mb) *free_mb = we.free_mb;
    return ok;
}
