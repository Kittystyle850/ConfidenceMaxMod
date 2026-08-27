cmake_minimum_required(VERSION 3.15)
project(ConfidenceMaxMod VERSION 1.0)

# Forza la compilazione a 32-bit per il gioco
set(CMAKE_GENERATOR_PLATFORM Win32)

add_library(ConfidenceMaxMod SHARED main.cpp)

# Ottimizzazioni per la release
target_compile_options(ConfidenceMaxMod PRIVATE /O2 /Ob2 /Oi /Ot)
