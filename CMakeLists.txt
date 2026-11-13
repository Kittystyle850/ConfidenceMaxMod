cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(OnimushaMod VERSION 1.0 LANGUAGES C CXX)

# ─────────────────────────────────────────────────────────────
# MinHook: populate sources only, DO NOT process its CMakeLists.
# SOURCE_SUBDIR points to a non-existent dir → FetchContent_MakeAvailable
# populates the source tree but skips add_subdirectory().
# We then build a static lib with a guaranteed name: minhook.lib.
# ─────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
    SOURCE_SUBDIR  __skip_minhook_cmake__
)
FetchContent_MakeAvailable(minhook)

if(NOT EXISTS "${minhook_SOURCE_DIR}/src/hook.c")
    message(FATAL_ERROR "MinHook sources not found at ${minhook_SOURCE_DIR}/src — FetchContent failed")
endif()

file(GLOB MINHOOK_SOURCES
    "${minhook_SOURCE_DIR}/src/*.c"
    "${minhook_SOURCE_DIR}/src/hde/*.c"
)
if(NOT MINHOOK_SOURCES)
    message(FATAL_ERROR "No MinHook .c files found under ${minhook_SOURCE_DIR}/src")
endif()

add_library(minhook STATIC ${MINHOOK_SOURCES})
set_target_properties(minhook PROPERTIES
    OUTPUT_NAME "minhook"
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)
target_include_directories(minhook PUBLIC
    "${minhook_SOURCE_DIR}/include"
    "${minhook_SOURCE_DIR}/src/hde"
)
target_compile_definitions(minhook PRIVATE _CRT_SECURE_NO_WARNINGS)

message(STATUS "MinHook sources: ${minhook_SOURCE_DIR} (${CMAKE_MATCH_COUNT} files)")

# ─────────────────────────────────────────────────────────────
# Main DLL
# ─────────────────────────────────────────────────────────────
add_library(OnimushaMod SHARED main.cpp)
set_target_properties(OnimushaMod PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "OnimushaMod")

target_include_directories(OnimushaMod PRIVATE "${minhook_SOURCE_DIR}/include")

if(MSVC)
    target_compile_options(OnimushaMod PRIVATE /O2 /EHsc /W3)
endif()
target_compile_definitions(OnimushaMod PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(OnimushaMod PRIVATE minhook kernel32)
