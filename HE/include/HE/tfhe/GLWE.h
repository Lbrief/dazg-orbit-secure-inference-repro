// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/GLWE.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <vector>
#include "Torus32.h"
#include "Params.h"

namespace dazg_orbit::tfhe {

struct GlweCiphertext {
    std::vector<std::vector<Torus32>> a;  // k polynomials
    std::vector<Torus32> b;               // one body polynomial
    double current_noise = 0.0;
};

class GlweEngine {
public:
    explicit GlweEngine(GlweParams params) : params_(params) {}
    const GlweParams& params() const { return params_; }

private:
    GlweParams params_;
};

}  // namespace dazg_orbit::tfhe
