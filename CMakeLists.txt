cmake_minimum_required(VERSION 3.15)
project(CT2CheatDLL VERSION 0.3 LANGUAGES CXX)

# CaptainTsubasa2WF-Win64-Shipping.exe is x64. Do NOT force Win32.
# No .asm codecave yet in this research build - add ASM_MASM back to the
# LANGUAGES line above + a hooks.asm source once write_trap.h (F6) has
# identified the real function to hook.

add_library(CT2CheatDLL SHARED main.cpp)

set_target_properties(CT2CheatDLL PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

target_compile_options(CT2CheatDLL PRIVATE /std:c++17 /O2 /Ob2 /Oi /Ot /EHsc)
target_compile_definitions(CT2CheatDLL PRIVATE UNICODE _UNICODE)
target_link_libraries(CT2CheatDLL PRIVATE user32 gdi32 comctl32)
