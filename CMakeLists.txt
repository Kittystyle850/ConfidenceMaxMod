cmake_minimum_required(VERSION 3.15)
project(CT2UniqueLimit4Mod VERSION 1.0 LANGUAGES CXX)

add_library(CT2UniqueLimit4Mod SHARED
    main.cpp
)

set_target_properties(CT2UniqueLimit4Mod PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME "CT2_UniqueLimit4"
)

if (MSVC)
    target_compile_options(CT2UniqueLimit4Mod PRIVATE /std:c++17 /O2 /Ob2 /Oi /Ot /EHsc)
endif()

target_link_libraries(CT2UniqueLimit4Mod PRIVATE kernel32)
