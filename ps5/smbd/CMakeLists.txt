# prospero-smbd, the product server (phase 2). The PS5 preset builds the
# payload prospero-smbd.elf, the host preset a Linux build for the CI tests.
add_executable(prospero-smbd main.c backend.c config.c diag.c discovery.c hmac.c log.c service.c)
target_compile_definitions(prospero-smbd PRIVATE
  PROSPERO_VERSION="${PROJECT_VERSION}")
target_link_libraries(prospero-smbd PRIVATE smb2)
if(PS5)
  set_target_properties(prospero-smbd PROPERTIES SUFFIX .elf)
endif()
