cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(OnimushaMod VERSION 1.0 LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.3
)
set(MINHOOK_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(MINHOOK_BUILD_STATIC_LIB ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(minhook)

if(NOT EXISTS "${minhook_SOURCE_DIR}/include/MinHook.h")
    message(FATAL_ERROR "MinHook.h not found at ${minhook_SOURCE_DIR}/include/MinHook.h — FetchContent failed")
endif()
message(STATUS "MinHook sources: ${minhook_SOURCE_DIR}")

add_library(OnimushaMod SHARED main.cpp)
set_target_properties(OnimushaMod PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "OnimushaMod")

target_include_directories(OnimushaMod PRIVATE ${minhook_SOURCE_DIR}/include)

if(MSVC)
    target_compile_options(OnimushaMod PRIVATE /O2 /EHsc /W3)
endif()
target_compile_definitions(OnimushaMod PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(OnimushaMod PRIVATE minhook kernel32)