# Claude Usage Tracker – native app

A native Win32/C++ build of the Claude Usage Tracker (no Electron, no Node). By default it docks into the Windows taskbar and shows your Claude usage credits at a glance. It reads the same config file as the Electron app (see the [main README](../README.md)).

The widget is labelled **Claude Usage** and shows:

- a continuous green-to-red gradient bar filled to the current usage,
- the percentage with one decimal place (e.g. `1.4%`, `97.0%`),
- the absolute amount: `$ used / limit` (e.g. `$6.19 / $500`),
- the time until the usage resets, when the data source reports it (hidden otherwise).

The widget has a fixed size of 440×36 DIPs (scaled to the monitor DPI; per-monitor DPI aware). It never takes focus. Only one instance can run at a time.

## Docking modes

Set with the `dock` config option or the right-click menu (*Dock mode*):

| `dock` | Behaviour |
| --- | --- |
| `"embed"` (default) | The widget is a child window of the Windows taskbar (`Shell_TrayWnd`), placed just left of the notification area and vertically centred in the taskbar. |
| `"overlay"` | A topmost window positioned over the taskbar in the same place. It re-asserts topmost when the foreground window changes and is hidden while a full-screen app is in the foreground. |
| `"none"` | A free-floating, always-on-top window placed using `anchor`/`offsetX`/`offsetY` or `x`/`y`, like the Electron app. Also hidden while a full-screen app is in the foreground. |

Fallbacks (re-evaluated every second):

- If the taskbar is vertical (left/right), is auto-hidden, is too short for the widget, or cannot be found, the widget falls back to **floating**. It docks again automatically once the taskbar is usable. This applies to both `embed` and `overlay`.
- If embedding into the taskbar fails (the re-parenting is rejected), the widget falls back to **overlay**. Embedding is retried when the dock mode is changed from the menu or when Explorer restarts.
- An unknown `dock` value is treated as `"embed"`.

Only the primary taskbar (`Shell_TrayWnd`) is used.

## Interaction

- **Close:** hovering shows a subtle **×** at the top-right; clicking it quits the app.
- **Move:** press and drag on the widget body; the position is saved to the config on release.
  - Docked (`embed`/`overlay`): movement is horizontal only, constrained to the taskbar; `offsetX` is saved.
  - Floating (`none`): free movement; `x` and `y` are saved (and then override `anchor`/offsets).
- **Context menu:** right-click the widget (or the tray icon):
  - **Refresh** – fetch usage now
  - **Toggle overlay** – show or hide the widget (a left-click on the tray icon does this too)
  - **Move overlay (drag)** – check to show a dashed outline and a move cursor on the widget; uncheck to remove them. Dragging works with or without this mode, and the native widget is never click-through.
  - **Dock mode** – *Embed in taskbar*, *Overlay on taskbar* or *Floating*; saved to `dock` in the config and applied immediately
  - **Undock (float)** – same as *Floating* (disabled when already `none`)
  - **Open config file** – opens `config.json` in the default editor
  - **Exit**

The tray tooltip shows the latest error message, if any.

## Data sources

1. **OAuth usage endpoint (primary)** – `GET https://api.anthropic.com/api/oauth/usage` (WinHTTP, system proxy). The `extra_usage` object supplies the utilization percent, used amount, monthly limit and currency. The access token is read from `%USERPROFILE%\.claude\.credentials.json` (`claudeAiOauth.accessToken`) and sent only in the `Authorization` header. A missing, invalid or expired token makes this source fail.
2. **`claude` CLI via ConPTY (fallback)** – if the OAuth call fails, the app starts `claude` (via `%ComSpec% /c claude`) in a Windows pseudo-console (140×50) inside a kill-on-close job object, accepts the folder-trust prompt if shown, sends `/usage`, and parses the usage-credits section of the output. It gives up after 30 seconds. The process is started in the config directory (`%APPDATA%\claude-usage-tracker`).

If both fail, the error messages are combined (`oauth: …; pty: …`) and shown via the badge below and in the tray tooltip.

## Refresh and status badges

- Data refreshes every **5 minutes** by default (`refreshMinutes`), and on demand via *Refresh*. A refresh request is ignored while one is already running. Fetching runs on a background thread.
- If a refresh fails but earlier data exists, the widget keeps showing it dimmed with a **stale** badge.
- If a refresh fails and there is no data yet, it shows an **error** badge.
- Before any data exists, the percent shows `--`.
- The display is repainted every 30 seconds so the reset countdown stays current.

## Configuration

File: `%APPDATA%\claude-usage-tracker\config.json` (shared with the Electron app; created with defaults on first run). Restart the app after editing, except for dock mode changed via the menu. Unknown keys are preserved when the app saves the file. An unreadable or invalid file falls back to defaults.

| Option | Default | Description |
| --- | --- | --- |
| `anchor` | `"bottom-right"` | Floating only. Screen corner used for placement: `top-left`, `top-right`, `bottom-left` or `bottom-right` (a string containing `right`/`bottom` selects that side; otherwise left/top), relative to the primary display's work area. |
| `offsetX` | `16` | Floating: horizontal distance (px) from the anchored screen edge. Docked: distance in **physical pixels** from the left edge of the notification area (or the taskbar's right edge if it cannot be found) to the widget's right edge. Saved when dragging a docked widget. |
| `offsetY` | `8` | Floating only. Vertical distance (px) from the anchored edge. Ignored when docked (the widget is vertically centred in the taskbar). |
| `x`, `y` | `null` | Floating only. Absolute position; when both are numbers they override `anchor`/offsets. Set automatically after dragging a floating widget. Ignored when docked. |
| `display` | `"used"` | `"used"` shows percent used; `"remaining"` shows percent remaining (the bar fills accordingly). Invalid values fall back to `"used"`. |
| `refreshMinutes` | `5` | Refresh interval in minutes (must be ≥ 1, otherwise 5). |
| `opacity` | `1` | Widget opacity, clamped to 0.1–1 (`0` is treated as `1`). |
| `dock` | `"embed"` | `"embed"`, `"overlay"` or `"none"` (see [Docking modes](#docking-modes)). Invalid values fall back to `"embed"`. |

Floating positions are clamped to the virtual screen.

## Build

Requires Visual Studio 2022 (C++ desktop workload) and CMake ≥ 3.20. Dependencies (nlohmann/json, doctest) are vendored under `third_party/`.

The easiest way is the build script, which locates CMake, configures only when needed, builds, runs the tests and copies the exe to `native/dist/ClaudeUsageTracker.exe`:

```
nativeuild.cmd                  # Release build + tests
nativeuild.cmd -Clean -NoTests   # move native/build to native/.trash/, rebuild, skip tests
nativeuild.cmd -Config Debug -Run
```

Options: `-Config Release|Debug` (default Release), `-Clean`, `-NoTests`, `-Run`. The script exits non-zero on any failure. Or run CMake directly:

```
cmake -S native -B native/build -G "Visual Studio 17 2022" -A x64
cmake --build native/build --config Release
ctest --test-dir native/build -C Release
```

The executable is written to `native/build/Release/ClaudeUsageTracker.exe`. It links the C runtime statically, so it is a single self-contained exe: no installer or redistributable is needed. The unit tests read fixtures from `native/tests/fixtures`. The build also produces a small `cut_probe` console helper for the data sources.

## Project layout

```
native/
  CMakeLists.txt     Targets: cut_core, cut_sources, cut_probe, cut_tests, ClaudeUsageTracker
  app.manifest       Per-monitor DPI v2, UTF-8 code page, Windows 10+ compatibility
  app.rc             Embeds the manifest and version info
  src/main.cpp       Entry point, single-instance mutex
  src/app.cpp        Controller window: tray, context menu, refresh scheduling, timers, quit
  src/panel.cpp      The widget window: painting, hover ×, drag, right-click
  src/taskbar.cpp    Dock: taskbar lookup, embed/overlay/floating placement and fallbacks
  src/layout.cpp     Pure geometry: floating/dock positions, taskbar edge, auto-hide detection
  src/uihelpers.cpp  DPI scaling, opacity clamp, dock offset, menu index helpers
  src/sources.cpp    Data fetching: OAuth endpoint, ConPTY `claude /usage` fallback
  src/parser.cpp     Parses OAuth JSON and /usage text output; duration formatting
  src/view.cpp       Turns data into display state (percent text, fill, amounts, reset, badge)
  src/config.cpp     Defaults, load/save config
  src/probe.cpp      Console probe for the data sources
  tests/             Unit tests (doctest)
  third_party/       Vendored nlohmann/json and doctest
```

## Known limitations

- Windows 10 1903 or later / Windows 11 only (ConPTY, per-monitor DPI v2 and the UTF-8 code page manifest setting).
- `embed` relies on the undocumented `Shell_TrayWnd` / `TrayNotifyWnd` window structure and re-parenting a window into the taskbar; it may break with Windows updates. `overlay` or `none` are the fallbacks.
- If Explorer restarts, the widget is recreated (and embedding retried).
- Config changes require a restart, except dock mode changed via the menu. Because the app saves its in-memory config when you drag or change dock mode, edits made to the file while the app is running may be overwritten.
- Rounded corners use a window region and are not anti-aliased.
- Depends on the Claude Code credentials file; the app does not refresh OAuth tokens, so an expired token means falling back to the slower ConPTY source until you use Claude Code again.
- The OAuth usage endpoint is undocumented and may change; the `/usage` text parser likewise depends on the CLI's output format.
- Only the usage-credits (`extra_usage`) figure is shown, not session or weekly limits.
- Docking uses the primary taskbar only; vertical and auto-hidden taskbars are not supported (the widget floats instead).
