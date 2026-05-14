cmake_minimum_required(VERSION 3.22)
project(monoblok_c_spike C)

set(MONOBLOK_C_VERSION "0.0.0-c-spike" CACHE STRING "monoblok-c version")
set(MB_BUILD_PLATFORM "${CMAKE_SYSTEM_NAME}")
set(MB_BUILD_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
set(MB_BUILD_COMPILER "${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
configure_file(src/mb_version.h.in generated/mb_version.h @ONLY)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS OFF)

option(MB_ASAN "Build with AddressSanitizer and UBSan" OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

if(MB_ASAN)
  add_compile_options(-fsanitize=address,undefined)
  add_link_options(-fsanitize=address,undefined)
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
  src/buf.c
  src/proto.c
  src/router.c
)

set(PB_CORE
  src/patchbay/pb_arena.c
  src/patchbay/pb_eval.c
  src/patchbay/pb_json.c
  src/patchbay/pb_program.c
  src/patchbay/pb_sexpr.c
  src/patchbay/pb_soundcheck.c
  src/patchbay/pb_validate.c
)

add_library(yyjson STATIC vendor/yyjson/src/yyjson.c)
target_include_directories(yyjson PUBLIC vendor/yyjson/src)

add_library(monoblok_core STATIC ${MB_CORE})
target_include_directories(monoblok_core PUBLIC src)
target_include_directories(monoblok_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_options(monoblok_core PRIVATE ${MB_WARNINGS})

add_executable(unit-tests test/unit.c)
target_link_libraries(unit-tests PRIVATE monoblok_core)
target_compile_options(unit-tests PRIVATE ${MB_WARNINGS})

add_library(patchbay_sexpr STATIC ${PB_CORE})
target_include_directories(patchbay_sexpr PUBLIC src)
target_include_directories(patchbay_sexpr PUBLIC src/patchbay)
target_include_directories(patchbay_sexpr PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(patchbay_sexpr PUBLIC yyjson)
target_compile_options(patchbay_sexpr PRIVATE ${MB_WARNINGS})

add_executable(sexpr-tests test/sexpr_unit.c)
target_link_libraries(sexpr-tests PRIVATE patchbay_sexpr)
target_compile_options(sexpr-tests PRIVATE ${MB_WARNINGS})

add_executable(eval-tests test/eval_unit.c)
target_link_libraries(eval-tests PRIVATE patchbay_sexpr ${MB_MATH_LIB})
target_compile_options(eval-tests PRIVATE ${MB_WARNINGS})

add_executable(json-tests test/json_unit.c)
target_link_libraries(json-tests PRIVATE patchbay_sexpr ${MB_MATH_LIB})
target_compile_options(json-tests PRIVATE ${MB_WARNINGS})

add_executable(pb-dump src/patchbay/pb_dump.c)
target_link_libraries(pb-dump PRIVATE patchbay_sexpr)
target_compile_options(pb-dump PRIVATE ${MB_WARNINGS})

enable_testing()
add_test(NAME unit COMMAND unit-tests)
add_test(NAME sexpr COMMAND sexpr-tests)
add_test(NAME eval COMMAND eval-tests)
add_test(NAME json COMMAND json-tests)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libuv/CMakeLists.txt")
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(vendor/libuv EXCLUDE_FROM_ALL)
  set(MB_UV_TARGET uv_a)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUV REQUIRED IMPORTED_TARGET libuv)
  set(MB_UV_TARGET PkgConfig::LIBUV)
endif()

add_executable(monoblok-c
  src/server/conn.c
  src/main.c
  src/server/server.c
)
target_link_libraries(monoblok-c PRIVATE monoblok_core patchbay_sexpr ${MB_UV_TARGET} ${MB_MATH_LIB})
target_compile_options(monoblok-c PRIVATE ${MB_WARNINGS})
target_include_directories(monoblok-c PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(monoblok-c PRIVATE src/server src/patchbay)

add_custom_target(smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/smoke.sh $<TARGET_FILE:monoblok-c>
  DEPENDS monoblok-c
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

add_custom_target(soundcheck
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test/soundcheck.sh $<TARGET_FILE:monoblok-c>
  DEPENDS monoblok-c
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)
