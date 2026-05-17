cmake_minimum_required(VERSION 3.22)

set(MONOBLOK_FALLBACK_VERSION "0.0.0")
set(MONOBLOK_DETECTED_VERSION "${MONOBLOK_FALLBACK_VERSION}")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE MB_GIT_TAG_RESULT
    OUTPUT_VARIABLE MB_GIT_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(MB_GIT_TAG_RESULT EQUAL 0)
    string(REGEX REPLACE "^v" "" MB_GIT_TAG_VERSION "${MB_GIT_TAG}")
    if(MB_GIT_TAG_VERSION MATCHES "^[0-9]+(\\.[0-9]+){0,3}$")
      set(MONOBLOK_DETECTED_VERSION "${MB_GIT_TAG_VERSION}")
    endif()
  endif()
endif()
set(MONOBLOK_VERSION "${MONOBLOK_DETECTED_VERSION}" CACHE STRING "monoblok version")

project(monoblok VERSION "${MONOBLOK_VERSION}" LANGUAGES C)

set(MB_BUILD_PLATFORM "${CMAKE_SYSTEM_NAME}")
set(MB_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
set(MB_BUILD_COMPILER "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
set(MB_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
if(NOT MB_BUILD_TYPE)
  set(MB_BUILD_TYPE "multi-config/unspecified")
endif()
set(MB_BUILD_INFO "${MB_BUILD_PLATFORM}/${MB_BUILD_ARCH} ${MB_BUILD_COMPILER} ${MB_BUILD_TYPE}")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(MB_ASAN "Build with AddressSanitizer and UBSan" OFF)
option(MB_NATS_TLS "Build nats.c with TLS/OpenSSL support" ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  set(MB_BUILD_TYPE "${CMAKE_BUILD_TYPE}")
  set(MB_BUILD_INFO "${MB_BUILD_PLATFORM}/${MB_BUILD_ARCH} ${MB_BUILD_COMPILER} ${MB_BUILD_TYPE}")
endif()

configure_file(src/mb_version.h.in generated/mb_version.h @ONLY)

if(MB_ASAN)
  add_compile_options(-fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
endif()

set(MB_LOAD_SOAK_SUBSCRIBERS 64)
set(MB_LOAD_SOAK_MESSAGES_PER_SUBJECT 1024)
set(MB_LOAD_SOAK_FLUSH_EVERY 0)
set(MB_LOAD_SOAK_TIMEOUT 60)
if(MB_ASAN)
  set(MB_LOAD_SOAK_SUBSCRIBERS 8)
  set(MB_LOAD_SOAK_MESSAGES_PER_SUBJECT 64)
  set(MB_LOAD_SOAK_FLUSH_EVERY 16)
endif()

set(MB_WARNINGS
  -Wall
  -Wextra
  -Wpedantic
  -Wconversion
  -Wshadow
  -Wstrict-prototypes
  -Wmissing-prototypes
)

if(APPLE)
  set(MB_MATH_LIB "")
else()
  set(MB_MATH_LIB m)
endif()

set(MB_CORE
  src/array.c
  src/buf.c
  src/fs.c
  src/proto.c
  src/router.c
  src/slice.c
)

set(PB_CORE
  src/patchbay/pb_arena.c
  src/patchbay/pb_forms.c
  src/patchbay/pb_eval.c
  src/patchbay/pb_json.c
  src/patchbay/pb_program.c
  src/patchbay/pb_sexpr.c
  src/patchbay/pb_soundcheck.c
  src/patchbay/pb_validate.c
)

set(PB_FORM_FRAGMENTS
  src/patchbay/pb_form_chunk.h
  src/patchbay/pb_form_bar.c
  src/patchbay/pb_form_clocks.c
  src/patchbay/pb_form_json.c
  src/patchbay/pb_form_numbers.c
  src/patchbay/pb_form_state.c
  src/patchbay/pb_form_text.c
  src/patchbay/pb_form_windows.c
)
set_source_files_properties(${PB_FORM_FRAGMENTS} PROPERTIES HEADER_FILE_ONLY TRUE)

add_library(yyjson STATIC vendor/yyjson/src/yyjson.c)
target_include_directories(yyjson PUBLIC vendor/yyjson/src)

add_library(monoblok_core STATIC ${MB_CORE})
target_include_directories(monoblok_core PUBLIC src)
target_include_directories(monoblok_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(monoblok_core PRIVATE ${MB_WARNINGS})

add_executable(unit-tests test/unit.c)
target_link_libraries(unit-tests PRIVATE monoblok_core)
target_compile_options(unit-tests PRIVATE ${MB_WARNINGS})

add_library(patchbay_sexpr STATIC ${PB_CORE} ${PB_FORM_FRAGMENTS})
target_include_directories(patchbay_sexpr PUBLIC src)
target_include_directories(patchbay_sexpr PUBLIC src/patchbay)
target_include_directories(patchbay_sexpr PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(patchbay_sexpr PUBLIC yyjson monoblok_core ${MB_MATH_LIB})
target_compile_options(patchbay_sexpr PRIVATE ${MB_WARNINGS})

add_executable(sexpr-tests test/sexpr_unit.c)
target_link_libraries(sexpr-tests PRIVATE patchbay_sexpr)
target_compile_options(sexpr-tests PRIVATE ${MB_WARNINGS})

add_executable(eval-tests test/eval_unit.c)
target_link_libraries(eval-tests PRIVATE patchbay_sexpr)
target_compile_options(eval-tests PRIVATE ${MB_WARNINGS})

add_executable(correctness-tests test/correctness_unit.c)
target_link_libraries(correctness-tests PRIVATE patchbay_sexpr)
target_compile_options(correctness-tests PRIVATE ${MB_WARNINGS})

add_executable(json-tests test/json_unit.c)
target_link_libraries(json-tests PRIVATE patchbay_sexpr)
target_compile_options(json-tests PRIVATE ${MB_WARNINGS})

add_executable(program-tests test/program_unit.c)
target_link_libraries(program-tests PRIVATE patchbay_sexpr)
target_compile_options(program-tests PRIVATE ${MB_WARNINGS})

add_executable(snapshot-tests test/snapshot_unit.c src/snapshot.c)
target_link_libraries(snapshot-tests PRIVATE patchbay_sexpr)
target_compile_options(snapshot-tests PRIVATE ${MB_WARNINGS})

add_executable(bridge-tests test/bridge_unit.c test/fake_nats.c src/bridge.c src/nats_common.c)
target_include_directories(bridge-tests PRIVATE test/fakes src/patchbay)
target_link_libraries(bridge-tests PRIVATE monoblok_core)
target_compile_options(bridge-tests PRIVATE ${MB_WARNINGS})

add_executable(bench-patchbay src/bench_patchbay.c)
target_link_libraries(bench-patchbay PRIVATE patchbay_sexpr)
target_compile_options(bench-patchbay PRIVATE ${MB_WARNINGS})

add_executable(pb-dump src/patchbay/pb_dump.c)
target_link_libraries(pb-dump PRIVATE patchbay_sexpr)
target_compile_options(pb-dump PRIVATE ${MB_WARNINGS})

enable_testing()
add_test(NAME unit COMMAND unit-tests)
add_test(NAME sexpr COMMAND sexpr-tests)
add_test(NAME eval COMMAND eval-tests)
add_test(NAME correctness COMMAND correctness-tests)
add_test(NAME json COMMAND json-tests)
add_test(NAME program COMMAND program-tests)
add_test(NAME snapshot COMMAND snapshot-tests)
add_test(NAME bridge COMMAND bridge-tests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libuv/CMakeLists.txt")
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
  add_subdirectory(vendor/libuv EXCLUDE_FROM_ALL)
  set(MB_UV_TARGET uv_a)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
  set(MB_UV_TARGET PkgConfig::LIBUV)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/nats.c/CMakeLists.txt")
  set(NATS_BUILD_WITH_TLS ${MB_NATS_TLS} CACHE BOOL "" FORCE)
  set(NATS_BUILD_OPENSSL_STATIC_LIBS OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_LIB_SHARED OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_LIB_STATIC ON CACHE BOOL "" FORCE)
  set(NATS_BUILD_STREAMING OFF CACHE BOOL "" FORCE)
  set(NATS_BUILD_USE_SODIUM OFF CACHE BOOL "" FORCE)
  add_subdirectory(vendor/nats.c EXCLUDE_FROM_ALL)
  set(MB_NATS_TARGET nats_static)
else()
  set(MB_NATS_TARGET "")
endif()

add_executable(importer-tests test/importer_unit.c test/fake_nats.c src/importer.c src/nats_common.c)
target_include_directories(importer-tests PRIVATE test/fakes src/patchbay)
target_link_libraries(importer-tests PRIVATE monoblok_core ${MB_UV_TARGET})
target_compile_options(importer-tests PRIVATE ${MB_WARNINGS})
add_test(NAME importer COMMAND importer-tests)

add_executable(monoblok
  src/bridge.c
  src/importer.c
  src/nats_common.c
  src/snapshot.c
  src/server/conn.c
  src/main.c
  src/server/server.c
)
target_link_libraries(monoblok PRIVATE patchbay_sexpr ${MB_UV_TARGET} ${MB_NATS_TARGET})
target_compile_options(monoblok PRIVATE ${MB_WARNINGS})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_compile_definitions(monoblok PRIVATE _GNU_SOURCE)
endif()
target_include_directories(monoblok PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(monoblok PRIVATE src/server src/patchbay)

add_test(NAME cli COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/cli_unit.sh $<TARGET_FILE:monoblok>)

add_custom_target(smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/smoke.sh $<TARGET_FILE:monoblok>
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/soundcheck.sh $<TARGET_FILE:monoblok>
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test/load_smoke.py $<TARGET_FILE:monoblok>
  COMMAND ${CMAKE_COMMAND} -E env MB_LOAD_SUBSCRIBERS=${MB_LOAD_SOAK_SUBSCRIBERS} MB_LOAD_MESSAGES_PER_SUBJECT=${MB_LOAD_SOAK_MESSAGES_PER_SUBJECT} MB_LOAD_FLUSH_EVERY=${MB_LOAD_SOAK_FLUSH_EVERY} MB_LOAD_TIMEOUT=${MB_LOAD_SOAK_TIMEOUT}
          python3 ${CMAKE_CURRENT_SOURCE_DIR}/test/load_smoke.py $<TARGET_FILE:monoblok>
  DEPENDS monoblok
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(soundcheck
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/soundcheck.sh $<TARGET_FILE:monoblok>
  DEPENDS monoblok
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(load-smoke
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/test/load_smoke.py $<TARGET_FILE:monoblok>
  DEPENDS monoblok
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(load-soak
  COMMAND ${CMAKE_COMMAND} -E env MB_LOAD_SUBSCRIBERS=${MB_LOAD_SOAK_SUBSCRIBERS} MB_LOAD_MESSAGES_PER_SUBJECT=${MB_LOAD_SOAK_MESSAGES_PER_SUBJECT} MB_LOAD_FLUSH_EVERY=${MB_LOAD_SOAK_FLUSH_EVERY} MB_LOAD_TIMEOUT=${MB_LOAD_SOAK_TIMEOUT}
          python3 ${CMAKE_CURRENT_SOURCE_DIR}/test/load_smoke.py $<TARGET_FILE:monoblok>
  DEPENDS monoblok
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(tidy
  COMMAND ${CMAKE_COMMAND} -E echo "No clang-tidy checks configured for monoblok."
)
