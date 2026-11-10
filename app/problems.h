// LiDE50 Scan - every problem the app can run into, in plain language.
//
// Each problem has a short code (quoted in the log and in bug reports), a title,
// a sentence or two on what happened, numbered steps to fix it, and technical
// details for "Copy details". The engine, saving and setup code build these;
// the window shows them with show_problem().
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
#pragma once

#include <string>
#include <vector>

struct Lide50CalibReport;

enum class FixAction {
    None,
    EnableAccess,       // run the one-time WinUSB setup
    ManualSetup,        // show the Device Manager steps
    OpenDeviceManager,
    OpenRansomwareSettings, // Windows Security > Ransomware protection
    TryAgain,           // repeat the action that failed
    RestartApp,
};

struct Problem {
    std::string code;                 // e.g. "CAL-NO-LIGHT"
    std::wstring title;
    std::wstring what;                // what happened, one or two sentences
    std::vector<std::wstring> steps;  // what to try, in order
    std::wstring details;             // technical detail (status, driver text, numbers)
    FixAction fix = FixAction::None;
    std::wstring fix_label;           // button text for `fix`
    bool warning = false;             // the action finished, but the result may be wrong

    bool empty() const { return code.empty(); }
    // full text for the clipboard / log
    std::wstring report_text() const;
};

// ---- scanner connection ----
Problem problem_not_connected();
// the scanner is plugged in but Windows reports a problem with it (CM_PROB_* code)
Problem problem_device_error(unsigned cm_problem, const std::wstring& service);
// another driver (for example Canon's) is assigned to the scanner
Problem problem_other_driver(const std::wstring& service);
// opening the scanner failed; `status` is a SANE_Status
Problem problem_open_failed(int status, const std::string& driver_message);

// ---- scanning ----
// sane_start failed; uses the calibration report to name the cause
Problem problem_scan_start(int status, const std::string& driver_message, const Lide50CalibReport& cal);
// reading image data failed after `lines` lines (0 = nothing arrived)
Problem problem_scan_read(int status, const std::string& driver_message, int lines, int total_lines);
Problem problem_scan_settings(const std::string& option, int status);
Problem problem_bad_image_format(const std::string& details);
Problem problem_not_enough_memory(double needed_mb, double available_mb, bool before_start);
Problem problem_engine_stopped(const std::string& details);
Problem problem_unexpected(const std::string& where, const std::string& details);

// warnings after a scan that finished
Problem warning_led_unsettled(const Lide50CalibReport& cal);
Problem warning_one_colour(int strong_channel, const double means[3]);
Problem warning_blank(bool all_dark, double mean);
Problem warning_head_return(int status, const std::string& driver_message);

// ---- saving ----
// `win32_error` from GetLastError(), `needed_mb`/`free_mb` when known
Problem problem_save(unsigned long win32_error, const std::wstring& path, double needed_mb, double free_mb,
                     const std::wstring& detail);
Problem problem_save_memory(double needed_mb);

// ---- one-time access setup ----
Problem problem_setup_cancelled();
Problem problem_setup_failed(const std::wstring& details);
Problem problem_setup_timeout();
Problem problem_setup_not_started(unsigned long win32_error);

// ---- app ----
Problem problem_previous_crash(const std::wstring& when, const std::wstring& what);
Problem problem_display(const std::string& details);

// ---- troubleshooting guide (menu) ----
Problem help_not_found();
Problem help_colours();
Problem help_noise_stretched();
Problem help_crashes();

// plain names
const wchar_t* colour_name(int channel);   // L"red", L"green", L"blue"
std::string sane_status_name(int status);  // "SANE_STATUS_IO_ERROR" etc. for details
