# DAZG-Orbit Project Source File
# Component: Layer/LinearLayer/CMakeLists.txt
# Purpose: DAZG-Orbit build or experiment integration component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
file(GLOB_RECURSE DAZG_LINEAR_LAYER_SRC CONFIGURE_DEPENDS src/*.cpp include/*.h)
add_library(LinearLayer STATIC ${DAZG_LINEAR_LAYER_SRC})
target_include_directories(LinearLayer PUBLIC include)
target_link_libraries(LinearLayer PUBLIC LinearOperator NonlinearOperator Utils HEXL::hexl dazg_platform)
