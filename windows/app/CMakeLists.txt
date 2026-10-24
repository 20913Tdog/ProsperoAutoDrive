# ProsperoAutoDrive.exe (phase 3) and its state machine. A Windows GUI
# program (tray), its own installer; manifest asInvoker.
add_subdirectory(core)

enable_language(RC)
add_executable(ProsperoAutoDrive WIN32 main.cpp agent.cpp setup.cpp ui.cpp app.rc)
target_compile_definitions(ProsperoAutoDrive PRIVATE PROSPERO_VERSION="${PROJECT_VERSION}")
target_link_libraries(ProsperoAutoDrive PRIVATE prospero-core prospero-client mpr netapi32 shell32 ole32 uuid)
