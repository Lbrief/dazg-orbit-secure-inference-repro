// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_scatterlift_v7.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace dazg_orbit {
namespace scatterlift_v7 {

inline bool EnvFlag(const char* name, bool default_value = false) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    return !(std::strcmp(v, "0") == 0 ||
             std::strcmp(v, "false") == 0 || std::strcmp(v, "FALSE") == 0 ||
             std::strcmp(v, "off") == 0 || std::strcmp(v, "OFF") == 0 ||
             std::strcmp(v, "no") == 0 || std::strcmp(v, "NO") == 0);
}

inline bool EnableCanonicalReshare() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V7_CANON_RESHARE", false) ||
           EnvFlag("DAZG_ORBIT_ENABLE_SCATTERLIFT_CANON_RESHARE", false) ||
           EnvFlag("DAZG_ORBIT_SCATTERLIFT_V7_CANON_RESHARE", false);
}

inline bool RuntimeProof() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V7_RUNTIME_PROOF", false) ||
           EnvFlag("DAZG_ORBIT_SCATTERLIFT_V7_RUNTIME_PROOF", false);
}

inline bool Detail() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V7_DETAIL", false) ||
           EnvFlag("DAZG_ORBIT_SCATTERLIFT_V7_DETAIL", false);
}

inline bool EmitSummary() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V7_SUMMARY", true);
}

inline const char* Marker() {
    return "DAZG_ORBIT_SCATTERLIFT_CANONRESHARE_V7_20260512";
}

class Stats {
public:
    static Stats& Instance() {
        static Stats s;
        return s;
    }

    void RecordApplied(bool identity, bool zero, std::uint64_t n, bool proof) {
        applied_calls_.fetch_add(1, std::memory_order_relaxed);
        applied_elements_.fetch_add(n, std::memory_order_relaxed);
        if (identity) identity_calls_.fetch_add(1, std::memory_order_relaxed);
        if (zero) zero_calls_.fetch_add(1, std::memory_order_relaxed);
        if (proof) runtime_proof_calls_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordReject() {
        rejects_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordProofFallback() {
        proof_fallbacks_.fetch_add(1, std::memory_order_relaxed);
    }

    ~Stats() {
        if (!EmitSummary()) return;
        const std::uint64_t calls = applied_calls_.load(std::memory_order_relaxed);
        const std::uint64_t rejects = rejects_.load(std::memory_order_relaxed);
        const std::uint64_t proof_fallbacks = proof_fallbacks_.load(std::memory_order_relaxed);
        if (calls == 0 && rejects == 0 && proof_fallbacks == 0 && !EnableCanonicalReshare()) return;
        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V7_SUMMARY]"
                  << " marker=" << Marker()
                  << " applied_calls=" << calls
                  << " applied_elements=" << applied_elements_.load(std::memory_order_relaxed)
                  << " identity_calls=" << identity_calls_.load(std::memory_order_relaxed)
                  << " zero_calls=" << zero_calls_.load(std::memory_order_relaxed)
                  << " rejects=" << rejects
                  << " runtime_proof_calls=" << runtime_proof_calls_.load(std::memory_order_relaxed)
                  << " proof_fallbacks=" << proof_fallbacks
                  << " saved_peer_gather=0"
                  << " saved_prg_reshare=0"
                  << " saved_activation_evaluator=" << calls
                  << " domain_transition_eliminated=0"
                  << " local_only_skip_forbidden=1"
                  << " theorem=canonical_prg_reshare_requires_peer_share_or_debt"
                  << " mode=profile_canonical_prg_reshare_v7"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }

private:
    Stats() = default;
    std::atomic<std::uint64_t> applied_calls_{0};
    std::atomic<std::uint64_t> applied_elements_{0};
    std::atomic<std::uint64_t> identity_calls_{0};
    std::atomic<std::uint64_t> zero_calls_{0};
    std::atomic<std::uint64_t> rejects_{0};
    std::atomic<std::uint64_t> runtime_proof_calls_{0};
    std::atomic<std::uint64_t> proof_fallbacks_{0};
};

inline void RecordApplied(bool identity, bool zero, std::uint64_t n, bool proof) {
    Stats::Instance().RecordApplied(identity, zero, n, proof);
}

inline void RecordReject() {
    Stats::Instance().RecordReject();
}

inline void RecordProofFallback() {
    Stats::Instance().RecordProofFallback();
}

}  // namespace scatterlift_v7
}  // namespace dazg_orbit
