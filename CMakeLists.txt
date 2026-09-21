cmake_minimum_required(VERSION 3.15)
project(MindCopTrainer VERSION 1.0 LANGUAGES CXX)

# MindCop.exe gira a 64 bit (base 0x140000000).
# NON forzare Win32 come nella vecchia repo!
# Se in futuro il gioco diventasse 32-bit, metti: set(CMAKE_GENERATOR_PLATFORM Win32)

add_library(MindCopTrainer SHARED main.cpp)

target_compile_options(MindCopTrainer PRIVATE /O2 /Ob2 /Oi /Ot /EHsc)
