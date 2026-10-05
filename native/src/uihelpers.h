#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "layout.h"

// uihelpers.h - Small pure helpers shared by the UI code (kept Win32-free for tests).

/// 8-bit RGB colour.
struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
    bool operator==(const Rgb&) const = default;
};

/// Linear blend of fg over bg by alpha (0-1).
Rgb blend(Rgb fg, Rgb bg, double alpha);
/// Converts device-independent pixels to physical pixels at `dpi` (96 = 100%).
int scaleDip(int dip, int dpi);
/// Clamps to [0.1, 1]; 0/NaN mean "unset" -> 1 so the panel cannot be made invisible.
double clampOpacity(double v);
/// Inverse of dock placement: derives the saved offsetX from a dragged x.
int dockOffsetFromX(int x, const Rect& taskbar, const std::optional<Rect>& tray, int width);
/// Maps config dock value ("embed"/"overlay"/"none") to its menu radio index.
int dockMenuIndex(const std::string& dock);
/// Inverse of dockMenuIndex.
std::string dockFromMenuIndex(int index);
/// Truncates to the tray tooltip limit (127 chars + NUL).
std::wstring clampTip(const std::wstring& s, size_t maxChars = 127);
