// DAZG-Orbit Project Source File
// Component: Layer/NonlinearLayer/include/NonlinearLayer/TFHESelector.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>
#include "HE/tfhe/BFELutEncoder.h"
#include "HE/tfhe/PBS.h"

namespace dazg_orbit::tfhe {

class TFHESelector {
public:
    TFHESelector(BFELutEncoder encoder, EncodedTables tables)
        : encoder_(std::move(encoder)),
          tables_(std::move(tables)),
          pbs_(tables_) {}

    std::uint32_t forward_control_word_from_real(double x) const {
        const int q = encoder_.real_to_index(x);
        return pbs_.bootstrap_cw(q);
    }

    double forward_value_from_real(double x) const {
        const auto cw = forward_control_word_from_real(x);
        return encoder_.evaluate_from_control_word(cw, tables_);
    }

    const EncodedTables& tables() const { return tables_; }

private:
    BFELutEncoder encoder_;
    EncodedTables tables_;
    SimulatedPBS pbs_;
};

}  // namespace dazg_orbit::tfhe
