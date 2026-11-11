// The GC9A01A 240x240 round LCD (Waveshare ESP32-S3-LCD-1.28; pins from the
// settings). Draws core/screen_draw pages a band at a time, redrawing only
// when the page changes.
#pragma once

#include <functional>

#include "screen_draw.h"
#include "unit_settings.h"

namespace screen {

const screen_draw::Fonts &fonts();

// Starts the screen task: it calls `view` twice a second and redraws when the
// result changes.
void start(const core::Pins &pins, std::function<screen_draw::View()> view);
void set_brightness(int pct);       // 0-100, applied by the screen task
void set_sleep(bool on);            // standby: backlight off, panel asleep

}  // namespace screen
