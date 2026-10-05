#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include "parser.h"

// view.h - Turns parsed usage data into display-ready strings (no Win32).

/// One rendered usage row: bar fill (0-100) and preformatted texts ("--" reset = hidden).
struct Row {
    double fill = 0;
    std::string pctText, resetText, amountText;
    bool operator==(const Row&) const = default;
};

/// Everything the panel needs to paint; `stale` dims it, `badge` shows an error/status tag.
struct ViewState {
    Row credits;
    bool stale = false;
    std::string badge, error;
    bool dragging = false;
    bool light = false; // light-mode palette (dark text)
};

/// Currency-formatted amount ("$1,234.50", "CODE 1,234.50"); mirrors Intl.NumberFormat output.
std::string formatMoney(double value, const std::optional<std::string>& currency, int fractionDigits);
/// "used / limit" text (limit shown without decimals when whole); empty when amounts are unknown.
std::string formatAmounts(const Credits& win);
/// Builds a row; `display` selects used vs remaining percentage.
Row buildRow(const std::optional<Credits>& win, const std::string& display, int64_t nowMs);
/// Combines latest data and last error into the view (stale/badge when the fetch failed).
ViewState buildState(const std::optional<UsageData>& data, const std::string& error, const std::string& display,
                     bool dragging, int64_t nowMs);
