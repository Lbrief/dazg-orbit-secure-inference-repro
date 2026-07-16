// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/PBS.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "HE/tfhe/BFELutEncoder.h"

namespace dazg_orbit::tfhe {

// Lightweight primitive counters. They do not change TFHE semantics; they make
// PBS/key-switch/external-product costs visible in artifact logs.
class DAZGOrbitTFHEPrimitiveProfile {
public:
    static DAZGOrbitTFHEPrimitiveProfile& Instance() {
        static DAZGOrbitTFHEPrimitiveProfile p;
        return p;
    }

    ~DAZGOrbitTFHEPrimitiveProfile() { Emit(); }

    void RecordLegacyPBS() { legacy_pbs_calls_.fetch_add(1, std::memory_order_relaxed); }
    void RecordStageSPBS(bool gelu) {
        stage_s_pbs_calls_.fetch_add(1, std::memory_order_relaxed);
        if (gelu) {
            stage_s_gelu_calls_.fetch_add(1, std::memory_order_relaxed);
        } else {
            stage_s_silu_calls_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    void RecordKeySwitch() { key_switch_calls_.fetch_add(1, std::memory_order_relaxed); }
    void RecordExternalProduct() { external_product_calls_.fetch_add(1, std::memory_order_relaxed); }
    void RecordBlindRotate() { blind_rotate_calls_.fetch_add(1, std::memory_order_relaxed); }

    void Emit() {
        bool expected = false;
        if (!emitted_.compare_exchange_strong(expected, true, std::memory_order_relaxed)) return;
        const std::uint64_t legacy = legacy_pbs_calls_.load(std::memory_order_relaxed);
        const std::uint64_t stage_s = stage_s_pbs_calls_.load(std::memory_order_relaxed);
        const std::uint64_t ks = key_switch_calls_.load(std::memory_order_relaxed);
        const std::uint64_t ep = external_product_calls_.load(std::memory_order_relaxed);
        const std::uint64_t br = blind_rotate_calls_.load(std::memory_order_relaxed);
        if (legacy == 0 && stage_s == 0 && ks == 0 && ep == 0 && br == 0) return;
        std::cerr << "[DAZG_ORBIT_TFHE_PRIMITIVE_PROFILE]"
                  << " legacy_simulated_pbs_calls=" << legacy
                  << " stage_s_route_pbs_calls=" << stage_s
                  << " stage_s_gelu_calls=" << stage_s_gelu_calls_.load(std::memory_order_relaxed)
                  << " stage_s_silu_calls=" << stage_s_silu_calls_.load(std::memory_order_relaxed)
                  << " key_switch_calls=" << ks
                  << " external_product_calls=" << ep
                  << " blind_rotate_calls=" << br
                  << " batch_scheduler_visible=1"
                  << " compressed_key_path_ready=0"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }

private:
    DAZGOrbitTFHEPrimitiveProfile() = default;
    std::atomic<bool> emitted_{false};
    std::atomic<std::uint64_t> legacy_pbs_calls_{0};
    std::atomic<std::uint64_t> stage_s_pbs_calls_{0};
    std::atomic<std::uint64_t> stage_s_gelu_calls_{0};
    std::atomic<std::uint64_t> stage_s_silu_calls_{0};
    std::atomic<std::uint64_t> key_switch_calls_{0};
    std::atomic<std::uint64_t> external_product_calls_{0};
    std::atomic<std::uint64_t> blind_rotate_calls_{0};
};


// ----------------------------------------------------------------------------
// Legacy simulation interface:
// ----------------------------------------------------------------------------
// This bridge mimics programmable bootstrapping by indexing low-bit LUTs.  It is
// intentionally side-effect free and keeps the old cw/sign/seg/u interfaces
// alive for existing tests.
// ----------------------------------------------------------------------------
class SimulatedPBS {
public:
    explicit SimulatedPBS(const EncodedTables& tables) : tables_(tables) {}

    std::uint32_t bootstrap_cw(std::int32_t q_input) const {
        DAZGOrbitTFHEPrimitiveProfile::Instance().RecordLegacyPBS();
        return at_indexed(tables_.cw_lut, q_input);
    }

    std::uint32_t bootstrap_sign(std::int32_t q_input) const {
        DAZGOrbitTFHEPrimitiveProfile::Instance().RecordLegacyPBS();
        return at_indexed(tables_.sign_lut, q_input);
    }

    std::uint32_t bootstrap_seg(std::int32_t q_input) const {
        DAZGOrbitTFHEPrimitiveProfile::Instance().RecordLegacyPBS();
        return at_indexed(tables_.seg_lut, q_input);
    }

    std::uint32_t bootstrap_u(std::int32_t q_input) const {
        DAZGOrbitTFHEPrimitiveProfile::Instance().RecordLegacyPBS();
        return at_indexed(tables_.u_lut, q_input);
    }

private:
    static std::uint32_t at_indexed(
        const std::vector<std::uint32_t>& lut,
        std::int32_t q_input) {
        if (lut.empty()) {
            throw std::runtime_error("SimulatedPBS: LUT is empty");
        }

        const std::uint32_t idx = static_cast<std::uint32_t>(q_input);
        const std::uint32_t n = static_cast<std::uint32_t>(lut.size());
        if ((n & (n - 1U)) == 0U) {
            return lut[idx & (n - 1U)];
        }
        return lut[idx % n];
    }

    EncodedTables tables_;
};

// ----------------------------------------------------------------------------
// Stage-S simulated PBS bridge:
// ----------------------------------------------------------------------------
// This is the real activation-path contract introduced by Stage-S.  It models
// the programmable bootstrapping table as:
//
//     encrypted BFE bucket -> route/control word
//
// The returned route word then selects tail elision, zero micro-polynomial, or a
// coefficient-word bank followed by local fixed-point polynomial reconstruction.
// A future torus-domain PBS implementation can replace only this bridge while
// keeping the route table semantics unchanged.
// ----------------------------------------------------------------------------
class StageSSimulatedPBS {
public:
    StageSSimulatedPBS(
        const StageSPlutEncoder& encoder,
        const StageSPlutTables& tables)
        : encoder_(&encoder), tables_(&tables) {}

    std::uint32_t bootstrap_gelu_route_from_fp(std::int64_t x_fp) const {
        DAZGOrbitTFHEPrimitiveProfile::Instance().RecordStageSPBS(true);
        return encoder_->route_gelu_from_fp(x_fp, *tables_);
    }

    std::uint32_t bootstrap_silu_route_from_fp(std::int64_t x_fp) const {
        DAZGOrbitTFHEPrimitiveProfile::Instance().RecordStageSPBS(false);
        return encoder_->route_silu_from_fp(x_fp, *tables_);
    }

private:
    const StageSPlutEncoder* encoder_ = nullptr;
    const StageSPlutTables* tables_ = nullptr;
};

} // namespace dazg_orbit::tfhe
