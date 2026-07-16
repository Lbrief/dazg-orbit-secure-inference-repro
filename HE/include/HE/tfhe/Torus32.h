// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/Torus32.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>
#include <cmath>

namespace dazg_orbit::tfhe {

using Torus32 = std::uint32_t;

inline constexpr double kTwo32 = 4294967296.0;

inline Torus32 double_to_torus32(double x) {
    const double scaled = std::ldexp(x, 32);
    const auto rounded = static_cast<std::int64_t>(std::llround(scaled));
    return static_cast<Torus32>(rounded);
}

inline double torus32_to_double(Torus32 x) {
    return static_cast<std::int64_t>(x) / kTwo32;
}

}  // namespace dazg_orbit::tfhe
