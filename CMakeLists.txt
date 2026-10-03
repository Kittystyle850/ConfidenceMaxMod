cmake_minimum_required(VERSION 3.15)
project(CT2CheatDLL VERSION 1.0 LANGUAGES CXX ASM_MASM)

# CaptainTsubasa2WF-Win64-Shipping.exe is x64. Do NOT force Win32.

add_library(CT2CheatDLL SHARED main.cpp hooks.asm)

set_source_files_properties(hooks.asm PROPERTIES LANGUAGE ASM_MASM)

set_target_properties(CT2CheatDLL PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(CT2CheatDLL PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/std:c++17 /O2 /Ob2 /Oi /Ot /EHsc>
)
target_compile_definitions(CT2CheatDLL PRIVATE UNICODE _UNICODE)
