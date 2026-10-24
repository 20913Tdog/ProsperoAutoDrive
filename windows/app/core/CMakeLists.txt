# The connection state machine of ProsperoAutoDrive.exe without Win32 calls,
# so its unit test runs in the host preset (Linux) as well (E1).
add_library(prospero-core STATIC machine.cpp)
target_include_directories(prospero-core PUBLIC .)
target_compile_features(prospero-core PUBLIC cxx_std_17)

add_executable(prospero-core-test test_machine.cpp)
target_link_libraries(prospero-core-test PRIVATE prospero-core)
