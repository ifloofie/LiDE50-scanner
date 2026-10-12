// LiDE50 Scan - non-destructive image adjustments.
#pragma once
#include "image.h"

struct Adjustments {
    int brightness = 0;      // -100..100
    int contrast = 0;        // -100..100
    int saturation = 0;      // -100..100 (color only)
    int gamma = 100;         // 30..300  -> 0.30..3.00
    int sharpness = 0;       // 0..100
    int threshold = 128;     // 1..254 (black & white only)
    int denoise = 0;         // 0..100 (smooths paper grain / descreen)
    bool auto_levels = false;
    bool invert = false;
    bool black_white = false;// render as black & white (bilevel)
    bool grayscale = false;  // render color scan as gray
    int rotation = 0;        // 0, 90, 180, 270 (clockwise)
    bool flip_h = false;

    bool operator==(const Adjustments& o) const;
    bool operator!=(const Adjustments& o) const { return !(*this == o); }
};

// Downscale so that neither side exceeds max_side (box filter). Returns src copy if smaller.
Image downscale(const Image& src, int max_side);

// Crop a rectangle in pixel units (clamped).
Image crop(const Image& src, int x, int y, int w, int h);

// Apply all adjustments. `scale_hint` is the ratio of this image's resolution
// to the full-resolution scan, so sharpening/denoise radii look the same on
// the small on-screen copy and on the saved file.
Image apply_adjustments(const Image& src, const Adjustments& a, double scale_hint = 1.0);

// Finds the bounding box of the document on a preview (non-background area).
// Returns false if nothing distinct was found.
bool detect_content(const Image& preview, int& x, int& y, int& w, int& h);
