#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

// config.h - User settings persisted as JSON (same file/schema as the Electron app).

/// Settings. `extra` preserves unknown keys so saving never drops data written by
/// other versions of the app.
struct Config {
    std::string anchor = "bottom-right";
    int offsetX = 16;
    int offsetY = 8;
    std::optional<int> x, y;
    std::string display = "used";
    double refreshMinutes = 5;
    double opacity = 1.0;
    std::string dock = "embed";
    nlohmann::json extra = nlohmann::json::object();
};

/// Loads and validates; a missing file is created with defaults, invalid values fall back to defaults.
Config loadConfig(const std::filesystem::path& file);
/// Sets `lightMode` in `extra` from the system theme if it is not stored yet
/// (first startup). Returns true when it was added, i.e. the config needs saving.
bool ensureLightMode(Config& cfg, bool systemLight);
/// Writes known keys over a copy of `extra`; throws on I/O failure.
void saveConfig(const std::filesystem::path& file, const Config& cfg);
