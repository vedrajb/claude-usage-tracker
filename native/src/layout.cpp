// layout.cpp - Pure placement math (ports of the Electron app's JS logic; see layout.h).
#include "layout.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {
// Math.round semantics (halves round up, unlike std::round which rounds away from zero).
int jsRound(double v) { return static_cast<int>(std::floor(v + 0.5)); }
}

Point computePosition(const Config& cfg, const Rect& workArea, const Size& size)
{
    if (cfg.x && cfg.y) return {*cfg.x, *cfg.y};
    bool right = cfg.anchor.find("right") != std::string::npos;
    bool bottom = cfg.anchor.find("bottom") != std::string::npos;
    int x = right ? workArea.x + workArea.width - size.width - cfg.offsetX : workArea.x + cfg.offsetX;
    int y = bottom ? workArea.y + workArea.height - size.height - cfg.offsetY : workArea.y + cfg.offsetY;
    return {x, y};
}

Point computeDragPosition(PointD startWin, PointD startCursor, PointD cursor)
{
    return {jsRound(startWin.x + cursor.x - startCursor.x), jsRound(startWin.y + cursor.y - startCursor.y)};
}

std::optional<Point> computeDockPosition(const Rect& taskbar, const std::optional<Rect>& tray, const Size& size, int offsetX)
{
    // Only horizontal taskbars are supported for docking; callers fall back to floating.
    if (taskbar.width <= taskbar.height) return std::nullopt;
    if (size.height > taskbar.height) return std::nullopt;
    int rightEdge = tray ? tray->x : taskbar.x + taskbar.width;
    int x = std::max(rightEdge - size.width - offsetX, taskbar.x);
    int y = taskbar.y + (taskbar.height - size.height) / 2;
    return Point{x, y};
}

int clampDockDrag(int x, const Rect& taskbar, const Size& size)
{
    int maxX = std::max(taskbar.x, taskbar.x + taskbar.width - size.width);
    return std::min(std::max(x, taskbar.x), maxX);
}

std::optional<TaskbarPlan> taskbarFromBoundsAndWork(const Rect& bounds, const Rect& work)
{
    int top = work.y - bounds.y;
    int bottom = (bounds.y + bounds.height) - (work.y + work.height);
    int left = work.x - bounds.x;
    int right = (bounds.x + bounds.width) - (work.x + work.width);

    int best = std::max(std::max(top, bottom), std::max(left, right));
    if (best <= 0) return std::nullopt;
    if (top == best) return TaskbarPlan{{bounds.x, bounds.y, bounds.width, top}, Edge::Top};
    if (bottom == best) return TaskbarPlan{{bounds.x, work.y + work.height, bounds.width, bottom}, Edge::Bottom};
    if (left == best) return TaskbarPlan{{bounds.x, bounds.y, left, bounds.height}, Edge::Left};
    return TaskbarPlan{{work.x + work.width, bounds.y, right, bounds.height}, Edge::Right};
}

Edge deriveTaskbarEdge(const Rect& taskbar, const Rect& monitor)
{
    if (taskbar.width >= taskbar.height) {
        int above = std::abs(taskbar.y - monitor.y);
        int below = std::abs((monitor.y + monitor.height) - (taskbar.y + taskbar.height));
        return above < below ? Edge::Top : Edge::Bottom;
    }
    int before = std::abs(taskbar.x - monitor.x);
    int after = std::abs((monitor.x + monitor.width) - (taskbar.x + taskbar.width));
    return before < after ? Edge::Left : Edge::Right;
}

bool isTaskbarAutoHidden(const Rect& taskbar, const Rect& monitor, const Rect& work)
{
    if (taskbar.width <= 0 || taskbar.height <= 0) return true;
    // Work area == monitor means the taskbar reserves no space, i.e. it auto-hides.
    if (work == monitor) return true;
    int vx = std::min(taskbar.x + taskbar.width, monitor.x + monitor.width) - std::max(taskbar.x, monitor.x);
    int vy = std::min(taskbar.y + taskbar.height, monitor.y + monitor.height) - std::max(taskbar.y, monitor.y);
    if (vx <= 0 || vy <= 0) return true;
    // Hidden when less than half of the bar is on the monitor (slid off-screen).
    return static_cast<long long>(vx) * vy * 2 < static_cast<long long>(taskbar.width) * taskbar.height;
}

bool coversMonitor(const Rect& window, const Rect& monitor)
{
    if (monitor.width <= 0 || monitor.height <= 0) return false;
    return window.x <= monitor.x && window.y <= monitor.y &&
           window.x + window.width >= monitor.x + monitor.width &&
           window.y + window.height >= monitor.y + monitor.height;
}
