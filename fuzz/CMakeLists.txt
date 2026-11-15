# CMakeLists.txt for libFuzzer targets in fuzz/

set(FUZZ_TARGETS
  fuzz_decompress_stream
  fuzz_bwt_decode
  fuzz_cm1_decode
  fuzz_x86_decode
  fuzz_arm64_decode
  fuzz_libsais_unbwt
  fuzz_cm1_decode_big
)

foreach(target ${FUZZ_TARGETS})
  if(target STREQUAL "fuzz_cm1_decode_big")
    add_executable(${target} fuzz_cm1_decode.cpp)
    target_compile_definitions(${target} PRIVATE APX_FUZZ_MAX_OUT=1048576)
  else()
    add_executable(${target} ${target}.cpp)
  endif()
  target_link_libraries(${target} PRIVATE apixel_core)
  # libFuzzer requires compiler and linker flags for sanitizers and fuzzer runtime.
  target_compile_options(${target} PRIVATE
    -fsanitize=fuzzer,address,undefined
    -fno-omit-frame-pointer
    -fno-sanitize-recover=undefined
    -g
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion
  )
  target_link_options(${target} PRIVATE
    -fsanitize=fuzzer,address,undefined
  )
endforeach()

# Standalone seed generator helper for stage decoders.
add_executable(seed_gen seed_gen.cpp)
target_link_libraries(seed_gen PRIVATE apixel_core)
target_compile_options(seed_gen PRIVATE
  -fsanitize=address,undefined
  -fno-omit-frame-pointer
  -g
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion
)
target_link_options(seed_gen PRIVATE
  -fsanitize=address,undefined
)
