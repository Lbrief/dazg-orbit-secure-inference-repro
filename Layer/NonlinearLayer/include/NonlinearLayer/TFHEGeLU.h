// DAZG-Orbit Project Source File
// Component: Layer/NonlinearLayer/include/NonlinearLayer/TFHEGeLU.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>
#include <vector>

#include "NonlinearLayer/TFHESelector.h"

namespace dazg_orbit::tfhe {

class TFHEGeLU {
public:
    static constexpr int kDAZGBitWidth = 32;
    static constexpr int kDAZGScaleBits = 16;

    // Default Stage-F entry.  It builds the same DA-ZG/RLUT GeLU configuration
    // used by the standalone integer-core tests, so callers can now instantiate
    // TFHEGeLU directly instead of creating a private harness around
    // HE::EvalBFEGeLUClear(...).
    TFHEGeLU();

    explicit TFHEGeLU(const BFELutEncoder& encoder);

    // Real-valued convenience API.  Internally this now routes through the
    // fixed-point DA-ZG main path:
    //
    //   real x -> Q16 fixed-point -> HE::EvalBFEGeLUClear -> real y
    //
    double forward(double x) const;
    std::vector<double> forward(const std::vector<double>& xs) const;

    // Main fixed-point API for DAZG-Orbit integration.  The scale is Q16 and the
    // arithmetic bit-width is 32 by default.
    std::int64_t forward_fixed(std::int64_t x_fp) const;
    std::vector<std::int64_t> forward_fixed(
        const std::vector<std::int64_t>& xs_fp) const;

    // Debug/control-word compatibility API.  This intentionally keeps the old
    // BFELutEncoder/TFHESelector control-word view available for diagnostics.
    std::uint32_t control_word(double x) const;
    std::vector<std::uint32_t> control_words(
        const std::vector<double>& xs) const;

    const TFHESelector& selector() const { return selector_; }

    static std::int64_t real_to_fixed(double x);
    static double fixed_to_real(std::int64_t x_fp);

    static constexpr int bit_width() noexcept { return kDAZGBitWidth; }
    static constexpr int scale_bits() noexcept { return kDAZGScaleBits; }

private:
    BFELutEncoder encoder_;
    EncodedTables tables_;
    TFHESelector selector_;
};

}  // namespace dazg_orbit::tfhe
