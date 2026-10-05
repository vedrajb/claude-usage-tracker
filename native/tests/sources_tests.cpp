#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include "sources.h"

namespace fs = std::filesystem;

namespace {
fs::path tempCreds(const std::string& name, const std::string* content)
{
    fs::path dir = fs::temp_directory_path() / ("cut_src_" + name);
    fs::create_directories(dir);
    fs::path f = dir / "creds.json";
    fs::remove(f);
    if (content) std::ofstream(f, std::ios::binary) << *content;
    return f;
}

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}
}

TEST_CASE("detectTrustPrompt")
{
    CHECK(detectTrustPrompt("Yes, I trust this folder\nEnter to confirm"));
    CHECK(detectTrustPrompt("YES, I TRUST THIS FOLDER ... ENTER TO CONFIRM"));
    CHECK_FALSE(detectTrustPrompt("Yes, I trust this folder"));
    CHECK_FALSE(detectTrustPrompt("Enter to confirm"));
}

TEST_CASE("detectReadyPrompt")
{
    CHECK(detectReadyPrompt("abc\n? for shortcuts"));
    CHECK(detectReadyPrompt("?   for shortcuts"));
    CHECK_FALSE(detectReadyPrompt("?for shortcuts"));
    CHECK(detectReadyPrompt("x \xE2\x9D\xAF y"));
    CHECK(detectReadyPrompt("line1\n>  \nline3"));
    CHECK(detectReadyPrompt("line1\r\n> \r\n"));
    CHECK(detectReadyPrompt("foo>"));
    CHECK_FALSE(detectReadyPrompt("a > b\nc"));
    CHECK_FALSE(detectReadyPrompt(""));
}

TEST_CASE("readAccessToken")
{
    SUBCASE("missing file") {
        CHECK_THROWS_WITH_AS(readAccessToken(tempCreds("missing", nullptr)),
                             "No OAuth credentials found", std::runtime_error);
    }
    SUBCASE("no token") {
        std::string c = R"({"claudeAiOauth":{}})";
        CHECK_THROWS_WITH_AS(readAccessToken(tempCreds("notoken", &c)),
                             "No OAuth credentials found", std::runtime_error);
    }
    SUBCASE("expired") {
        std::string c = R"({"claudeAiOauth":{"accessToken":"t","expiresAt":1000}})";
        CHECK_THROWS_WITH_AS(readAccessToken(tempCreds("expired", &c)),
                             "OAuth token expired", std::runtime_error);
    }
    SUBCASE("valid") {
        std::string c = R"({"claudeAiOauth":{"accessToken":"tok","expiresAt":)" +
                        std::to_string(nowMs() + 3600000) + "}}";
        CHECK(readAccessToken(tempCreds("valid", &c)) == "tok");
    }
    SUBCASE("valid without expiry") {
        std::string c = R"({"claudeAiOauth":{"accessToken":"tok"}})";
        CHECK(readAccessToken(tempCreds("noexp", &c)) == "tok");
    }
}

TEST_CASE("CancelToken")
{
    CancelToken t;
    int a = 0, b = 0;
    CHECK_FALSE(t.cancelled());
    CHECK(t.track(&a));
    CHECK(t.release(&a));
    CHECK_FALSE(t.release(&a));
    t.cancel();
    CHECK(t.cancelled());
    CHECK_FALSE(t.release(&b));
    CHECK_FALSE(t.track(&a));
}
