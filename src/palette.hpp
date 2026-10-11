// Shared status palette (from the VB DiskMonitor, with Richard's critical red override).
// Every component that shows a status colour as hex/RGB uses these values; do not restyle them.
// The CLI terminal output uses the terminal's own 16 ANSI colours (red/green/yellow) and has no hex values.
#pragma once
#include <cstdint>

namespace hdmon::palette {

struct Rgb { std::uint8_t r, g, b; };

// Tray circle / status colours.
inline constexpr const char* STATUS_OK       = "#00FF00";
inline constexpr const char* STATUS_WARNING  = "#FFFF00";
inline constexpr const char* STATUS_CRITICAL = "#FF0000";
inline constexpr const char* STATUS_UNKNOWN  = "#8A8F98";

// Main-window row tints (always drawn with black text).
inline constexpr const char* ROW_OK       = "#00FF00";
inline constexpr const char* ROW_WARNING  = "#FFFF00";
inline constexpr const char* ROW_CRITICAL = "#FF0000";
inline constexpr const char* ROW_UNKNOWN  = "#E4E6EA";

inline constexpr const char* ROW_TEXT = "#000000";

// Tray icon geometry: filled, anti-aliased circle at (2,2) size 28 on a transparent 32 px icon.
inline constexpr int ICON_SIZE = 32;
inline constexpr int CIRCLE_X = 2, CIRCLE_Y = 2, CIRCLE_D = 28;

// "#RRGGBB" -> Rgb (constexpr so the tests can pin the exact numbers).
constexpr int hex_nibble(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0;
}
constexpr Rgb to_rgb(const char* hex) {
    return Rgb{static_cast<std::uint8_t>(hex_nibble(hex[1]) * 16 + hex_nibble(hex[2])),
               static_cast<std::uint8_t>(hex_nibble(hex[3]) * 16 + hex_nibble(hex[4])),
               static_cast<std::uint8_t>(hex_nibble(hex[5]) * 16 + hex_nibble(hex[6]))};
}

}  // namespace hdmon::palette
