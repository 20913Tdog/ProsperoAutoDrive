# ProsperoAutoDrive.exe (phase 3) and its state machine.
add_subdirectory(core)

add_executable(ProsperoAutoDrive main.cpp agent.cpp)
target_link_libraries(ProsperoAutoDrive PRIVATE prospero-core prospero-client mpr)
