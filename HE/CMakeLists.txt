# DAZG-Orbit Project Source File
# Component: HE/CMakeLists.txt
# Purpose: DAZG-Orbit build or experiment integration component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set(DAZG_HE_SRC
  src/NetIO.cpp
  src/UnifiedHE.cpp
  src/tfhe/BFELutEncoder.cpp
  src/tfhe/BFEReLUClear.cpp
  src/tfhe/DAZGRLut.cpp
  src/tfhe/BFEGeLUClear.cpp)
add_library(HE STATIC ${DAZG_HE_SRC})
target_include_directories(HE PUBLIC include)
target_link_libraries(HE PUBLIC SEAL::seal Datatype Utils dazg_platform)

if(DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX)
  target_compile_definitions(HE PRIVATE DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX=1)
endif()
