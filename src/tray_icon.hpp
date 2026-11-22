// Toolkit-free tray icon: a status-coloured circle filling the canvas, with the
// app icon centred inside it. Both have a black stroke so they read on any taskbar.
#pragma once
#include "palette.hpp"
#include <cstdint>

namespace hdmon::tray {

// Stroke width and circle fill diameter for a size x size tray icon.
// At palette::ICON_SIZE these match palette::STROKE_W and palette::CIRCLE_D.
// The fill plus its stroke nearly fills the canvas; the app icon sits inside the fill.
inline int stroke_w(int size) { return size < 32 ? 1 : (2 * size) / 32; }
inline int circle_d(int size) { return size - 2 * stroke_w(size); }

// Render a size x size unpremultiplied RGBA icon (size*size*4 bytes).
void render(std::uint8_t* rgba, int size, palette::Rgb status);

}  // namespace hdmon::tray
