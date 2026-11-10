// LiDE50 Scan - diagnostics passed from the built-in driver to the app.
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the LiDE50 Scan contributors
#include "lide50_diag.h"

#include <cstdio>
#include <cstring>

namespace {
Lide50CalibReport g_report;
char g_error[512];
char g_error_func[96];
} // namespace

extern "C" void lide50_diag_reset_scan(void)
{
    std::memset(&g_report, 0, sizeof(g_report));
    g_report.led_dim_channel = -1;
    g_report.led_bright_channel = -1;
    g_report.low_light_channel = -1;
    g_error[0] = '\0';
    g_error_func[0] = '\0';
}

extern "C" Lide50CalibReport* lide50_diag_calib(void) { return &g_report; }

extern "C" void lide50_diag_note_error(const char* func, const char* message)
{
    // keep the first (innermost) error of an operation; later ones are usually consequences
    if (g_error[0]) return;
    std::snprintf(g_error, sizeof(g_error), "%s", message ? message : "");
    std::snprintf(g_error_func, sizeof(g_error_func), "%s", func ? func : "");
}

extern "C" const char* lide50_diag_last_error(void) { return g_error; }
extern "C" const char* lide50_diag_last_error_func(void) { return g_error_func; }

namespace {
struct InitReport {
    InitReport() { lide50_diag_reset_scan(); }
} g_init_report;
} // namespace
