cmake_minimum_required(VERSION 3.15)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: no VC++ redist needed

project(ShadowOfWarMod VERSION 1.0 LANGUAGES CXX ASM_MASM)

add_library(ShadowOfWarMod SHARED
    main.cpp
    stubs.asm
)

set_target_properties(ShadowOfWarMod PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "ShadowOfWarMod"
)

if (MSVC)
    target_compile_options(ShadowOfWarMod PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/O2 /EHsc /W3>)
endif()

target_compile_definitions(ShadowOfWarMod PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ShadowOfWarMod PRIVATE kernel32)
