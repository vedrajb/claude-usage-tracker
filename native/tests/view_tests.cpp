#include <doctest/doctest.h>

#include "view.h"

namespace {

const int64_t NOW = 1'000'000'000'000;

Credits win(double percent, std::optional<int64_t> resetsAt = std::nullopt)
{
    Credits c;
    c.percent = percent;
    c.resetsAt = resetsAt;
    return c;
}

Row row(double fill, const char* pct, const char* reset, const char* amount)
{
    return Row{fill, pct, reset, amount};
}

} // namespace

TEST_CASE("buildRow: used display")
{
    auto r = buildRow(win(97, NOW + (3 * 1440 + 18 * 60) * 60000LL), "used", NOW);
    CHECK(r == row(97, "97.0%", "3d 18h", ""));
}

TEST_CASE("buildRow: remaining display fills by shown percent")
{
    auto r = buildRow(win(90), "remaining", NOW);
    CHECK(r.pctText == "10.0%");
    CHECK(r.fill == 10);
    CHECK(r.resetText == "--");
}

TEST_CASE("buildRow: fill equals percent, is clamped; null window is empty")
{
    CHECK(buildRow(win(2), "used", NOW).fill == 2);
    CHECK(buildRow(win(0), "used", NOW).fill == 0);
    CHECK(buildRow(win(130), "used", NOW).fill == 100);
    CHECK(buildRow(win(-5), "used", NOW).fill == 0);
    CHECK(buildRow(std::nullopt, "used", NOW) == row(0, "--", "--", ""));
}

TEST_CASE("buildRow: amountText formats used / limit with currency")
{
    Credits w = win(1.238);
    w.used = 6.19;
    w.limit = 500;
    w.currency = "USD";
    CHECK(buildRow(w, "used", NOW).amountText == "$6.19 / $500");
    Credits w2 = w;
    w2.limit = 500.5;
    CHECK(buildRow(w2, "used", NOW).amountText == "$6.19 / $500.50");
    Credits w3 = w;
    w3.currency = "EUR";
    CHECK(buildRow(w3, "used", NOW).amountText == "\xE2\x82\xAC" "6.19 / \xE2\x82\xAC" "500");
    Credits w4 = w;
    w4.currency = "bad!";
    CHECK(buildRow(w4, "used", NOW).amountText == "6.19 bad! / 500 bad!");
}

TEST_CASE("buildRow: amountText empty when values missing")
{
    CHECK(buildRow(win(5), "used", NOW).amountText == "");
    Credits a = win(5);
    a.used = 1;
    CHECK(buildRow(a, "used", NOW).amountText == "");
    a.used.reset();
    a.limit = 1;
    CHECK(buildRow(a, "used", NOW).amountText == "");
}

TEST_CASE("formatMoney: symbols, grouping, other currencies")
{
    CHECK(formatMoney(1234567.891, std::string("USD"), 2) == "$1,234,567.89");
    CHECK(formatMoney(1234, std::string("GBP"), 0) == "\xC2\xA3" "1,234");
    CHECK(formatMoney(5, std::string("JPY"), 0) == "\xC2\xA5" "5");
    CHECK(formatMoney(5, std::string("CAD"), 2) == "CA$5.00");
    CHECK(formatMoney(5, std::string("AUD"), 2) == "A$5.00");
    CHECK(formatMoney(1, std::string("CHF"), 2) == "CHF\xC2\xA0" "1.00");
    CHECK(formatMoney(-1.5, std::string("usd"), 2) == "-$1.50");
    CHECK(formatMoney(2, std::nullopt, 2) == "$2.00");
}

TEST_CASE("buildState: stale vs error badge")
{
    UsageData data;
    data.credits = win(10);
    CHECK(buildState(data, "x", "used", false, NOW).badge == "stale");
    CHECK(buildState(std::nullopt, "x", "used", false, NOW).badge == "error");
    CHECK(buildState(data, "", "used", false, NOW).badge == "");
    CHECK(buildState(data, "x", "used", true, NOW).stale);
    CHECK(buildState(data, "x", "used", true, NOW).dragging);
}

TEST_CASE("buildState: only a credits row, with fill equal to percent and no reset text")
{
    UsageData data;
    data.credits = win(91.4);
    auto s = buildState(data, "", "used", false, NOW);
    CHECK(s.credits == row(91.4, "91.4%", "--", ""));
    UsageData none;
    CHECK(buildState(none, "", "used", false, NOW).credits.pctText == "--");
}

TEST_CASE("buildRow: percent shown with one decimal place")
{
    CHECK(buildRow(win(1.238), "used", NOW).pctText == "1.2%");
    CHECK(buildRow(win(1.4), "used", NOW).pctText == "1.4%");
    CHECK(buildRow(win(0), "used", NOW).pctText == "0.0%");
    CHECK(buildRow(win(130), "used", NOW).pctText == "100.0%");
}
