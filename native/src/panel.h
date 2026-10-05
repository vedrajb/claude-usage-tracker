#pragma once
#include <windows.h>
#include <functional>
#include "view.h"

// panel.h - The widget window and the private messages it sends to the controller.
// Messages sent to the controller window (WM_APP range):
//   DRAGMOVE (send, lParam=POINT* desired, adjustable), DRAGEND (send, lParam=POINT*),
//   MENU (post, wParam/lParam = screen x/y), QUIT (post), RELAYOUT (post, after DPI change).
constexpr UINT WM_CUT_DRAGMOVE = WM_APP + 20;
constexpr UINT WM_CUT_DRAGEND = WM_APP + 21;
constexpr UINT WM_CUT_MENU = WM_APP + 22;
constexpr UINT WM_CUT_QUIT = WM_APP + 23;
constexpr UINT WM_CUT_RELAYOUT = WM_APP + 24;

constexpr int PANEL_WIDTH_DIP = 440;
constexpr int PANEL_HEIGHT_DIP = 36;

namespace panel {

/// Registers the window class; call once before create().
bool registerClass(HINSTANCE hInst);
/// Creates the panel window; returns nullptr on failure.
HWND create(HINSTANCE hInst, HWND controller, bool topmost, bool layered, double opacity, int dpi, int x, int y,
            std::function<void(HWND)> onDestroyed = {});
/// Updates what is displayed.
void setState(HWND hwnd, const ViewState& vs);
/// Changes the scale factor (DPI) of an existing panel.
void setDpi(HWND hwnd, int dpi);

} // namespace panel
