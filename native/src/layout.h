#pragma once
#include <optional>
#include "config.h"

// layout.h - Pure geometry/placement math (no Win32), so it is unit-testable.

/// Position/size in screen pixels.
struct Rect {
    int x = 0, y = 0, width = 0, height = 0;
    bool operator==(const Rect&) const = default;
};
struct Size {
    int width = 0, height = 0;
};
struct Point {
    int x = 0, y = 0;
    bool operator==(const Point&) const = default;
};
struct PointD {
    double x = 0, y = 0;
};

/// Screen edge the taskbar is attached to.
enum class Edge { Top, Bottom, Left, Right };

struct TaskbarPlan {
    Rect rect;
    Edge edge = Edge::Bottom;
};

/// Floating position from anchor + offsets inside the work area (or explicit x/y).
Point computePosition(const Config& cfg, const Rect& workArea, const Size& size);
/// New window position for a drag: start position plus cursor delta.
Point computeDragPosition(PointD startWin, PointD startCursor, PointD cursor);
/// Docked position left of the tray, vertically centred; nullopt if it does not fit.
std::optional<Point> computeDockPosition(const Rect& taskbar, const std::optional<Rect>& tray, const Size& size, int offsetX);
/// Keeps a docked panel within the taskbar horizontally while dragging.
int clampDockDrag(int x, const Rect& taskbar, const Size& size);
/// Infers taskbar rect/edge from monitor bounds minus work area.
std::optional<TaskbarPlan> taskbarFromBoundsAndWork(const Rect& bounds, const Rect& work);
/// Which monitor edge the taskbar rect hugs.
Edge deriveTaskbarEdge(const Rect& taskbar, const Rect& monitor);
/// Auto-hidden taskbars are mostly off-screen and leave the work area untouched.
bool isTaskbarAutoHidden(const Rect& taskbar, const Rect& monitor, const Rect& work);
/// True if the window covers the whole monitor (fullscreen heuristic).
bool coversMonitor(const Rect& window, const Rect& monitor);
