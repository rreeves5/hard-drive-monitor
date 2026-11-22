#include "tray_icon.hpp"
#include "app_icon_rgba.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace hdmon::tray {
namespace {

void over(std::uint8_t* d, int r, int g, int b, int a) {
    if (a <= 0) return;
    if (a >= 255 || d[3] == 0) {
        d[0] = static_cast<std::uint8_t>(r);
        d[1] = static_cast<std::uint8_t>(g);
        d[2] = static_cast<std::uint8_t>(b);
        d[3] = static_cast<std::uint8_t>(a >= 255 ? 255 : a);
        return;
    }
    const int sa = a, da = d[3], inv = 255 - sa;
    const int outa = sa + (da * inv + 127) / 255;
    auto ch = [&](int s, int dc) {
        return (s * sa + dc * da * inv / 255 + outa / 2) / outa;
    };
    d[0] = static_cast<std::uint8_t>(ch(r, d[0]));
    d[1] = static_cast<std::uint8_t>(ch(g, d[1]));
    d[2] = static_cast<std::uint8_t>(ch(b, d[2]));
    d[3] = static_cast<std::uint8_t>(outa);
}

// Area-average scale with alpha-weighted RGB (straight alpha in, straight out).
void scale_rgba(const std::uint8_t* src, int sw, int sh,
                std::uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) {
        const double y0 = static_cast<double>(y) * sh / dh;
        const double y1 = static_cast<double>(y + 1) * sh / dh;
        const int iy0 = static_cast<int>(y0);
        const int iy1 = std::min(sh, static_cast<int>(std::ceil(y1)));
        for (int x = 0; x < dw; ++x) {
            const double x0 = static_cast<double>(x) * sw / dw;
            const double x1 = static_cast<double>(x + 1) * sw / dw;
            const int ix0 = static_cast<int>(x0);
            const int ix1 = std::min(sw, static_cast<int>(std::ceil(x1)));
            double ar = 0, ag = 0, ab = 0, aa = 0, area = 0;
            for (int iy = iy0; iy < iy1; ++iy) {
                const double yw = std::min(y1, static_cast<double>(iy + 1)) - std::max(y0, static_cast<double>(iy));
                if (yw <= 0) continue;
                for (int ix = ix0; ix < ix1; ++ix) {
                    const double xw = std::min(x1, static_cast<double>(ix + 1)) - std::max(x0, static_cast<double>(ix));
                    if (xw <= 0) continue;
                    const double w = xw * yw;
                    const std::uint8_t* p = src + (iy * sw + ix) * 4;
                    const double aw = p[3] * w;
                    ar += p[0] * aw;
                    ag += p[1] * aw;
                    ab += p[2] * aw;
                    aa += aw;
                    area += w;
                }
            }
            std::uint8_t* d = dst + (y * dw + x) * 4;
            if (aa > 0 && area > 0) {
                d[0] = static_cast<std::uint8_t>(ar / aa + 0.5);
                d[1] = static_cast<std::uint8_t>(ag / aa + 0.5);
                d[2] = static_cast<std::uint8_t>(ab / aa + 0.5);
                d[3] = static_cast<std::uint8_t>(std::min(255.0, aa / area + 0.5));
            } else {
                d[0] = d[1] = d[2] = d[3] = 0;
            }
        }
    }
}

void blit_outlined(std::uint8_t* dst, int size,
                   const std::uint8_t* src, int sw, int sh,
                   int ox, int oy, double radius) {
    const int rint = static_cast<int>(std::ceil(radius));
    const double r2 = (radius + 0.01) * (radius + 0.01);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int m = 0;
            for (int sy = y - oy - rint; sy <= y - oy + rint; ++sy) {
                if (sy < 0 || sy >= sh) continue;
                for (int sx = x - ox - rint; sx <= x - ox + rint; ++sx) {
                    if (sx < 0 || sx >= sw) continue;
                    const int dx = x - (ox + sx), dy = y - (oy + sy);
                    if (dx * dx + dy * dy > r2) continue;
                    const int a = src[(sy * sw + sx) * 4 + 3];
                    if (a > m) m = a;
                }
            }
            if (m) over(dst + (y * size + x) * 4, 0, 0, 0, m);
        }
    }
    for (int y = 0; y < sh; ++y) {
        for (int x = 0; x < sw; ++x) {
            const int dx = ox + x, dy = oy + y;
            if (dx < 0 || dy < 0 || dx >= size || dy >= size) continue;
            const std::uint8_t* s = src + (y * sw + x) * 4;
            over(dst + (dy * size + dx) * 4, s[0], s[1], s[2], s[3]);
        }
    }
}

void fill_circle(std::uint8_t* dst, int size, double cx, double cy, double r,
                 double stroke, palette::Rgb c) {
    const double rad = r + stroke + 1;
    const int x0 = std::max(0, static_cast<int>(cx - rad));
    const int x1 = std::min(size, static_cast<int>(cx + rad + 1));
    const int y0 = std::max(0, static_cast<int>(cy - rad));
    const int y1 = std::min(size, static_cast<int>(cy + rad + 1));
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            const double sa = std::min(1.0, std::max(0.0, (r + stroke) + 0.5 - d));
            const double fa = std::min(1.0, std::max(0.0, r + 0.5 - d));
            std::uint8_t* p = dst + (y * size + x) * 4;
            over(p, 0, 0, 0, static_cast<int>(sa * 255 + 0.5));
            over(p, c.r, c.g, c.b, static_cast<int>(fa * 255 + 0.5));
        }
    }
}

void render_native(std::uint8_t* rgba, int size, palette::Rgb status) {
    std::memset(rgba, 0, static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
    const int stroke = stroke_w(size);
    const int cd = circle_d(size);
    const int slot_h = size - (cd + 2 * stroke);   // gap 0, no padding
    int nw = 1, nh = 1;
    if (slot_h > 2 * stroke) {
        const int max_w = size - 2 * stroke;
        const int max_h = slot_h - 2 * stroke;
        if (max_w > 0 && max_h > 0) {
            const double sc = std::min(static_cast<double>(max_w) / APP_ICON_W,
                                       static_cast<double>(max_h) / APP_ICON_H);
            nw = std::max(1, static_cast<int>(std::lround(APP_ICON_W * sc)));
            nh = std::max(1, static_cast<int>(std::lround(APP_ICON_H * sc)));
            while (nw + 2 * stroke > size || nh + 2 * stroke > slot_h) {
                if (nw >= nh && nw > 1) --nw;
                else if (nh > 1) --nh;
                else break;
            }
        }
        std::vector<std::uint8_t> scaled(static_cast<size_t>(nw) * nh * 4);
        scale_rgba(APP_ICON_RGBA, APP_ICON_W, APP_ICON_H, scaled.data(), nw, nh);
        const int ox = (size - nw) / 2;
        const int oy = stroke;   // leave stroke room above the icon
        blit_outlined(rgba, size, scaled.data(), nw, nh, ox, oy, stroke + 0.5);
    }
    const double r = cd / 2.0;
    const double cx = size / 2.0;
    const double cy = size - stroke - r;
    fill_circle(rgba, size, cx, cy, r, stroke, status);
}

}  // namespace

void render(std::uint8_t* rgba, int size, palette::Rgb status) {
    if (size <= 0) return;
    // 16 px (and any other exact divisor of 32) is drawn at 32 and boxed down so
    // the app icon's platter and LED stay readable in a small tray.
    if (size < palette::ICON_SIZE && palette::ICON_SIZE % size == 0) {
        std::vector<std::uint8_t> hi(static_cast<size_t>(palette::ICON_SIZE) * palette::ICON_SIZE * 4);
        render_native(hi.data(), palette::ICON_SIZE, status);
        scale_rgba(hi.data(), palette::ICON_SIZE, palette::ICON_SIZE, rgba, size, size);
        return;
    }
    render_native(rgba, size, status);
}

}  // namespace hdmon::tray
