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

  # Our changes to libsmb2 are patch files (third_party/patches/libsmb2-*),
  # applied to a copy in the build tree; the submodule stays untouched. The
  # copy is redone when the pinned commit or a patch changes.
  # On a Windows machine patch.exe comes with Git for Windows (usr/bin).
  find_package(Git QUIET)
  set(_patch_hints "")
  if(GIT_EXECUTABLE)
    get_filename_component(_git_dir "${GIT_EXECUTABLE}" DIRECTORY)
    list(APPEND _patch_hints "${_git_dir}/../usr/bin")
  endif()
  find_program(PATCH_EXECUTABLE patch HINTS ${_patch_hints} REQUIRED)
  set(_smb2_orig "${CMAKE_SOURCE_DIR}/third_party/libsmb2")
  set(_smb2_src "${CMAKE_BINARY_DIR}/libsmb2-patched")
  file(GLOB _smb2_patches "${CMAKE_SOURCE_DIR}/third_party/patches/libsmb2-*.patch")
  list(SORT _smb2_patches)
  set(_smb2_stamp "")
  file(GLOB_RECURSE _smb2_files "${_smb2_orig}/lib/*" "${_smb2_orig}/include/*"
       "${_smb2_orig}/CMakeLists.txt")
  foreach(_f IN LISTS _smb2_files _smb2_patches)
    file(SHA256 "${_f}" _h)
    string(APPEND _smb2_stamp "${_h}\n")
  endforeach()
  string(SHA256 _smb2_stamp "${_smb2_stamp}")
  set(_smb2_old "")
  if(EXISTS "${_smb2_src}/.prospero-stamp")
    file(READ "${_smb2_src}/.prospero-stamp" _smb2_old)
  endif()
  if(NOT _smb2_old STREQUAL _smb2_stamp)
    file(REMOVE_RECURSE "${_smb2_src}")
    file(COPY "${_smb2_orig}/" DESTINATION "${_smb2_src}" PATTERN ".git" EXCLUDE)
    foreach(_p IN LISTS _smb2_patches)
      message(STATUS "libsmb2: applying ${_p}")
      execute_process(COMMAND "${PATCH_EXECUTABLE}" -p1 --batch --forward -i "${_p}"
                      WORKING_DIRECTORY "${_smb2_src}" RESULT_VARIABLE _rc)
      if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "libsmb2: ${_p} does not apply")
      endif()
    endforeach()
    file(WRITE "${_smb2_src}/.prospero-stamp" "${_smb2_stamp}")
  endif()
  add_subdirectory("${_smb2_src}" "${CMAKE_BINARY_DIR}/libsmb2" EXCLUDE_FROM_ALL SYSTEM)
endif()

# Warnings are errors on every target and toolchain (our code only).
add_compile_options(-Wall -Wextra -Werror)

if(NOT WIN32)
  add_subdirectory(ps5/spikes/s1-portprobe)
  add_subdirectory(ps5/spikes/smbserver)
  add_subdirectory(ps5/smbd)
else()
  add_subdirectory(windows)
endif()
