// Tray icon raster: status circle filling the canvas, app icon centred inside, black stroke on both.
#include "doctest.h"
#include "palette.hpp"
#include "tray_icon.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace p = hdmon::palette;
namespace tr = hdmon::tray;

static const char* kStatus[] = {"OK", "WARNING", "CRITICAL", "UNKNOWN"};

static p::Rgb status_rgb(const char* name) {
    if (std::string(name) == "OK") return p::to_rgb(p::STATUS_OK);
    if (std::string(name) == "WARNING") return p::to_rgb(p::STATUS_WARNING);
    if (std::string(name) == "CRITICAL") return p::to_rgb(p::STATUS_CRITICAL);
    return p::to_rgb(p::STATUS_UNKNOWN);
}

static const std::uint8_t* pix(const std::vector<std::uint8_t>& b, int size, int x, int y) {
    return b.data() + (y * size + x) * 4;
}

TEST_CASE("tray geometry scales from the 32 px constants") {
    CHECK(tr::stroke_w(p::ICON_SIZE) == p::STROKE_W);
    CHECK(tr::circle_d(p::ICON_SIZE) == p::CIRCLE_D);
    CHECK(tr::stroke_w(16) == 1);
    CHECK(tr::circle_d(16) == 14);
    CHECK(tr::stroke_w(24) == 1);
    CHECK(tr::circle_d(24) == 22);
}

TEST_CASE("tray icon: app icon inside a stroked status circle at 16/24/32") {
    for (int size : {16, 24, 32}) {
        const int stroke = tr::stroke_w(size);
        const int cd = tr::circle_d(size);
        const int cx = size / 2;
        // Just inside the top of the fill (the landscape drive leaves a status ring there).
        const int ring_y = stroke + 2;
        INFO("size=" << size);
        for (const char* name : kStatus) {
            p::Rgb want = status_rgb(name);
            std::vector<std::uint8_t> buf(static_cast<size_t>(size) * size * 4, 0x7f);
            tr::render(buf.data(), size, want);
            INFO("status=" << name);

            const std::uint8_t* ring = pix(buf, size, cx, ring_y);
            CHECK(ring[0] == want.r);
            CHECK(ring[1] == want.g);
            CHECK(ring[2] == want.b);
            CHECK(ring[3] == 255);

            const std::uint8_t* corner = pix(buf, size, 0, size - 1);
            CHECK(corner[3] == 0);

            // Stroke ring just inside the bottom edge, on the vertical centre line.
            const int sy = size - 1 - (stroke > 1 ? 1 : 0);
            const std::uint8_t* s = pix(buf, size, cx, sy);
            CHECK(s[0] <= 20);
            CHECK(s[1] <= 20);
            CHECK(s[2] <= 20);
            CHECK(s[3] >= 200);

            // The app icon is inside the circle: some opaque pixel that is not the
            // status colour and not pure black (the blue drive body or platter).
            bool found_icon = false;
            const int x0 = (size - cd) / 2, x1 = x0 + cd;
            for (int y = x0; y < x1 && !found_icon; ++y) {
                for (int x = x0; x < x1; ++x) {
                    const std::uint8_t* q = pix(buf, size, x, y);
                    if (q[3] < 200) continue;
                    const bool black = q[0] <= 20 && q[1] <= 20 && q[2] <= 20;
                    const bool status = q[0] == want.r && q[1] == want.g && q[2] == want.b;
                    if (!black && !status) found_icon = true;
                }
            }
            CHECK(found_icon);
        }
    }
}

TEST_CASE("tray icon: 32 px fill ellipse matches palette CIRCLE_*") {
    p::Rgb ok = p::to_rgb(p::STATUS_OK);
    std::vector<std::uint8_t> buf(static_cast<size_t>(p::ICON_SIZE) * p::ICON_SIZE * 4);
    tr::render(buf.data(), p::ICON_SIZE, ok);
    // Top of the fill, on the vertical centre line — status ring, not the icon.
    const int x = p::CIRCLE_X + p::CIRCLE_D / 2;
    const int y = p::CIRCLE_Y + 2;
    const std::uint8_t* ring = pix(buf, p::ICON_SIZE, x, y);
    CHECK(ring[0] == 0);
    CHECK(ring[1] == 255);
    CHECK(ring[2] == 0);
    CHECK(ring[3] == 255);
    // A couple of pixels left of the fill, in the stroke (mid-left).
    const int mid_y = p::CIRCLE_Y + p::CIRCLE_D / 2;
    const std::uint8_t* st = pix(buf, p::ICON_SIZE, p::CIRCLE_X - 1, mid_y);
    CHECK(st[0] <= 20);
    CHECK(st[1] <= 20);
    CHECK(st[2] <= 20);
    CHECK(st[3] >= 200);
}
