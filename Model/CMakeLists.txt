# DAZG-Orbit Project Source File
# Component: Model/CMakeLists.txt
# Purpose: DAZG-Orbit build or experiment integration component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
file(GLOB_RECURSE DAZG_MODEL_SRC CONFIGURE_DEPENDS src/*.cpp include/*.h)
add_library(Model STATIC ${DAZG_MODEL_SRC})
target_include_directories(Model PUBLIC include)
target_link_libraries(Model PUBLIC LinearLayer NonlinearLayer dazg_platform)
