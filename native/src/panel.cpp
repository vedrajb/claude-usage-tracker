// panel.cpp - The usage widget window: GDI painting (double-buffered), hover "x",
// drag-to-move and right-click menu. It never takes focus or does any I/O; all
// decisions (move, quit, menu) are forwarded to the controller window by message.
#include "panel.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include "uihelpers.h"

namespace panel {
namespace {

constexpr wchar_t CLASS_NAME[] = L"CUT_Panel";

constexpr Rgb BG{20, 22, 28}; // also the layered colour key: pixels of this colour are transparent
constexpr Rgb HOVER_BG{52, 56, 66};
// Light-mode palette (dark text for light taskbars).
constexpr Rgb L_LABEL{0x4a, 0x50, 0x5c};
constexpr Rgb L_TEXT{0x14, 0x16, 0x1c};
constexpr Rgb L_REF{0xf3, 0xf3, 0xf3};
constexpr Rgb L_HOVER_BG{0xd6, 0xd9, 0xe0};
constexpr Rgb L_TRACK{0x00, 0x00, 0x00};
constexpr Rgb LABEL{0xaa, 0xb0, 0xbb};
constexpr Rgb TEXT{0xe8, 0xea, 0xed};
constexpr Rgb BADGE{0xf2, 0xa9, 0x3b};
constexpr Rgb WHITE{255, 255, 255};
constexpr Rgb CLOSE_RED{239, 75, 75};
constexpr Rgb GREEN{0x3e, 0xc5, 0x5a};
constexpr Rgb YELLOW{0xf2, 0xc5, 0x3b};
constexpr Rgb RED{0xef, 0x4b, 0x4b};
constexpr Rgb DRAG_OUTLINE{0x6e, 0xa8, 0xff};
constexpr double STALE_ALPHA = 0.7;

// Per-window state, heap-allocated in create() and owned by the HWND via
// GWLP_USERDATA; freed in WM_NCDESTROY.
struct Data {
    HWND controller = nullptr;
    std::function<void(HWND)> onDestroyed;
    ViewState vs;
    int dpi = 96;
    double opacity = 1.0;
    HFONT font = nullptr, badgeFont = nullptr;
    bool hover = false, hoverClose = false, tracking = false;
    bool pressClose = false, dragging = false, moved = false;
    POINT dragCursor{}, dragWin{};
};

Data* data(HWND h) { return reinterpret_cast<Data*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

COLORREF rgb(Rgb c) { return RGB(c.r, c.g, c.b); }

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void freeFonts(Data& d)
{
    if (d.font) DeleteObject(d.font);
    if (d.badgeFont) DeleteObject(d.badgeFont);
    d.font = d.badgeFont = nullptr;
}

void rebuildFonts(Data& d)
{
    freeFonts(d);
    auto make = [&](int px) {
        return CreateFontW(-scaleDip(px, d.dpi), 0, 0, 0, 600, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    };
    d.font = make(12);
    d.badgeFont = make(11);
}

// Rounded corners via a window region. On success the system takes ownership of
// the HRGN (must NOT be deleted); only delete it if SetWindowRgn failed.
void applyRegion(HWND h, const Data& d)
{
    RECT rc;
    GetClientRect(h, &rc);
    HRGN rgn = CreateRectRgn(0, 0, rc.right, rc.bottom);
    if (rgn && !SetWindowRgn(h, rgn, TRUE)) DeleteObject(rgn);
}

RECT closeRect(const Data& d, int width)
{
    int x = width - scaleDip(4, d.dpi) - scaleDip(16, d.dpi);
    int y = scaleDip(2, d.dpi);
    return {x, y, x + scaleDip(16, d.dpi), y + scaleDip(16, d.dpi)};
}

void drawText(HDC dc, const std::wstring& t, RECT rc, UINT flags, Rgb color)
{
    SetTextColor(dc, rgb(color));
    DrawTextW(dc, t.c_str(), static_cast<int>(t.size()), &rc, flags | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

int textWidth(HDC dc, const std::wstring& t)
{
    SIZE sz{};
    GetTextExtentPoint32W(dc, t.c_str(), static_cast<int>(t.size()), &sz);
    return sz.cx;
}

void fillRect(HDC dc, const RECT& rc, Rgb c)
{
    HBRUSH b = CreateSolidBrush(rgb(c));
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

TRIVERTEX vertex(int x, int y, Rgb c)
{
    return TRIVERTEX{x, y, static_cast<COLOR16>(c.r << 8), static_cast<COLOR16>(c.g << 8),
                     static_cast<COLOR16>(c.b << 8), 0};
}

// Track + gradient fill. The fill is clipped to the track's rounded shape so the
// gradient gets proper pill ends; green->yellow->red is two half-width gradients
// so yellow sits at the midpoint of the bar rather than of the filled part.
void drawBar(HDC dc, int x, int y, int w, int h, double fillPct, Rgb track, double alpha)
{
    HRGN trackRgn = CreateRectRgn(x, y, x + w, y + h);
    HBRUSH tb = CreateSolidBrush(rgb(track));
    FillRgn(dc, trackRgn, tb);
    DeleteObject(tb);

    int fw = static_cast<int>(std::lround(w * fillPct / 100.0));
    if (fw > 0) {
        HRGN fillRgn = CreateRectRgn(x, y, x + fw, y + h);
        CombineRgn(fillRgn, fillRgn, trackRgn, RGN_AND);
        SelectClipRgn(dc, fillRgn);
        Rgb g = blend(GREEN, BG, alpha), yl = blend(YELLOW, BG, alpha), r = blend(RED, BG, alpha);
        int mid = x + w / 2;
        GRADIENT_RECT gr{0, 1};
        TRIVERTEX left[2] = {vertex(x, y, g), vertex(mid, y + h, yl)};
        TRIVERTEX right[2] = {vertex(mid, y, yl), vertex(x + w, y + h, r)};
        GradientFill(dc, left, 2, &gr, 1, GRADIENT_FILL_RECT_H);
        GradientFill(dc, right, 2, &gr, 1, GRADIENT_FILL_RECT_H);
        SelectClipRgn(dc, nullptr);
        DeleteObject(fillRgn);
    }
    DeleteObject(trackRgn);
}

// Draws everything into an off-screen DIB and blits once, so there is no flicker
// (WM_ERASEBKGND is suppressed). Also used for WM_PRINTCLIENT. GDI objects
// selected into the DC are restored before deletion.
void paint(HWND h, Data& d, HDC target, bool present)
{
    RECT rc;
    GetClientRect(h, &rc);
    int W = rc.right, H = rc.bottom, s = d.dpi;
    if (W <= 0 || H <= 0) return;

    HDC dc = CreateCompatibleDC(target);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(target, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) {
        DeleteDC(dc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);

    fillRect(dc, rc, BG);
    SetBkMode(dc, TRANSPARENT);
    // Stale data is shown dimmed by blending toward the background colour.
    const double a = d.vs.stale ? STALE_ALPHA : 1.0;
    const bool light = d.vs.light;
    const Rgb ref = light ? L_REF : BG, labelC = light ? L_LABEL : LABEL, textC = light ? L_TEXT : TEXT;
    const Row& row = d.vs.credits;

    HGDIOBJ oldFont = SelectObject(dc, d.font);
    int pad = scaleDip(8, s), gap = scaleDip(6, s);
    int h1 = H / 2;
    RECT line{0, 0, 0, h1};

    // Line 1: bar + percentage.
    int pctW = scaleDip(40, s);
    int barH = scaleDip(11, s);
    int padR = scaleDip(5, s);
    int barW = W - pad - padR - gap - pctW;
    drawBar(dc, pad, (h1 - barH) / 2 + scaleDip(2, s), barW, barH, row.fill, (d.vs.light ? blend(L_TRACK, L_REF, 0.16) : blend(WHITE, BG, 0.16)), a);
    line.left = pad + barW + gap;
    line.right = W - padR;
    line.top = scaleDip(2, s);
    drawText(dc, widen(row.pctText), line, DT_RIGHT, blend(textC, ref, a));

    // Line 2: absolute amounts (+ reset time when present).
    SelectObject(dc, d.badgeFont);
    RECT l2{pad, h1, W - padR, H - scaleDip(2, s)};
    std::wstring bottom = widen(row.amountText);
    if (row.resetText != "--") bottom += (bottom.empty() ? L"" : L"  \u00B7  ") + widen(row.resetText);
    drawText(dc, bottom, l2, DT_CENTER | DT_END_ELLIPSIS, blend(labelC, ref, a));

    if (!d.vs.badge.empty()) {
        SelectObject(dc, d.badgeFont);
        RECT br{pad, scaleDip(1, s), W - scaleDip(22, s), scaleDip(1, s) + scaleDip(10, s)};
        drawText(dc, widen(d.vs.badge), br, DT_LEFT, blend(BADGE, ref, a));
    }

    if (d.hover) {
        SelectObject(dc, d.font);
        RECT cr = closeRect(d, W);
        if (d.hoverClose) {
            HBRUSH b = CreateSolidBrush(rgb(blend(CLOSE_RED, BG, 0.8)));
            HGDIOBJ ob = SelectObject(dc, b);
            HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
            Rectangle(dc, cr.left, cr.top, cr.right + 1, cr.bottom + 1);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(b);
        }
        drawText(dc, L"\u00D7", cr, DT_CENTER, d.vs.light ? (d.hoverClose ? WHITE : blend(L_TEXT, L_REF, 0.45)) : (d.hoverClose ? TEXT : blend(TEXT, BG, 0.45)));
    }

    if (d.hover && !d.vs.dragging) {
        HPEN pen = CreatePen(PS_SOLID, 1, rgb(blend(light ? L_TEXT : TEXT, ref, 0.55)));
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, W, H, scaleDip(5, s), scaleDip(5, s));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }

    if (d.vs.dragging) {
        HPEN pen = CreatePen(PS_DASH, 1, rgb(DRAG_OUTLINE));
        HGDIOBJ op = SelectObject(dc, pen);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, 0, 0, W, H, scaleDip(5, s), scaleDip(5, s));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
    }

    SelectObject(dc, oldFont);
    if (present) {
        // Per-pixel alpha: drawn pixels are opaque, the background is alpha 1 -
        // invisible, yet still hit-testable, so hover/right-click work over the
        // whole widget instead of only on the painted bar/text.
        auto* px = static_cast<uint32_t*>(bits);
        const uint32_t key = (static_cast<uint32_t>(BG.r) << 16) | (BG.g << 8) | BG.b;
        for (int i = 0; i < W * H; ++i) px[i] = ((px[i] & 0x00FFFFFF) == key) ? 0x01000000u : (px[i] | 0xFF000000u);
        POINT src{0, 0};
        SIZE sz{W, H};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, static_cast<BYTE>(std::lround(clampOpacity(d.opacity) * 255)), AC_SRC_ALPHA};
        UpdateLayeredWindow(h, nullptr, nullptr, &sz, dc, &src, 0, &bf, ULW_ALPHA);
    } else {
        BitBlt(target, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    }
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
}

// Fonts and the corner region are DPI dependent, so rebuild them on change.
void setDpiImpl(HWND h, Data& d, int dpi)
{
    if (dpi <= 0 || d.dpi == dpi) return;
    d.dpi = dpi;
    rebuildFonts(d);
    applyRegion(h, d);
    InvalidateRect(h, nullptr, FALSE);
}

POINT cursor()
{
    POINT p{};
    GetCursorPos(&p);
    return p;
}

// Window procedure. Runs on the UI thread and must never block: when embedded
// the panel is a child of the taskbar and shares its input queue state, so a
// stall here could freeze the taskbar.
LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    Data* d = data(h);
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(h, m, w, l);
    }
    case WM_NCDESTROY:
        // Last message for the window: detach and free state first, then notify the
        // Dock. The callback is moved out beforehand because it may recurse into
        // code that re-creates a window, and the owner uses the HWND value only to
        // check identity (handle values can be reused by a new window).
        if (d) {
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            auto notify = std::move(d->onDestroyed);
            freeFonts(*d);
            delete d;
            if (notify) notify(h);
        }
        return DefWindowProcW(h, m, w, l);
    case WM_ERASEBKGND:
        return 1;
    // Clicking must never activate us (focus would leave the user's app and the
    // taskbar would flash); handle the click but stay inactive.
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SIZE:
        if (d) applyRegion(h, *d);
        return 0;
    // Per-monitor DPI v2: the window is resized to the system-suggested rect, except
    // when embedded (WS_CHILD) where the Dock owns the geometry and re-lays out.
    case WM_DPICHANGED:
        if (d) {
            setDpiImpl(h, *d, static_cast<int>(HIWORD(w)));
            const RECT* sr = reinterpret_cast<const RECT*>(l);
            if (sr && !d->dragging && !(GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD))
                SetWindowPos(h, nullptr, sr->left, sr->top, sr->right - sr->left, sr->bottom - sr->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            PostMessageW(d->controller, WM_CUT_RELAYOUT, 0, 0);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (d) paint(h, *d, dc, (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_LAYERED) != 0);
        EndPaint(h, &ps);
        return 0;
    }
    // Lets DWM/PrintWindow capture the content (e.g. taskbar thumbnails/previews).
    case WM_PRINTCLIENT:
        if (d) paint(h, *d, reinterpret_cast<HDC>(w), false);
        return 0;
    case WM_SETCURSOR:
        if (d && d->vs.dragging && LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!d) break;
        // Request WM_MOUSELEAVE once so the hover "x" can be hidden again.
        if (!d->tracking) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            d->tracking = TrackMouseEvent(&t) != 0;
        }
        RECT rc;
        GetClientRect(h, &rc);
        RECT cr = closeRect(*d, rc.right);
        POINT pt{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
        bool hc = PtInRect(&cr, pt) != 0;
        if (!d->hover || hc != d->hoverClose) {
            d->hover = true;
            d->hoverClose = hc;
            InvalidateRect(h, nullptr, FALSE);
        }
        // Drag uses screen coordinates (the window itself moves under the cursor,
        // so client coordinates would drift). The controller may clamp the target
        // (SendMessage so it can adjust `want` in place before we return).
        if (d->dragging) {
            POINT c = cursor();
            POINT want{d->dragWin.x + c.x - d->dragCursor.x, d->dragWin.y + c.y - d->dragCursor.y};
            if (want.x != d->dragWin.x || want.y != d->dragWin.y) d->moved = true;
            SendMessageW(d->controller, WM_CUT_DRAGMOVE, 0, reinterpret_cast<LPARAM>(&want));
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (d) {
            d->tracking = false;
            d->hover = d->hoverClose = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!d) break;
        d->pressClose = d->hoverClose;
        if (!d->pressClose) {
            RECT wr;
            GetWindowRect(h, &wr);
            d->dragging = true;
            d->moved = false;
            d->dragCursor = cursor();
            d->dragWin = {wr.left, wr.top};
        }
        // Capture keeps mouse messages flowing while dragging outside the window.
        SetCapture(h);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (!d) break;
        bool wasDrag = d->dragging, pressClose = d->pressClose, moved = d->moved;
        d->dragging = d->pressClose = d->moved = false;
        if (GetCapture() == h) ReleaseCapture();
        if (pressClose && d->hoverClose) {
            PostMessageW(d->controller, WM_CUT_QUIT, 0, 0);
        } else if (wasDrag && moved) {
            RECT wr;
            GetWindowRect(h, &wr);
            POINT p{wr.left, wr.top};
            SendMessageW(d->controller, WM_CUT_DRAGEND, 0, reinterpret_cast<LPARAM>(&p));
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        // Posted (not sent) so the modal menu loop runs from the controller, not
        // inside this window's message handler.
        if (!d) break;
        POINT c = cursor();
        PostMessageW(d->controller, WM_CUT_MENU, static_cast<WPARAM>(c.x), static_cast<LPARAM>(c.y));
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

/// Registers the panel window class (idempotent).
bool registerClass(HINSTANCE hInst)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/// Creates the panel (hidden until positioned). `onDestroyed` fires from WM_NCDESTROY.
HWND create(HINSTANCE hInst, HWND controller, bool topmost, bool layered, double opacity, int dpi, int x, int y,
            std::function<void(HWND)> onDestroyed)
{
    auto* d = new Data;
    d->controller = controller;
    d->onDestroyed = std::move(onDestroyed);
    d->dpi = dpi;
    rebuildFonts(*d);
    // TOOLWINDOW: no taskbar button / Alt-Tab entry; NOACTIVATE: never gets focus.
    // Always created as WS_POPUP; the Dock converts it to WS_CHILD for embedding.
    DWORD ex = WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (topmost ? WS_EX_TOPMOST : 0) | (layered ? WS_EX_LAYERED : 0);
    HWND h = CreateWindowExW(ex, CLASS_NAME, L"Claude Usage Tracker", WS_POPUP, x, y, scaleDip(PANEL_WIDTH_DIP, dpi),
                             scaleDip(PANEL_HEIGHT_DIP, dpi), nullptr, nullptr, hInst, d);
    if (!h) return nullptr;
    d->opacity = opacity;
    return h;
}

/// Replaces the displayed state and repaints.
void setState(HWND hwnd, const ViewState& vs)
{
    Data* d = data(hwnd);
    if (!d) return;
    d->vs = vs;
    InvalidateRect(hwnd, nullptr, FALSE);
}

/// Width in pixels needed to show the current state compactly (never below a small minimum bar).
int preferredWidth(HWND hwnd)
{
    Data* d = data(hwnd);
    if (!d) return 0;
    int s = d->dpi;
    int pad = scaleDip(8, s), padR = scaleDip(5, s), gap = scaleDip(6, s);
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, d->font);
    int pctW = std::max(textWidth(dc, widen(d->vs.credits.pctText)), textWidth(dc, L"100.0%"));
    SelectObject(dc, d->badgeFont);
    std::wstring bottom = widen(d->vs.credits.amountText);
    if (d->vs.credits.resetText != "--") bottom += (bottom.empty() ? L"" : L"  \u00B7  ") + widen(d->vs.credits.resetText);
    int bottomW = textWidth(dc, bottom);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    int line1 = pad + scaleDip(36, s) + gap + pctW + padR;
    int line2 = pad + bottomW + padR + scaleDip(2, s);
    return std::max(line1, line2);
}

/// Sets overall opacity (layered panels) and repaints.
void setOpacity(HWND hwnd, double opacity)
{
    Data* d = data(hwnd);
    if (!d) return;
    d->opacity = opacity;
    InvalidateRect(hwnd, nullptr, FALSE);
}

/// Applies a new DPI (fonts, region, repaint); no-op if unchanged.
void setDpi(HWND hwnd, int dpi)
{
    Data* d = data(hwnd);
    if (d) setDpiImpl(hwnd, *d, dpi);
}

} // namespace panel
