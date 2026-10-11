cmake_minimum_required(VERSION 3.16)
project(heapfdr VERSION 0.4.0 DESCRIPTION "Heap flight data recorder for Linux" LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "heapfdr relies on glibc and LD_PRELOAD, so it only builds on Linux")
endif()

find_package(Threads REQUIRED)

# The preload library
add_library(heapfdr SHARED
  src/config.c
  src/crash.c
  src/elfsym.c
  src/hooks.c
  src/ledger.c
  src/out.c
  src/report.c
  src/stack.c
)
target_compile_definitions(heapfdr PRIVATE _GNU_SOURCE)
target_compile_options(heapfdr PRIVATE -Wall -Wextra -Wshadow -fno-omit-frame-pointer)
target_link_libraries(heapfdr PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
target_link_options(heapfdr PRIVATE "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")
set_target_properties(heapfdr PROPERTIES
  C_VISIBILITY_PRESET hidden
  LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map)

# Demo and test programs. The demo is built without optimisation so the
# planted bugs stay exactly as written.
add_executable(telemetry-demo examples/telemetry_demo.c)
target_compile_options(telemetry-demo PRIVATE -O0 -g -fno-omit-frame-pointer -Wall -Wextra)

add_executable(heap-stress tests/stress.c)
target_compile_options(heap-stress PRIVATE -O1 -g -Wall -Wextra)
target_link_libraries(heap-stress PRIVATE Threads::Threads)

add_executable(heap-api tests/api.c)
target_compile_options(heap-api PRIVATE -O0 -g -Wall -Wextra)

# Helper scripts next to the library, so ./build/heapfdr works right away
configure_file(tools/heapfdr ${CMAKE_CURRENT_BINARY_DIR}/heapfdr COPYONLY)
configure_file(tools/heapfdr-symbolize ${CMAKE_CURRENT_BINARY_DIR}/heapfdr-symbolize COPYONLY)

enable_testing()
add_test(NAME scenarios
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.sh ${CMAKE_CURRENT_BINARY_DIR})

install(TARGETS heapfdr LIBRARY DESTINATION lib)
install(PROGRAMS tools/heapfdr tools/heapfdr-symbolize DESTINATION bin)
