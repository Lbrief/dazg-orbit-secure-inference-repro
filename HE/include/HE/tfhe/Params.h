// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/Params.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstddef>
#include <cstdint>

namespace dazg_orbit::tfhe {

struct LweParams {
    std::size_t n = 630;
    double alpha = 3.05e-5;
};

struct GlweParams {
    std::size_t k = 1;
    std::size_t N = 1024;
    double alpha = 3.05e-5;
};

struct GadgetParams {
    std::size_t base_log = 7;
    std::size_t level_count = 4;
};

struct BootstrapParams {
    LweParams lwe;
    GlweParams glwe;
    GadgetParams gadget;
};

}  // namespace dazg_orbit::tfhe
