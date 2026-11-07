/* LiDE50 Scan - diagnostics passed from the built-in driver to the app.
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
    LIDE50_LED_UNSETTLED = 2,   /* did not settle; the closest safe values were used for this scan only */
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
    unsigned white_level[3];     /* coarse gain: 95th percentile of the white strip, R G B (16 bit) */
    int low_light_channel;       /* -1, or the colour that saw almost no light (the "locked" check) */
    int reset_bad_exposure;      /* an out-of-range exposure was found and reset to the default */
    int cache_rejected;          /* a cached calibration was ignored because its values were invalid */
};

void lide50_diag_reset_scan(void);
struct Lide50CalibReport* lide50_diag_calib(void);

/* last error text thrown inside the driver (empty if none since the last reset) */
void lide50_diag_note_error(const char* func, const char* message);
const char* lide50_diag_last_error(void);
const char* lide50_diag_last_error_func(void);

/* safe exposure range for the LiDE 50 LEDs (the driver's table default is 0x400) */
#define LIDE50_LED_EXPOSURE_MIN 0x0040u
#define LIDE50_LED_EXPOSURE_MAX 0x2000u

#ifdef __cplusplus
}
#endif

#endif
