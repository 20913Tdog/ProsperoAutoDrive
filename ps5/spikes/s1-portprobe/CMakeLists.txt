# Spike S1: port probe. The PS5 preset builds the payload, the host preset
# a Linux variant of the same source for the CI tests.
add_executable(s1-portprobe main.c)

if(PS5)
  set_target_properties(s1-portprobe PROPERTIES
    OUTPUT_NAME prospero-s1-portprobe
    SUFFIX .elf)
endif()
