cmake_minimum_required(VERSION 3.21)
project(paintify-obs LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The Paintify OBS Spout bridge currently requires Windows")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OBS_SOURCE_DIR "" CACHE PATH "OBS source checkout matching the installed OBS major/minor version")
set(OBS_LIBRARY "" CACHE FILEPATH "obs.lib import library from the matching OBS installation")
if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs-module.h")
    message(FATAL_ERROR "Set OBS_SOURCE_DIR to an OBS Studio source checkout")
endif()
if(NOT EXISTS "${OBS_LIBRARY}")
    message(FATAL_ERROR "Set OBS_LIBRARY to an obs.lib import library")
endif()

find_package(Spout2 CONFIG REQUIRED)
if(NOT TARGET Spout2::SpoutDX)
    message(FATAL_ERROR "Spout2 was installed without its dx feature")
endif()

add_library(paintify-obs MODULE paintify-filter.cpp)
target_include_directories(paintify-obs PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${OBS_SOURCE_DIR}/libobs")
target_compile_definitions(paintify-obs PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(paintify-obs PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(paintify-obs PRIVATE "${OBS_LIBRARY}" Spout2::SpoutDX d3d11 dxgi)
set_target_properties(paintify-obs PROPERTIES PREFIX "" OUTPUT_NAME "paintify-obs")
