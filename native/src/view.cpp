// view.cpp - Display formatting. Output matches the Electron renderer (JS parity),
// including Intl-style currency symbols and grouping.
#include "view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

std::string fixed(double v, int digits)
{
    char buf[512];
    std::snprintf(buf, sizeof buf, "%.*f", digits, v);
    return buf;
}

bool wellFormedCode(const std::string& c)
{
    if (c.size() != 3) return false;
    return std::all_of(c.begin(), c.end(), [](char ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'); });
}

// True when the fetch error means the Claude CLI has no usable login.
bool looksLoggedOut(const std::string& e)
{
    for (const char* k : {"No OAuth credentials", "OAuth token expired", "Invalid OAuth token", "HTTP 401", "HTTP 403"})
        if (e.find(k) != std::string::npos) return true;
    return false;
}

} // namespace

std::string formatMoney(double value, const std::optional<std::string>& currency, int fractionDigits)
{
    std::string code = currency && !currency->empty() ? *currency : "USD";
    // Intl.NumberFormat throws on malformed codes and the JS caller falls back to
    // "<value> <code>"; reproduce that fallback.
    if (!wellFormedCode(code) || !std::isfinite(value)) return fixed(value, fractionDigits) + " " + (currency ? *currency : "null");
    for (auto& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    std::string num = fixed(std::fabs(value), fractionDigits);
    size_t dot = num.find('.');
    std::string intPart = num.substr(0, dot), frac = dot == std::string::npos ? "" : num.substr(dot);
    for (int i = static_cast<int>(intPart.size()) - 3; i > 0; i -= 3) intPart.insert(static_cast<size_t>(i), ",");
    num = intPart + frac;

    // No minus sign for values that round to zero ("-0.00" is never shown).
    std::string sign = (value < 0 && num.find_first_not_of("0,.") != std::string::npos) ? "-" : "";
    const char* sym = nullptr;
    if (code == "USD") sym = "$";
    else if (code == "EUR") sym = "\xE2\x82\xAC";
    else if (code == "GBP") sym = "\xC2\xA3";
    else if (code == "JPY") sym = "\xC2\xA5";
    else if (code == "CAD") sym = "CA$";
    else if (code == "AUD") sym = "A$";
    if (sym) return sign + sym + num;
    // Unknown symbols: ISO code + NBSP, as Intl does.
    return sign + code + "\xC2\xA0" + num;
}

std::string formatAmounts(const Credits& win)
{
    if (!win.used || !win.limit || !std::isfinite(*win.used) || !std::isfinite(*win.limit)) return "";
    int limitDigits = *win.limit == std::floor(*win.limit) ? 0 : 2;
    return formatMoney(*win.used, win.currency, 2) + " / " + formatMoney(*win.limit, win.currency, limitDigits);
}

Row buildRow(const std::optional<Credits>& win, const std::string& display, int64_t nowMs)
{
    // "--" for reset text means "hide the reset column".
    if (!win) return Row{0, "--", "--", ""};
    double used = win->percent;
    double shown = display == "remaining" ? 100 - used : used;
    Row r;
    r.fill = std::min(100.0, std::max(0.0, shown));
    r.pctText = fixed(r.fill, 1) + "%";
    r.amountText = formatAmounts(*win);
    r.resetText = win->resetsAt && *win->resetsAt != 0 ? formatRemaining(*win->resetsAt - nowMs) : "--";
    return r;
}

ViewState buildState(const std::optional<UsageData>& data, const std::string& error, const std::string& display,
                     bool dragging, int64_t nowMs)
{
    ViewState s;
    s.credits = buildRow(data ? data->credits : std::nullopt, display, nowMs);
    // A failed refresh keeps the last good data visible but dimmed ("stale").
    s.stale = !error.empty();
    s.badge = error.empty() ? "" : (data ? "stale" : "error");
    s.error = error;
    s.dragging = dragging;
    if (!data && looksLoggedOut(error)) {
        s.credits.amountText = "Logged out - run claude /login";
        s.badge.clear();
    }
    return s;
}
