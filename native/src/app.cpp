// app.cpp - Controller: a hidden top-level window that owns the tray icon, context
// menu, refresh timers and the background fetch worker, and drives the Dock.
// The UI thread only handles messages and timers; all network/process I/O runs on
// a worker thread so the UI (and, when embedded, the taskbar) never stalls.
#include "app.h"

#include <windowsx.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include "config.h"
#include "panel.h"
#include "sources.h"
#include "taskbar.h"
#include "uihelpers.h"
#include "view.h"

namespace {

constexpr wchar_t CLASS_NAME[] = L"CUT_Controller";
constexpr wchar_t TIP_BASE[] = L"Claude Usage Tracker";

// Worker -> UI doorbell; the payload travels via the mutex-guarded mailbox_.
constexpr UINT WM_FETCH_DONE = WM_APP + 1;
constexpr UINT WM_TRAY = WM_APP + 10;

constexpr UINT_PTR TIMER_REFRESH = 1, TIMER_REPAINT = 2, TIMER_DOCK = 3, TIMER_LAYOUT = 4, TIMER_QUIT = 5;
// Max time to wait for the worker to honour cancellation before hard-exiting.
constexpr ULONGLONG QUIT_WAIT_MS = 5000;

enum MenuId : int {
    IDM_REFRESH = 1,
    IDM_TOGGLE,
    IDM_MOVE,
    IDM_UNDOCK,
    IDM_OPEN_CONFIG,
    IDM_EXIT,
    IDM_DOCK_BASE = 10
};

struct FetchResult {
    bool ok = false;
    UsageData data;
    std::string error;
};

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::filesystem::path userDataDir()
{
    PWSTR p = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    return dir / L"claude-usage-tracker";
}

// Draws the tray icon in code (no .ico resource): a 16x16 design scaled to the
// small-icon size, as a 32-bit ARGB DIB plus the required (unused) mask bitmap.
HICON makeTrayIcon()
{
    int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    if (size < 16) size = 16;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    if (!color || !mask) {
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
        return nullptr;
    }
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int sx = x * 16 / size, sy = y * 16 / size;
            bool inside = sx >= 1 && sx <= 14 && sy >= 1 && sy <= 14;
            bool bar = (sy == 5 || sy == 6 || sy == 9 || sy == 10) && sx >= 3 && sx <= 12;
            px[y * size + x] = !inside ? 0u : bar ? 0xFFFFFFFFu : 0xFF3B8CFFu;
        }
    }
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

// Application object; bound to the controller HWND through GWLP_USERDATA.
class App {
public:
    explicit App(HINSTANCE hInst) : hInst_(hInst), cfgFile_(userDataDir() / L"config.json") {}
    ~App() { stopWorker(); }

    bool init();

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT m, WPARAM w, LPARAM l);

    void onCreate();
    void onDestroy();
    void refresh();
    void onFetchDone();
    void beginQuit();
    void stopWorker();
    void pushState();
    void scheduleRefresh();
    bool trayNotify(DWORD msg);
    void addTray();
    void updateTip();
    void showMenu(POINT pt);
    void setDock(const std::string& dock);
    void saveCfg();

    HINSTANCE hInst_;
    HWND hwnd_ = nullptr;
    std::filesystem::path cfgFile_;
    Config cfg_;
    std::unique_ptr<Dock> dock_;
    std::optional<UsageData> data_;
    std::string error_;
    bool dragMode_ = false, trayAdded_ = false, quitting_ = false;
    ULONGLONG quitStart_ = 0;
    HICON icon_ = nullptr;
    UINT wmTaskbarCreated_ = 0;
    // Worker state. cancel_/done_ are shared_ptrs so the thread keeps them alive
    // even if App moves on to a new fetch; done_ lets the UI poll without joining.
    std::thread worker_;
    std::shared_ptr<CancelToken> cancel_;
    std::shared_ptr<std::atomic<bool>> done_;
    // Mailbox: worker deposits one result under the mutex, then rings the
    // WM_FETCH_DONE doorbell; the UI thread drains it. Keeps data off the message
    // queue (no raw pointers in LPARAM) and coalesces results.
    std::mutex mailboxMu_;
    std::optional<FetchResult> mailbox_;
};

bool App::init()
{
    if (!panel::registerClass(hInst_)) return false;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst_;
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassExW(&wc)) return false;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, CLASS_NAME, TIP_BASE, WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, hInst_,
                            this);
    return hwnd_ != nullptr;
}

LRESULT CALLBACK App::wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    // WM_NCCREATE arrives before CreateWindowEx returns, so bind `this` here.
    if (m == WM_NCCREATE) {
        auto* self = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT App::handle(UINT m, WPARAM w, LPARAM l)
{
    // While shutting down, ignore anything that would touch the (destroyed) dock/tray
    // or start new work; timers still run so TIMER_QUIT can finish the exit.
    if (quitting_ && m != WM_TIMER && m != WM_DESTROY && m != WM_NCDESTROY) {
        if (m == WM_TRAY || m == WM_CUT_MENU || m == WM_FETCH_DONE || m == WM_CUT_RELAYOUT ||
            (wmTaskbarCreated_ && m == wmTaskbarCreated_) || m == WM_DISPLAYCHANGE || m == WM_SETTINGCHANGE)
            return 0;
    }
    // Explorer restarted (crash or user restart): the tray icon and the taskbar
    // window we were embedded in are gone. Re-add the icon and rebuild the dock,
    // retrying embedding since the earlier failure may have been transient.
    if (wmTaskbarCreated_ && m == wmTaskbarCreated_) {
        trayAdded_ = false;
        addTray();
        if (dock_) dock_->rebuild(cfg_, true);
        pushState();
        return 0;
    }
    switch (m) {
    case WM_CREATE:
        onCreate();
        return 0;
    case WM_DESTROY:
        onDestroy();
        return 0;
    case WM_TIMER:
        if (quitting_ && w != TIMER_QUIT) return 0;
        switch (w) {
        case TIMER_REFRESH: refresh(); break;
        case TIMER_REPAINT: pushState(); break;
        // 1s watchdog: retries a missing tray icon and lets the Dock detect a
        // replaced taskbar, mode changes and fullscreen apps.
        case TIMER_DOCK:
            if (!trayAdded_) addTray();
            dock_->tick(cfg_);
            break;
        // One-shot debounce for display/DPI/settings changes, which arrive in bursts
        // while the taskbar is still being re-laid out by the shell.
        case TIMER_LAYOUT:
            KillTimer(hwnd_, TIMER_LAYOUT);
            dock_->tick(cfg_);
            break;
        case TIMER_QUIT:
            if (done_ && done_->load()) {
                KillTimer(hwnd_, TIMER_QUIT);
                DestroyWindow(hwnd_);
            } else if (GetTickCount64() - quitStart_ >= QUIT_WAIT_MS) {
                ExitProcess(0);
            }
            break;
        }
        return 0;
    case WM_FETCH_DONE:
        onFetchDone();
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
    case WM_CUT_RELAYOUT:
        SetTimer(hwnd_, TIMER_LAYOUT, 400, nullptr);
        return 0;
    // Sent synchronously by the panel; the Dock may adjust the POINT in place.
    case WM_CUT_DRAGMOVE:
        dock_->dragMove(*reinterpret_cast<POINT*>(l));
        return 0;
    case WM_CUT_DRAGEND:
        dock_->dragEnd(cfg_, *reinterpret_cast<POINT*>(l));
        saveCfg();
        return 0;
    case WM_CUT_MENU:
        showMenu({static_cast<int>(w), static_cast<int>(l)});
        return 0;
    case WM_CUT_QUIT:
    case WM_CLOSE:
        beginQuit();
        return 0;
    case WM_TRAY:
        switch (LOWORD(l)) {
        // NOTIFYICON_VERSION_4: event in LOWORD(lParam), anchor point packed in wParam.
        case NIN_SELECT:
        case NIN_KEYSELECT:
            dock_->toggleVisible();
            break;
        case WM_CONTEXTMENU:
            showMenu({GET_X_LPARAM(w), GET_Y_LPARAM(w)});
            break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void App::onCreate()
{
    wmTaskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    cfg_ = loadConfig(cfgFile_);
    dock_ = std::make_unique<Dock>(hInst_, hwnd_);
    icon_ = makeTrayIcon();
    addTray();
    dock_->rebuild(cfg_, true);
    refresh();
    pushState();
    scheduleRefresh();
    SetTimer(hwnd_, TIMER_REPAINT, 30000, nullptr);
    SetTimer(hwnd_, TIMER_DOCK, 1000, nullptr);
}

// Tear-down order matters: stop timers (no re-entrancy), remove tray icon and
// dock window, then join the worker, and only then post WM_QUIT.
void App::onDestroy()
{
    quitting_ = true;
    KillTimer(hwnd_, TIMER_REFRESH);
    KillTimer(hwnd_, TIMER_REPAINT);
    KillTimer(hwnd_, TIMER_DOCK);
    KillTimer(hwnd_, TIMER_LAYOUT);
    KillTimer(hwnd_, TIMER_QUIT);
    if (trayAdded_) trayNotify(NIM_DELETE);
    trayAdded_ = false;
    if (dock_) dock_->destroy();
    stopWorker();
    if (icon_) DestroyIcon(icon_);
    icon_ = nullptr;
    PostQuitMessage(0);
}

// Non-blocking quit: cancel the in-flight request and, if the worker has not
// finished, poll via TIMER_QUIT instead of joining (a blocking join on the UI
// thread could hang the taskbar when embedded). Falls back to ExitProcess after
// QUIT_WAIT_MS. Dock and tray are removed immediately so the UI vanishes at once.
void App::beginQuit()
{
    if (quitting_) return;
    quitting_ = true;
    KillTimer(hwnd_, TIMER_REFRESH);
    KillTimer(hwnd_, TIMER_REPAINT);
    KillTimer(hwnd_, TIMER_DOCK);
    KillTimer(hwnd_, TIMER_LAYOUT);
    if (dock_) dock_->destroy();
    if (trayAdded_) trayNotify(NIM_DELETE);
    trayAdded_ = false;
    if (cancel_) cancel_->cancel();
    if (worker_.joinable() && done_ && !done_->load()) {
        quitStart_ = GetTickCount64();
        SetTimer(hwnd_, TIMER_QUIT, 50, nullptr);
        return;
    }
    DestroyWindow(hwnd_);
}

// Cancels and joins; called only when the worker is known to finish promptly
// (cancel aborts the HTTP / child-process wait).
void App::stopWorker()
{
    if (cancel_) cancel_->cancel();
    if (worker_.joinable()) worker_.join();
}

// Starts a fetch unless one is still running (skip rather than queue). The
// previous finished thread is joined here, which is instant as done_ is set.
void App::refresh()
{
    if (quitting_) return;
    if (worker_.joinable()) {
        if (!done_->load()) return;
        worker_.join();
    }
    cancel_ = std::make_shared<CancelToken>();
    done_ = std::make_shared<std::atomic<bool>>(false);
    worker_ = std::thread([this, cancel = cancel_, done = done_, dir = cfgFile_.parent_path()] {
        FetchResult r;
        try {
            r.data = fetchUsage(dir, cancel.get());
            r.ok = true;
        } catch (const std::exception& e) {
            r.error = e.what();
        } catch (...) {
            r.error = "Unknown error";
        }
        {
            std::lock_guard<std::mutex> lk(mailboxMu_);
            mailbox_ = std::move(r);
        }
        // Order: publish result, mark done, then ring the doorbell, so the UI never
        // sees the doorbell before the data. PostMessage (not Send) never blocks us.
        done->store(true);
        PostMessageW(hwnd_, WM_FETCH_DONE, 0, 0);
    });
}

void App::onFetchDone()
{
    std::optional<FetchResult> r;
    {
        std::lock_guard<std::mutex> lk(mailboxMu_);
        r = std::move(mailbox_);
        mailbox_.reset();
    }
    if (!r) return;
    if (r->ok) {
        data_ = std::move(r->data);
        error_.clear();
    } else {
        error_ = r->error.empty() ? "Unknown error" : r->error;
    }
    pushState();
    updateTip();
}

void App::pushState()
{
    if (!dock_) return;
    ViewState vs = buildState(data_, error_, cfg_.display, dragMode_, nowMs());
    // Until the first result arrives, show a placeholder in the amount column.
    bool fetching = worker_.joinable() && done_ && !done_->load();
    if (fetching && !data_) vs.credits.amountText = "Fetching...";
    dock_->setView(vs);
}

// SetTimer takes a UINT; clamp huge configured intervals instead of overflowing.
void App::scheduleRefresh()
{
    double ms = cfg_.refreshMinutes * 60000.0;
    UINT interval = ms >= 2147483647.0 ? 2147483647u : static_cast<UINT>(ms);
    SetTimer(hwnd_, TIMER_REFRESH, interval, nullptr);
}

bool App::trayNotify(DWORD msg)
{
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = icon_;
    std::wstring tip = TIP_BASE;
    if (!error_.empty()) tip += L"\n" + widen(error_);
    tip = clampTip(tip);
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    // NIF_SHOWTIP is required with VERSION_4 or the standard tooltip is suppressed.
    nid.uVersion = NOTIFYICON_VERSION_4;
    bool ok = Shell_NotifyIconW(msg, &nid) != FALSE;
    if (ok && msg == NIM_ADD) Shell_NotifyIconW(NIM_SETVERSION, &nid);
    return ok;
}

void App::addTray()
{
    if (!trayAdded_) trayAdded_ = trayNotify(NIM_ADD);
}

void App::updateTip()
{
    if (trayAdded_) trayNotify(NIM_MODIFY);
}

// Persisting is best-effort; a write failure must not crash the UI.
void App::saveCfg()
{
    try {
        saveConfig(cfgFile_, cfg_);
    } catch (const std::exception&) {
    }
}

void App::setDock(const std::string& dock)
{
    cfg_.dock = dock;
    saveCfg();
    dock_->rebuild(cfg_, true);
}

void App::showMenu(POINT pt)
{
    HMENU menu = CreatePopupMenu();
    HMENU sub = CreatePopupMenu();
    if (!menu || !sub) {
        if (menu) DestroyMenu(menu);
        if (sub) DestroyMenu(sub);
        return;
    }
    AppendMenuW(sub, MF_STRING, IDM_DOCK_BASE, L"Embed in taskbar");
    AppendMenuW(sub, MF_STRING, IDM_DOCK_BASE + 1, L"Overlay on taskbar");
    AppendMenuW(sub, MF_STRING, IDM_DOCK_BASE + 2, L"Floating");
    CheckMenuRadioItem(sub, IDM_DOCK_BASE, IDM_DOCK_BASE + 2, IDM_DOCK_BASE + dockMenuIndex(cfg_.dock), MF_BYCOMMAND);

    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"Refresh");
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE, L"Toggle overlay");
    AppendMenuW(menu, MF_STRING | (dragMode_ ? MF_CHECKED : 0), IDM_MOVE, L"Move overlay (drag)");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"Dock mode");
    AppendMenuW(menu, MF_STRING | (cfg_.dock == "none" ? MF_GRAYED : 0), IDM_UNDOCK, L"Undock (float)");
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CONFIG, L"Open config file");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // Documented TrackPopupMenu workaround: the owner must be foreground or the
    // menu will not dismiss when the user clicks elsewhere; the WM_NULL posted
    // afterwards forces a message-loop turn so the menu closes correctly next time.
    // Menu opens above the cursor when it is in the lower half of the screen.
    SetForegroundWindow(hwnd_);
    UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | (pt.y > GetSystemMetrics(SM_CYSCREEN) / 2 ? TPM_BOTTOMALIGN : 0);
    int cmd = TrackPopupMenuEx(menu, flags, pt.x, pt.y, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
    case IDM_REFRESH: refresh(); break;
    case IDM_TOGGLE: dock_->toggleVisible(); break;
    case IDM_MOVE:
        dragMode_ = !dragMode_;
        pushState();
        break;
    case IDM_UNDOCK: setDock("none"); break;
    case IDM_OPEN_CONFIG: ShellExecuteW(nullptr, L"open", cfgFile_.c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case IDM_EXIT: beginQuit(); break;
    case IDM_DOCK_BASE:
    case IDM_DOCK_BASE + 1:
    case IDM_DOCK_BASE + 2: setDock(dockFromMenuIndex(cmd - IDM_DOCK_BASE)); break;
    }
}

} // namespace

/// Creates the controller and runs the message loop; returns the exit code.
int runApp(HINSTANCE hInst)
{
    App app(hInst);
    if (!app.init()) return 1;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
