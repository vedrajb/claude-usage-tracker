// taskbar.cpp - Dock: finds the taskbar and places the panel window as an embedded
// child of Shell_TrayWnd, a topmost overlay, or a free-floating window, with
// fallbacks between them. All placement is driven by a periodic tick() on the UI thread.
#include "taskbar.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cstring>
#include "panel.h"
#include "uihelpers.h"

namespace {

// Panel the WinEvent hook should keep topmost. The hook callback has no user-data
// parameter, hence a file-level global (only one overlay/floating panel exists).
HWND g_topmostPanel = nullptr;

Rect toRect(const RECT& r) { return {r.left, r.top, r.right - r.left, r.bottom - r.top}; }

// Picks the DPI to scale the panel with. Docked modes follow the taskbar window's
// DPI (the panel is its child/overlay); floating follows the panel or, before it
// exists, the monitor at the target point. System DPI is the last resort.
int dpiFor(DockMode m, const TaskbarInfo& info, HWND self = nullptr, Point at = {})
{
    if (m != DockMode::Floating && info.hwnd) {
        UINT d = GetDpiForWindow(info.hwnd);
        if (d) return static_cast<int>(d);
    } else if (m == DockMode::Floating) {
        if (self) {
            UINT d = GetDpiForWindow(self);
            if (d) return static_cast<int>(d);
        } else {
            HMONITOR mon = MonitorFromPoint(POINT{at.x, at.y}, MONITOR_DEFAULTTONEAREST);
            UINT dx = 0, dy = 0;
            if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx) return static_cast<int>(dx);
        }
    }
    return static_cast<int>(GetDpiForSystem());
}

Size sizeFor(int dpi) { return {scaleDip(PANEL_WIDTH_DIP, dpi), scaleDip(PANEL_HEIGHT_DIP, dpi)}; }

// Floating position from config, clamped to the *virtual* screen so a panel saved
// on a now-disconnected monitor can never end up unreachable.
Point floatingPosition(const Config& cfg, const Size& size)
{
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    Point p = computePosition(cfg, toRect(wa), size);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    p.x = std::max(vx, std::min(p.x, vx + vw - size.width));
    p.y = std::max(vy, std::min(p.y, vy + vh - size.height));
    return p;
}

// Shell/desktop windows (and our own) look "fullscreen" because they cover the
// monitor; they must not count as a fullscreen app.
bool isShellClass(HWND h)
{
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0 || wcscmp(cls, L"CUT_Panel") == 0 ||
           wcscmp(cls, L"CUT_Controller") == 0;
}

// Heuristic fullscreen detection (games, video, presentations): foreground window
// that is visible, not minimized/cloaked (UWP background windows are cloaked but
// still "visible"), has no full caption (borderless/fullscreen, not merely maximized)
// and covers its whole monitor. Used only for non-embedded modes so the topmost
// overlay does not draw over fullscreen content.
bool fullscreenAppActive()
{
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindowVisible(fg) || IsIconic(fg) || isShellClass(fg)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;
    if ((GetWindowLongPtrW(fg, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) return false;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    RECT wr;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!mon || !GetMonitorInfoW(mon, &mi) || !GetWindowRect(fg, &wr)) return false;
    return coversMonitor(toRect(wr), toRect(mi.rcMonitor));
}

// Overlay hook: when any other app takes the foreground it can push itself above
// us in the topmost band, so re-assert HWND_TOPMOST. Runs out-of-context (posted
// to our UI thread), so touching the window here is safe.
void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD)
{
    if (g_topmostPanel && IsWindow(g_topmostPanel))
        SetWindowPos(g_topmostPanel, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

} // namespace

// Queries the primary taskbar's geometry, edge, auto-hide state and the
// notification-area rect (used to dock just left of the tray). Fields stay
// invalid/empty if Explorer is not running (e.g. during an Explorer restart).
TaskbarInfo getTaskbarInfo()
{
    TaskbarInfo t;
    t.hwnd = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (t.hwnd) {
        RECT r;
        MONITORINFO mi{sizeof(mi)};
        HMONITOR mon = MonitorFromWindow(t.hwnd, MONITOR_DEFAULTTONEAREST);
        if (mon && GetMonitorInfoW(mon, &mi) && GetWindowRect(t.hwnd, &r)) {
            t.valid = true;
            t.rect = toRect(r);
            t.edge = deriveTaskbarEdge(t.rect, toRect(mi.rcMonitor));
            t.autoHide = isTaskbarAutoHidden(t.rect, toRect(mi.rcMonitor), toRect(mi.rcWork));
        }
    }
    if (t.hwnd) {
        HWND tray = FindWindowExW(t.hwnd, nullptr, L"TrayNotifyWnd", nullptr);
        RECT r;
        if (tray && GetWindowRect(tray, &r) && r.right > r.left) t.tray = toRect(r);
    }
    return t;
}

Dock::Dock(HINSTANCE hInst, HWND controller) : hInst_(hInst), controller_(controller) {}

Dock::~Dock() { destroy(); }

// Decides the effective mode. Floating when docking is disabled, the taskbar is
// missing/auto-hidden (an embedded child would be hidden or clipped with it) or
// there is no room; Overlay if requested or if embedding previously failed.
DockMode Dock::resolve(const Config& cfg, const TaskbarInfo& info) const
{
    if (cfg.dock == "none" || !info.valid) return DockMode::Floating;
    Size sz = sizeFor(dpiFor(DockMode::Embed, info));
    if (!computeDockPosition(info.rect, info.tray, sz, cfg.offsetX)) return DockMode::Floating;
    if (info.autoHide) return DockMode::Floating;
    if (cfg.dock == "overlay") return DockMode::Overlay;
    if (!info.hwnd) return DockMode::Floating;
    return embedFailed_ ? DockMode::Overlay : DockMode::Embed;
}

void Dock::destroy()
{
    removeHook();
    HWND h = hwnd_;
    hwnd_ = nullptr;
    if (h && IsWindow(h)) DestroyWindow(h);
}

void Dock::rebuild(const Config& cfg, bool retryEmbed)
{
    destroy();
    if (retryEmbed) embedFailed_ = false;
    TaskbarInfo info = getTaskbarInfo();
    create(resolve(cfg, info), cfg, info);
}

// Called from the panel's WM_NCDESTROY via callback. Clears our handle (and the
// hook global) only if it still refers to the destroyed window, so a stale
// notification for an old panel cannot null out a freshly created one that
// reused the handle value.
void Dock::onPanelDestroyed(HWND h)
{
    if (g_topmostPanel == h) g_topmostPanel = nullptr;
    if (hwnd_ == h) hwnd_ = nullptr;
}

void Dock::create(DockMode m, const Config& cfg, const TaskbarInfo& info)
{
    info_ = info;
    int dpi = dpiFor(m, info, nullptr, m == DockMode::Floating ? floatingPosition(cfg, sizeFor(dpiFor(m, info))) : Point{});
    size_ = sizeFor(dpi);
    Point pos = m == DockMode::Floating ? floatingPosition(cfg, size_)
                                        : computeDockPosition(info.rect, info.tray, size_, cfg.offsetX).value_or(Point{});
    double opacity = clampOpacity(cfg.opacity);

    if (m == DockMode::Embed) {
        // Embedding: create as a normal popup first, then swap to WS_CHILD and
        // reparent into Shell_TrayWnd (SetParent requires the style change to be
        // done by us; WS_CHILD cannot be passed at creation without a parent).
        // Always layered: on Windows 11 the XAML taskbar content is composited
        // above plain child HWNDs, hiding them; a layered child is composited by
        // DWM on top of it. If reparenting silently fails (e.g. a
        // different-integrity or protected taskbar) we fall back to overlay.
        hwnd_ = panel::create(hInst_, controller_, false, false, opacity, dpi, pos.x, pos.y, [this](HWND h) { onPanelDestroyed(h); });
        if (hwnd_) {
            SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
            SetParent(hwnd_, info.hwnd);
            if (GetParent(hwnd_) != info.hwnd) {
                DestroyWindow(hwnd_);
                hwnd_ = nullptr;
            } else {
                // WS_EX_LAYERED must be applied after SetParent; layering set on the
                // popup does not survive reparenting and the panel stays hidden.
                SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, GetWindowLongPtrW(hwnd_, GWL_EXSTYLE) | WS_EX_LAYERED);
                SetLayeredWindowAttributes(hwnd_, 0, static_cast<BYTE>(std::lround(opacity * 255)), LWA_ALPHA);
            }
        }
        if (!hwnd_) {
            embedFailed_ = true;
            m = DockMode::Overlay;
        }
    }
    if (!hwnd_) hwnd_ = panel::create(hInst_, controller_, true, true, opacity, dpi, pos.x, pos.y, [this](HWND h) { onPanelDestroyed(h); });
    mode_ = m;
    if (!hwnd_) return;

    panel::setState(hwnd_, view_);
    if (m == DockMode::Embed) {
        // Child coordinates are relative to the taskbar, so convert from screen.
        // HWND_TOP keeps us above the taskbar's own children (task list, tray);
        // SWP_FRAMECHANGED applies the style swap made above.
        POINT p{pos.x, pos.y};
        ScreenToClient(info.hwnd, &p);
        SetWindowPos(hwnd_, HWND_TOP, p.x, p.y, size_.width, size_.height,
                     SWP_NOACTIVATE | SWP_FRAMECHANGED | (userHidden_ ? 0 : SWP_SHOWWINDOW));
    } else {
        g_topmostPanel = hwnd_;
        installHook();
        SetWindowPos(hwnd_, HWND_TOPMOST, pos.x, pos.y, size_.width, size_.height,
                     SWP_NOACTIVATE | (userHidden_ ? 0 : SWP_SHOWWINDOW));
    }
    fsHidden_ = false;
}

// Out-of-context hook (no DLL injection); SKIPOWNPROCESS avoids reacting to our
// own windows. Tied to overlay/floating modes only.
void Dock::installHook()
{
    removeHook();
    hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, onForeground, 0, 0,
                            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void Dock::removeHook()
{
    if (hook_) UnhookWinEvent(hook_);
    hook_ = nullptr;
    g_topmostPanel = nullptr;
}

void Dock::setView(const ViewState& vs)
{
    view_ = vs;
    if (hwnd_) panel::setState(hwnd_, view_);
}

void Dock::toggleVisible()
{
    userHidden_ = !userHidden_;
    applyVisibility();
}

// Embedded panels are never hidden for fullscreen: they live inside the taskbar,
// which the shell already hides. SW_SHOWNA avoids activating (stealing focus).
void Dock::applyVisibility()
{
    if (!hwnd_) return;
    bool want = !userHidden_ && !(fsHidden_ && mode_ != DockMode::Embed);
    bool shown = (GetWindowLongPtrW(hwnd_, GWL_STYLE) & WS_VISIBLE) != 0;
    if (want != shown) ShowWindow(hwnd_, want ? SW_SHOWNA : SW_HIDE);
}

void Dock::moveTo(Point pos)
{
    POINT p{pos.x, pos.y};
    if (mode_ == DockMode::Embed) ScreenToClient(GetParent(hwnd_), &p);
    SetWindowPos(hwnd_, nullptr, p.x, p.y, size_.width, size_.height, SWP_NOACTIVATE | SWP_NOZORDER);
}

// Other taskbar children can be re-created/raised by Explorer; only touch Z-order
// when we are not already first child, to avoid needless repaints/flicker.
void Dock::reassertZ()
{
    if (mode_ == DockMode::Embed) {
        HWND parent = GetParent(hwnd_);
        if (parent && GetWindow(parent, GW_CHILD) != hwnd_)
            SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
}

// Repositions/resizes to the current taskbar geometry. Skipped while the user is
// dragging (mouse capture) and when nothing changed, to avoid fighting the drag
// and flicker.
void Dock::placeWith(const Config& cfg, const TaskbarInfo& info)
{
    if (!hwnd_ || !IsWindow(hwnd_) || GetCapture() == hwnd_) return;
    info_ = info;
    int dpi = dpiFor(mode_, info, hwnd_);
    panel::setDpi(hwnd_, dpi);
    size_ = sizeFor(dpi);
    std::optional<Point> pos = mode_ == DockMode::Floating
                                   ? std::optional<Point>(floatingPosition(cfg, size_))
                                   : computeDockPosition(info.rect, info.tray, size_, cfg.offsetX);
    if (!pos) return;
    RECT cur;
    GetWindowRect(hwnd_, &cur);
    if (cur.left != pos->x || cur.top != pos->y || cur.right - cur.left != size_.width ||
        cur.bottom - cur.top != size_.height)
        moveTo(*pos);
    reassertZ();
}

void Dock::place(const Config& cfg) { placeWith(cfg, getTaskbarInfo()); }

// Periodic watchdog on the UI thread (must stay cheap and never block: a blocked
// UI thread that owns a child of the taskbar can hang Explorer's shell). Rebuilds
// when the resolved mode changed, the panel is gone, or the taskbar was
// replaced - e.g. after an Explorer restart the old Shell_TrayWnd is destroyed
// (our child with it) and a new one appears; this also backs up TaskbarCreated.
void Dock::tick(const Config& cfg)
{
    TaskbarInfo info = getTaskbarInfo();
    bool bad = !hwnd_ || resolve(cfg, info) != mode_;
    if (!bad && mode_ == DockMode::Embed && GetParent(hwnd_) != info.hwnd) bad = true;
    if (bad) {
        destroy();
        create(resolve(cfg, info), cfg, info);
        return;
    }
    if (mode_ != DockMode::Embed) fsHidden_ = fullscreenAppActive();
    applyVisibility();
    placeWith(cfg, info);
}

void Dock::dragMove(POINT& desired)
{
    if (!hwnd_) return;
    Point p{desired.x, desired.y};
    if (mode_ != DockMode::Floating) {
        p.x = clampDockDrag(p.x, info_.rect, size_);
        p.y = info_.rect.y + (info_.rect.height - size_.height) / 2;
    }
    moveTo(p);
}

void Dock::dragEnd(Config& cfg, POINT pos)
{
    if (mode_ == DockMode::Floating) {
        cfg.x = pos.x;
        cfg.y = pos.y;
    } else {
        cfg.offsetX = dockOffsetFromX(pos.x, info_.rect, info_.tray, size_.width);
    }
}
