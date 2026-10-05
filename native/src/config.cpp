// config.cpp - Load/save of the shared JSON settings file.
#include "config.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

// Keys we manage; anything else is carried through in Config::extra untouched.
const char* const KNOWN[] = {"anchor", "offsetX", "offsetY", "x", "y", "display", "refreshMinutes", "opacity", "dock"};

bool isKnown(const std::string& k)
{
    for (auto* n : KNOWN) {
        if (k == n) return true;
    }
    return false;
}

// JS-like number handling: rounds, rejects NaN/inf and absurd values that would overflow int.
std::optional<int> asInt(const nlohmann::json& v)
{
    if (!v.is_number()) return std::nullopt;
    double d = v.get<double>();
    if (!std::isfinite(d) || std::fabs(d) > 2e9) return std::nullopt;
    return static_cast<int>(std::lround(d));
}

} // namespace

Config loadConfig(const std::filesystem::path& file)
{
    Config cfg;
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        try {
            saveConfig(file, cfg);
        } catch (const std::exception&) {
        }
        return cfg;
    }

    // Corrupt/unreadable file: use defaults but do not overwrite it, so the user can fix it.
    nlohmann::json user;
    try {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        user = nlohmann::json::parse(ss.str());
    } catch (const std::exception&) {
        return cfg;
    }
    if (!user.is_object()) return cfg;

    for (auto& [k, v] : user.items()) {
        if (!isKnown(k)) cfg.extra[k] = v;
    }
    auto get = [&](const char* k) -> const nlohmann::json* {
        auto it = user.find(k);
        return it == user.end() ? nullptr : &*it;
    };
    if (auto* v = get("anchor"); v && v->is_string()) cfg.anchor = v->get<std::string>();
    if (auto* v = get("offsetX")) { if (auto n = asInt(*v)) cfg.offsetX = *n; }
    if (auto* v = get("offsetY")) { if (auto n = asInt(*v)) cfg.offsetY = *n; }
    if (auto* v = get("x")) cfg.x = asInt(*v);
    if (auto* v = get("y")) cfg.y = asInt(*v);
    if (auto* v = get("display"); v && v->is_string()) cfg.display = v->get<std::string>();
    if (auto* v = get("refreshMinutes"); v && v->is_number()) cfg.refreshMinutes = v->get<double>();
    if (auto* v = get("opacity"); v && v->is_number()) cfg.opacity = v->get<double>();
    if (auto* v = get("dock"); v && v->is_string()) cfg.dock = v->get<std::string>();

    // Normalise invalid values; `!(x >= 1)` also catches NaN.
    if (cfg.display != "used" && cfg.display != "remaining") cfg.display = "used";
    if (!(cfg.refreshMinutes >= 1)) cfg.refreshMinutes = 5;
    if (cfg.dock != "embed" && cfg.dock != "overlay" && cfg.dock != "none") cfg.dock = "embed";
    return cfg;
}

void saveConfig(const std::filesystem::path& file, const Config& cfg)
{
    nlohmann::json j = cfg.extra.is_object() ? cfg.extra : nlohmann::json::object();
    j["anchor"] = cfg.anchor;
    j["offsetX"] = cfg.offsetX;
    j["offsetY"] = cfg.offsetY;
    j["x"] = cfg.x ? nlohmann::json(*cfg.x) : nlohmann::json(nullptr);
    j["y"] = cfg.y ? nlohmann::json(*cfg.y) : nlohmann::json(nullptr);
    j["display"] = cfg.display;
    j["refreshMinutes"] = cfg.refreshMinutes;
    j["opacity"] = cfg.opacity;
    j["dock"] = cfg.dock;

    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot write config file");
    out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool ensureLightMode(Config& cfg, bool systemLight)
{
    if (!cfg.extra.is_object()) cfg.extra = nlohmann::json::object();
    if (cfg.extra.contains("lightMode") && cfg.extra["lightMode"].is_boolean()) return false;
    cfg.extra["lightMode"] = systemLight;
    return true;
}
