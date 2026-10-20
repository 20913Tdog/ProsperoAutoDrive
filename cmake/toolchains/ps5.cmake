# PS5 payloads: clang (>= 20) targeting x86_64-sie-ps5 with the ps5-payload-sdk.
#
# The SDK compiler wrappers are deliberately not used: the Windows ones add
# crt1.o although clang >= 20 already does, which breaks linking with
# duplicate symbols. Linking still goes through the SDK's prospero-lld, which
# the clang PS5 driver runs by name. -B points it at the host's variant:
# SDK/win/prospero-lld.exe on Windows (needs ld.lld on PATH), SDK/bin/prospero-lld
# on Linux (needs LLVM_CONFIG or an llvm-config-NN on PATH). Without -B, a
# Windows host may pick the bash script in SDK/bin and fail to execute it.
#
# Inputs (environment): PS5_PAYLOAD_SDK, LLVM_ROOT.

set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_VERSION 9)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(PS5 TRUE)

set(PS5_PAYLOAD_SDK "$ENV{PS5_PAYLOAD_SDK}")
set(LLVM_ROOT "$ENV{LLVM_ROOT}")
if(NOT PS5_PAYLOAD_SDK OR NOT LLVM_ROOT)
  message(FATAL_ERROR "PS5_PAYLOAD_SDK and LLVM_ROOT must be set (see CMakePresets.json)")
endif()
file(TO_CMAKE_PATH "${PS5_PAYLOAD_SDK}" PS5_PAYLOAD_SDK)
file(TO_CMAKE_PATH "${LLVM_ROOT}" LLVM_ROOT)

if(CMAKE_HOST_WIN32)
  set(_exe ".exe")
  set(_lld_dir "${PS5_PAYLOAD_SDK}/win")
else()
  set(_exe "")
  set(_lld_dir "${PS5_PAYLOAD_SDK}/bin")
endif()

set(CMAKE_C_COMPILER "${LLVM_ROOT}/bin/clang${_exe}")
set(CMAKE_CXX_COMPILER "${LLVM_ROOT}/bin/clang++${_exe}")
set(CMAKE_C_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_CXX_COMPILER_TARGET x86_64-sie-ps5)
set(CMAKE_AR "${LLVM_ROOT}/bin/llvm-ar${_exe}" CACHE FILEPATH "")
set(CMAKE_RANLIB "${LLVM_ROOT}/bin/llvm-ranlib${_exe}" CACHE FILEPATH "")

set(CMAKE_SYSROOT "${PS5_PAYLOAD_SDK}")
set(CMAKE_FIND_ROOT_PATH "${PS5_PAYLOAD_SDK}/target")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Same flags and libraries as the SDK's prospero-clang wrapper.
set(_ps5_flags "-fvisibility-nodllstorageclass=default -fno-stack-protector -fno-plt -femulated-tls")
set(_ps5_flags "${_ps5_flags} -isystem \"${PS5_PAYLOAD_SDK}/target/include\"")
set(CMAKE_C_FLAGS_INIT "${_ps5_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_ps5_flags}")
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-B\"${_lld_dir}\" -L\"${PS5_PAYLOAD_SDK}/target/lib\" -L\"${PS5_PAYLOAD_SDK}/target/user/homebrew/lib\"")
set(CMAKE_C_STANDARD_LIBRARIES "-lc -lkernel_web -lSceLibcInternal -lSceNet")
set(CMAKE_CXX_STANDARD_LIBRARIES "-lc++ -lc++abi -lunwind ${CMAKE_C_STANDARD_LIBRARIES}")

set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
