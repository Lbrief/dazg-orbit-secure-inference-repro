# DAZG-Orbit Project Source File
# Component: experiments/n100_checkpoint013/CMakeLists.txt
# Purpose: DAZG-Orbit build or experiment integration component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
set(DAZG_N100_SRC "${CMAKE_CURRENT_LIST_DIR}/src/dazg_checkpoint013_n100_executor.cpp")
set(DAZG_STAGE_TEST "${CMAKE_CURRENT_LIST_DIR}/src/stage_s_contract_test.cpp")
add_executable(dazg_checkpoint013_n100_executor "${DAZG_N100_SRC}")
target_compile_definitions(dazg_checkpoint013_n100_executor PRIVATE
  DAZG_ORBIT_V720_ACTIVE_BINARY=1 DAZG_CHECKPOINT013_N100_FAIL_CLOSED=1)
target_compile_options(dazg_checkpoint013_n100_executor PRIVATE -O2)
target_include_directories(dazg_checkpoint013_n100_executor PRIVATE
  "${CMAKE_SOURCE_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/src"
  "${CMAKE_CURRENT_LIST_DIR}/src/qahl_v645_src")
target_link_libraries(dazg_checkpoint013_n100_executor PRIVATE
  "-Wl,--start-group"
  HE Utils Model Datatype LinearLayer NonlinearLayer LinearOperator NonlinearOperator OTProtocol OTPrimitive
  "-Wl,--end-group" dazg_platform)
set_target_properties(dazg_checkpoint013_n100_executor PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stage4_bin")

add_executable(dazg_checkpoint013_stage_s_contract_test "${DAZG_STAGE_TEST}")
target_compile_options(dazg_checkpoint013_stage_s_contract_test PRIVATE -O2)
target_include_directories(dazg_checkpoint013_stage_s_contract_test PRIVATE
  "${CMAKE_SOURCE_DIR}/HE/include" "${CMAKE_SOURCE_DIR}/Utils/include")
target_link_libraries(dazg_checkpoint013_stage_s_contract_test PRIVATE
  "-Wl,--start-group" HE Utils "-Wl,--end-group" dazg_platform)
set_target_properties(dazg_checkpoint013_stage_s_contract_test PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/stage4_bin")
