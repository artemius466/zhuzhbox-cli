# Tests. Everything network-dependent points at tests/mock_server.py, so
# `ctest` never touches the internet and never uploads anything real.

add_executable(zhuzhbox_unit_tests unit_tests.c)
target_link_libraries(zhuzhbox_unit_tests PRIVATE zhuzhbox_core)

if(MSVC)
  target_compile_options(zhuzhbox_unit_tests PRIVATE /W4)
else()
  target_compile_options(zhuzhbox_unit_tests PRIVATE -Wall -Wextra -Wshadow)
endif()

add_test(NAME unit COMMAND zhuzhbox_unit_tests)

find_package(Python3 COMPONENTS Interpreter)

if(Python3_Interpreter_FOUND)
  # The harnesses need a POSIX shell. BASH_LAUNCHER is a *list*, because the
  # two cases need different argument counts: passing a literal "bash" to an
  # absolute bash path would make bash try to run a script called "bash".
  find_program(BASH_EXECUTABLE NAMES bash)
  if(BASH_EXECUTABLE)
    set(BASH_LAUNCHER "${BASH_EXECUTABLE}")
  elseif(NOT WIN32)
    set(BASH_LAUNCHER /usr/bin/env bash)
  else()
    set(BASH_LAUNCHER "")
  endif()

  if(BASH_LAUNCHER)
    add_test(NAME cli
      COMMAND ${BASH_LAUNCHER}
              ${CMAKE_CURRENT_SOURCE_DIR}/cli_tests.sh
              $<TARGET_FILE:zhuzhbox>
              ${CMAKE_CURRENT_SOURCE_DIR}/mock_server.py)
    set_tests_properties(cli PROPERTIES
      TIMEOUT 900
      ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")

    # Slower and timing-sensitive: it interrupts a real upload mid-flight.
    add_test(NAME resume
      COMMAND ${BASH_LAUNCHER}
              ${CMAKE_CURRENT_SOURCE_DIR}/resume_test.sh
              $<TARGET_FILE:zhuzhbox>
              ${CMAKE_CURRENT_SOURCE_DIR}/mock_server.py)
    set_tests_properties(resume PROPERTIES
      TIMEOUT 600
      ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}")
  else()
    message(STATUS "bash not found; the end-to-end tests will be skipped")
  endif()
else()
  message(STATUS "Python 3 not found; the end-to-end tests will be skipped")
endif()

# Source hygiene rules from the spec that are cheap enough to enforce as tests.
add_test(NAME hygiene
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/..
          -P ${CMAKE_CURRENT_SOURCE_DIR}/hygiene.cmake)
