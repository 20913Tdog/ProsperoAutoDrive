# Spike S2: read-only SMB server prototype on libsmb2. The PS5 preset builds
# the payload, the host preset a Linux variant for the CI tests.
add_executable(s2-smbserver main.c fsro.c testdata.c)
target_link_libraries(s2-smbserver PRIVATE smb2)

if(PS5)
  set_target_properties(s2-smbserver PROPERTIES
    OUTPUT_NAME prospero-s2-smbserver
    SUFFIX .elf)
endif()
