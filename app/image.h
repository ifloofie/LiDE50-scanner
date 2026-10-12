// LiDE50 Scan - simple 8-bit image container.
#pragma once
#include <cstdint>
#include <vector>

struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;      // 1 = gray, 3 = RGB
    int dpi = 0;
    bool bilevel = false;  // gray image that only contains 0/255 (black & white)
    std::vector<std::uint8_t> data;

    bool empty() const { return width <= 0 || height <= 0 || data.empty(); }
    std::size_t stride() const { return static_cast<std::size_t>(width) * channels; }
    std::uint8_t* row(int y) { return data.data() + stride() * y; }
    const std::uint8_t* row(int y) const { return data.data() + stride() * y; }
    void alloc(int w, int h, int c)
    {
        width = w; height = h; channels = c;
        data.assign(static_cast<std::size_t>(w) * h * c, 0);
    }
};
