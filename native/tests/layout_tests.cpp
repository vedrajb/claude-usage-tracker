#include <doctest/doctest.h>

#include "layout.h"

namespace {
Config cfgOf(const char* anchor, int ox, int oy)
{
    Config c;
    c.anchor = anchor;
    c.offsetX = ox;
    c.offsetY = oy;
    return c;
}
} // namespace

TEST_CASE("computePosition: anchors and absolute override")
{
    Rect wa{0, 0, 1920, 1040};
    Size size{300, 70};
    CHECK(computePosition(cfgOf("bottom-right", 10, 5), wa, size) == Point{1610, 965});
    CHECK(computePosition(cfgOf("top-left", 10, 5), wa, size) == Point{10, 5});
    CHECK(computePosition(cfgOf("top-right", 10, 5), wa, size) == Point{1610, 5});
    CHECK(computePosition(cfgOf("bottom-left", 10, 5), wa, size) == Point{10, 965});
    Config c = cfgOf("top-left", 10, 5);
    c.x = 40;
    c.y = 50;
    CHECK(computePosition(c, wa, size) == Point{40, 50});
    c.y.reset();
    CHECK(computePosition(c, wa, size) == Point{10, 5});
}

TEST_CASE("computePosition: work area origin is honored")
{
    CHECK(computePosition(cfgOf("bottom-right", 10, 5), Rect{100, 50, 800, 600}, Size{300, 70}) == Point{590, 575});
}

TEST_CASE("computeDragPosition: offsets window by cursor delta")
{
    CHECK(computeDragPosition({100, 200}, {50, 60}, {80, 40}) == Point{130, 180});
    CHECK(computeDragPosition({100, 200}, {50, 60}, {50, 60}) == Point{100, 200});
    CHECK(computeDragPosition({0, 0}, {0, 0}, {10.6, -3.4}) == Point{11, -3});
    CHECK(computeDragPosition({0, 0}, {0, 0}, {0.5, -0.5}) == Point{1, 0});
}

TEST_CASE("computeDockPosition: right of taskbar without tray")
{
    Rect tb{0, 1040, 1920, 40};
    auto p = computeDockPosition(tb, std::nullopt, Size{200, 30}, 8);
    REQUIRE(p.has_value());
    CHECK(*p == Point{1920 - 200 - 8, 1045});
}

TEST_CASE("computeDockPosition: left of tray")
{
    Rect tb{0, 1040, 1920, 48};
    auto p = computeDockPosition(tb, Rect{1500, 1040, 420, 48}, Size{200, 30}, 8);
    REQUIRE(p.has_value());
    CHECK(*p == Point{1500 - 200 - 8, 1049});
}

TEST_CASE("computeDockPosition: clamps to taskbar left edge")
{
    Rect tb{100, 0, 250, 40};
    auto p = computeDockPosition(tb, std::nullopt, Size{300, 30}, 8);
    REQUIRE(p.has_value());
    CHECK(p->x == 100);
}

TEST_CASE("computeDockPosition: vertical taskbar or too tall gives nullopt")
{
    CHECK_FALSE(computeDockPosition(Rect{0, 0, 48, 1080}, std::nullopt, Size{40, 30}, 8).has_value());
    CHECK_FALSE(computeDockPosition(Rect{0, 0, 40, 40}, std::nullopt, Size{20, 20}, 8).has_value());
    CHECK_FALSE(computeDockPosition(Rect{0, 1040, 1920, 40}, std::nullopt, Size{200, 41}, 8).has_value());
    CHECK(computeDockPosition(Rect{0, 1040, 1920, 40}, std::nullopt, Size{200, 40}, 8).has_value());
}

TEST_CASE("clampDockDrag: clamps horizontally")
{
    Rect tb{0, 1040, 1920, 40};
    Size size{200, 30};
    CHECK(clampDockDrag(-50, tb, size) == 0);
    CHECK(clampDockDrag(500, tb, size) == 500);
    CHECK(clampDockDrag(5000, tb, size) == 1720);
    CHECK(clampDockDrag(5000, Rect{100, 0, 150, 40}, size) == 100);
}

TEST_CASE("taskbarFromBoundsAndWork: detects edge")
{
    Rect bounds{0, 0, 1920, 1080};
    auto b = taskbarFromBoundsAndWork(bounds, Rect{0, 0, 1920, 1040});
    REQUIRE(b.has_value());
    CHECK(b->edge == Edge::Bottom);
    CHECK(b->rect == Rect{0, 1040, 1920, 40});

    auto t = taskbarFromBoundsAndWork(bounds, Rect{0, 40, 1920, 1040});
    REQUIRE(t.has_value());
    CHECK(t->edge == Edge::Top);
    CHECK(t->rect == Rect{0, 0, 1920, 40});

    auto l = taskbarFromBoundsAndWork(bounds, Rect{60, 0, 1860, 1080});
    REQUIRE(l.has_value());
    CHECK(l->edge == Edge::Left);
    CHECK(l->rect == Rect{0, 0, 60, 1080});

    auto r = taskbarFromBoundsAndWork(bounds, Rect{0, 0, 1860, 1080});
    REQUIRE(r.has_value());
    CHECK(r->edge == Edge::Right);
    CHECK(r->rect == Rect{1860, 0, 60, 1080});

    CHECK_FALSE(taskbarFromBoundsAndWork(bounds, bounds).has_value());
}

TEST_CASE("deriveTaskbarEdge")
{
    Rect mon{0, 0, 1920, 1080};
    CHECK(deriveTaskbarEdge({0, 1040, 1920, 40}, mon) == Edge::Bottom);
    CHECK(deriveTaskbarEdge({0, 0, 1920, 40}, mon) == Edge::Top);
    CHECK(deriveTaskbarEdge({0, 0, 60, 1080}, mon) == Edge::Left);
    CHECK(deriveTaskbarEdge({1860, 0, 60, 1080}, mon) == Edge::Right);
    CHECK(deriveTaskbarEdge({-1920, 1040, 1920, 40}, {-1920, 0, 1920, 1080}) == Edge::Bottom);
}

TEST_CASE("isTaskbarAutoHidden")
{
    Rect mon{0, 0, 1920, 1080};
    CHECK_FALSE(isTaskbarAutoHidden({0, 1040, 1920, 40}, mon, {0, 0, 1920, 1040}));
    CHECK(isTaskbarAutoHidden({0, 1040, 1920, 40}, mon, mon));
    CHECK(isTaskbarAutoHidden({0, 1078, 1920, 40}, mon, {0, 0, 1920, 1040}));
    CHECK(isTaskbarAutoHidden({0, 1100, 1920, 40}, mon, {0, 0, 1920, 1040}));
    CHECK(isTaskbarAutoHidden({0, 0, 0, 0}, mon, {0, 0, 1920, 1040}));
}

TEST_CASE("coversMonitor")
{
    Rect mon{0, 0, 1920, 1080};
    CHECK(coversMonitor({0, 0, 1920, 1080}, mon));
    CHECK(coversMonitor({-8, -8, 1936, 1096}, mon));
    CHECK_FALSE(coversMonitor({0, 0, 1920, 1040}, mon));
    CHECK_FALSE(coversMonitor({100, 0, 1920, 1080}, mon));
    CHECK_FALSE(coversMonitor({0, 0, 100, 100}, {0, 0, 0, 0}));
}
