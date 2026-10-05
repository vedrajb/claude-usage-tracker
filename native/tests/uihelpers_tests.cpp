#include <doctest/doctest.h>
#include <cmath>
#include "uihelpers.h"

TEST_CASE("blend")
{
    CHECK(blend({255, 255, 255}, {20, 22, 28}, 0.16) == Rgb{58, 59, 64});
    CHECK(blend({10, 20, 30}, {200, 200, 200}, 1.0) == Rgb{10, 20, 30});
    CHECK(blend({10, 20, 30}, {200, 200, 200}, 0.0) == Rgb{200, 200, 200});
    CHECK(blend({10, 20, 30}, {200, 200, 200}, 5.0) == Rgb{10, 20, 30});
}

TEST_CASE("scaleDip")
{
    CHECK(scaleDip(440, 96) == 440);
    CHECK(scaleDip(36, 144) == 54);
    CHECK(scaleDip(10, 120) == 13);
    CHECK(scaleDip(10, 0) == 10);
}

TEST_CASE("clampOpacity")
{
    CHECK(clampOpacity(0.5) == doctest::Approx(0.5));
    CHECK(clampOpacity(0.01) == doctest::Approx(0.1));
    CHECK(clampOpacity(7) == doctest::Approx(1.0));
    CHECK(clampOpacity(0) == doctest::Approx(1.0));
    CHECK(clampOpacity(std::nan("")) == doctest::Approx(1.0));
}

TEST_CASE("dockOffsetFromX inverts computeDockPosition")
{
    Rect tb{0, 1040, 1920, 40};
    Size sz{440, 36};
    std::optional<Rect> tray = Rect{1500, 1040, 420, 40};
    for (int off : {0, 16, 200}) {
        auto p = computeDockPosition(tb, tray, sz, off);
        REQUIRE(p);
        CHECK(dockOffsetFromX(p->x, tb, tray, sz.width) == off);
        auto q = computeDockPosition(tb, std::nullopt, sz, off);
        REQUIRE(q);
        CHECK(dockOffsetFromX(q->x, tb, std::nullopt, sz.width) == off);
    }
    CHECK(dockOffsetFromX(5000, tb, tray, sz.width) == 0);
}

TEST_CASE("dock menu mapping")
{
    CHECK(dockMenuIndex("embed") == 0);
    CHECK(dockMenuIndex("overlay") == 1);
    CHECK(dockMenuIndex("none") == 2);
    CHECK(dockMenuIndex("bogus") == 0);
    for (int i = 0; i < 3; ++i) CHECK(dockMenuIndex(dockFromMenuIndex(i)) == i);
    CHECK(dockFromMenuIndex(9) == "embed");
}

TEST_CASE("clampTip")
{
    CHECK(clampTip(L"abc", 127) == L"abc");
    CHECK(clampTip(std::wstring(300, L'x')).size() == 127);
    std::wstring s(126, L'x');
    s += L'\xD83D';
    s += L'\xDE00';
    CHECK(clampTip(s).size() == 126);
}
