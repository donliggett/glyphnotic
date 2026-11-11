cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/glyph-filter.cpp
    src/glyph-filter.hpp
    src/modes.cpp
    src/modes.hpp
    src/presets.cpp
    src/presets.hpp
    src/palette.cpp
    src/palette.hpp
    src/settings.hpp
    src/font.cpp
    src/font.hpp
    src/grid.hpp
)

# Debug tools, never for release builds: the filter's Dump grid button and
# tools/harness (a headless libobs runner for the reference comparison).
option(GLYPHNOTIC_DEBUG_TOOLS "Build the Dump grid button and the test harness" OFF)
if(GLYPHNOTIC_DEBUG_TOOLS)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE GLYPHNOTIC_DEBUG_TOOLS)
  add_executable(glyphnotic-harness tools/harness/harness.cpp)
  target_link_libraries(glyphnotic-harness PRIVATE OBS::libobs)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

# OBS 33 reads plugin metadata (display name, id, version, links) from data/manifest.json.
configure_file(cmake/common/manifest.json.in "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" @ONLY)
target_add_resource(${CMAKE_PROJECT_NAME} "${CMAKE_CURRENT_BINARY_DIR}/manifest.json")
