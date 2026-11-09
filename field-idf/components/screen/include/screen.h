// Status screen on the GC9A01A 240x240 round LCD (Waveshare ESP32-S3-LCD-1.28;
// pins in Kconfig). Shows a title and a few lines of text, centred and cut to
// fit the circle: the first line in the medium font, the rest small.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace screen {

struct Content {
    std::string title;
    std::vector<std::string> lines;
    bool operator==(const Content &o) const { return title == o.title && lines == o.lines; }
};

// Starts the screen task: it calls `content` once a second and redraws when
// the result changes. Does nothing when the screen is disabled in Kconfig.
void start(std::function<Content()> content);

}  // namespace screen
