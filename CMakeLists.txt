# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(SDL_emfs VERSION 0.1.0 LANGUAGES C CXX)

if(NOT TARGET SDL3::SDL3)
    if(EMSCRIPTEN)
        find_package(SDL3 REQUIRED CONFIG)
    else()
        find_package(SDL3 3.2 REQUIRED CONFIG)
    endif()
endif()

add_library(SDL_emfs STATIC
    src/async_file.cpp
    src/context.cpp
    src/index.cpp
    src/native.cpp
    src/path.cpp
    src/sync_file.cpp
)
add_library(SDL_emfs::SDL_emfs ALIAS SDL_emfs)

target_compile_features(SDL_emfs PRIVATE cxx_std_17)
target_include_directories(SDL_emfs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(SDL_emfs PUBLIC SDL3::SDL3)

if(EMSCRIPTEN)
    target_sources(SDL_emfs PRIVATE src/emscripten/web_bridge.cpp)
    set_property(TARGET SDL_emfs APPEND PROPERTY INTERFACE_LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/emscripten/sdl_emfs_library.js"
    )
    target_link_options(SDL_emfs PUBLIC
        "SHELL:--js-library \"${CMAKE_CURRENT_SOURCE_DIR}/src/emscripten/sdl_emfs_library.js\""
        "SHELL:-lidbfs.js"
    )
endif()

option(SDL_EMFS_BUILD_EXAMPLES "Build SDL_emfs examples" OFF)
if(SDL_EMFS_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
