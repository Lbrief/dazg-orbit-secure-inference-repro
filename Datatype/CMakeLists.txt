# DAZG-Orbit Project Source File
# Component: Datatype/CMakeLists.txt
# Purpose: DAZG-Orbit build or experiment integration component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
file(GLOB_RECURSE DAZG_DATATYPE_SRC CONFIGURE_DEPENDS src/*.cpp include/*.h)
add_library(Datatype STATIC ${DAZG_DATATYPE_SRC})
target_include_directories(Datatype PUBLIC include)
target_link_libraries(Datatype PUBLIC SEAL::seal dazg_platform)
