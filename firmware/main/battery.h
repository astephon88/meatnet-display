#pragma once

#include <optional>

// The battery divider's pin in microvolts (ADC1 with curve-fitting
// calibration), or nothing when there is no battery pin (-1). The pin is
// fixed by the first call.
std::optional<double> battery_uv(int pin);
