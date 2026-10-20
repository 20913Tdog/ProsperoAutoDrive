cmake_minimum_required(VERSION 3.25)
project(ProsperoAutoDrive VERSION 0.1.0 LANGUAGES C)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# PS5 code builds for the PS5 and, for tests, for the Linux host.
if(NOT WIN32)
  # libsmb2 (LGPL-2.1), pinned as a git submodule. Static, NTLM only, no
  # Kerberos, no DCE/RPC. Added before our warning flags and as SYSTEM so that
  # -Werror only applies to our own code. CMP0077 lets the variables below
  # override the library's option() defaults.
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(BUILD_SHARED_LIBS OFF)
  set(ENABLE_LIBKRB5 OFF)
  set(ENABLE_GSSAPI OFF)
  set(ENABLE_LIBDCERPC OFF)
  set(ENABLE_EXAMPLES OFF)
  set(ENABLE_UTILS OFF)
  add_subdirectory(third_party/libsmb2 EXCLUDE_FROM_ALL SYSTEM)
endif()

# Warnings are errors on every target and toolchain (our code only).
add_compile_options(-Wall -Wextra -Werror)

if(NOT WIN32)
  add_subdirectory(ps5/spikes/s1-portprobe)
  add_subdirectory(ps5/spikes/smbserver)
else()
  add_subdirectory(windows/spikes/s7-discover)
endif()
