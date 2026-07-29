# Source-hygiene checks from the acceptance list, run as a CTest case so they
# fail the build rather than living in a checklist nobody reruns.
#
#   cmake -DSOURCE_DIR=<repo> -P tests/hygiene.cmake

if(NOT DEFINED SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR is required")
endif()

set(FAILURES "")

file(GLOB_RECURSE SOURCES "${SOURCE_DIR}/src/*.c" "${SOURCE_DIR}/src/*.h")

foreach(file ${SOURCES})
  file(RELATIVE_PATH rel "${SOURCE_DIR}" "${file}")
  file(READ "${file}" contents)

  # Platform conditionals live in exactly one place.
  if(contents MATCHES "#if(def)? *_WIN32" OR contents MATCHES "defined\\(_WIN32\\)")
    if(NOT rel MATCHES "src/util/platform\\.(c|h)$")
      list(APPEND FAILURES "${rel}: _WIN32 conditional outside util/platform")
    endif()
  endif()

  # cJSON is reached only through the util/json wrapper.
  if(contents MATCHES "cJSON")
    if(NOT rel MATCHES "src/util/json\\.(c|h)$")
      list(APPEND FAILURES "${rel}: references cJSON outside util/json")
    endif()
  endif()

  # No unbounded string operations, anywhere.
  foreach(banned strcpy strcat sprintf gets alloca)
    if(contents MATCHES "[^_a-zA-Z0-9]${banned}\\(")
      list(APPEND FAILURES "${rel}: uses ${banned}()")
    endif()
  endforeach()

  # exit() outside main would skip the session and shelf flush.
  if(contents MATCHES "[^_a-zA-Z0-9]exit\\(")
    if(NOT rel MATCHES "src/main\\.c$")
      list(APPEND FAILURES "${rel}: calls exit() outside main")
    endif()
  endif()
endforeach()

if(FAILURES)
  foreach(failure ${FAILURES})
    message(SEND_ERROR "hygiene: ${failure}")
  endforeach()
  message(FATAL_ERROR "source hygiene checks failed")
endif()

message(STATUS "source hygiene: clean")
