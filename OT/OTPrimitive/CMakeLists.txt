# DAZG-Orbit Project Source File
# Component: OT/OTPrimitive/CMakeLists.txt
# Purpose: DAZG-Orbit build or experiment integration component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
file(GLOB_RECURSE DAZG_OT_PRIMITIVE_SRC CONFIGURE_DEPENDS src/*.cpp include/*.h)
add_library(OTPrimitive STATIC ${DAZG_OT_PRIMITIVE_SRC})
target_include_directories(OTPrimitive PUBLIC include)
target_link_libraries(OTPrimitive PUBLIC Datatype Utils dazg_platform)
