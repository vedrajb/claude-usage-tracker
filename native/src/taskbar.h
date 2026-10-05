#pragma once
#include <windows.h>
#include <optional>
#include "config.h"
#include "layout.h"
#include "view.h"

// taskbar.h - Taskbar discovery and the Dock that positions the usage panel.

/// Snapshot of the primary taskbar. `valid` is false when it cannot be found.
struct TaskbarInfo {
    bool valid = false;
    Rect rect;
    Edge edge = Edge::Bottom;
    HWND hwnd = nullptr;
    std::optional<Rect> tray;
    bool autoHide = false;
};

/// Reads the current taskbar state (cheap; safe to call every tick).
TaskbarInfo getTaskbarInfo();

/// Embed: child of the taskbar; Overlay: topmost popup over it; Floating: free window.
enum class DockMode { Embed, Overlay, Floating };

/// Owns the panel window and keeps it docked; all methods run on the UI thread.
class Dock {
public:
    Dock(HINSTANCE hInst, HWND controller);
    ~Dock();
    Dock(const Dock&) = delete;
    Dock& operator=(const Dock&) = delete;

    void rebuild(const Config& cfg, bool retryEmbed);
    void tick(const Config& cfg);
    void place(const Config& cfg);
    void setView(const ViewState& vs);
    void toggleVisible();
    void dragMove(POINT& desired);
    void dragEnd(Config& cfg, POINT pos);
    void destroy();

    HWND hwnd() const { return hwnd_; }
    DockMode mode() const { return mode_; }

private:
    DockMode resolve(const Config& cfg, const TaskbarInfo& info) const;
    void create(DockMode m, const Config& cfg, const TaskbarInfo& info);
    void placeWith(const Config& cfg, const TaskbarInfo& info);
    void moveTo(Point pos);
    void reassertZ();
    void applyVisibility();
    void onPanelDestroyed(HWND h);
    void installHook();
    void removeHook();

    HINSTANCE hInst_;
    HWND controller_;
    HWND hwnd_ = nullptr;
    DockMode mode_ = DockMode::Floating;
    ViewState view_;
    Size size_{};
    TaskbarInfo info_;
    bool userHidden_ = false, fsHidden_ = false, embedFailed_ = false;
    HWINEVENTHOOK hook_ = nullptr;
};
