# libFuzzer harness targets. Only built with -DMB_FUZZ=ON (clang required).
# Each harness is one fuzz_<name>.c with LLVMFuzzerTestOneInput, a seed corpus
# in corpus/<name>/, and an optional dictionary in dict/<name>.dict.

function(mb_add_fuzzer name)
  add_executable(fuzz-${name} fuzz_${name}.c ${ARGN})
  target_link_options(fuzz-${name} PRIVATE -fsanitize=fuzzer)
  target_compile_options(fuzz-${name} PRIVATE ${MB_WARNINGS})
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(fuzz-${name} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
  list(APPEND MB_FUZZ_TARGETS fuzz-${name})
  set(MB_FUZZ_TARGETS ${MB_FUZZ_TARGETS} PARENT_SCOPE)
endfunction()

# NATS client protocol parser (src/proto.c): bytes straight off the socket.
mb_add_fuzzer(proto)
target_link_libraries(fuzz-proto PRIVATE monoblok_core)

# HTTP/SSE adapter request parsing (src/server/http_parse.c).
mb_add_fuzzer(http)
target_link_libraries(fuzz-http PRIVATE monoblok_core)

# Patchbay source loaders: EDN, JSON and YAML spellings.
mb_add_fuzzer(patchbay_edn)
target_link_libraries(fuzz-patchbay_edn PRIVATE patchbay_sexpr)
mb_add_fuzzer(patchbay_json)
target_link_libraries(fuzz-patchbay_json PRIVATE patchbay_sexpr)
mb_add_fuzzer(patchbay_yaml)
target_link_libraries(fuzz-patchbay_yaml PRIVATE patchbay_sexpr)

# Non-backtracking regex engine: pattern compile + match against payloads.
mb_add_fuzzer(regex)
target_link_libraries(fuzz-regex PRIVATE patchbay_sexpr)

add_custom_target(fuzzers DEPENDS ${MB_FUZZ_TARGETS})
