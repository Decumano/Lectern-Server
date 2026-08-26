cmake_minimum_required(VERSION 3.25)
project(lectern-server-cpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
  # sqlite3/getenv style C APIs are used deliberately; don't fail on the
  # "use the _s variant" advisories MSVC raises for them.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Drogon CONFIG REQUIRED)
find_package(unofficial-sodium CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(cpr CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

# Everything except main() lives in a library so the tests can link it.
add_library(lectern_core STATIC
  src/util.cpp
  src/db.cpp
  src/state.cpp
  src/session.cpp
  src/auth.cpp
  src/workspace.cpp
  src/share.cpp
  src/fonts.cpp
  src/export.cpp
  src/downloads.cpp
)

target_include_directories(lectern_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(lectern_core PUBLIC
  Drogon::Drogon
  unofficial-sodium::sodium
  nlohmann_json::nlohmann_json
  cpr::cpr
  spdlog::spdlog
  unofficial::sqlite3::sqlite3
)

add_executable(lectern-server src/main.cpp)
target_link_libraries(lectern-server PRIVATE lectern_core)

# ── Tests ──
include(CTest)
if(BUILD_TESTING)
  find_package(Catch2 3 CONFIG REQUIRED)
  add_executable(lectern-tests tests/tests.cpp)
  target_link_libraries(lectern-tests PRIVATE lectern_core Catch2::Catch2WithMain)
  add_test(NAME lectern-tests COMMAND lectern-tests)
endif()

# The migrations directory is read at startup, relative to the working
# directory by default; copy it next to the binary so a build tree can run
# in place without MIGRATIONS_DIR being set.
add_custom_command(TARGET lectern-server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          ${CMAKE_CURRENT_SOURCE_DIR}/migrations
          $<TARGET_FILE_DIR:lectern-server>/migrations
)
