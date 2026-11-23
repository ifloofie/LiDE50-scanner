/* LiDE50 Scan - diagnostics passed from the built-in driver to the app.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 the LiDE50 Scan contributors
 *
 * The SANE interface only returns a status code. These functions keep the
 * driver's own error text and the results of the last calibration, so the app
 * can tell the user what actually went wrong and what to try.
 * All functions are called on the scanner engine thread only. */
#ifndef LIDE50_DIAG_H
#define LIDE50_DIAG_H

#ifdef __cplusplus
extern "C" {
#endif

/* LED (lamp) calibration outcome */
enum Lide50LedResult {
    LIDE50_LED_NOT_RUN = 0,
    LIDE50_LED_OK = 1,          /* all three colours reached the target brightness */
    LIDE50_LED_UNSETTLED = 2,   /* did not settle; the closest safe values were used and are not cached */
};

struct Lide50CalibReport {
    int ran;                     /* a full calibration ran during the last sane_start */
    int led_result;              /* enum Lide50LedResult */
    int led_tries;
    unsigned led_target;         /* 16-bit target level */
    unsigned led_exposure[3];    /* exposure used for the scan, R G B */
    unsigned led_average[3];     /* brightness seen on the last LED test, R G B (16 bit) */
    int led_saturated;           /* the sensor was overexposed even at minimum exposure (stray light) */
    int led_dim_channel;         /* -1, or the colour (0 R, 1 G, 2 B) that stayed too dark */
    int led_bright_channel;      /* -1, or the colour that stayed too bright */
    int led_limit_channel;       /* -1, or a colour whose LED is weak: it runs at the longest exposure and
                                    reaches at least half the target; gain and shading make up the rest */
    unsigned white_level[3];     /* coarse gain: 95th percentile of the white strip, R G B (16 bit) */
    int low_light_channel;       /* -1, the colour that saw almost no light (the "locked" check), or 3 = gray (all LEDs) */
    int reset_bad_exposure;      /* an out-of-range exposure was found and reset to the default */
    int cache_rejected;          /* a cached calibration was ignored because its values were invalid */
};

void lide50_diag_reset_scan(void);
struct Lide50CalibReport* lide50_diag_calib(void);

/* last error text thrown inside the driver (empty if none since the last reset) */
void lide50_diag_note_error(const char* func, const char* message);
const char* lide50_diag_last_error(void);
const char* lide50_diag_last_error_func(void);

/* Safe exposure range for the LiDE 50 LEDs (the driver's table default is 0x400).
 *
 * The maximum must stay below the line period of the calibration scans (5320 at 600 dpi,
 * 10520 at 1200 dpi, minus the 0x50 dummy period). The LED calibration changes only the
 * exposure registers; the line period was set before it starts. An exposure longer than
 * the line period cannot be measured: the LED is simply on for the whole line, so the
 * reading stops rising while the value keeps growing, and the real scan (whose line
 * period is stretched to fit) then gets more light than was calibrated for. */
#define LIDE50_LED_EXPOSURE_MIN 0x0040u
#define LIDE50_LED_EXPOSURE_MAX 0x1400u
/* a colour at the longest exposure is still usable when it reaches this share of the target */
#define LIDE50_LED_LIMIT_OK_PERCENT 50u

#ifdef __cplusplus
}
#endif

#endif
