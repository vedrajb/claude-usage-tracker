# Native Win32 build of Claude Usage Tracker. Targets: cut_core (pure logic),
# cut_sources (data fetching), cut_probe (console helper), cut_tests (doctest,
# run by ctest) and the ClaudeUsageTracker GUI exe. Build via native/build.cmd.
cmake_minimum_required(VERSION 3.20)
project(ClaudeUsageTracker LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static CRT => single self-contained exe, no redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive-)
endif()

# Win32-free logic, shared by the app and the unit tests.
add_library(cut_core STATIC
  src/parser.cpp
  src/view.cpp
  src/config.cpp
  src/layout.cpp
  src/uihelpers.cpp)
target_include_directories(cut_core PUBLIC src third_party)

add_library(cut_sources STATIC src/sources.cpp)
target_link_libraries(cut_sources PUBLIC cut_core winhttp shell32 ole32)

add_executable(cut_probe src/probe.cpp)
target_link_libraries(cut_probe PRIVATE cut_sources)

# Test fixtures (sample /usage outputs) live in tests/fixtures.
enable_testing()
get_filename_component(CUT_FIXTURE_DIR "${CMAKE_SOURCE_DIR}/tests/fixtures" ABSOLUTE)
add_executable(cut_tests
  tests/test_main.cpp
  tests/parser_tests.cpp
  tests/view_tests.cpp
  tests/config_tests.cpp
  tests/layout_tests.cpp
  tests/sources_tests.cpp
  tests/uihelpers_tests.cpp)
target_link_libraries(cut_tests PRIVATE cut_core cut_sources)
target_compile_definitions(cut_tests PRIVATE FIXTURE_DIR="${CUT_FIXTURE_DIR}")
add_test(NAME cut_tests COMMAND cut_tests)


# GUI subsystem exe. The manifest (DPI awareness etc.) is embedded via app.rc, so
# the linker's auto-generated manifest is disabled to avoid a duplicate.
add_executable(ClaudeUsageTracker WIN32 src/main.cpp src/app.cpp src/panel.cpp src/taskbar.cpp app.rc)
target_link_libraries(ClaudeUsageTracker PRIVATE cut_core cut_sources
  user32 gdi32 shell32 msimg32 winhttp ole32 dwmapi advapi32 shcore)
set_target_properties(ClaudeUsageTracker PROPERTIES LINK_FLAGS "/MANIFEST:NO")
set_source_files_properties(app.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/app.manifest")
