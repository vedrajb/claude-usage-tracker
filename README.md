# Claude Usage Tracker

A small native Windows (C++/Win32) widget that shows your Claude usage credits docked in the taskbar: a green-to-red bar, the percent used and `$ used / limit`. It uses about 3 MB of memory.

## Build and run

```
build.bat                  # Release build + unit tests
build.bat -Clean -Run      # clean rebuild, then start the app
build.bat -Config Debug    # Debug build
build.bat -NoTests         # skip unit tests
```

Requirements: Visual Studio 2022 (C++ workload) and CMake.

Output: `native\dist\ClaudeUsageTracker.exe`, a single self-contained exe that you can copy anywhere.

Right-click the widget (or the tray icon) for Refresh, Dock mode, Open config file and Exit.

Configuration: `%APPDATA%\claude-usage-tracker\config.json`.

See [native/README.md](native/README.md) for full documentation: docking modes, data sources, configuration options, project layout and known limitations.
