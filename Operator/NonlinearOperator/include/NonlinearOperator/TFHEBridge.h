// DAZG-Orbit Project Source File
// Component: Operator/NonlinearOperator/include/NonlinearOperator/TFHEBridge.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>
#include <vector>

namespace dazg_orbit::tfhe {

// This bridge is the intended seam between the existing DAZG-Orbit nonlinear
// operator stack and the new TFHE-style selector branch.
//
// Phase 1 integration:
//   - keep the original tensor path
//   - quantize activations into low-bit scalars
//   - call TFHE selector / simulated PBS
//   - decode the control word
//   - evaluate the local polynomial using the old numeric path
struct TFHEBridgeOutput {
    std::vector<std::uint32_t> control_words;
    std::vector<double> approx_values;
};

}  // namespace dazg_orbit::tfhe
