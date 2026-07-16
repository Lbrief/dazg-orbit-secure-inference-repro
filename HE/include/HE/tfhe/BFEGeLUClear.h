// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/BFEGeLUClear.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>
#include <vector>

namespace HE {

void EvalBFEGeLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale,
    std::vector<std::int64_t>& y_fp);

std::vector<std::int64_t> EvalBFEGeLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale);

void EvalBFESiLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale,
    std::vector<std::int64_t>& y_fp);

std::vector<std::int64_t> EvalBFESiLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale);

} // namespace HE
