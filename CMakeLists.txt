cmake_minimum_required(VERSION 3.15)
project(MindCopTrainer VERSION 1.1 LANGUAGES CXX)

# MindCop.exe è x64 (base 0x140000000).
# NON forzare Win32 o la DLL non sarà iniettabile.
# Se un domani il gioco diventasse 32-bit, aggiungi:
#   set(CMAKE_GENERATOR_PLATFORM Win32)
# oppure passa -A Win32 al comando cmake.

add_library(MindCopTrainer SHARED main.cpp)

target_compile_options(MindCopTrainer PRIVATE /O2 /Ob2 /Oi /Ot /EHsc)
target_compile_definitions(MindCopTrainer PRIVATE UNICODE _UNICODE)
