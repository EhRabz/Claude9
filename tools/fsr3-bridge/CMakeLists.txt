# Builds fsr3_bridge.dll against AMD's FidelityFX SDK 2.3.0.
# The SDK dir comes in as -DFSR3_SDK_DIR=<sdk root> from
# get-fsr3.ps1. SDK 2.x splits the headers: the API core lives in
# Kits/FidelityFX/api/include and each effect's headers in
# Kits/FidelityFX/<effect>/include, so both are on the path.
# (SDK 1.1.4 kept everything under ffx-api/include.)
cmake_minimum_required(VERSION 3.15)
project(fsr3_bridge CXX)

add_library(fsr3_bridge SHARED fsr3_bridge.cpp fsr3_bridge.h)

target_compile_features(fsr3_bridge PRIVATE cxx_std_17)

target_include_directories(fsr3_bridge PRIVATE
    ${FSR3_SDK_DIR}/Kits/FidelityFX/api/include
    ${FSR3_SDK_DIR}/Kits/FidelityFX/framegeneration/include)

target_link_libraries(fsr3_bridge PRIVATE d3d12 dxgi)
