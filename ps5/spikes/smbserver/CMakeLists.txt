# SMB server prototype on libsmb2, in four variants from the same sources:
#   s2-smbserver  spike S2/S2b, read-only share "s2"
#   s3-smbserver  spike S3, read-write share "s3"
#   s4-smbserver  spike S4, read-write share "s4" with a 256 MiB file
#   s7-smbserver  spike S7, read-only share "s7" plus UDP discovery
# The PS5 preset builds the payloads, the host preset Linux variants for the
# CI tests.
foreach(spike 2 3 4 7)
  set(target s${spike}-smbserver)
  add_executable(${target} main.c backend.c discovery.c testdata.c)
  target_compile_definitions(${target} PRIVATE SPIKE=${spike})
  target_link_libraries(${target} PRIVATE smb2)
  if(PS5)
    set_target_properties(${target} PROPERTIES
      OUTPUT_NAME prospero-${target}
      SUFFIX .elf)
  endif()
endforeach()
