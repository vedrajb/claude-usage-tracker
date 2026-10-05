#include <doctest/doctest.h>

#include <chrono>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "parser.h"

namespace {

using nlohmann::json;

int64_t utcMs(int y, unsigned mo, unsigned d, int h = 0, int mi = 0)
{
    using namespace std::chrono;
    sys_time<milliseconds> t = sys_days{year{y} / month{mo} / day{d}} + hours{h} + minutes{mi};
    return t.time_since_epoch().count();
}

const int64_t NOW = utcMs(2025, 10, 4, 10, 0);

std::string fixture(const char* name)
{
    std::ifstream in(std::string(FIXTURE_DIR) + "/" + name, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("parseResetText: time only, later today")
{
    CHECK(parseResetText("Resets 4pm (Europe/London)", NOW) == utcMs(2025, 10, 4, 15));
}

TEST_CASE("parseResetText: time only rolls to tomorrow")
{
    CHECK(parseResetText("Resets 9am (UTC)", NOW) == utcMs(2025, 10, 5, 9));
}

TEST_CASE("parseResetText: date and minutes")
{
    CHECK(parseResetText("Resets Oct 8, 2:30pm (Europe/London)", NOW) == utcMs(2025, 10, 8, 13, 30));
}

TEST_CASE("parseResetText: 12am / 12pm")
{
    CHECK(parseResetText("Resets 12am (UTC)", NOW) == utcMs(2025, 10, 5, 0));
    CHECK(parseResetText("Resets 12pm (UTC)", NOW) == utcMs(2025, 10, 4, 12));
}

TEST_CASE("parseResetText: past date rolls to next year")
{
    CHECK(parseResetText("Resets Jan 2, 1am (UTC)", NOW) == utcMs(2026, 1, 2, 1));
}

TEST_CASE("parseResetText: unknown timezone falls back to local without throwing")
{
    CHECK(parseResetText("Resets 4pm (Not/AZone)", NOW).has_value());
}

TEST_CASE("parseResetText: no match returns null")
{
    CHECK_FALSE(parseResetText("nothing here", NOW).has_value());
}

TEST_CASE("parseResetText: DST gap and overlap are handled")
{
    CHECK(parseResetText("Resets Mar 9, 2:30am (America/New_York)", utcMs(2025, 3, 1)).has_value());
    CHECK(parseResetText("Resets Nov 2, 1:30am (America/New_York)", utcMs(2025, 10, 20)) == utcMs(2025, 11, 2, 5, 30));
}

TEST_CASE("parseUsageText: usage credits percent from fixture (no time-of-day reset)")
{
    auto r = parseUsageText(fixture("usage-credits-only.txt"), NOW);
    REQUIRE(r.credits.has_value());
    CHECK(r.credits->percent == 0);
    CHECK_FALSE(r.credits->resetsAt.has_value());
    CHECK_FALSE(r.credits->used.has_value());
}

TEST_CASE("parseUsageText: non-zero credits without reset line")
{
    auto r = parseUsageText(fixture("usage-credits-no-reset.txt"), NOW);
    REQUIRE(r.credits.has_value());
    CHECK(r.credits->percent == 1);
    CHECK_FALSE(r.credits->resetsAt.has_value());
}

TEST_CASE("parseUsageText: session/week-only output throws")
{
    CHECK_THROWS_WITH_AS(parseUsageText(fixture("usage-limits.txt"), NOW), doctest::Contains("No usage credits"),
                         std::runtime_error);
}

TEST_CASE("parseUsageText: ANSI redraws use latest values and cursor-forward acts as space")
{
    const std::string esc = "\x1b";
    const std::string raw =
        esc + "[2J" + esc + "[1;1HUsage credits" + esc + "[3;1H" + esc + "[38;5;75m\xE2\x96\x8D" + esc + "[0m  5% used" +
        esc + "[1;1HUsage" + esc + "[1Ccredits" + esc + "[3;1H\xE2\x96\x88\xE2\x96\x88  12%" + esc + "[1Cused" + esc +
        "[4;1HResets" + esc + "[1C4pm" + esc + "[1C(UTC)";
    auto r = parseUsageText(raw, NOW);
    REQUIRE(r.credits.has_value());
    CHECK(r.credits->percent == 12);
    CHECK(r.credits->resetsAt == utcMs(2025, 10, 4, 16));
}

TEST_CASE("parseUsageText: does not read the next section percent")
{
    CHECK_THROWS(parseUsageText("Usage credits\n spent\n\n d to day\n 40% used", NOW));
}

TEST_CASE("stripAnsi removes escape sequences")
{
    CHECK(stripAnsi("\x1b[31mred\x1b[0m\x1b]0;title\x07") == "red");
}

TEST_CASE("stripAnsi: spacing, cursor, charset, keypad, CR, unterminated OSC")
{
    CHECK(stripAnsi("a\x1b[3Cb") == "a b");
    CHECK(stripAnsi("a\x1b[2;1Hb") == "a\nb");
    CHECK(stripAnsi("a\x1b(Bb\x1b)0c\x1b=d\x1b>e") == "abcde");
    CHECK(stripAnsi("a\rb") == "a\nb");
    CHECK(stripAnsi("\x1b]0;t\x1b\\x") == "x");
    CHECK(stripAnsi("\x1b]0;unterminated") == "\x1b]0;unterminated");
}

TEST_CASE("stripAnsi: large buffer does not overflow the stack")
{
    std::string big;
    for (int i = 0; i < 200000; ++i) big += "\x1b[31mab\x1b[0m\r";
    CHECK(stripAnsi(big).size() == 200000 * 3);
}

TEST_CASE("parseOAuthUsage: uses extra_usage.utilization")
{
    auto r = parseOAuthUsage(json::parse(R"({
        "five_hour": {"utilization": 99, "resets_at": "2025-10-04T15:00:00Z"},
        "extra_usage": {"is_enabled": true, "monthly_limit": 50000, "used_credits": 619, "utilization": 1.238,
                        "currency": "USD", "decimal_places": 2}})"));
    REQUIRE(r.credits.has_value());
    CHECK(r.credits->percent == 1.238);
    CHECK_FALSE(r.credits->resetsAt.has_value());
    CHECK(r.credits->used == doctest::Approx(6.19));
    CHECK(r.credits->limit == 500);
    CHECK(r.credits->currency == "USD");
}

TEST_CASE("parseOAuthUsage: amounts honor decimal_places and are null when missing")
{
    auto a = parseOAuthUsage(json::parse(
        R"({"extra_usage": {"is_enabled": true, "utilization": 1, "used_credits": 5, "monthly_limit": 100, "currency": "JPY", "decimal_places": 0}})"));
    CHECK(a.credits->used == 5);
    CHECK(a.credits->limit == 100);
    CHECK(a.credits->currency == "JPY");

    auto b = parseOAuthUsage(json::parse(R"({"extra_usage": {"is_enabled": true, "utilization": 1, "used_credits": 5}})"));
    REQUIRE(b.credits.has_value());
    CHECK(b.credits->percent == 1);
    CHECK_FALSE(b.credits->used.has_value());
    CHECK_FALSE(b.credits->limit.has_value());
    CHECK_FALSE(b.credits->currency.has_value());

    auto c = parseOAuthUsage(json::parse(
        R"({"extra_usage": {"is_enabled": true, "utilization": 1, "used_credits": null, "monthly_limit": null}})"));
    CHECK_FALSE(c.credits->used.has_value());
}

TEST_CASE("parseOAuthUsage: clamps utilization to 0-100")
{
    CHECK(parseOAuthUsage(json::parse(R"({"extra_usage": {"is_enabled": true, "utilization": 150}})")).credits->percent == 100);
    CHECK(parseOAuthUsage(json::parse(R"({"extra_usage": {"is_enabled": true, "utilization": -3}})")).credits->percent == 0);
}

TEST_CASE("parseOAuthUsage: numeric strings are accepted like JS Number()")
{
    CHECK(parseOAuthUsage(json::parse(R"({"extra_usage": {"utilization": "5"}})")).credits->percent == 5);
}

TEST_CASE("parseOAuthUsage: disabled / null / non-numeric extra_usage gives null credits")
{
    CHECK_FALSE(parseOAuthUsage(json::parse(R"({"extra_usage": null})")).credits.has_value());
    CHECK_FALSE(parseOAuthUsage(json::parse(R"({"extra_usage": {"is_enabled": false, "utilization": 5}})")).credits.has_value());
    CHECK_FALSE(parseOAuthUsage(json::parse(R"({"extra_usage": {"is_enabled": true, "utilization": null}})")).credits.has_value());
    CHECK_FALSE(parseOAuthUsage(json::parse(R"({"extra_usage": {"is_enabled": true, "utilization": "x"}})")).credits.has_value());
}

TEST_CASE("parseOAuthUsage: rejects unrecognized payloads")
{
    CHECK_THROWS_WITH_AS(parseOAuthUsage(json::parse(R"({"error": "x"})")), "Unrecognized usage response", std::runtime_error);
    CHECK_THROWS(parseOAuthUsage(json::parse(R"({"five_hour": {"utilization": 1}})")));
    CHECK_THROWS(parseOAuthUsage(json(nullptr)));
}

TEST_CASE("formatRemaining")
{
    CHECK(formatRemaining((4 * 60 + 39) * 60000) == "4h 39m");
    CHECK(formatRemaining((3 * 1440 + 18 * 60) * 60000) == "3d 18h");
    CHECK(formatRemaining(5 * 60000) == "5m");
    CHECK(formatRemaining(-1000) == "0m");
    CHECK(formatRemaining(std::nullopt) == "--");
}

TEST_CASE("levelFor thresholds")
{
    CHECK(levelFor(0) == "ok");
    CHECK(levelFor(69.9) == "ok");
    CHECK(levelFor(70) == "warn");
    CHECK(levelFor(90) == "crit");
}
