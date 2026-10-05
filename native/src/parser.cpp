// parser.cpp - Parsers for the OAuth usage JSON and the CLI /usage screen.
// Written to match the Electron app's JS implementation (src/) on the shared
// fixtures. Where C++ <regex> differs from JS (UTF-16 offsets, \s, case folding,
// Intl/Date time-zone handling), hand-rolled scanners or helpers are used and
// annotated below.
#include "parser.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <stdexcept>
#include <string_view>
#include <nlohmann/json.hpp>

namespace {

constexpr char ESC = '\x1b';
constexpr char BEL = '\x07';

bool isParam(char c) { return (c >= '0' && c <= '9') || c == ';' || c == '?'; }
bool isAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool isSpace(char c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

// ANSI stripping is done in ordered passes (OSC, cursor-forward, cursor/erase, other)
// to match the JS regex chain; the order affects the resulting spacing/newlines.

// Index just past "ESC [ params" at i, or 0 if absent.
size_t csiEnd(const std::string& s, size_t i)
{
    if (i + 1 >= s.size() || s[i] != ESC || s[i + 1] != '[') return 0;
    size_t j = i + 2;
    while (j < s.size() && isParam(s[j])) ++j;
    return j;
}

// Removes OSC sequences (ESC ] ... BEL or ESC \); unterminated ones are left as-is, like the JS regex.
std::string passOsc(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == ESC && i + 1 < s.size() && s[i + 1] == ']') {
            size_t j = i + 2;
            while (j < s.size() && s[j] != BEL && s[j] != ESC) ++j;
            if (j < s.size()) {
                if (s[j] == BEL) { i = j + 1; continue; }
                if (j + 1 < s.size() && s[j + 1] == '\\') { i = j + 2; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}

// Replaces CSI sequences whose final byte satisfies finalOk with repl() output.
template <typename Pred, typename Repl>
std::string passCsi(const std::string& s, Pred finalOk, Repl repl)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == ESC) {
            size_t j = csiEnd(s, i);
            if (j && j < s.size() && finalOk(s[j])) {
                repl(out);
                i = j + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// Drops remaining CSI, charset-select (ESC ( / ESC )) and keypad-mode (ESC = / >) sequences.
std::string passOther(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == ESC) {
            size_t j = csiEnd(s, i);
            if (j && j < s.size() && isAlpha(s[j])) { i = j + 1; continue; }
            if (i + 2 < s.size() && (s[i + 1] == '(' || s[i + 1] == ')') &&
                (isAlpha(s[i + 2]) || (s[i + 2] >= '0' && s[i + 2] <= '9'))) {
                i += 3;
                continue;
            }
            if (i + 1 < s.size() && (s[i + 1] == '=' || s[i + 1] == '>')) { i += 2; continue; }
        }
        out += s[i++];
    }
    return out;
}

const char* const MONTHS[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

// The /usage screen prints a zone name in parentheses; unknown or absent -> local zone.
const std::chrono::time_zone* resolveZone(const std::string& zoneRaw, bool has)
{
    if (!has) return std::chrono::current_zone();
    size_t b = 0, e = zoneRaw.size();
    while (b < e && isSpace(zoneRaw[b])) ++b;
    while (e > b && isSpace(zoneRaw[e - 1])) --e;
    try {
        return std::chrono::locate_zone(std::string_view(zoneRaw).substr(b, e - b));
    } catch (const std::exception&) {
        return std::chrono::current_zone();
    }
}

// Converts wall-clock time in `zone` to epoch ms. Out-of-range day/month values roll
// over like JS Date, which the std::chrono day arithmetic reproduces.
int64_t wallToInstant(const std::chrono::time_zone* zone, int y, int mo0, int d, int h, int mi)
{
    using namespace std::chrono;
    local_days day = local_days{year{y} / month{static_cast<unsigned>(mo0 + 1)} / 1} + days{d - 1};
    local_time<seconds> lt = time_point_cast<seconds>(day) + hours{h} + minutes{mi};
    local_info info = zone->get_info(lt);
    // Ambiguous: earliest offset; nonexistent: offset before the gap.
    sys_time<seconds> st{lt.time_since_epoch() - info.first.offset};
    return duration_cast<milliseconds>(st.time_since_epoch()).count();
}

// Byte offset after n UTF-16 code units (JS slice(0, n)); JS counts UTF-16 units,
// not bytes, and astral characters count as two.
size_t utf16Prefix(const std::string& s, size_t n)
{
    size_t units = 0, i = 0;
    while (i < s.size() && units < n) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        units += len == 4 ? 2 : 1;
        i += len;
    }
    return std::min(i, s.size());
}

// ASCII case-insensitive literal match (lit must be lowercase), replacing the JS /i flag.
bool matchCi(const std::string& s, size_t i, const char* lit)
{
    for (; *lit; ++lit, ++i) {
        if (i >= s.size() || lower(s[i]) != *lit) return false;
    }
    return true;
}

// Start of the last match of /Usage\s+credits/i, else std::string::npos.
size_t lastHeading(const std::string& s)
{
    size_t last = std::string::npos;
    for (size_t i = 0; i + 5 <= s.size(); ++i) {
        if (!matchCi(s, i, "usage")) continue;
        size_t j = i + 5;
        if (j >= s.size() || !isSpace(s[j])) continue;
        while (j < s.size() && isSpace(s[j])) ++j;
        if (matchCi(s, j, "credits")) last = i;
    }
    return last;
}

// First match of /\n\s*(?:Current\s+(?:session|week)|d to day)/i, else std::string::npos.
size_t nextSection(const std::string& s)
{
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\n') continue;
        size_t j = i + 1;
        while (j < s.size() && isSpace(s[j])) ++j;
        if (matchCi(s, j, "d to day")) return i;
        if (matchCi(s, j, "current")) {
            size_t k = j + 7;
            if (k < s.size() && isSpace(s[k])) {
                while (k < s.size() && isSpace(s[k])) ++k;
                if (matchCi(s, k, "session") || matchCi(s, k, "week")) return i;
            }
        }
    }
    return std::string::npos;
}

// Finds the *last* "Usage credits" heading (the screen may be redrawn several times),
// bounds the section by the next heading or 400 UTF-16 units, and reads "N% used".
std::optional<Credits> parseSection(const std::string& text, int64_t now)
{
    static const std::regex percentRe(R"((\d{1,3}(?:\.\d+)?)\s*%\s*used)", std::regex::ECMAScript | std::regex::icase);
    size_t start = lastHeading(text);
    if (start == std::string::npos) return std::nullopt;
    std::string rest = text.substr(start + 1);
    size_t next = nextSection(rest);
    std::string body = next != std::string::npos ? rest.substr(0, next) : rest.substr(0, utf16Prefix(rest, 400));
    std::smatch pct;
    if (!std::regex_search(body, pct, percentRe)) return std::nullopt;
    Credits c;
    c.percent = std::min(100.0, std::max(0.0, std::stod(pct[1].str())));
    c.resetsAt = parseResetText(body.substr(static_cast<size_t>(pct.position(0))), now);
    return c;
}

// Emulates JS Number(x): numeric strings parse, blank -> 0, junk -> NaN (nullopt), bool -> 0/1.
std::optional<double> jsNumber(const nlohmann::json& v)
{
    if (v.is_number()) return v.get<double>();
    if (v.is_boolean()) return v.get<bool>() ? 1.0 : 0.0;
    if (!v.is_string()) return std::nullopt;
    const std::string s = v.get<std::string>();
    size_t b = 0, e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    if (b == e) return 0.0;
    std::string t = s.substr(b, e - b);
    char* end = nullptr;
    double d = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return std::nullopt;
    return d;
}

// Maps the API's extra_usage object to Credits. Amounts arrive in minor units
// (cents) scaled by decimal_places; they are kept only when both used and limit exist.
std::optional<Credits> normalizeCredits(const nlohmann::json& extra)
{
    if (!extra.is_object()) return std::nullopt;
    auto at = [&](const char* k) -> const nlohmann::json* {
        auto it = extra.find(k);
        return it == extra.end() ? nullptr : &*it;
    };
    if (auto* en = at("is_enabled"); en && en->is_boolean() && !en->get<bool>()) return std::nullopt;
    auto* util = at("utilization");
    if (!util || util->is_null()) return std::nullopt;
    auto percent = jsNumber(*util);
    if (!percent || !std::isfinite(*percent)) return std::nullopt;

    int decimals = 2;
    if (auto* dp = at("decimal_places"); dp && dp->is_number()) {
        double d = dp->get<double>();
        if (std::isfinite(d) && d == std::floor(d) && d >= 0 && d < 300) decimals = static_cast<int>(d);
    }
    auto toMajor = [&](const char* k) -> std::optional<double> {
        auto* v = at(k);
        if (!v || v->is_null() || (v->is_string() && v->get<std::string>().empty())) return std::nullopt;
        auto n = jsNumber(*v);
        if (!n || !std::isfinite(*n)) return std::nullopt;
        return *n / std::pow(10.0, decimals);
    };
    auto used = toMajor("used_credits");
    auto limit = toMajor("monthly_limit");
    bool hasAmounts = used && limit;

    Credits c;
    c.percent = std::min(100.0, std::max(0.0, *percent));
    if (hasAmounts) {
        c.used = used;
        c.limit = limit;
        if (auto* cur = at("currency"); cur && cur->is_string()) c.currency = cur->get<std::string>();
    }
    return c;
}

} // namespace

std::string stripAnsi(std::string text)
{
    std::string s = passOsc(text);
    s = passCsi(s, [](char c) { return c == 'C'; }, [](std::string& o) { o += ' '; });
    s = passCsi(s, [](char c) { return std::string_view("ABDEFGHJKSTfsu").find(c) != std::string_view::npos; },
                [](std::string& o) { o += '\n'; });
    s = passOther(s);
    std::replace(s.begin(), s.end(), '\r', '\n');
    return s;
}

std::optional<int64_t> parseResetText(const std::string& text, int64_t nowMs)
{
    using namespace std::chrono;
    static const std::regex resetRe(
        R"(Resets?\s+(?:([A-Za-z]{3})[a-z]*\s+(\d{1,2}),?\s+)?(\d{1,2})(?::(\d{2}))?\s*([ap]m)(?:\s*\(([^)]+)\))?)",
        std::regex::ECMAScript | std::regex::icase);
    std::smatch m;
    if (!std::regex_search(text, m, resetRe)) return std::nullopt;

    const std::chrono::time_zone* zone = resolveZone(m[6].str(), m[6].matched);
    int hour = std::stoi(m[3].str()) % 12;
    if (lower(m[5].str()[0]) == 'p') hour += 12;
    int minute = m[4].matched ? std::stoi(m[4].str()) : 0;

    zoned_time<milliseconds> nowZ{zone, sys_time<milliseconds>{milliseconds{nowMs}}};
    year_month_day ymd{floor<days>(nowZ.get_local_time())};
    int ny = static_cast<int>(ymd.year());
    int nm = static_cast<int>(static_cast<unsigned>(ymd.month())) - 1;
    int nd = static_cast<int>(static_cast<unsigned>(ymd.day()));

    if (m[1].matched) {
        std::string mon = m[1].str();
        for (auto& c : mon) c = lower(c);
        int month = -1;
        for (int i = 0; i < 12; ++i) {
            if (mon == MONTHS[i]) month = i;
        }
        if (month < 0) return std::nullopt;
        int day = std::stoi(m[2].str());
        int64_t ts = wallToInstant(zone, ny, month, day, hour, minute);
        if (ts < nowMs) ts = wallToInstant(zone, ny + 1, month, day, hour, minute);
        return ts;
    }
    int64_t ts = wallToInstant(zone, ny, nm, nd, hour, minute);
    if (ts < nowMs) ts = wallToInstant(zone, ny, nm, nd + 1, hour, minute);
    return ts;
}

UsageData parseUsageText(const std::string& raw, int64_t nowMs)
{
    std::string text = stripAnsi(raw);
    auto credits = parseSection(text, nowMs);
    if (!credits) throw std::runtime_error("No usage credits found in /usage output");
    UsageData d;
    d.credits = credits;
    return d;
}

UsageData parseOAuthUsage(const nlohmann::json& json)
{
    if (!json.is_object() || !json.contains("extra_usage")) throw std::runtime_error("Unrecognized usage response");
    UsageData d;
    d.credits = normalizeCredits(json["extra_usage"]);
    return d;
}

std::string formatRemaining(std::optional<int64_t> ms)
{
    if (!ms) return "--";
    int64_t v = *ms;
    int64_t q = v / 60000;
    if (v % 60000 != 0 && v < 0) --q;
    int64_t totalMin = std::max<int64_t>(0, q);
    int64_t d = totalMin / 1440;
    int64_t h = (totalMin % 1440) / 60;
    int64_t m = totalMin % 60;
    if (d > 0) return std::to_string(d) + "d " + std::to_string(h) + "h";
    if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m";
    return std::to_string(m) + "m";
}

std::string levelFor(double percentUsed)
{
    if (percentUsed >= 90) return "crit";
    if (percentUsed >= 70) return "warn";
    return "ok";
}
