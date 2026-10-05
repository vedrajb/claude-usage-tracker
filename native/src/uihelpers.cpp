// uihelpers.cpp - Small pure helpers (colour blend, DPI scaling, dock menu mapping).
#include "uihelpers.h"

#include <algorithm>
#include <cmath>

namespace {
uint8_t mix(uint8_t f, uint8_t b, double a)
{
    return static_cast<uint8_t>(std::lround(f * a + b * (1 - a)));
}
const char* const DOCKS[] = {"embed", "overlay", "none"};
}

Rgb blend(Rgb fg, Rgb bg, double alpha)
{
    alpha = std::min(1.0, std::max(0.0, alpha));
    return {mix(fg.r, bg.r, alpha), mix(fg.g, bg.g, alpha), mix(fg.b, bg.b, alpha)};
}

int scaleDip(int dip, int dpi)
{
    return static_cast<int>(std::lround(dip * (dpi > 0 ? dpi : 96) / 96.0));
}

double clampOpacity(double v)
{
    if (!std::isfinite(v) || v == 0) return 1.0;
    return std::min(1.0, std::max(0.1, v));
}

int dockOffsetFromX(int x, const Rect& taskbar, const std::optional<Rect>& tray, int width)
{
    int rightEdge = tray ? tray->x : taskbar.x + taskbar.width;
    return std::max(0, rightEdge - width - x);
}

int dockMenuIndex(const std::string& dock)
{
    for (int i = 0; i < 3; ++i) {
        if (dock == DOCKS[i]) return i;
    }
    return 0;
}

std::string dockFromMenuIndex(int index)
{
    return index >= 0 && index < 3 ? DOCKS[index] : DOCKS[0];
}

std::wstring clampTip(const std::wstring& s, size_t maxChars)
{
    if (s.size() <= maxChars) return s;
    size_t n = maxChars;
    // Do not cut between the halves of a surrogate pair (would show a broken glyph).
    if (n > 0 && s[n - 1] >= 0xD800 && s[n - 1] <= 0xDBFF) --n;
    return s.substr(0, n);
}
