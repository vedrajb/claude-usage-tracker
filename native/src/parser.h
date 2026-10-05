#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

// parser.h - Parsing of usage data from the OAuth JSON and the CLI's /usage screen.
// Behaviour intentionally mirrors the Electron app's JavaScript parser (src/) so
// both apps show identical numbers; see parser.cpp for parity notes.

/// Credits window: percent used (0-100), optional reset time (epoch ms) and amounts.
struct Credits {
    double percent = 0;
    std::optional<int64_t> resetsAt;
    std::optional<double> used, limit;
    std::optional<std::string> currency;
};

/// Parsed result; `source` is "oauth" or "pty". `credits` is empty if none found.
struct UsageData {
    std::optional<Credits> credits;
    std::string source;
};

/// Removes ANSI/VT escape sequences from terminal output.
std::string stripAnsi(std::string text);
/// Parses "Resets ..." text into an epoch-ms time relative to `nowMs`.
std::optional<int64_t> parseResetText(const std::string& text, int64_t nowMs);
/// Parses the CLI /usage screen text.
UsageData parseUsageText(const std::string& raw, int64_t nowMs);
/// Parses the OAuth usage API response.
UsageData parseOAuthUsage(const nlohmann::json& json);
/// "3d 4h" / "2h 5m" / "7m" remaining-time text; "--" when unknown.
std::string formatRemaining(std::optional<int64_t> ms);
/// Severity bucket: "crit" >= 90, "warn" >= 70, else "ok".
std::string levelFor(double percentUsed);
