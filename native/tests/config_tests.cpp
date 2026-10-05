#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include "config.h"

namespace fs = std::filesystem;

namespace {

struct TempDir {
    fs::path path;
    TempDir()
    {
        path = fs::temp_directory_path() / ("cut-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write(const fs::path& p, const std::string& s)
{
    std::ofstream(p, std::ios::binary) << s;
}

} // namespace

TEST_CASE("loadConfig: creates defaults and sanitizes bad values")
{
    TempDir d;
    auto file = d.path / "sub" / "config.json";
    Config c = loadConfig(file);
    CHECK(c.refreshMinutes == 5);
    CHECK(c.dock == "embed");
    CHECK(c.anchor == "bottom-right");
    CHECK(c.offsetX == 16);
    CHECK(c.offsetY == 8);
    CHECK(c.opacity == 1.0);
    CHECK_FALSE(c.x.has_value());
    CHECK(fs::exists(file));

    write(file, R"({"display": "bogus", "refreshMinutes": 0})");
    Config c2 = loadConfig(file);
    CHECK(c2.display == "used");
    CHECK(c2.refreshMinutes == 5);
}

TEST_CASE("loadConfig: malformed JSON gives defaults and leaves file alone")
{
    TempDir d;
    auto file = d.path / "config.json";
    write(file, "{ not json");
    Config c = loadConfig(file);
    CHECK(c.display == "used");
    CHECK(c.refreshMinutes == 5);
    std::ifstream in(file);
    std::string s;
    std::getline(in, s);
    CHECK(s == "{ not json");
}

TEST_CASE("loadConfig: dock validation")
{
    TempDir d;
    auto file = d.path / "config.json";
    for (const char* v : {"embed", "overlay", "none"}) {
        write(file, std::string(R"({"dock": ")") + v + R"("})");
        CHECK(loadConfig(file).dock == v);
    }
    write(file, R"({"dock": "bogus"})");
    CHECK(loadConfig(file).dock == "embed");
    write(file, R"({"dock": 3})");
    CHECK(loadConfig(file).dock == "embed");
}

TEST_CASE("saveConfig: roundtrip preserves values and unknown keys")
{
    TempDir d;
    auto file = d.path / "nested" / "config.json";
    write(d.path / "seed.json", R"({"anchor": "top-left", "x": 10, "y": 20, "custom": {"a": 1}, "opacity": 0.5})");
    Config c = loadConfig(d.path / "seed.json");
    CHECK(c.anchor == "top-left");
    CHECK(c.x == 10);
    CHECK(c.y == 20);
    CHECK(c.opacity == 0.5);
    c.dock = "overlay";
    saveConfig(file, c);

    Config r = loadConfig(file);
    CHECK(r.anchor == "top-left");
    CHECK(r.x == 10);
    CHECK(r.dock == "overlay");
    CHECK(r.extra["custom"]["a"] == 1);
}

TEST_CASE("saveConfig: unicode path and null x/y")
{
    TempDir d;
    auto file = d.path / fs::path(u8"\u00e9\u00e8") / "config.json";
    saveConfig(file, Config{});
    CHECK(fs::exists(file));
    Config r = loadConfig(file);
    CHECK_FALSE(r.x.has_value());
    CHECK_FALSE(r.y.has_value());
}
