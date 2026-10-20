cmake_minimum_required(VERSION 3.25)
project(ProsperoAutoDrive VERSION 0.1.0 LANGUAGES C)

# Warnings are errors on every target and toolchain.
add_compile_options(-Wall -Wextra -Werror)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# PS5 code builds for the PS5 and, for tests, for the Linux host.
if(NOT WIN32)
  add_subdirectory(ps5/spikes/s1-portprobe)
endif()
