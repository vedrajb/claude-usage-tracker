// probe.cpp - Console helper to try the data sources by hand (--pty forces the PTY path).
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include "sources.h"

int main(int argc, char** argv)
{
    try {
        auto cwd = std::filesystem::temp_directory_path() / "cut_probe";
        bool pty = argc > 1 && std::string(argv[1]) == "--pty";
        UsageData d = pty ? fetchViaPty(cwd) : fetchUsage(cwd);
        std::printf("source=%s\n", d.source.c_str());
        if (d.credits) {
            std::printf("percent=%.2f\n", d.credits->percent);
            if (d.credits->used) std::printf("used=%.2f\n", *d.credits->used);
            if (d.credits->limit) std::printf("limit=%.2f\n", *d.credits->limit);
        } else {
            std::printf("no credits data\n");
        }
        return 0;
    } catch (const std::exception& e) {
        std::printf("error: %s\n", e.what());
        return 1;
    }
}
