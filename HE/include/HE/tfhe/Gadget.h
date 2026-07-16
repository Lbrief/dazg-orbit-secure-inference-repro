// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/Gadget.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>
#include <vector>
#include "Params.h"

namespace dazg_orbit::tfhe {

class GadgetDecomposer {
public:
    explicit GadgetDecomposer(GadgetParams params) : params_(params) {}

    std::vector<std::uint32_t> decompose(std::uint32_t value) const {
        std::vector<std::uint32_t> out(params_.level_count, 0U);
        const std::uint32_t mask = (1U << params_.base_log) - 1U;
        for (std::size_t l = 0; l < params_.level_count; ++l) {
            out[l] = (value >> (l * params_.base_log)) & mask;
        }
        return out;
    }

private:
    GadgetParams params_;
};

}  // namespace dazg_orbit::tfhe
