// Toolkit-free tray icon: the app icon above a status-coloured circle, both
// with a black stroke so the pair reads on a light or dark taskbar.
#pragma once
#include "palette.hpp"
#include <cstdint>

namespace hdmon::tray {

// Stroke width and circle fill diameter for a size x size tray icon.
// At palette::ICON_SIZE these match palette::STROKE_W and palette::CIRCLE_D.
inline int stroke_w(int size) { return size < 32 ? 1 : (2 * size) / 32; }
inline int circle_d(int size) { return size * 10 / 32 < 5 ? 5 : size * 10 / 32; }

// Render a size x size unpremultiplied RGBA icon (size*size*4 bytes).
void render(std::uint8_t* rgba, int size, palette::Rgb status);

}  // namespace hdmon::tray
