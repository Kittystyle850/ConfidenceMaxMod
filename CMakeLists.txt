cmake_minimum_required(VERSION 3.15)
project(CT2CheatDLL VERSION 1.0 LANGUAGES CXX ASM_MASM)

# CaptainTsubasa2WF-Win64-Shipping.exe is x64. Do NOT force Win32.

add_library(CT2CheatDLL SHARED main.cpp hooks.asm)

set_source_files_properties(hooks.asm PROPERTIES LANGUAGE ASM_MASM)

target_compile_options(CT2CheatDLL PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:/O2 /Ob2 /Oi /Ot /EHsc>
)
target_compile_definitions(CT2CheatDLL PRIVATE UNICODE _UNICODE)
