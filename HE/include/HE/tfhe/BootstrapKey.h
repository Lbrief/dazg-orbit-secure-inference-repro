// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/BootstrapKey.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>
#include <vector>
#include "Params.h"
#include "LWE.h"
#include "GLWE.h"

namespace dazg_orbit::tfhe {

struct BootstrapKey {
    BootstrapParams params;
    // Real TFHE needs encrypted gadget decompositions of the LWE key
    // under the GLWE key. This skeleton intentionally keeps the type open.
    std::vector<std::uint32_t> opaque_payload;
};

}  // namespace dazg_orbit::tfhe
