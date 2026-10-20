# Spike S7: discovery client for Windows (llvm-mingw, statically linked).
enable_language(CXX)
add_executable(s7-discover main.cpp)
set_target_properties(s7-discover PROPERTIES
  OUTPUT_NAME prospero-s7-discover
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON)
target_compile_definitions(s7-discover PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE _WIN32_WINNT=0x0A00)
target_link_libraries(s7-discover PRIVATE ws2_32 iphlpapi bcrypt)
