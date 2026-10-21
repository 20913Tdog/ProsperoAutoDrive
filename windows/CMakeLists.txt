# Windows programs (llvm-mingw, statically linked, MIT).
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)

# Client side of discovery and service channel, shared by the probe and
# later ProsperoAutoDrive.exe.
add_library(prospero-client STATIC common/client.cpp)
target_include_directories(prospero-client PUBLIC common)
target_link_libraries(prospero-client PUBLIC ws2_32 iphlpapi bcrypt advapi32)

add_executable(prospero-probe tools/probe/main.cpp)
target_link_libraries(prospero-probe PRIVATE prospero-client mpr)

add_subdirectory(spikes/s7-discover)
