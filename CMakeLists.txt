cmake_minimum_required(VERSION 3.18)
project(CloseOreUI LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Build target declaration.
#
# This project has exactly one target: the mcpelauncher mod ABI on Linux x86_64.
# The three parameters below are what CI fills in. They are recorded in the
# binary so a runtime log can say which game version a build was made for.
# ---------------------------------------------------------------------------

if(NOT CLOSEOREUI_MOD_VERSION)
    set(CLOSEOREUI_MOD_VERSION "dev")
endif()

if(NOT CLOSEOREUI_TARGET_ABI)
    set(CLOSEOREUI_TARGET_ABI "x86_64")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

add_library(closeoreui SHARED
        src/mod.cpp
        src/target.h
        src/game_version.cpp
        src/config.cpp
        src/json.cpp
        src/log.cpp
        src/module_scan.cpp
        src/elf_image.cpp
        src/eh_frame.cpp
        src/x64_decoder.cpp
        src/inline_hook.cpp
        src/discover.cpp
        src/oreui_config.cpp
        src/oreui_hook.cpp
        src/menu.cpp
)

target_include_directories(closeoreui PRIVATE src)

target_compile_options(closeoreui PRIVATE
        -O2
        -Wall
        -Wextra
        -Wno-unused-parameter
        -fno-omit-frame-pointer
)

target_compile_definitions(closeoreui PRIVATE
        CLOSEOREUI_MOD_VERSION="${CLOSEOREUI_MOD_VERSION}"
        CLOSEOREUI_TARGET_ABI="${CLOSEOREUI_TARGET_ABI}"
)

# liblog and libdl are provided by mcpelauncher's own bionic linker; libc++_shared
# ships with the game. Nothing else is required — no third-party libraries.
target_link_libraries(closeoreui PRIVATE log dl c++_shared)

set_target_properties(closeoreui PROPERTIES
        OUTPUT_NAME "closeoreui"
        PREFIX "lib"
        SUFFIX ".so"
)

install(TARGETS closeoreui LIBRARY DESTINATION .)
