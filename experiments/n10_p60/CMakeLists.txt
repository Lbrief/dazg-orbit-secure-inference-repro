# DAZG-Orbit Project Source File
# Component: experiments/n10_p60/CMakeLists.txt
# Purpose: DAZG-Orbit build or experiment integration component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set(DAZG_N10_SRC "${CMAKE_CURRENT_LIST_DIR}/src/qahl_v720_to_h8_exact_executor.cpp")
add_executable(qahl_v720_to_h8_exact_executor "${DAZG_N10_SRC}")
target_compile_definitions(qahl_v720_to_h8_exact_executor PRIVATE DAZG_ORBIT_V720_ACTIVE_BINARY=1)
target_compile_options(qahl_v720_to_h8_exact_executor PRIVATE -O2)
target_include_directories(qahl_v720_to_h8_exact_executor PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/src"
  "${CMAKE_CURRENT_LIST_DIR}/src/qahl_v645_src")
target_link_libraries(qahl_v720_to_h8_exact_executor PRIVATE
  "-Wl,--start-group"
  HE Utils Model Datatype LinearLayer NonlinearLayer LinearOperator NonlinearOperator OTProtocol OTPrimitive
  "-Wl,--end-group" dazg_platform)
set_target_properties(qahl_v720_to_h8_exact_executor PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/v720_bin")
