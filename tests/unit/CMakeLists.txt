# Unit tests for the parts of Glyphnotic that don't need OBS.
#
# A standalone project: it needs only a C++ compiler, no OBS sources or
# dependencies. From the repository root:
#
#   cmake -S tests/unit -B build_tests
#   cmake --build build_tests --config Release
#   ctest --test-dir build_tests -C Release --output-on-failure

cmake_minimum_required(VERSION 3.28...3.30)

project(glyphnotic-unit-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GLYPHNOTIC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
cmake_path(NORMAL_PATH GLYPHNOTIC_ROOT)

if(MSVC)
  set(warning_flags /W4 /WX /utf-8)
else()
  set(warning_flags -Wall -Wextra -Werror)
endif()

add_executable(font-test font-test.cpp "${GLYPHNOTIC_ROOT}/src/font.cpp")
target_include_directories(font-test PRIVATE "${GLYPHNOTIC_ROOT}/src")
target_compile_options(font-test PRIVATE ${warning_flags})
target_compile_definitions(font-test PRIVATE GLYPHNOTIC_FONT_PATH="${GLYPHNOTIC_ROOT}/data/fonts/Uni2-Fixed16.psf")

add_executable(grid-test grid-test.cpp)
target_include_directories(grid-test PRIVATE "${GLYPHNOTIC_ROOT}/src")
target_compile_options(grid-test PRIVATE ${warning_flags})

add_executable(palette-test palette-test.cpp "${GLYPHNOTIC_ROOT}/src/palette.cpp" "${GLYPHNOTIC_ROOT}/src/presets.cpp")
target_include_directories(palette-test PRIVATE "${GLYPHNOTIC_ROOT}/src")
target_compile_options(palette-test PRIVATE ${warning_flags})

add_executable(motion-test motion-test.cpp)
target_include_directories(motion-test PRIVATE "${GLYPHNOTIC_ROOT}/src")
target_compile_options(motion-test PRIVATE ${warning_flags})

enable_testing()
add_test(NAME font-test COMMAND font-test)
add_test(NAME grid-test COMMAND grid-test)
add_test(NAME palette-test COMMAND palette-test)
add_test(NAME motion-test COMMAND motion-test)
