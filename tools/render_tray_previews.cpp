// Write tray-icon PNG previews (no extra libraries: uncompressed PNG).
// Usage: render_tray_previews <outdir>
#include "tray_icon.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static std::uint32_t crc32(const std::uint8_t* p, size_t n) {
    static std::uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        init = true;
    }
    std::uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static std::uint32_t adler32(const std::uint8_t* p, size_t n) {
    std::uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void be32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    o.push_back(static_cast<std::uint8_t>(v >> 24));
    o.push_back(static_cast<std::uint8_t>(v >> 16));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
    o.push_back(static_cast<std::uint8_t>(v));
}

static void chunk(std::vector<std::uint8_t>& o, const char typ[4],
                  const std::uint8_t* data, size_t n) {
    be32(o, static_cast<std::uint32_t>(n));
    size_t at = o.size();
    o.insert(o.end(), typ, typ + 4);
    o.insert(o.end(), data, data + n);
    be32(o, crc32(o.data() + at, 4 + n));
}

// PNG with uncompressed deflate blocks (no zlib dependency).
static bool write_png(const std::string& path, const std::uint8_t* rgba, int size) {
    std::vector<std::uint8_t> raw(static_cast<size_t>(size) * (1 + size * 4));
    for (int y = 0; y < size; ++y) {
        raw[static_cast<size_t>(y) * (1 + size * 4)] = 0;   // filter None
        std::memcpy(raw.data() + static_cast<size_t>(y) * (1 + size * 4) + 1,
                    rgba + static_cast<size_t>(y) * size * 4,
                    static_cast<size_t>(size) * 4);
    }
    // zlib: 0x78 0x01, uncompressed blocks, adler32
    std::vector<std::uint8_t> z;
    z.push_back(0x78);
    z.push_back(0x01);
    size_t off = 0;
    while (off < raw.size()) {
        size_t n = std::min<size_t>(raw.size() - off, 65535);
        const bool last = off + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        std::uint16_t nlen = static_cast<std::uint16_t>(~n);
        z.push_back(static_cast<std::uint8_t>(nlen));
        z.push_back(static_cast<std::uint8_t>(nlen >> 8));
        z.insert(z.end(), raw.begin() + static_cast<int>(off), raw.begin() + static_cast<int>(off + n));
        off += n;
    }
    be32(z, adler32(raw.data(), raw.size()));

    std::vector<std::uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    std::uint8_t ihdr[13] = {};
    ihdr[3] = static_cast<std::uint8_t>(size);        // width (size <= 255)
    ihdr[7] = static_cast<std::uint8_t>(size);        // height
    ihdr[8] = 8;                                      // bit depth
    ihdr[9] = 6;                                      // RGBA
    chunk(png, "IHDR", ihdr, 13);
    chunk(png, "IDAT", z.data(), z.size());
    chunk(png, "IEND", nullptr, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: render_tray_previews <outdir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    struct { const char* file; const char* hex; } colours[] = {
        {"ok",        hdmon::palette::STATUS_OK},
        {"warning",   hdmon::palette::STATUS_WARNING},
        {"critical",  hdmon::palette::STATUS_CRITICAL},
        {"unknown",   hdmon::palette::STATUS_UNKNOWN},
    };
    int n = 0;
    for (int size : {16, 24, 32}) {
        for (auto& c : colours) {
            std::vector<std::uint8_t> px(static_cast<size_t>(size) * size * 4);
            hdmon::tray::render(px.data(), size, hdmon::palette::to_rgb(c.hex));
            const std::string path = dir + "/" + c.file + "-" + std::to_string(size) + ".png";
            if (!write_png(path, px.data(), size)) {
                std::fprintf(stderr, "failed to write %s\n", path.c_str());
                return 1;
            }
            std::printf("%s\n", path.c_str());
            ++n;
        }
    }
    return n == 12 ? 0 : 1;
}
