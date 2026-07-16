# DAZG-Orbit Project Source File
# Component: CMakeLists.txt
# Purpose: DAZG-Orbit build or experiment integration component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
cmake_minimum_required(VERSION 3.20)
project(DAZGOrbitSecureInferenceRepro VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(DAZG_BUILD_N10 "Build the frozen P60 N=10 target" ON)
option(DAZG_BUILD_N100 "Build the checkpoint-013 N=100 target" ON)
option(DAZG_USE_RANDOM_DEVICE "Use OS randomness instead of RDSEED" ON)
option(DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX "Enable checkpoint-013 Stage-S GeLU tail contract" OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(HEXL 1.2.6 CONFIG REQUIRED)
find_package(SEAL 4.1.2 EXACT CONFIG REQUIRED)
find_library(GMP_LIBRARY NAMES gmp REQUIRED)

add_library(dazg_platform INTERFACE)
target_include_directories(dazg_platform INTERFACE
  "${CMAKE_SOURCE_DIR}/Extern/emp-tool"
  "${CMAKE_SOURCE_DIR}/Extern/emp-ot")
target_link_libraries(dazg_platform INTERFACE Threads::Threads OpenSSL::Crypto ${GMP_LIBRARY} dl)
target_compile_options(dazg_platform INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-maes;-msse4.1;-mavx;-mavx2;-mno-avx512f;-mno-avx512bw;-mno-avx512vl>)
if(DAZG_USE_RANDOM_DEVICE)
  target_compile_definitions(dazg_platform INTERFACE EMP_USE_RANDOM_DEVICE=1)
else()
  target_compile_options(dazg_platform INTERFACE -mrdseed)
endif()

add_subdirectory(Datatype)
add_subdirectory(Utils)
add_subdirectory(HE)
add_subdirectory(OT)
add_subdirectory(Operator)
add_subdirectory(Layer)
add_subdirectory(Model)

if(DAZG_BUILD_N10)
  add_subdirectory(experiments/n10_p60)
endif()
if(DAZG_BUILD_N100)
  add_subdirectory(experiments/n100_checkpoint013)
endif()
