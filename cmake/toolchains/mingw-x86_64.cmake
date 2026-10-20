# Windows x86_64 programs with llvm-mingw (clang + MinGW-w64, UCRT).
# Everything is linked statically: the result needs only DLLs that ship
# with Windows 10/11. Works the same on a Linux and a Windows host.
#
# Inputs (environment): LLVM_ROOT.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(LLVM_ROOT "$ENV{LLVM_ROOT}")
if(NOT LLVM_ROOT)
  message(FATAL_ERROR "LLVM_ROOT must be set (see CMakePresets.json)")
endif()
file(TO_CMAKE_PATH "${LLVM_ROOT}" LLVM_ROOT)

if(CMAKE_HOST_WIN32)
  set(_exe ".exe")
else()
  set(_exe "")
endif()

set(CMAKE_C_COMPILER "${LLVM_ROOT}/bin/x86_64-w64-mingw32-clang${_exe}")
set(CMAKE_CXX_COMPILER "${LLVM_ROOT}/bin/x86_64-w64-mingw32-clang++${_exe}")
set(CMAKE_RC_COMPILER "${LLVM_ROOT}/bin/x86_64-w64-mingw32-windres${_exe}")

set(CMAKE_FIND_ROOT_PATH "${LLVM_ROOT}/x86_64-w64-mingw32")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
