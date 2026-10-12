// LiDE50 Scan - portable scanning application for the Canon CanoScan LiDE 50
// on 64-bit Windows 10/11. The scanner driver is built into this executable.
// The interface is drawn with Direct2D in a "liquid glass" style with light
// and dark themes.
//
// Licensed GPL-2.0-or-later (it contains code from SANE's genesys backend).

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <initguid.h>
#include <usbiodef.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "image.h"
#include "log.h"
#include "process.h"
#include "scanner_engine.h"
#include "ui/gfx.h"
#include "ui/ui.h"
#include "winusb_setup.h"
#include "writers.h"

extern "C" void lide50_set_usb_trace(int on);

namespace {

const wchar_t kAppName[] = L"LiDE50 Scan";
const wchar_t kMainClass[] = L"LiDE50ScanGlass";

enum : UINT {
    WM_ENGINE = WM_APP + 1,      // wParam = EngineEvent, lParam = std::string*
    WM_SETUP_DONE = WM_APP + 2,  // wParam = success, lParam = std::wstring*
    WM_SAVE_DONE = WM_APP + 3,   // wParam = success, lParam = std::wstring*
};

enum : int { ID_PREVIEW = 100, ID_SCAN, ID_SAVE_PAGE, ID_SAVE_ALL, ID_SHOWGLASS };
enum : UINT_PTR { TIMER_FRAME = 1, TIMER_LIVE = 2, TIMER_USBCHECK = 3 };

const int kResolutions[] = {75, 150, 200, 300, 600, 1200, 2400};
const int kResCount = static_cast<int>(sizeof(kResolutions) / sizeof(kResolutions[0]));

struct AreaPreset { const wchar_t* name; double w, h; };
const AreaPreset kAreas[] = {
    {L"Entire glass", 0, 0},
    {L"Custom selection", -1, -1},
    {L"A4", 210, 297},
    {L"US Letter", 215.9, 279.4},
    {L"A5", 148, 210},
    {L"Photo 6 × 4 in", 152.4, 101.6},
    {L"Photo 4 × 6 in", 101.6, 152.4},
    {L"Photo 7 × 5 in", 177.8, 127},
    {L"Photo 5 × 7 in", 127, 177.8},
    {L"Business card", 85, 55},
};
const int kAreaCount = static_cast<int>(sizeof(kAreas) / sizeof(kAreas[0]));
const int kAreaGlass = 0, kAreaCustom = 1;

const std::vector<std::wstring> kFormatNames = {L"PDF document", L"Word document", L"PNG image",
                                                 L"JPEG image", L"TIFF image", L"Bitmap image"};

// ------------------------------------------------------------------- state

struct Page {
    Image raw;              // full resolution as scanned
    Image small;            // downscaled copy of raw (cached)
    Image display;          // adjusted, downscaled for the screen
    Image thumb;            // small copy of display for the list
    Adjustments adj, display_adj;
    bool display_valid = false;
    unsigned version = 0;
    int number = 0;
    double born = 0;
    GpuImage gi_display, gi_thumb;
};

struct RectMM { double l = 0, t = 0, r = 0, b = 0; };

enum class View { Glass, Page, Scanning };
enum class ScannerUi { Searching, NotFound, NeedsSetup, Connecting, Ready, Error };

HINSTANCE g_inst;
HWND g_main;
HACCEL g_accel;
Gfx g_gfx;
Ui g_ui(g_gfx);
float g_dpi = 96.0f;
bool g_rendering = false;
bool g_force_close = false;
bool g_active = true;      // window is in the foreground (background motion pauses otherwise)
LARGE_INTEGER g_qpf, g_qp0;

ScannerEngine g_engine;
bool g_simulate = false;
bool g_log = false;
bool g_fake_usb = false;
std::wstring g_data_dir, g_ini;
UsbAccessState g_usb_state = UsbAccessState::NotConnected;
ScannerUi g_scanner_ui = ScannerUi::Searching;
std::wstring g_scanner_name = L"Canon CanoScan LiDE 50";
std::wstring g_scanner_detail = L"Looking for the scanner";
std::wstring g_status;
bool g_setup_running = false;
std::atomic<bool> g_saving{false};

// settings
bool g_dark = false;
bool g_motion = true;
int g_dpi_idx = 3;
int g_area_idx = 0;
int g_format_idx = 0;
int g_quality = 90;
bool g_lossless = false;

std::vector<std::unique_ptr<Page>> g_pages;
int g_selected = -1;
int g_page_counter = 0;

Image g_preview;
RectMM g_preview_area;
bool g_have_preview = false;
unsigned g_preview_version = 0;
Image g_preview_display;
Adjustments g_preview_display_adj;
bool g_preview_display_valid = false;
GpuImage g_gi_preview;

RectMM g_sel;
Adjustments g_default_adj;

View g_view = View::Glass;

// live scan
Image g_live;
double g_live_factor = 1.0;
int g_live_rows_done = 0, g_live_rows_uploaded = 0;
int g_live_src_width = 0;
bool g_live_preview = false;
ScanRequest g_live_req;
int g_live_lines_done = 0, g_live_lines_total = 0;
GpuImage g_gi_live;
unsigned g_live_gen = 0;

// page view zoom
float g_zoom = 0;  // 0 = fit
float g_pan_x = 0, g_pan_y = 0;

// scrolling panels
float g_side_scroll = 0, g_side_target = 0, g_side_content = 0;
float g_list_scroll = 0, g_list_target = 0, g_list_content = 0;
double g_side_scroll_time = -10, g_list_scroll_time = -10;

// canvas selection drag
int g_drag_mode = 0;
float g_drag_x = 0, g_drag_y = 0;
RectMM g_drag_orig;

// ------------------------------------------------------------- utilities

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

std::wstring exe_path()
{
    wchar_t p[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH * 4);
    return p;
}

std::wstring exe_dir()
{
    std::wstring p = exe_path();
    return p.substr(0, p.find_last_of(L"\\/"));
}

bool dir_writable(const std::wstring& dir)
{
    std::wstring probe = dir + L"\\.lide50_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

int ini_int(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"settings", key, def, g_ini.c_str()); }
void ini_set(const wchar_t* key, int v)
{
    WritePrivateProfileStringW(L"settings", key, std::to_wstring(v).c_str(), g_ini.c_str());
}
std::wstring ini_str(const wchar_t* key)
{
    wchar_t buf[MAX_PATH * 2] = {};
    GetPrivateProfileStringW(L"settings", key, L"", buf, MAX_PATH * 2, g_ini.c_str());
    return buf;
}
void ini_set_str(const wchar_t* key, const std::wstring& v)
{
    WritePrivateProfileStringW(L"settings", key, v.c_str(), g_ini.c_str());
}

double now_seconds()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart - g_qp0.QuadPart) / static_cast<double>(g_qpf.QuadPart);
}

void redraw() { InvalidateRect(g_main, nullptr, FALSE); }

// draws a frame immediately (used for mouse button changes)
void frame_now()
{
    InvalidateRect(g_main, nullptr, FALSE);
    UpdateWindow(g_main);
}

void set_status(const std::wstring& s) { g_status = s; redraw(); }

bool system_prefers_dark()
{
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v == 0;
    return false;
}

void apply_title_bar_theme()
{
    BOOL dark = g_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(g_main, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    Theme t = g_dark ? theme_dark() : theme_light();
    COLORREF cap = RGB(static_cast<int>(t.base2.r * 255), static_cast<int>(t.base2.g * 255),
                       static_cast<int>(t.base2.b * 255));
    DwmSetWindowAttribute(g_main, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof(cap));
    COLORREF txt = g_dark ? RGB(245, 245, 247) : RGB(28, 28, 30);
    DwmSetWindowAttribute(g_main, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof(txt));
}

// ------------------------------------------------------- page/adj helpers

Page* selected_page()
{
    if (g_view != View::Page) return nullptr;
    if (g_selected < 0 || g_selected >= static_cast<int>(g_pages.size())) return nullptr;
    return g_pages[static_cast<size_t>(g_selected)].get();
}

Adjustments& current_adj()
{
    if (Page* p = selected_page()) return p->adj;
    return g_default_adj;
}

int mode_of(const Adjustments& a, int channels)
{
    return a.black_white ? 2 : ((a.grayscale || channels == 1) ? 1 : 0);
}

ColorMode next_scan_mode()
{
    if (g_default_adj.black_white) return ColorMode::BlackWhite;
    if (g_default_adj.grayscale) return ColorMode::Gray;
    return ColorMode::Color;
}

int ui_dpi() { return kResolutions[std::clamp(g_dpi_idx, 0, kResCount - 1)]; }
OutFormat ui_format() { return static_cast<OutFormat>(std::clamp(g_format_idx, 0, 5)); }

void ensure_page_display(Page& p)
{
    if (p.display_valid && p.display_adj == p.adj) return;
    if (p.small.empty()) p.small = downscale(p.raw, 1800);
    double hint = p.raw.width > 0 ? static_cast<double>(p.small.width) / p.raw.width : 1.0;
    p.display = apply_adjustments(p.small, p.adj, hint);
    p.display_adj = p.adj;
    p.display_valid = true;
    p.thumb = downscale(p.display, 400);
    ++p.version;
}

void show_page(int index)
{
    if (index < 0 || index >= static_cast<int>(g_pages.size())) return;
    g_selected = index;
    g_view = View::Page;
    g_zoom = 0;
    g_pan_x = g_pan_y = 0;
    redraw();
}

void show_glass()
{
    g_view = View::Glass;
    redraw();
}

std::wstring timestamp_name()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t buf[64];
    swprintf(buf, 64, L"Scan_%04d-%02d-%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

void apply_area_preset(int i)
{
    double bw = g_engine.bed_width_mm(), bh = g_engine.bed_height_mm();
    if (i == kAreaGlass) g_sel = {0, 0, bw, bh};
    else if (i > kAreaCustom && i < kAreaCount) g_sel = {0, 0, std::min(bw, kAreas[i].w), std::min(bh, kAreas[i].h)};
    g_area_idx = i;
    ini_set(L"area", i);
    redraw();
}

// -------------------------------------------------------------- scanner

void set_scanner_state(ScannerUi s, const std::wstring& detail)
{
    g_scanner_ui = s;
    g_scanner_detail = detail;
    redraw();
}

void check_scanner()
{
    if (g_simulate || g_fake_usb) {
        if (!g_engine.connected() && !g_engine.busy()) {
            set_scanner_state(ScannerUi::Connecting, L"Connecting");
            g_engine.connect();
        }
        return;
    }
    UsbAccessInfo info = usb_detect_lide50();
    UsbAccessState prev = g_usb_state;
    g_usb_state = info.state;
    if (prev != info.state || info.state != UsbAccessState::Ready) {
        const char* st = info.state == UsbAccessState::Ready ? "ready (WinUSB assigned)"
                         : info.state == UsbAccessState::NeedsSetup ? "plugged in, needs one-time access setup"
                                                                    : "not connected";
        logf("INFO", "USB check: Canon LiDE 50 %s; device id '%s', driver service '%s', problem code %u", st,
             to_utf8(info.instance_id).c_str(), to_utf8(info.service).c_str(), info.problem);
    }
    switch (info.state) {
        case UsbAccessState::NotConnected:
            if (g_engine.connected() && !g_engine.busy()) g_engine.disconnect();
            set_scanner_state(ScannerUi::NotFound, L"Not connected");
            break;
        case UsbAccessState::NeedsSetup:
            set_scanner_state(ScannerUi::NeedsSetup, L"Needs permission");
            break;
        case UsbAccessState::Ready:
            if (prev != UsbAccessState::Ready || !g_engine.connected()) {
                set_scanner_state(ScannerUi::Connecting, L"Connecting");
                if (!g_engine.busy()) g_engine.connect();
            }
            break;
    }
}

void begin_scan(const ScanRequest& r)
{
    LOG_UI("starting %s: %d dpi, mode %d, area %.1f,%.1f - %.1f,%.1f mm", r.preview ? "preview" : "scan", r.dpi,
           static_cast<int>(r.mode), r.tl_x, r.tl_y, r.br_x, r.br_y);
    g_live = Image();
    g_live_rows_done = g_live_rows_uploaded = 0;
    g_live_src_width = 0;
    g_live_preview = r.preview;
    g_live_req = r;
    g_live_lines_done = 0;
    g_live_lines_total = 0;
    g_gi_live.release();
    ++g_live_gen;
    g_view = r.preview ? View::Glass : View::Scanning;
    g_ui.close_popover();
    g_engine.scan(r);
    SetTimer(g_main, TIMER_LIVE, 66, nullptr);
    redraw();
}

void start_scan(bool preview)
{
    if (!g_engine.connected() || g_engine.busy()) return;
    LOG_UI("clicked '%s'", preview ? "Preview" : "Scan");
    ScanRequest r;
    r.preview = preview;
    if (preview) {
        r.dpi = 75;
        r.mode = ColorMode::Color;
        r.tl_x = 0; r.tl_y = 0; r.br_x = g_engine.bed_width_mm(); r.br_y = g_engine.bed_height_mm();
        begin_scan(r);
        return;
    }
    r.dpi = ui_dpi();
    r.mode = next_scan_mode();
    r.tl_x = g_sel.l; r.tl_y = g_sel.t; r.br_x = g_sel.r; r.br_y = g_sel.b;
    double mb = (r.br_x - r.tl_x) / 25.4 * r.dpi * (r.br_y - r.tl_y) / 25.4 * r.dpi *
                (r.mode == ColorMode::Color ? 3 : 1) / 1048576.0;
    if (mb > 900) {
        wchar_t msg[300];
        swprintf(msg, 300, L"This scan needs about %.0f MB of memory and will take a long time. "
                           L"Selecting a smaller area on the preview or a lower resolution makes it faster.", mb);
        g_ui.alert(L"Large scan", msg, {L"Cancel", L"Scan anyway"}, [r](int b) { if (b == 1) begin_scan(r); }, 0, 0);
        return;
    }
    begin_scan(r);
}

// Copies newly scanned rows into the downscaled live image.
void update_live()
{
    ScanProgress& pr = g_engine.progress();
    std::lock_guard<std::mutex> lock(pr.mutex);
    if (pr.image.empty() || !pr.active) return;
    const Image& src = pr.image;
    if (g_live.empty() || g_live_src_width != src.width) {
        int total = pr.lines_total > 0 ? pr.lines_total : src.height;
        g_live_factor = std::min(1.0, 1600.0 / std::max(src.width, total));
        int lw = std::max(1, static_cast<int>(src.width * g_live_factor));
        int lh = std::max(1, static_cast<int>(total * g_live_factor));
        g_live.alloc(lw, lh, src.channels);
        std::fill(g_live.data.begin(), g_live.data.end(), g_dark ? 40 : 235);
        g_live.dpi = static_cast<int>(src.dpi * g_live_factor);
        g_live_rows_done = g_live_rows_uploaded = 0;
        g_live_src_width = src.width;
        g_gi_live.release();
        ++g_live_gen;
    }
    g_live_lines_done = pr.lines_done;
    g_live_lines_total = pr.lines_total;
    const double f = g_live_factor;
    const int c = src.channels;
    while (g_live_rows_done < g_live.height) {
        int y0 = static_cast<int>(g_live_rows_done / f);
        int y1 = std::max(y0 + 1, static_cast<int>((g_live_rows_done + 1) / f));
        if (y1 > pr.lines_done || y1 > src.height) break;
        std::uint8_t* out = g_live.row(g_live_rows_done);
        for (int x = 0; x < g_live.width; ++x) {
            int x0 = static_cast<int>(x / f), x1 = std::max(x0 + 1, std::min(src.width, static_cast<int>((x + 1) / f)));
            for (int k = 0; k < c; ++k) {
                unsigned sum = 0, cnt = 0;
                for (int sy = y0; sy < y1; sy += std::max(1, (y1 - y0) / 4))
                    for (int sx = x0; sx < x1; sx += std::max(1, (x1 - x0) / 4)) { sum += src.row(sy)[sx * c + k]; ++cnt; }
                out[x * c + k] = static_cast<std::uint8_t>(sum / std::max(1u, cnt));
            }
        }
        ++g_live_rows_done;
    }
}

// ---------------------------------------------------------------- saving

std::wstring documents_dir()
{
    std::wstring last = ini_str(L"last_dir");
    if (!last.empty() && GetFileAttributesW(last.c_str()) != INVALID_FILE_ATTRIBUTES) return last;
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

bool ask_save_path(OutFormat f, const std::wstring& suggested, std::wstring& path)
{
    wchar_t file[MAX_PATH * 4] = {};
    std::wstring name = suggested + L"." + format_extension(f);
    wcsncpy(file, name.c_str(), MAX_PATH * 4 - 1);
    std::wstring dir = documents_dir();
    std::wstring filter = std::wstring(format_filter(f), wcslen(format_filter(f)) + 1);
    const wchar_t* second = format_filter(f) + wcslen(format_filter(f)) + 1;
    filter += std::wstring(second) + L'\0' + L'\0';
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    ofn.lpstrDefExt = format_extension(f);
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return false;
    path = file;
    ini_set_str(L"last_dir", path.substr(0, path.find_last_of(L"\\/")));
    return true;
}

void save_pages(bool all)
{
    if (g_pages.empty() || g_saving) return;
    LOG_UI("clicked '%s'", all ? "Save all" : "Save scan");
    std::vector<int> idx;
    if (all) {
        for (size_t i = 0; i < g_pages.size(); ++i) idx.push_back(static_cast<int>(i));
    } else {
        int i = g_selected >= 0 && g_selected < static_cast<int>(g_pages.size()) ? g_selected
                                                                                   : static_cast<int>(g_pages.size()) - 1;
        idx.push_back(i);
    }
    OutFormat f = ui_format();
    std::wstring path;
    if (!ask_save_path(f, timestamp_name(), path)) return;

    SaveOptions opt;
    opt.jpeg_quality = g_quality;
    opt.pdf_lossless = g_lossless;

    std::vector<std::pair<const Image*, Adjustments>> jobs;
    for (int i : idx) jobs.emplace_back(&g_pages[static_cast<size_t>(i)]->raw, g_pages[static_cast<size_t>(i)]->adj);

    g_saving = true;
    set_status(L"Saving");
    logw("UI", L"saving " + std::to_wstring(jobs.size()) + L" scan(s) as " + format_extension(f) + L" to " + path);
    std::thread([jobs, f, path, opt]() {
        prepare_thread_for_crash_logging("save");
        std::wstring err;
        bool ok = true;
        std::vector<Image> processed;
        processed.reserve(jobs.size());
        for (auto& j : jobs) processed.push_back(apply_adjustments(*j.first, j.second, 1.0));
        if (format_multipage(f) || processed.size() == 1) {
            std::vector<const Image*> ptrs;
            for (auto& p : processed) ptrs.push_back(&p);
            ok = write_document(path, f, ptrs, opt, err);
        } else {
            std::wstring base = path, ext;
            size_t dot = path.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > path.find_last_of(L"\\/")) { base = path.substr(0, dot); ext = path.substr(dot); }
            for (size_t i = 0; i < processed.size() && ok; ++i) {
                wchar_t num[16];
                swprintf(num, 16, L"_%03zu", i + 1);
                ok = write_document(base + num + ext, f, {&processed[i]}, opt, err);
            }
        }
        if (ok) logw("INFO", L"saved " + std::to_wstring(processed.size()) + L" scan(s) to " + path);
        else logw("ERROR", L"saving to " + path + L" failed: " + err);
        std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
        auto* msg = new std::wstring(ok ? L"Saved " + name : err);
        PostMessageW(g_main, WM_SAVE_DONE, ok ? 1 : 0, reinterpret_cast<LPARAM>(msg));
    }).detach();
}

// ---------------------------------------------------------- elevation

void run_access_setup(bool remove)
{
    if (g_setup_running) return;
    g_setup_running = true;
    set_status(remove ? L"Removing scanner access" : L"Approve the Windows prompt to continue");
    std::thread([remove]() {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        std::wstring result_file = std::wstring(tmp) + L"lide50scan_setup_result.txt";
        DeleteFileW(result_file.c_str());
        std::wstring exe = exe_path();
        std::wstring params = std::wstring(remove ? L"--disable-access" : L"--enable-access") + L" \"" + result_file + L"\"";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.c_str();
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        LOG_INFO("asking Windows for administrator permission to %s scanner access", remove ? "remove" : "set up");
        bool ok = false;
        std::wstring msg;
        if (!ShellExecuteExW(&sei)) {
            DWORD e = GetLastError();
            msg = e == ERROR_CANCELLED ? L"The Windows permission prompt was cancelled, so nothing was changed."
                                       : L"The setup step could not start (error " + std::to_wstring(e) + L").";
        } else {
            WaitForSingleObject(sei.hProcess, 120000);
            DWORD code = 1;
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            ok = code == 0;
            FILE* f = _wfopen(result_file.c_str(), L"rt, ccs=UTF-16LE");
            if (f) {
                wchar_t line[1024];
                while (fgetws(line, 1024, f)) msg += line;
                fclose(f);
                DeleteFileW(result_file.c_str());
            }
            if (msg.empty()) msg = ok ? L"Done." : L"Setup failed.";
        }
        PostMessageW(g_main, WM_SETUP_DONE, ok ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(msg)));
    }).detach();
}

int run_elevated_helper(bool remove, const std::wstring& result_file)
{
    std::wstring msg;
    bool ok = remove ? usb_unbind_winusb(msg) : usb_bind_winusb(msg);
    FILE* f = _wfopen(result_file.c_str(), L"wt, ccs=UTF-16LE");
    if (f) { fputws(msg.c_str(), f); fclose(f); }
    return ok ? 0 : 1;
}

// ------------------------------------------------------------- actions

void delete_selected_page()
{
    if (!selected_page() || g_saving) return;
    LOG_UI("deleted scan %d", g_pages[static_cast<size_t>(g_selected)]->number);
    g_pages.erase(g_pages.begin() + g_selected);
    if (g_pages.empty()) {
        g_selected = -1;
        show_glass();
    } else {
        show_page(std::min(g_selected, static_cast<int>(g_pages.size()) - 1));
    }
}

void move_selected_page(int dir)
{
    if (!selected_page() || g_saving) return;
    int j = g_selected + dir;
    if (j < 0 || j >= static_cast<int>(g_pages.size())) return;
    std::swap(g_pages[static_cast<size_t>(g_selected)], g_pages[static_cast<size_t>(j)]);
    LOG_UI("moved scan %s", dir < 0 ? "up" : "down");
    show_page(j);
}

void set_theme(bool dark)
{
    g_dark = dark;
    ini_set(L"theme", dark ? 2 : 1);
    LOG_UI("theme changed to %s", dark ? "dark" : "light");
    apply_title_bar_theme();
    redraw();
}

void show_about()
{
    g_ui.alert(L"LiDE50 Scan 1.3",
               L"Portable scanning for the Canon CanoScan LiDE 50 on 64-bit Windows. The scanner driver is built "
               L"in and talks to the scanner through Windows' own WinUSB driver, so nothing else needs installing.\n\n"
               L"Built on the SANE genesys backend (GPL-2.0-or-later), libusb (LGPL-2.1), miniz (MIT) and "
               L"stb_image_write (public domain). LiDE50 Scan is free software under the GNU GPL v2 or later. "
               L"Canon and CanoScan are trademarks of Canon Inc.; this app is not affiliated with Canon.",
               {L"OK"}, nullptr);
}

void open_more_menu(const RectF& anchor)
{
    std::vector<Ui::MenuItem> m;
    bool has = !g_pages.empty() && !g_saving;
    m.push_back({L"Save all scans", Icon::Save, true, has, [] { save_pages(true); }});
    m.push_back({L"Clear all scans", Icon::Trash, true, has, [] {
                     g_ui.alert(L"Clear all scans?", L"Scans you haven't saved will be lost.",
                                {L"Keep them", L"Clear all"}, [](int b) {
                                    if (b != 1) return;
                                    LOG_UI("cleared all scans");
                                    g_pages.clear();
                                    g_selected = -1;
                                    show_glass();
                                }, 0, 0);
                 }});
    m.push_back({L"", Icon::Check, false, false, nullptr, true});
    m.push_back({g_motion ? L"Turn off background motion" : L"Turn on background motion", Icon::Layers, true, true, [] {
                     g_motion = !g_motion;
                     ini_set(L"motion", g_motion ? 1 : 0);
                     LOG_UI("background motion %s", g_motion ? "on" : "off");
                 }});
    m.push_back({L"", Icon::Check, false, false, nullptr, true});
    m.push_back({L"Enable scanner access", Icon::Shield, true, !g_simulate && !g_setup_running,
                 [] { run_access_setup(false); }});
    m.push_back({L"Remove scanner access", Icon::Plug, true, !g_simulate && !g_setup_running, [] {
                     g_ui.alert(L"Remove scanner access?",
                                L"Windows will treat the LiDE 50 as an unknown device again until you set up access.",
                                {L"Cancel", L"Remove"}, [](int b) { if (b == 1) run_access_setup(true); }, 0, 0);
                 }});
    m.push_back({L"Manual setup steps", Icon::Doc, true, true,
                 [] { g_ui.alert(L"Manual setup", usb_manual_setup_instructions(), {L"OK"}, nullptr); }});
    m.push_back({L"", Icon::Check, false, false, nullptr, true});
    m.push_back({L"Open the log file", Icon::Doc, true, true, [] {
                     ShellExecuteW(g_main, L"open", L"notepad.exe", (L"\"" + log_path() + L"\"").c_str(), nullptr,
                                   SW_SHOWNORMAL);
                 }});
    m.push_back({L"Show the log in its folder", Icon::Layers, true, true, [] {
                     std::wstring args = L"/select,\"" + log_path() + L"\"";
                     ShellExecuteW(g_main, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
                 }});
    m.push_back({L"Open the settings folder", Icon::Layers, true, true, [] {
                     ShellExecuteW(g_main, L"open", g_data_dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                 }});
    m.push_back({L"", Icon::Check, false, false, nullptr, true});
    m.push_back({L"About LiDE50 Scan", Icon::Scan, true, true, [] { show_about(); }});
    g_ui.menu("more", anchor, std::move(m));
}

// --------------------------------------------------------------- engine

void on_engine_event(EngineEvent e, const std::string& text)
{
    static const char* names[] = {"Connected", "ConnectFailed", "ScanStarted", "ScanFinished",
                                  "ScanFailed", "ScanCancelled", "ButtonPressed", "Status",
                                  "Parking", "Idle"};
    int ei = static_cast<int>(e);
    logf(e == EngineEvent::ScanFailed || e == EngineEvent::ConnectFailed ? "WARN" : "UI", "event %s%s%s",
         (ei >= 0 && ei < 10) ? names[ei] : "?", text.empty() ? "" : ": ", text.c_str());
    switch (e) {
        case EngineEvent::Connected:
            g_scanner_name = widen(text);
            set_scanner_state(ScannerUi::Ready, L"Ready");
            g_status.clear();
            g_sel.r = std::min(g_sel.r, g_engine.bed_width_mm());
            g_sel.b = std::min(g_sel.b, g_engine.bed_height_mm());
            if (g_area_idx == kAreaGlass || g_area_idx == kAreaCustom) apply_area_preset(kAreaGlass);
            else apply_area_preset(g_area_idx);
            break;
        case EngineEvent::ConnectFailed: {
            std::wstring t = widen(text);
            bool access = t.find(L"access") != std::wstring::npos;
            set_scanner_state(access && !g_simulate ? ScannerUi::NeedsSetup : ScannerUi::Error,
                              access ? L"Needs permission" : L"Couldn't connect");
            g_status = t;
            break;
        }
        case EngineEvent::Status:
            g_status = widen(text);
            redraw();
            break;
        case EngineEvent::ScanStarted:
            redraw();
            break;
        case EngineEvent::ScanFinished: {
            KillTimer(g_main, TIMER_LIVE);
            Image img = g_engine.take_result();
            g_status.clear();
            if (g_live_preview) {
                LOG_INFO("preview received: %d x %d px", img.width, img.height);
                g_preview = std::move(img);
                g_preview_area = {g_live_req.tl_x, g_live_req.tl_y, g_live_req.br_x, g_live_req.br_y};
                g_have_preview = true;
                g_preview_display_valid = false;
                ++g_preview_version;
                g_view = View::Glass;
                g_ui.toast(L"Preview ready. Drag on the glass to choose what to scan.", 1);
            } else {
                auto p = std::make_unique<Page>();
                p->raw = std::move(img);
                p->adj = g_default_adj;
                p->number = ++g_page_counter;
                p->born = g_ui.time;
                if (p->raw.channels == 1 && !p->adj.black_white) p->adj.grayscale = true;
                LOG_INFO("scan %d added: %d x %d px, %d channel(s), %d dpi", p->number, p->raw.width, p->raw.height,
                         p->raw.channels, p->raw.dpi);
                g_pages.push_back(std::move(p));
                show_page(static_cast<int>(g_pages.size()) - 1);
                g_list_target = 1e9f; // scroll the list to the new scan
                std::wstring s = L"Scan " + std::to_wstring(g_page_counter) + L" finished";
                if (!text.empty()) s += L" (" + widen(text) + L")";
                g_ui.toast(s, 1);
            }
            g_live = Image();
            g_gi_live.release();
            redraw();
            break;
        }
        case EngineEvent::ScanFailed:
            KillTimer(g_main, TIMER_LIVE);
            g_live = Image();
            g_gi_live.release();
            if (g_view == View::Scanning) g_view = g_pages.empty() ? View::Glass : View::Page;
            if (g_view == View::Page && !selected_page()) g_view = View::Glass;
            if (!g_engine.connected()) set_scanner_state(ScannerUi::Connecting, L"Reconnecting");
            g_status.clear();
            g_ui.alert(L"The scan didn't finish", widen(text), {L"OK"}, nullptr);
            check_scanner();
            redraw();
            break;
        case EngineEvent::ScanCancelled:
            KillTimer(g_main, TIMER_LIVE);
            g_live = Image();
            g_gi_live.release();
            if (g_view == View::Scanning) g_view = selected_page() ? View::Page : View::Glass;
            if (g_view == View::Page && !selected_page()) g_view = View::Glass;
            g_status.clear();
            g_ui.toast(L"Scan cancelled", 2);
            redraw();
            break;
        case EngineEvent::Parking:
            g_status = L"Returning the scan head";
            redraw();
            break;
        case EngineEvent::Idle:
            g_status.clear();
            redraw();
            break;
        case EngineEvent::ButtonPressed:
            if (!g_engine.busy() && g_engine.connected() && !g_ui.modal_open()) {
                g_ui.toast(L"Scanner button pressed", 0);
                start_scan(false);
            }
            break;
    }
}

// ================================================================= drawing

Ui& ui = g_ui;
Gfx& gx = g_gfx;

// ---------------------------------------------------------------- canvas

struct Geo { float x, y, s; }; // screen = x + mm * s

Geo glass_geo(const RectF& area)
{
    float bw = static_cast<float>(g_engine.bed_width_mm()), bh = static_cast<float>(g_engine.bed_height_mm());
    float m = 46.0f;
    float s = std::max(0.05f, std::min((area.w - 2 * m) / bw, (area.h - 2 * m - 30.0f) / bh));
    return {area.cx() - bw * s * 0.5f, area.y + (area.h - 30.0f) * 0.5f - bh * s * 0.5f, s};
}

enum Hit { HIT_NONE = 0, HIT_L = 1, HIT_T = 2, HIT_R = 4, HIT_B = 8, HIT_MOVE = 16 };

int hit_selection(const Geo& g, float px, float py)
{
    float l = g.x + static_cast<float>(g_sel.l) * g.s, r = g.x + static_cast<float>(g_sel.r) * g.s;
    float t = g.y + static_cast<float>(g_sel.t) * g.s, b = g.y + static_cast<float>(g_sel.b) * g.s;
    float tol = 8.0f;
    int h = 0;
    bool in_y = py >= t - tol && py <= b + tol, in_x = px >= l - tol && px <= r + tol;
    if (in_y && std::fabs(px - l) <= tol) h |= HIT_L;
    if (in_y && std::fabs(px - r) <= tol) h |= HIT_R;
    if (in_x && std::fabs(py - t) <= tol) h |= HIT_T;
    if (in_x && std::fabs(py - b) <= tol) h |= HIT_B;
    if (!h && px > l && px < r && py > t && py < b) h = HIT_MOVE;
    return h;
}

void info_pill(const RectF& area, const std::wstring& text)
{
    float tw = std::min(gx.text_width(text, 12.5f, 500), area.w - 60.0f);
    RectF r{area.cx() - tw * 0.5f - 18.0f, area.b() - 44.0f, tw + 36.0f, 32.0f};
    ui.glass(r, 16.0f, 0.8f, true);
    gx.text(text, r, 12.5f, 500, ui.t.text2, Align::Center, Align::Center);
}

void scan_glow(const RectF& img, float y)
{
    // the scan head: a bright line with a soft trail of light above it
    float trail = std::min(70.0f, y - img.y);
    if (trail > 1.0f)
        gx.fill_rrect_vgrad(RectF{img.x, y - trail, img.w, trail}, 0, ui.t.accent.with_a(0.0f), ui.t.accent.with_a(0.22f));
    gx.fill_rrect(RectF{img.x - 4.0f, y - 2.0f, img.w + 8.0f, 4.0f}, 2.0f, ui.t.accent.with_a(0.45f));
    gx.fill_rrect(RectF{img.x - 2.0f, y - 1.0f, img.w + 4.0f, 2.0f}, 1.0f, Color::hex(0xFFFFFF, 0.95f));
}

// uploads newly scanned rows of the live image to the GPU
void sync_live_bitmap()
{
    if (g_live.empty()) return;
    if (!g_gi_live.bmp || g_gi_live.device_gen != gx.device_generation()) {
        gx.update_image(g_gi_live, g_live, g_live_gen);
        g_live_rows_uploaded = g_live_rows_done;
    } else if (g_live_rows_uploaded < g_live_rows_done) {
        gx.update_image_rows(g_gi_live, g_live, g_live_rows_uploaded, g_live_rows_done);
        g_live_rows_uploaded = g_live_rows_done;
    }
}

void draw_glass_view(const RectF& area, float vis)
{
    Geo g = glass_geo(area);
    float bw = static_cast<float>(g_engine.bed_width_mm()), bh = static_cast<float>(g_engine.bed_height_mm());
    RectF bed{g.x, g.y, bw * g.s, bh * g.s};
    RectF body = bed.inset(-18.0f);
    body.h += 10.0f;

    // scanner body
    gx.shadow(body, 22.0f, 26.0f, ui.t.shadow.alpha(1.2f), 12.0f);
    gx.fill_rrect_vgrad(body, 22.0f, lerp(ui.t.scanner_body, Color::hex(0xFFFFFF), 0.10f), ui.t.scanner_body);
    gx.stroke_rrect_vgrad(body.inset(0.75f), 21.0f, Color::hex(0xFFFFFF, 0.22f), Color::hex(0xFFFFFF, 0.02f), 1.2f);
    bool on = g_engine.connected();
    float pulse = g_engine.busy() ? 0.5f + 0.5f * std::sin(static_cast<float>(ui.time) * 5.0f) : 1.0f;
    gx.fill_ellipse(body.cx(), body.b() - 8.0f, 2.6f, 2.6f,
                    on ? ui.t.success.alpha(0.5f + 0.5f * pulse) : Color::hex(0x777777, 0.6f));

    // glass
    gx.fill_rrect(bed, 6.0f, ui.t.scanner_glass);
    const bool busy = g_engine.busy();
    const bool live_preview = busy && g_live_preview && !g_live.empty();
    if (live_preview) {
        const ScanRequest& r = g_live_req;
        RectF dst{g.x + static_cast<float>(r.tl_x) * g.s, g.y + static_cast<float>(r.tl_y) * g.s,
                  static_cast<float>(r.br_x - r.tl_x) * g.s, static_cast<float>(r.br_y - r.tl_y) * g.s};
        float frac = static_cast<float>(g_live_rows_done) / std::max(1, g_live.height);
        RectF part = dst;
        part.h = dst.h * frac;
        if (g_live_rows_done > 0) {
            sync_live_bitmap();
            RectF src{0, 0, static_cast<float>(g_live.width), static_cast<float>(g_live_rows_done)};
            gx.draw_image(g_gi_live, part, 1.0f, 0.0f, &src);
        }
        scan_glow(dst, part.b());
        ui.request_frame();
    } else if (g_have_preview) {
        if (!g_preview_display_valid || g_preview_display_adj != g_default_adj) {
            Adjustments a = g_default_adj;
            a.rotation = 0;
            a.flip_h = false;
            g_preview_display = apply_adjustments(g_preview, a, 1.0);
            g_preview_display_adj = g_default_adj;
            g_preview_display_valid = true;
            ++g_preview_version;
        }
        gx.update_image(g_gi_preview, g_preview_display, g_preview_version);
        RectF dst{g.x + static_cast<float>(g_preview_area.l) * g.s, g.y + static_cast<float>(g_preview_area.t) * g.s,
                  static_cast<float>(g_preview_area.r - g_preview_area.l) * g.s,
                  static_cast<float>(g_preview_area.b - g_preview_area.t) * g.s};
        gx.draw_image(g_gi_preview, dst, 1.0f, 5.0f);
    } else {
        std::wstring t = g_engine.connected()
                             ? L"Place your page face down in the blue corner,\nthen press Preview."
                             : L"The scanner glass shows up here\nonce the LiDE 50 is connected.";
        gx.text(t, bed.inset(20.0f), 14.0f, 500, ui.t.text2.alpha(0.9f), Align::Center, Align::Center, true);
    }
    // glass reflection
    gx.fill_rrect_vgrad(RectF{bed.x, bed.y, bed.w, bed.h * 0.35f}, 6.0f, Color::hex(0xFFFFFF, 0.10f),
                        Color::hex(0xFFFFFF, 0.0f));
    gx.stroke_rrect(bed, 6.0f, Color::hex(0x000000, 0.25f), 1.0f);

    // alignment corner
    gx.line(bed.x + 3, bed.y + 3, bed.x + 22, bed.y + 3, ui.t.accent, 3.0f);
    gx.line(bed.x + 3, bed.y + 3, bed.x + 3, bed.y + 22, ui.t.accent, 3.0f);

    // selection
    if (!live_preview) {
        float l = g.x + static_cast<float>(g_sel.l) * g.s, t = g.y + static_cast<float>(g_sel.t) * g.s;
        float r = g.x + static_cast<float>(g_sel.r) * g.s, b = g.y + static_cast<float>(g_sel.b) * g.s;
        bool whole = g_sel.l <= 0.01 && g_sel.t <= 0.01 && g_sel.r >= bw - 0.01 && g_sel.b >= bh - 0.01;
        float wa = ui.anim("sel.whole", whole ? 0.0f : 1.0f, 12.0f);
        if (wa > 0.01f) {
            Color d = ui.t.dim.with_a(0.42f * wa);
            gx.fill_rect(RectF{bed.x, bed.y, bed.w, t - bed.y}, d);
            gx.fill_rect(RectF{bed.x, b, bed.w, bed.b() - b}, d);
            gx.fill_rect(RectF{bed.x, t, l - bed.x, b - t}, d);
            gx.fill_rect(RectF{r, t, bed.r() - r, b - t}, d);
        }
        RectF sr{l, t, r - l, b - t};
        gx.stroke_rrect(sr.inset(-1.0f), 4.0f, Color::hex(0xFFFFFF, 0.8f), 3.0f);
        gx.stroke_rrect(sr.inset(-1.0f), 4.0f, ui.t.accent, 1.8f);
        float hx[3] = {l, (l + r) * 0.5f, r}, hy[3] = {t, (t + b) * 0.5f, b};
        int hover_hit = ((!busy || g_engine.parking()) && ui.mouse_inside && area.contains(ui.mx, ui.my) && !ui.modal_open() &&
                         !ui.popover_open())
                            ? (g_drag_mode ? g_drag_mode : hit_selection(g, ui.mx, ui.my))
                            : 0;
        for (int xi = 0; xi < 3; ++xi)
            for (int yi = 0; yi < 3; ++yi) {
                if (xi == 1 && yi == 1) continue;
                int code = (xi == 0 ? HIT_L : xi == 2 ? HIT_R : 0) | (yi == 0 ? HIT_T : yi == 2 ? HIT_B : 0);
                bool hot = hover_hit && (hover_hit & code) == code && hover_hit != HIT_MOVE;
                float hv = ui.anim("h" + std::to_string(code), hot ? 1.0f : 0.0f, 18.0f);
                float rad = 5.0f + 2.5f * hv;
                gx.shadow(RectF{hx[xi] - rad, hy[yi] - rad, rad * 2, rad * 2}, rad, 3.0f, ui.t.shadow.alpha(0.8f), 1.0f);
                gx.fill_ellipse(hx[xi], hy[yi], rad, rad, Color::hex(0xFFFFFF));
                gx.stroke_ellipse(hx[xi], hy[yi], rad - 1.0f, rad - 1.0f, ui.t.accent, 1.6f);
            }
    }

    std::wstring info;
    if (live_preview) {
        int pct = g_live_lines_total > 0 ? g_live_lines_done * 100 / g_live_lines_total : 0;
        info = L"Previewing the glass, " + std::to_wstring(pct) + L"%";
    } else if (busy && g_engine.parking()) {
        info = L"Returning the scan head to the start. You can choose the next area now.";
    } else if (busy) {
        info = L"Warming up the lamp and calibrating";
    } else {
        wchar_t buf[160];
        swprintf(buf, 160, L"Scan area %.0f × %.0f mm. Drag on the glass to choose what to scan.",
                 g_sel.r - g_sel.l, g_sel.b - g_sel.t);
        info = buf;
    }
    info_pill(area, info);

    // interaction
    if (vis > 0.99f && (!busy || g_engine.parking())) {
        int h = hit_selection(g, ui.mx, ui.my);
        // a selection that covers the whole glass can't be moved, so dragging
        // inside it draws a new area instead
        bool whole_glass = g_sel.l <= 0.5 && g_sel.t <= 0.5 && g_sel.r >= bw - 0.5 && g_sel.b >= bh - 0.5;
        if (whole_glass && h == HIT_MOVE) h = HIT_NONE;
        if (ui.press("canvas.glass", area)) {
            g_drag_mode = h;
            if (!g_drag_mode) {
                double mx = std::clamp((ui.mx - g.x) / g.s, 0.0f, bw);
                double my = std::clamp((ui.my - g.y) / g.s, 0.0f, bh);
                g_sel = {mx, my, mx, my};
                g_drag_mode = HIT_R | HIT_B;
            }
            g_drag_orig = g_sel;
            g_drag_x = ui.mx;
            g_drag_y = ui.my;
        }
        int show = ui.dragging("canvas.glass") ? g_drag_mode : (ui.hover("canvas.glass", area) ? h : -1);
        if (show >= 0) {
            if (show == HIT_MOVE) ui.set_cursor(IDC_SIZEALL);
            else if (show == (HIT_L | HIT_T) || show == (HIT_R | HIT_B)) ui.set_cursor(IDC_SIZENWSE);
            else if (show == (HIT_R | HIT_T) || show == (HIT_L | HIT_B)) ui.set_cursor(IDC_SIZENESW);
            else if (show & (HIT_L | HIT_R)) ui.set_cursor(IDC_SIZEWE);
            else if (show & (HIT_T | HIT_B)) ui.set_cursor(IDC_SIZENS);
            else ui.set_cursor(IDC_CROSS);
        }
        if (ui.dragging("canvas.glass")) {
            double dx = (ui.mx - g_drag_x) / g.s, dy = (ui.my - g_drag_y) / g.s;
            RectMM s = g_drag_orig;
            if (g_drag_mode == HIT_MOVE) {
                double w = s.r - s.l, hh = s.b - s.t;
                s.l = std::clamp(s.l + dx, 0.0, bw - w);
                s.t = std::clamp(s.t + dy, 0.0, bh - hh);
                s.r = s.l + w;
                s.b = s.t + hh;
            } else {
                if (g_drag_mode & HIT_L) s.l = std::clamp(s.l + dx, 0.0, static_cast<double>(bw));
                if (g_drag_mode & HIT_R) s.r = std::clamp(s.r + dx, 0.0, static_cast<double>(bw));
                if (g_drag_mode & HIT_T) s.t = std::clamp(s.t + dy, 0.0, static_cast<double>(bh));
                if (g_drag_mode & HIT_B) s.b = std::clamp(s.b + dy, 0.0, static_cast<double>(bh));
            }
            g_sel = s;
            g_area_idx = kAreaCustom;
        } else if (g_drag_mode && ui.released) {
            if (g_sel.l > g_sel.r) std::swap(g_sel.l, g_sel.r);
            if (g_sel.t > g_sel.b) std::swap(g_sel.t, g_sel.b);
            if (g_sel.r - g_sel.l < 5 || g_sel.b - g_sel.t < 5) {
                g_sel = {0, 0, bw, bh};
                g_area_idx = kAreaGlass;
            }
            LOG_UI("scan area set to %.1f,%.1f - %.1f,%.1f mm", g_sel.l, g_sel.t, g_sel.r, g_sel.b);
            g_drag_mode = 0;
        }
    }
}

void image_layout(const RectF& area, float iw, float ih, RectF& dst, float& scale)
{
    float m = 34.0f;
    float fit = std::max(0.01f, std::min((area.w - 2 * m) / iw, (area.h - 2 * m - 40.0f) / ih));
    scale = g_zoom > 0 ? g_zoom : fit;
    float w = iw * scale, h = ih * scale;
    dst = {area.cx() - w * 0.5f + g_pan_x, area.y + (area.h - 40.0f) * 0.5f - h * 0.5f + g_pan_y, w, h};
}

void draw_page_view(const RectF& area, float vis)
{
    Page* p = selected_page();
    if (!p) return;
    ensure_page_display(*p);
    gx.update_image(p->gi_display, p->display, (static_cast<std::uint64_t>(p->number) << 32) | p->version);
    RectF dst;
    float scale;
    image_layout(area, static_cast<float>(p->display.width), static_cast<float>(p->display.height), dst, scale);
    ui.push_clip(area);
    gx.shadow(dst, 10.0f, 24.0f, ui.t.shadow.alpha(1.1f), 10.0f);
    gx.draw_image(p->gi_display, dst, 1.0f, 10.0f, nullptr, scale < 2.5f);
    gx.stroke_rrect(dst.inset(0.5f), 10.0f, ui.t.border, 1.0f);
    ui.pop_clip();

    int fw = p->raw.width, fh = p->raw.height;
    if (p->adj.rotation % 180 != 0) std::swap(fw, fh);
    const wchar_t* mode = p->adj.black_white ? L"black & white" : (p->adj.grayscale || p->raw.channels == 1) ? L"grayscale" : L"color";
    wchar_t buf[200];
    swprintf(buf, 200, L"Scan %d, %d × %d pixels at %d dpi in %ls. Scroll to zoom, double-click to fit.",
             p->number, fw, fh, p->raw.dpi, mode);
    info_pill(area, buf);

    if (vis > 0.99f) {
        if (ui.hover("canvas.page", area)) {
            ui.set_cursor(ui.dragging("canvas.page") ? IDC_SIZEALL : IDC_HAND);
            if (ui.wheel != 0) {
                float ns = std::clamp(scale * (ui.wheel > 0 ? 1.25f : 0.8f), 0.02f, 16.0f);
                float ix = (ui.mx - dst.x) / scale, iy = (ui.my - dst.y) / scale;
                g_zoom = ns;
                float nw = p->display.width * ns, nh = p->display.height * ns;
                g_pan_x = ui.mx - ix * ns - (area.cx() - nw * 0.5f);
                g_pan_y = ui.my - iy * ns - (area.y + (area.h - 40.0f) * 0.5f - nh * 0.5f);
                ui.wheel = 0;
            }
            if (ui.dbl) { g_zoom = 0; g_pan_x = g_pan_y = 0; }
        }
        if (ui.press("canvas.page", area)) { g_drag_x = ui.mx; g_drag_y = ui.my; }
        if (ui.dragging("canvas.page")) {
            g_pan_x += ui.mx - g_drag_x;
            g_pan_y += ui.my - g_drag_y;
            g_drag_x = ui.mx;
            g_drag_y = ui.my;
        }
    }
}

void draw_scanning_view(const RectF& area)
{
    int pct = g_live_lines_total > 0 ? g_live_lines_done * 100 / g_live_lines_total : 0;
    if (!g_live.empty()) {
        RectF dst;
        float scale;
        float sz = g_zoom;
        float px = g_pan_x, py = g_pan_y;
        g_zoom = 0; g_pan_x = g_pan_y = 0;
        image_layout(area, static_cast<float>(g_live.width), static_cast<float>(g_live.height), dst, scale);
        g_zoom = sz; g_pan_x = px; g_pan_y = py;
        gx.shadow(dst, 10.0f, 24.0f, ui.t.shadow.alpha(1.1f), 10.0f);
        gx.fill_rrect(dst, 10.0f, ui.t.glass_strong);
        float frac = static_cast<float>(g_live_rows_done) / std::max(1, g_live.height);
        if (g_live_rows_done > 0) {
            sync_live_bitmap();
            RectF part = dst;
            part.h = dst.h * frac;
            RectF src{0, 0, static_cast<float>(g_live.width), static_cast<float>(g_live_rows_done)};
            gx.draw_image(g_gi_live, part, 1.0f, 0.0f, &src);
        }
        gx.stroke_rrect(dst.inset(0.5f), 10.0f, ui.t.border, 1.0f);
        scan_glow(dst, dst.y + dst.h * frac);
    } else {
        // warming up: a calm breathing glass sheet
        float br = 0.5f + 0.5f * std::sin(static_cast<float>(ui.time) * 2.4f);
        RectF r{area.cx() - 140.0f, area.cy() - 110.0f, 280.0f, 180.0f};
        ui.glass(r, 22.0f, 1.0f, true);
        gx.fill_rrect(r.inset(30.0f, 70.0f), 4.0f, ui.t.accent.alpha(0.25f + 0.35f * br));
        gx.text(L"Warming up the lamp", RectF{r.x, r.b() + 12.0f, r.w, 24.0f}, 14.0f, 600, ui.t.text, Align::Center);
    }
    wchar_t buf[120];
    if (g_live.empty()) swprintf(buf, 120, L"Calibrating for %d dpi. This takes a few seconds.", g_live_req.dpi);
    else swprintf(buf, 120, L"Scanning at %d dpi, %d%%. Press Esc to cancel.", g_live_req.dpi, pct);
    info_pill(area, buf);
    ui.request_frame();
}

void draw_canvas(const RectF& area)
{
    float vg = ui.anim("view.glass", g_view == View::Glass ? 1.0f : 0.0f, 10.0f);
    float vp = ui.anim("view.page", g_view == View::Page ? 1.0f : 0.0f, 10.0f);
    float vs = ui.anim("view.scan", g_view == View::Scanning ? 1.0f : 0.0f, 10.0f);
    auto layer = [&](float v, auto&& fn) {
        if (v < 0.01f) return;
        float s = 0.97f + 0.03f * v;
        gx.push_opacity(v);
        gx.set_transform(D2D1::Matrix3x2F::Scale(s, s, D2D1::Point2F(area.cx(), area.cy())));
        fn();
        gx.reset_transform();
        gx.pop_opacity();
    };
    layer(vg, [&] { draw_glass_view(area, vg); });
    layer(vp, [&] { draw_page_view(area, vp); });
    layer(vs, [&] { draw_scanning_view(area); });
}

// --------------------------------------------------------------- sidebar

float g_y;          // layout cursor
float g_x0, g_w;    // content column

void section_title(const std::wstring& title, const std::wstring& right = L"")
{
    g_y += 10.0f;
    gx.text(title, RectF{g_x0 + 2.0f, g_y, g_w, 22.0f}, 15.0f, 600, ui.t.text, Align::Start, Align::Center);
    if (!right.empty())
        gx.text(right, RectF{g_x0, g_y, g_w - 2.0f, 22.0f}, 12.0f, 500, ui.t.text2, Align::End, Align::Center);
    g_y += 30.0f;
}

void labeled_dropdown(const std::string& id, const std::wstring& label, const std::vector<std::wstring>& items,
                      int sel, std::function<void(int)> pick, bool enabled = true)
{
    gx.text(label, RectF{g_x0 + 2.0f, g_y, 100.0f, 34.0f}, 13.0f, 400, enabled ? ui.t.text : ui.t.text3, Align::Start,
            Align::Center);
    ui.dropdown(id, RectF{g_x0 + 104.0f, g_y, g_w - 104.0f, 34.0f}, items, sel, std::move(pick), enabled);
    g_y += 42.0f;
}

void adjustments_changed()
{
    if (!selected_page()) g_preview_display_valid = false;
    redraw();
}

void draw_sidebar(const RectF& panel)
{
    ui.glass(panel, 28.0f, 1.3f, false, 0.25f);
    RectF view = panel.inset(0.0f, 14.0f);
    float max_scroll = std::max(0.0f, g_side_content - view.h);
    if (ui.mouse_inside && panel.contains(ui.mx, ui.my) && ui.wheel != 0 && !ui.modal_open() && !ui.popover_open()) {
        g_side_target -= ui.wheel * 70.0f;
        g_side_scroll_time = ui.time;
        ui.wheel = 0;
    }
    g_side_target = std::clamp(g_side_target, 0.0f, max_scroll);
    g_side_scroll = ui.anim("side.scroll", g_side_target, 16.0f);

    ui.push_clip(view);
    g_x0 = panel.x + 18.0f;
    g_w = panel.w - 36.0f;
    g_y = view.y + 6.0f - g_side_scroll;
    const float y_start = g_y;

    // ---- header: app identity + scanner status
    RectF badge{g_x0, g_y, 40.0f, 40.0f};
    gx.shadow(badge, 12.0f, 8.0f, ui.t.accent.alpha(0.35f), 3.0f);
    gx.fill_rrect_hgrad(badge, 12.0f, ui.t.accent, ui.t.accent2);
    gx.fill_rrect_vgrad(RectF{badge.x, badge.y, badge.w, 22.0f}, 12.0f, Color::hex(0xFFFFFF, 0.35f), Color::hex(0xFFFFFF, 0.0f));
    gx.icon(Icon::Scan, badge.inset(9.0f), Color::hex(0xFFFFFF), 1.8f);
    gx.text(L"LiDE50 Scan", RectF{g_x0 + 52.0f, g_y - 1.0f, g_w - 52.0f, 22.0f}, 18.0f, 700, ui.t.text, Align::Start,
            Align::Center);
    Color dot = ui.t.text3;
    bool pulse = false;
    switch (g_scanner_ui) {
        case ScannerUi::Ready: dot = g_engine.busy() ? ui.t.accent : ui.t.success; pulse = g_engine.busy(); break;
        case ScannerUi::NeedsSetup: dot = ui.t.warning; break;
        case ScannerUi::Connecting: case ScannerUi::Searching: dot = ui.t.accent; pulse = true; break;
        case ScannerUi::Error: dot = ui.t.danger; break;
        default: break;
    }
    std::wstring status = g_scanner_detail;
    if (g_scanner_ui == ScannerUi::Ready) {
        status = g_simulate ? L"Ready (simulated scanner)" : g_fake_usb ? L"Ready (test device)" : L"Canon LiDE 50 ready";
        if (g_engine.busy()) status = g_status.empty() ? L"Working" : g_status;
        else if (!g_status.empty()) status = g_status;
    }
    float pa = pulse ? 0.45f + 0.55f * (0.5f + 0.5f * std::sin(static_cast<float>(ui.time) * 4.0f)) : 1.0f;
    if (pulse) ui.request_frame();
    gx.fill_ellipse(g_x0 + 56.0f, g_y + 30.0f, 4.0f, 4.0f, dot.alpha(pa));
    gx.text(status, RectF{g_x0 + 66.0f, g_y + 21.0f, g_w - 66.0f, 18.0f}, 12.5f, 500, ui.t.text2, Align::Start,
            Align::Center);
    g_y += 52.0f;

    if (g_scanner_ui == ScannerUi::NeedsSetup || g_scanner_ui == ScannerUi::NotFound ||
        g_scanner_ui == ScannerUi::Error) {
        std::wstring msg = g_scanner_ui == ScannerUi::NeedsSetup
                               ? L"Windows needs your permission once so LiDE50 Scan can talk to the scanner."
                           : g_scanner_ui == ScannerUi::NotFound
                               ? L"Connect the scanner with its USB cable. It's powered over USB, so there's no power cable."
                               : (g_status.empty() ? std::wstring(L"The scanner couldn't be opened.") : g_status);
        float th = gx.text_height(msg, 12.5f, 400, g_w - 28.0f);
        float gh = th + 28.0f + (g_scanner_ui == ScannerUi::NeedsSetup ? 46.0f : 0.0f);
        RectF grp{g_x0, g_y, g_w, gh};
        ui.group(grp, 14.0f);
        gx.text(msg, RectF{grp.x + 14.0f, grp.y + 14.0f, g_w - 28.0f, th + 2.0f}, 12.5f, 400, ui.t.text2,
                Align::Start, Align::Start, true);
        if (g_scanner_ui == ScannerUi::NeedsSetup) {
            if (ui.button("enable", RectF{grp.x + 14.0f, grp.b() - 48.0f, g_w - 28.0f, 36.0f},
                          g_setup_running ? L"Waiting for Windows" : L"Enable scanner access", Icon::Shield, true,
                          ButtonStyle::Primary, !g_setup_running)) {
                LOG_UI("clicked 'Enable scanner access'");
                ui.defer([] { run_access_setup(false); });
            }
        }
        g_y += gh + 8.0f;
    }

    // ---- scan settings
    section_title(L"Scan");
    {
        int sel = mode_of(current_adj(), selected_page() ? selected_page()->raw.channels : 3);
        if (ui.segmented("mode", RectF{g_x0, g_y, g_w, 34.0f}, {L"Color", L"Grayscale", L"Black & white"}, sel)) {
            Adjustments& a = current_adj();
            a.grayscale = sel == 1;
            a.black_white = sel == 2;
            if (!selected_page()) ini_set(L"mode", sel);
            LOG_UI("color mode set to %d", sel);
            adjustments_changed();
        }
        g_y += 44.0f;
    }
    {
        std::vector<std::wstring> res;
        for (int r : kResolutions) {
            std::wstring s = std::to_wstring(r) + L" dpi";
            if (r == 75) s += L", screen";
            if (r == 300) s += L", documents";
            if (r == 600) s += L", photos";
            if (r == 2400) s += L", slowest";
            res.push_back(s);
        }
        labeled_dropdown("dpi", L"Resolution", res, g_dpi_idx, [](int i) {
            g_dpi_idx = i;
            ini_set(L"dpi", kResolutions[i]);
            LOG_UI("resolution set to %d dpi", kResolutions[i]);
        });
        std::vector<std::wstring> areas;
        for (auto& a : kAreas) areas.push_back(a.name);
        labeled_dropdown("area", L"Area", areas, g_area_idx, [](int i) {
            LOG_UI("scan area preset %d", i);
            if (i != kAreaCustom) apply_area_preset(i);
            else g_area_idx = i;
            show_glass();
        });
    }
    if (ui.button("detect", RectF{g_x0, g_y, g_w, 34.0f}, L"Find the page on the preview", Icon::Wand, true,
                  ButtonStyle::Glass, g_have_preview && (!g_engine.busy() || g_engine.parking()),
                  L"Sets the scan area to what's on the glass")) {
        int x, y, w, h;
        if (detect_content(g_preview, x, y, w, h)) {
            double mmpp = 25.4 / std::max(1, g_preview.dpi);
            g_sel = {g_preview_area.l + x * mmpp, g_preview_area.t + y * mmpp, g_preview_area.l + (x + w) * mmpp,
                     g_preview_area.t + (y + h) * mmpp};
            g_area_idx = kAreaCustom;
            show_glass();
            ui.toast(L"Scan area set to the page on the glass", 1);
            LOG_UI("auto-detected page: %.1f,%.1f - %.1f,%.1f mm", g_sel.l, g_sel.t, g_sel.r, g_sel.b);
        } else {
            ui.toast(L"No page found on the preview. Drag on the glass instead.", 2);
        }
    }
    g_y += 42.0f;
    {
        double w = g_sel.r - g_sel.l, h = g_sel.b - g_sel.t;
        int dpi = ui_dpi();
        long long pw = static_cast<long long>(w / 25.4 * dpi), ph = static_cast<long long>(h / 25.4 * dpi);
        double mb = pw * ph * (next_scan_mode() == ColorMode::Color ? 3 : 1) / 1048576.0;
        wchar_t buf[200];
        swprintf(buf, 200, L"%.0f × %.0f mm at %d dpi gives %lld × %lld pixels, about %.0f MB.", w, h, dpi,
                 pw, ph, std::max(1.0, mb));
        float th = gx.text_height(buf, 12.0f, 400, g_w - 4.0f);
        gx.text(buf, RectF{g_x0 + 2.0f, g_y, g_w - 4.0f, th + 2.0f}, 12.0f, 400, ui.t.text2, Align::Start, Align::Start,
                true);
        g_y += th + 10.0f;
    }

    // ---- adjustments
    Page* page = selected_page();
    section_title(L"Adjust", page ? L"Scan " + std::to_wstring(page->number) : L"Next scan");
    {
        Adjustments& a = current_adj();
        bool bw = a.black_white;
        bool color = !a.grayscale && !a.black_white && (!page || page->raw.channels == 3);
        auto num = [](int v, bool sign) {
            wchar_t b[16];
            swprintf(b, 16, sign ? L"%+d" : L"%d", v);
            return std::wstring(b);
        };
        RectF row{g_x0 + 2.0f, g_y, g_w - 4.0f, 44.0f};
        bool ch = false;
        ch |= ui.slider("brightness", row, L"Brightness", a.brightness, -100, 100, 0, num(a.brightness, true), true, true);
        row.y += 50.0f;
        ch |= ui.slider("contrast", row, L"Contrast", a.contrast, -100, 100, 0, num(a.contrast, true), true, true);
        row.y += 50.0f;
        ch |= ui.slider("saturation", row, L"Saturation", a.saturation, -100, 100, 0, num(a.saturation, true), color, true);
        row.y += 50.0f;
        wchar_t gb[16];
        swprintf(gb, 16, L"%.2f", a.gamma / 100.0);
        ch |= ui.slider("gamma", row, L"Midtones", a.gamma, 30, 300, 100, gb);
        row.y += 50.0f;
        ch |= ui.slider("sharpness", row, L"Sharpness", a.sharpness, 0, 100, 0, num(a.sharpness, false));
        row.y += 50.0f;
        ch |= ui.slider("denoise", row, L"Noise reduction", a.denoise, 0, 100, 0, num(a.denoise, false));
        row.y += 50.0f;
        ch |= ui.slider("threshold", row, L"Black & white threshold", a.threshold, 1, 254, 128, num(a.threshold, false), bw);
        row.y += 54.0f;
        g_y = row.y;
        if (ch) adjustments_changed();

        RectF grp{g_x0, g_y, g_w, 88.0f};
        ui.group(grp, 14.0f);
        if (ui.toggle("autolevels", RectF{grp.x + 14.0f, grp.y + 2.0f, grp.w - 26.0f, 42.0f}, L"Auto levels", a.auto_levels)) {
            LOG_UI("auto levels %s", a.auto_levels ? "on" : "off");
            adjustments_changed();
        }
        gx.line(grp.x + 14.0f, grp.y + 44.0f, grp.r() - 14.0f, grp.y + 44.0f, ui.t.border, 1.0f, false);
        if (ui.toggle("invert", RectF{grp.x + 14.0f, grp.y + 44.0f, grp.w - 26.0f, 42.0f}, L"Invert colors", a.invert)) {
            LOG_UI("invert %s", a.invert ? "on" : "off");
            adjustments_changed();
        }
        g_y += 98.0f;

        float bx = g_x0;
        if (ui.icon_button("rotl", RectF{bx, g_y, 38.0f, 38.0f}, Icon::RotateLeft, L"Rotate left")) {
            a.rotation = (a.rotation + 270) % 360; g_zoom = 0; g_pan_x = g_pan_y = 0; LOG_UI("rotate left"); adjustments_changed();
        }
        if (ui.icon_button("rotr", RectF{bx + 46.0f, g_y, 38.0f, 38.0f}, Icon::RotateRight, L"Rotate right")) {
            a.rotation = (a.rotation + 90) % 360; g_zoom = 0; g_pan_x = g_pan_y = 0; LOG_UI("rotate right"); adjustments_changed();
        }
        if (ui.icon_button("mirror", RectF{bx + 92.0f, g_y, 38.0f, 38.0f}, Icon::Mirror, L"Mirror")) {
            a.flip_h = !a.flip_h; LOG_UI("mirror"); adjustments_changed();
        }
        if (ui.button("reset", RectF{g_x0 + g_w - 156.0f, g_y + 2.0f, 66.0f, 34.0f}, L"Reset", Icon::Reset, false,
                      ButtonStyle::Plain, true, L"Undo all adjustments")) {
            Adjustments fresh;
            fresh.grayscale = a.grayscale;
            fresh.black_white = a.black_white;
            a = fresh;
            LOG_UI("adjustments reset");
            adjustments_changed();
        }
        if (ui.button("applyall", RectF{g_x0 + g_w - 88.0f, g_y + 2.0f, 88.0f, 34.0f}, L"Apply to all", Icon::Layers,
                      false, ButtonStyle::Plain, page && g_pages.size() > 1, L"Use these adjustments on every scan")) {
            for (auto& pg : g_pages) {
                Adjustments na = page->adj;
                if (pg->raw.channels == 1) na.grayscale = na.grayscale || !na.black_white;
                pg->adj = na;
            }
            LOG_UI("adjustments applied to all scans");
            ui.toast(L"Adjustments applied to all scans", 1);
        }
        g_y += 50.0f;
    }

    // ---- saving
    section_title(L"Save");
    labeled_dropdown("format", L"File type", kFormatNames, g_format_idx, [](int i) {
        g_format_idx = i;
        ini_set(L"format", i);
        LOG_UI("file type set to %ls", kFormatNames[static_cast<size_t>(i)].c_str());
    });
    {
        OutFormat f = ui_format();
        bool q = f == OutFormat::JPEG || f == OutFormat::DOCX || (f == OutFormat::PDF && !g_lossless);
        int qv = g_quality;
        if (ui.slider("quality", RectF{g_x0 + 2.0f, g_y, g_w - 4.0f, 44.0f}, L"Image quality", qv, 30, 100, 90,
                      std::to_wstring(qv), q)) {
            g_quality = qv;
            ini_set(L"jpeg_quality", qv);
        }
        g_y += 52.0f;
        if (f == OutFormat::PDF) {
            RectF grp{g_x0, g_y, g_w, 46.0f};
            ui.group(grp, 14.0f);
            bool l = g_lossless;
            if (ui.toggle("lossless", RectF{grp.x + 14.0f, grp.y + 2.0f, grp.w - 26.0f, 42.0f}, L"Lossless PDF, larger file", l)) {
                g_lossless = l;
                ini_set(L"lossless", l ? 1 : 0);
                LOG_UI("lossless PDF %s", l ? "on" : "off");
            }
            g_y += 56.0f;
        }
        bool can = !g_pages.empty() && !g_saving;
        float bw = (g_w - 10.0f) * 0.5f;
        if (ui.button("savepage", RectF{g_x0, g_y, bw, 40.0f}, L"Save scan", Icon::Save, true, ButtonStyle::Primary, can,
                      L"Ctrl+S"))
            ui.defer([] { save_pages(false); });
        if (ui.button("saveall", RectF{g_x0 + bw + 10.0f, g_y, bw, 40.0f},
                      format_multipage(f) ? L"Save all as one" : L"Save all", Icon::Layers, false, ButtonStyle::Glass,
                      can, L"Ctrl+Shift+S"))
            ui.defer([] { save_pages(true); });
        g_y += 52.0f;
    }

    g_side_content = g_y - y_start + 16.0f;
    ui.pop_clip();

    // scroll indicator
    if (max_scroll > 1.0f) {
        bool show = (ui.time - g_side_scroll_time < 1.2) || (panel.contains(ui.mx, ui.my) && ui.mouse_inside);
        float a = ui.anim("side.ind", show ? 1.0f : 0.0f, 8.0f);
        if (ui.time - g_side_scroll_time < 1.3) ui.request_frame();
        float th = std::max(30.0f, view.h * view.h / g_side_content);
        float ty = view.y + (view.h - th) * (g_side_scroll / max_scroll);
        gx.fill_rrect(RectF{panel.r() - 8.0f, ty, 4.0f, th}, 2.0f, ui.t.text.alpha(0.25f * a));
    }
}

// --------------------------------------------------------------- toolbar

void draw_toolbar(const RectF& center)
{
    const bool busy = g_engine.busy();
    const bool ready = g_engine.connected() && !busy;
    float b = ui.anim("tb.busy", busy ? 1.0f : 0.0f, 12.0f);
    const float h = 56.0f, pad = 8.0f, gap = 8.0f, ib = 40.0f;
    float left_w = lerpf(120.0f + gap + 132.0f, 250.0f + gap + 108.0f, b);
    float right_w = 4 * ib + 3 * 6.0f;
    float w = pad + left_w + 18.0f + right_w + pad;
    RectF bar{center.cx() - w * 0.5f, center.y, w, h};
    ui.glass(bar, h * 0.5f, 1.2f, false, 0.5f);

    float x = bar.x + pad, y = bar.y + pad, bh = h - 2 * pad;
    if (b < 0.99f) {
        float fade = (1.0f - b) * (1.0f - b);
        gx.push_opacity(fade);
        if (ui.button("tb.preview", RectF{x, y, 120.0f, bh}, L"Preview", Icon::Preview, true, ButtonStyle::Glass,
                      ready && b < 0.5f, L"Quick look at the whole glass (F5)"))
            ui.defer([] { start_scan(true); });
        if (ui.button("tb.scan", RectF{x + 120.0f + gap, y, 132.0f, bh}, L"Scan", Icon::Scan, true,
                      ButtonStyle::Primary, ready && b < 0.5f, L"Scan the selected area (F6)"))
            ui.defer([] { start_scan(false); });
        gx.pop_opacity();
    }
    if (b > 0.01f) {
        gx.push_opacity(b * b);
        RectF pc{x, y, 250.0f, bh};
        gx.fill_rrect(pc, bh * 0.5f, ui.t.track);
        const bool parking = g_engine.parking();
        float frac = parking ? 1.0f
                     : g_live_lines_total > 0 ? static_cast<float>(g_live_lines_done) / g_live_lines_total : 0.0f;
        float fa = ui.anim("tb.prog", frac, 8.0f);
        if ((parking || g_live_lines_total > 0) && fa > 0.005f) {
            RectF fill{pc.x, pc.y, std::max(bh, pc.w * fa), pc.h};
            gx.fill_rrect_hgrad(fill, bh * 0.5f, ui.t.accent, ui.t.accent2);
            gx.fill_rrect_vgrad(RectF{fill.x, fill.y, fill.w, fill.h * 0.55f}, bh * 0.5f, Color::hex(0xFFFFFF, 0.35f),
                                Color::hex(0xFFFFFF, 0.0f));
        }
        float sx = pc.x + std::fmod(static_cast<float>(ui.time) * 160.0f, pc.w + 120.0f) - 60.0f;
        gx.fill_rrect_radial(pc, bh * 0.5f, sx, pc.cy(), 70.0f, Color::hex(0xFFFFFF, 0.28f), Color::hex(0xFFFFFF, 0.0f));
        gx.stroke_rrect_vgrad(pc.inset(0.6f), bh * 0.5f, ui.t.rim_top, ui.t.rim_bottom, 1.0f);
        std::wstring label;
        int pct = static_cast<int>(frac * 100.0f + 0.5f);
        if (parking) label = L"Returning scan head";
        else if (g_live_lines_total <= 0) label = L"Calibrating";
        else label = (g_live_preview ? L"Previewing " : L"Scanning ") + std::to_wstring(pct) + L"%";
        Color lc = ((parking || g_live_lines_total > 0) && fa > 0.5f) ? ui.t.on_accent : ui.t.text;
        gx.text(label, pc, 13.0f, 600, lc, Align::Center, Align::Center);
        if (ui.button("tb.cancel", RectF{x + 250.0f + gap, y, 108.0f, bh}, L"Cancel", Icon::Cancel, true,
                      ButtonStyle::Glass, busy && !parking && b > 0.5f, L"Stop the scan (Esc)")) {
            g_engine.cancel();
            set_status(L"Cancelling");
            LOG_UI("clicked 'Cancel'");
        }
        gx.pop_opacity();
        ui.request_frame();
    }
    float dx = x + left_w + 9.0f;
    gx.line(dx, bar.y + 14.0f, dx, bar.b() - 14.0f, ui.t.border, 1.0f, false);
    float ix = dx + 9.0f;
    RectF r1{ix, y, ib, bh}, r2{ix + ib + 6.0f, y, ib, bh}, r3{ix + 2 * (ib + 6.0f), y, ib, bh}, r4{ix + 3 * (ib + 6.0f), y, ib, bh};
    if (ui.icon_button("tb.glass", r1, Icon::Glass, L"Show the scanner glass (F4)",
                       g_view != View::Glass && (!busy || g_engine.parking()), false)) {
        LOG_UI("clicked 'Show glass'");
        show_glass();
    }
    if (ui.icon_button("tb.fit", r2, Icon::Fit, L"Fit to window", g_view == View::Page, false)) {
        g_zoom = 0;
        g_pan_x = g_pan_y = 0;
    }
    if (ui.icon_button("tb.theme", r3, g_dark ? Icon::Sun : Icon::Moon, g_dark ? L"Light mode" : L"Dark mode", true, false))
        set_theme(!g_dark);
    if (ui.icon_button("tb.more", r4, Icon::More, L"More", true, false)) {
        RectF anchor = r4;
        ui.defer([anchor] { open_more_menu(anchor); });
    }
}

// ------------------------------------------------------------ scans list

void draw_scans(const RectF& panel)
{
    ui.glass(panel, 28.0f, 1.3f, false, 0.25f);
    gx.text(L"Scans", RectF{panel.x + 20.0f, panel.y + 16.0f, 120.0f, 26.0f}, 16.0f, 700, ui.t.text, Align::Start,
            Align::Center);
    if (!g_pages.empty()) {
        std::wstring n = std::to_wstring(g_pages.size());
        float nw = std::max(24.0f, gx.text_width(n, 12.0f, 600) + 14.0f);
        RectF badge{panel.r() - 20.0f - nw, panel.y + 18.0f, nw, 22.0f};
        gx.fill_rrect(badge, 11.0f, ui.t.accent.alpha(0.16f));
        gx.text(n, badge, 12.0f, 700, ui.t.accent, Align::Center, Align::Center);
    }
    RectF view{panel.x, panel.y + 52.0f, panel.w, panel.h - 52.0f - 64.0f};
    float max_scroll = std::max(0.0f, g_list_content - view.h);
    if (ui.mouse_inside && view.contains(ui.mx, ui.my) && ui.wheel != 0 && !ui.modal_open() && !ui.popover_open()) {
        g_list_target -= ui.wheel * 80.0f;
        g_list_scroll_time = ui.time;
        ui.wheel = 0;
    }
    if (g_list_target < 1e8f) g_list_target = std::clamp(g_list_target, 0.0f, max_scroll);
    g_list_scroll = ui.anim("list.scroll", std::min(g_list_target, max_scroll), 14.0f);

    if (g_pages.empty()) {
        RectF ic{panel.cx() - 22.0f, view.y + view.h * 0.32f, 44.0f, 44.0f};
        gx.icon(Icon::Layers, ic, ui.t.text3, 1.5f);
        gx.text(L"Finished scans show up here.", RectF{panel.x + 20.0f, ic.b() + 10.0f, panel.w - 40.0f, 40.0f}, 12.5f,
                500, ui.t.text2, Align::Center, Align::Start, true);
    }

    ui.push_clip(view);
    float y = view.y + 4.0f - g_list_scroll;
    const float cw = panel.w - 32.0f;
    for (size_t i = 0; i < g_pages.size(); ++i) {
        Page& p = *g_pages[i];
        ensure_page_display(p);
        float iw = static_cast<float>(p.thumb.width), ih = static_cast<float>(p.thumb.height);
        float s = std::min((cw - 16.0f) / iw, 190.0f / ih);
        float tw = iw * s, th = ih * s;
        RectF card{panel.x + 16.0f, y, cw, th + 16.0f + 30.0f};
        std::string id = "card." + std::to_string(p.number);
        bool h = ui.hover(id, card);
        if (h) ui.set_cursor(IDC_HAND);
        ui.press(id, card);
        bool sel = g_view == View::Page && static_cast<int>(i) == g_selected;
        if (ui.is_active(id) && ui.released && h) {
            LOG_UI("selected scan %d", p.number);
            show_page(static_cast<int>(i));
        }
        float hv = ui.anim(id + ".h", h ? 1.0f : 0.0f, 14.0f);
        float sv = ui.anim(id + ".s", sel ? 1.0f : 0.0f, 14.0f);
        // new scans land in the list with a small spring
        if (!ui.has_spring(id + ".land") && ui.time - p.born < 1.0) ui.set_spring(id + ".land", 0.0f);
        float land = ui.spring(id + ".land", 1.0f, 170.0f, 13.0f);
        RectF cr = card.scaled_about_center((0.82f + 0.18f * land) * (1.0f + 0.015f * hv));
        gx.push_opacity(std::clamp(land * 1.3f, 0.0f, 1.0f));
        if (sv > 0.01f) gx.shadow(cr, 16.0f, 10.0f, ui.t.accent.alpha(0.35f * sv), 2.0f);
        ui.glass(cr, 16.0f, 0.4f + 0.5f * hv, true, hv);
        RectF img{cr.cx() - tw * 0.5f, cr.y + 8.0f, tw, th};
        gx.update_image(p.gi_thumb, p.thumb, (static_cast<std::uint64_t>(p.number) << 32) | p.version);
        gx.draw_image(p.gi_thumb, img, 1.0f, 10.0f);
        gx.stroke_rrect(img.inset(0.5f), 10.0f, ui.t.border, 1.0f);
        gx.text(L"Scan " + std::to_wstring(p.number), RectF{cr.x + 12.0f, img.b() + 4.0f, cr.w * 0.5f, 24.0f}, 12.5f, 600,
                ui.t.text, Align::Start, Align::Center);
        gx.text(std::to_wstring(p.raw.dpi) + L" dpi", RectF{cr.cx(), img.b() + 4.0f, cr.w * 0.5f - 12.0f, 24.0f}, 11.5f,
                500, ui.t.text2, Align::End, Align::Center);
        if (sv > 0.01f) gx.stroke_rrect(cr.inset(-1.5f), 17.5f, ui.t.accent.alpha(sv), 2.5f);
        gx.pop_opacity();
        y += card.h + 12.0f;
    }
    g_list_content = y + g_list_scroll - view.y + 4.0f;
    ui.pop_clip();
    if (g_list_target > 1e8f) g_list_target = std::max(0.0f, g_list_content - view.h);

    bool has = selected_page() != nullptr && !g_saving;
    float by = panel.b() - 54.0f, bw = 40.0f;
    float bx = panel.cx() - (3 * bw + 2 * 10.0f) * 0.5f;
    if (ui.icon_button("list.up", RectF{bx, by, bw, bw}, Icon::Up, L"Move up", has && g_selected > 0))
        move_selected_page(-1);
    if (ui.icon_button("list.down", RectF{bx + bw + 10.0f, by, bw, bw}, Icon::Down, L"Move down",
                       has && g_selected + 1 < static_cast<int>(g_pages.size())))
        move_selected_page(1);
    if (ui.icon_button("list.del", RectF{bx + 2 * (bw + 10.0f), by, bw, bw}, Icon::Trash, L"Delete scan (Del)", has))
        delete_selected_page();
}

// ----------------------------------------------------------------- frame

void render()
{
    if (g_rendering) return;
    g_rendering = true;
    if (!gx.begin()) { g_rendering = false; return; }
    ui.begin_frame(now_seconds());
    float th = ui.anim("theme", g_dark ? 1.0f : 0.0f, 7.0f);
    ui.t = mix(theme_light(), theme_dark(), th);

    const float W = gx.width(), H = gx.height(), M = 14.0f;
    ui.background(RectF{0, 0, W, H}, g_motion && g_active && !IsIconic(g_main));
    float side_w = std::clamp(W * 0.25f, 300.0f, 340.0f);
    float list_w = std::clamp(W * 0.15f, 180.0f, 220.0f);
    RectF side{M, M, side_w, H - 2 * M};
    RectF scans{W - M - list_w, M, list_w, H - 2 * M};
    RectF center{side.r() + M, M, scans.x - M - (side.r() + M), H - 2 * M};
    RectF canvas{center.x, center.y + 64.0f, center.w, center.h - 64.0f};
    ui.toast_cx = canvas.cx();

    draw_canvas(canvas);
    draw_sidebar(side);
    draw_scans(scans);
    draw_toolbar(center);

    ui.end_frame();
    gx.end();
    g_rendering = false;

    for (auto& fn : ui.take_deferred()) fn();
    // 60 fps while something animates, 30 fps when only the background drifts
    UINT interval = ui.wants_frame() ? 15 : ui.wants_slow_frame() ? 33 : 0;
    static UINT current = 0;
    if (interval != current) {
        if (interval) SetTimer(g_main, TIMER_FRAME, interval, nullptr);
        else KillTimer(g_main, TIMER_FRAME);
        current = interval;
    }
}

// ----------------------------------------------------------------- window

float dpi_for(HWND hwnd)
{
    using Fn = UINT(WINAPI*)(HWND);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn) return static_cast<float>(fn(hwnd));
    HDC dc = GetDC(nullptr);
    float d = static_cast<float>(GetDeviceCaps(dc, LOGPIXELSX));
    ReleaseDC(nullptr, dc);
    return d;
}

void set_mouse(LPARAM lp)
{
    ui.mx = GET_X_LPARAM(lp) * 96.0f / g_dpi;
    ui.my = GET_Y_LPARAM(lp) * 96.0f / g_dpi;
}

LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_CREATE: {
            g_main = hwnd;
            g_dpi = dpi_for(hwnd);
            if (!gx.init(hwnd)) {
                LOG_ERROR("Direct2D could not start");
                return -1;
            }
            gx.set_dpi(g_dpi);
            RECT rc;
            GetClientRect(hwnd, &rc);
            gx.resize(static_cast<UINT>(rc.right), static_cast<UINT>(rc.bottom));
            apply_title_bar_theme();
            DEV_BROADCAST_DEVICEINTERFACE_W filter{};
            filter.dbcc_size = sizeof(filter);
            filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
            filter.dbcc_classguid = GUID_DEVINTERFACE_USB_DEVICE;
            RegisterDeviceNotificationW(hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_ACTIVATEAPP:
            g_active = wp != FALSE;
            redraw();
            return 0;
        case WM_PAINT:
            render();
            ValidateRect(hwnd, nullptr);
            return 0;
        case WM_SIZE:
            gx.resize(LOWORD(lp), HIWORD(lp));
            render();
            return 0;
        case WM_DPICHANGED: {
            g_dpi = static_cast<float>(HIWORD(wp));
            gx.set_dpi(g_dpi);
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize = {static_cast<LONG>(1060 * g_dpi / 96), static_cast<LONG>(660 * g_dpi / 96)};
            return 0;
        }
        case WM_MOUSEMOVE: {
            set_mouse(lp);
            if (!ui.mouse_inside) {
                ui.mouse_inside = true;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            redraw();
            return 0;
        }
        case WM_MOUSELEAVE:
            if (!ui.down) ui.mouse_inside = false;
            redraw();
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            set_mouse(lp);
            ui.mouse_inside = true;
            ui.down = true;
            ui.pressed = true;
            if (msg == WM_LBUTTONDBLCLK) ui.dbl = true;
            SetCapture(hwnd);
            // handle the press right away at the press position, so a quick
            // press-drag-release is never collapsed into a single frame
            frame_now();
            return 0;
        case WM_LBUTTONUP:
            set_mouse(lp);
            if (ui.down) frame_now();  // let any drag reach the release position first
            ui.down = false;
            ui.released = true;
            ReleaseCapture();
            frame_now();
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            ui.mx = pt.x * 96.0f / g_dpi;
            ui.my = pt.y * 96.0f / g_dpi;
            ui.wheel += GET_WHEEL_DELTA_WPARAM(wp) / static_cast<float>(WHEEL_DELTA);
            redraw();
            return 0;
        }
        case WM_KEYDOWN:
            ui.key = static_cast<int>(wp);
            if (!ui.modal_open() && !ui.popover_open()) {
                if (wp == VK_ESCAPE && g_engine.busy() && !g_engine.parking()) {
                    g_engine.cancel();
                    set_status(L"Cancelling");
                    LOG_UI("pressed Esc to cancel");
                }
                if (wp == VK_DELETE) delete_selected_page();
            }
            redraw();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                SetCursor(LoadCursor(nullptr, ui.cursor()));
                return TRUE;
            }
            break;
        case WM_COMMAND:
            if (ui.modal_open()) return 0;
            switch (LOWORD(wp)) {
                case ID_PREVIEW: start_scan(true); break;
                case ID_SCAN: start_scan(false); break;
                case ID_SAVE_PAGE: save_pages(false); break;
                case ID_SAVE_ALL: save_pages(true); break;
                case ID_SHOWGLASS: if (!g_engine.busy() || g_engine.parking()) show_glass(); break;
            }
            return 0;
        case WM_TIMER:
            if (wp == TIMER_FRAME) {
                redraw();
            } else if (wp == TIMER_LIVE) {
                update_live();
                redraw();
            } else if (wp == TIMER_USBCHECK) {
                KillTimer(hwnd, TIMER_USBCHECK);
                check_scanner();
            }
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED) {
                if (wp != DBT_DEVNODES_CHANGED)
                    LOG_INFO("Windows reported a USB device %s", wp == DBT_DEVICEARRIVAL ? "being plugged in" : "being removed");
                SetTimer(hwnd, TIMER_USBCHECK, 800, nullptr);
            }
            return TRUE;
        case WM_SETTINGCHANGE:
            if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 &&
                ini_int(L"theme", 0) == 0) {
                g_dark = system_prefers_dark();
                apply_title_bar_theme();
                redraw();
            }
            return 0;
        case WM_ENGINE: {
            std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lp));
            on_engine_event(static_cast<EngineEvent>(wp), *text);
            redraw();
            return 0;
        }
        case WM_SETUP_DONE: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            g_setup_running = false;
            g_status.clear();
            logw(wp ? "INFO" : "WARN", L"scanner access setup result: " + *text);
            ui.alert(wp ? L"Scanner access is set up" : L"Setup didn't finish", *text, {L"OK"}, nullptr);
            g_usb_state = UsbAccessState::NotConnected;  // force a reconnect
            check_scanner();
            redraw();
            return 0;
        }
        case WM_SAVE_DONE: {
            std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
            g_saving = false;
            g_status.clear();
            if (wp) ui.toast(*text, 1);
            else ui.alert(L"The file wasn't saved", *text, {L"OK"}, nullptr);
            redraw();
            return 0;
        }
        case WM_CLOSE:
            if (!g_force_close && !g_pages.empty()) {
                ui.alert(L"Close LiDE50 Scan?", L"Scans you haven't saved will be lost.", {L"Keep working", L"Close"},
                         [](int b) {
                             if (b == 1) {
                                 g_force_close = true;
                                 PostMessageW(g_main, WM_CLOSE, 0, 0);
                             }
                         }, 0, 0);
                redraw();
                return 0;
            }
            if (g_engine.busy()) g_engine.cancel();
            {
                WINDOWPLACEMENT wpl{};
                wpl.length = sizeof(wpl);
                GetWindowPlacement(hwnd, &wpl);
                ini_set(L"maximized", wpl.showCmd == SW_SHOWMAXIMIZED);
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    g_inst = inst;
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qp0);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if ((a == L"--enable-access" || a == L"--disable-access") && i + 1 < argc) {
            std::wstring fallback;
            PWSTR p = nullptr;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) fallback = std::wstring(p) + L"\\LiDE50Scan";
            CoTaskMemFree(p);
            log_open(exe_dir(), fallback);
            install_crash_handlers();
            LOG_INFO("elevated helper: %s scanner access", a == L"--disable-access" ? "removing" : "setting up");
            int rc = run_elevated_helper(a == L"--disable-access", argv[i + 1]);
            LOG_INFO("elevated helper finished with code %d", rc);
            return rc;
        }
        if (a == L"--simulate") g_simulate = true;
        if (a == L"--log") g_log = true;
        if (a == L"--fake-usb") g_fake_usb = true;
    }
    LocalFree(argv);

    std::wstring dir = exe_dir();
    if (dir_writable(dir)) {
        g_data_dir = dir + L"\\LiDE50Scan-data";
    } else {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) g_data_dir = std::wstring(p) + L"\\LiDE50Scan";
        CoTaskMemFree(p);
    }
    CreateDirectoryW(g_data_dir.c_str(), nullptr);
    g_ini = g_data_dir + L"\\settings.ini";

    if (!GetStdHandle(STD_ERROR_HANDLE) || GetStdHandle(STD_ERROR_HANDLE) == INVALID_HANDLE_VALUE) {
        freopen("NUL", "w", stdout);
        freopen("NUL", "w", stderr);
    }
    log_open(dir, g_data_dir);
    install_crash_handlers();
    log_system_info(g_data_dir, GetCommandLineW());
    if (g_log) lide50_set_usb_trace(1);
    if (g_fake_usb) g_simulate = false;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int theme = ini_int(L"theme", 0);  // 0 = follow Windows, 1 = light, 2 = dark
    g_dark = theme == 2 || (theme == 0 && system_prefers_dark());
    g_motion = ini_int(L"motion", 1) != 0;
    BOOL anims = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anims, 0);
    g_ui.reduce_motion = !anims;
    int mode = std::clamp(ini_int(L"mode", 0), 0, 2);
    g_default_adj.grayscale = mode == 1;
    g_default_adj.black_white = mode == 2;
    int dpi = ini_int(L"dpi", 300);
    for (int i = 0; i < kResCount; ++i) if (kResolutions[i] == dpi) g_dpi_idx = i;
    g_area_idx = ini_int(L"area", kAreaGlass);
    if (g_area_idx == kAreaCustom || g_area_idx < 0 || g_area_idx >= kAreaCount) g_area_idx = kAreaGlass;
    g_sel = {0, 0, 216, 297};
    g_format_idx = std::clamp(ini_int(L"format", 0), 0, 5);
    g_quality = std::clamp(ini_int(L"jpeg_quality", 90), 30, 100);
    g_lossless = ini_int(L"lossless", 0) != 0;
    LOG_INFO("appearance: %s theme, background motion %s, system animations %s", g_dark ? "dark" : "light",
             g_motion ? "on" : "off", anims ? "on" : "off");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = kMainClass;
    wc.hbrBackground = nullptr;
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    RegisterClassExW(&wc);

    std::wstring title = L"LiDE50 Scan";
    if (g_simulate) title += L"  (simulation, no scanner needed)";
    if (g_log) title += L"  (detailed log on)";
    if (g_fake_usb) title += L"  (fake USB test device)";
    HDC sdc = GetDC(nullptr);
    float sdpi = static_cast<float>(GetDeviceCaps(sdc, LOGPIXELSX));
    ReleaseDC(nullptr, sdc);
    HWND hwnd = CreateWindowExW(0, kMainClass, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                static_cast<int>(1360 * sdpi / 96), static_cast<int>(880 * sdpi / 96), nullptr,
                                nullptr, inst, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"LiDE50 Scan could not create its window (Direct2D unavailable).", kAppName, MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, ini_int(L"maximized", 0) ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(hwnd);
    LOG_INFO("main window created (%.0f dpi)", g_dpi);

    ACCEL acc[] = {
        {FCONTROL | FVIRTKEY, 'S', ID_SAVE_PAGE},
        {FCONTROL | FSHIFT | FVIRTKEY, 'S', ID_SAVE_ALL},
        {FVIRTKEY, VK_F5, ID_PREVIEW},
        {FVIRTKEY, VK_F6, ID_SCAN},
        {FVIRTKEY, VK_F4, ID_SHOWGLASS},
    };
    g_accel = CreateAcceleratorTableW(acc, static_cast<int>(sizeof(acc) / sizeof(acc[0])));

    // Driver detail: 4 = steps, calibration results, warnings and errors (default);
    // 7 = every register access and USB transfer (start with --log).
    g_engine.start(g_data_dir, g_simulate, g_fake_usb, g_log ? 7 : 4, [](EngineEvent e, const std::string& text) {
        PostMessageW(g_main, WM_ENGINE, static_cast<WPARAM>(e), reinterpret_cast<LPARAM>(new std::string(text)));
    });
    check_scanner();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (TranslateAcceleratorW(g_main, g_accel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    LOG_INFO("main window closed, shutting down");
    g_engine.stop();
    gx.shutdown();
    LOG_INFO("LiDE50 Scan exited normally");
    return 0;
}
