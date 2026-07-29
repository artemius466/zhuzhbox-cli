cmake_minimum_required(VERSION 3.16)
project(zhuzhbox C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(ZB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ZB_STATIC_CURL "Link libcurl statically where the platform allows it" OFF)
option(ZB_WERROR "Treat warnings as errors (on in CI)" OFF)
option(ZB_BUILD_TESTS "Build the test suite" OFF)

# ---------------------------------------------------------------------------
# Dependencies: libcurl is linked, cJSON is vendored.
# ---------------------------------------------------------------------------

if(ZB_STATIC_CURL)
  set(CURL_USE_STATIC_LIBS ON)
endif()
find_package(CURL 7.68 REQUIRED)

add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cJSON)
# Vendored unmodified: do not hold third-party code to our warning settings.
if(MSVC)
  target_compile_options(cjson PRIVATE /W0)
else()
  target_compile_options(cjson PRIVATE -w)
endif()

# ---------------------------------------------------------------------------
# The library holding everything except main(), so the tests can link it too.
# ---------------------------------------------------------------------------

set(ZB_SOURCES
  src/cli.c
  src/api/client.c
  src/api/upload.c
  src/api/collection.c
  src/api/meta.c
  src/commands/commands.c
  src/commands/upload.c
  src/commands/get.c
  src/commands/ls.c
  src/commands/rm.c
  src/commands/status.c
  src/commands/quota.c
  src/commands/stats.c
  src/commands/health.c
  src/commands/report.c
  src/commands/rules.c
  src/commands/config.c
  src/commands/shelf.c
  src/store/paths.c
  src/store/atomic.c
  src/store/shelf.c
  src/store/sessions.c
  src/store/config.c
  src/format/bytes.c
  src/format/time.c
  src/format/table.c
  src/format/color.c
  src/format/progress.c
  src/util/buf.c
  src/util/json.c
  src/util/str.c
  src/util/token.c
  src/util/mime.c
  src/util/error.c
  src/util/platform.c
)

add_library(zhuzhbox_core STATIC ${ZB_SOURCES})
target_include_directories(zhuzhbox_core PUBLIC src)
target_link_libraries(zhuzhbox_core PUBLIC CURL::libcurl cjson)

# 25 GB files mean 64-bit offsets everywhere, including on 32-bit builds.
target_compile_definitions(zhuzhbox_core PUBLIC _FILE_OFFSET_BITS=64)

if(MSVC)
  target_compile_options(zhuzhbox_core PRIVATE /W4 /utf-8)
  target_compile_definitions(zhuzhbox_core PRIVATE _CRT_SECURE_NO_WARNINGS)
  if(ZB_WERROR)
    target_compile_options(zhuzhbox_core PRIVATE /WX)
  endif()
else()
  target_compile_options(zhuzhbox_core PRIVATE
    -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
    -Wstrict-prototypes -Wmissing-prototypes -Wpointer-arith -Wwrite-strings)
  if(ZB_WERROR)
    target_compile_options(zhuzhbox_core PRIVATE -Werror)
  endif()
endif()

if(ZB_SANITIZE)
  if(MSVC)
    message(WARNING "ZB_SANITIZE is a no-op on MSVC; use the Clang or GCC build")
  else()
    target_compile_options(zhuzhbox_core PUBLIC
      -fsanitize=address,undefined -fno-omit-frame-pointer -g)
    target_link_options(zhuzhbox_core PUBLIC -fsanitize=address,undefined)
  endif()
endif()

# libm for the JSON number validation and the progress maths.
include(CheckLibraryExists)
check_library_exists(m floor "" ZB_HAVE_LIBM)
if(ZB_HAVE_LIBM)
  target_link_libraries(zhuzhbox_core PUBLIC m)
endif()

if(WIN32)
  target_link_libraries(zhuzhbox_core PUBLIC shell32 ole32)
endif()

# ---------------------------------------------------------------------------
# The executable
# ---------------------------------------------------------------------------

add_executable(zhuzhbox src/main.c)
target_link_libraries(zhuzhbox PRIVATE zhuzhbox_core)

install(TARGETS zhuzhbox RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# `cmake --build build --target rules` regenerates the committed rules header.
# ---------------------------------------------------------------------------

add_custom_target(rules
  COMMAND ${CMAKE_COMMAND} -E echo "Regenerating src/rules_generated.h"
  COMMAND tools/gen_rules --source vercel --output src/rules_generated.h
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  COMMENT "Regenerate rules_generated.h from the website sources")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(ZB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
