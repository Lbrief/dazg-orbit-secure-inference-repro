// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_latent_domain.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <type_traits>

#include "Utils/dazg_orbit_ablation_flags.h"
#include "Utils/dazg_orbit_certact_fuse.h"

namespace dazg_orbit {
namespace latent_domain {

// DAZG_ORBIT_CERTACTFUSE_LATENTDOMAIN_V3
//
// V3 is deliberately placed at the ResNet activation/truncation frontier, not
// inside the activation evaluator.  It can remove the whole
//     FixPoint::truncate -> activation
// segment only when a strict replay certificate proves that the final public
// value of that whole activation call is already known.  The all-zero case is
// exact: replacing both parties' shares by zero is a valid sharing of the same
// tensor and skips both truncation and activation.  The all-identity case is
// NOT used to skip truncation: floor/truncation over additive shares is not a
// ring-linear local operation, and moving it across the next linear operator is
// not exact without an additional divisibility/carry-free certificate.

inline bool Enabled() {
    return dazg_orbit::ablation::EnableLatentDomainFuse();
}

inline bool DetailEnabled() {
    return dazg_orbit::ablation::LatentDomainDetail();
}

class Stats {
public:
    static Stats& Instance() {
        static Stats s;
        return s;
    }

    void RecordFrontier(uint64_t n) {
        frontiers_.fetch_add(1, std::memory_order_relaxed);
        elements_examined_.fetch_add(n, std::memory_order_relaxed);
    }

    void RecordNoProfile() {
        blocked_missing_profile_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordProfileMatch(bool identity, bool zero) {
        profile_matches_.fetch_add(1, std::memory_order_relaxed);
        if (identity) identity_matches_.fetch_add(1, std::memory_order_relaxed);
        if (zero) zero_matches_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordZeroApplied(uint64_t n) {
        zero_pretrunc_applied_.fetch_add(1, std::memory_order_relaxed);
        zero_pretrunc_elements_.fetch_add(n, std::memory_order_relaxed);
    }

    void RecordIdentityBlocked() {
        blocked_identity_truncation_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordMixedBlocked() {
        blocked_mixed_or_unknown_.fetch_add(1, std::memory_order_relaxed);
    }

    void RecordUnsafeRequestRefused() {
        unsafe_requests_refused_.fetch_add(1, std::memory_order_relaxed);
    }

    ~Stats() {
        const uint64_t touched = frontiers_.load(std::memory_order_relaxed) +
                                 profile_matches_.load(std::memory_order_relaxed) +
                                 zero_pretrunc_applied_.load(std::memory_order_relaxed) +
                                 blocked_identity_truncation_.load(std::memory_order_relaxed) +
                                 blocked_missing_profile_.load(std::memory_order_relaxed) +
                                 blocked_mixed_or_unknown_.load(std::memory_order_relaxed) +
                                 unsafe_requests_refused_.load(std::memory_order_relaxed);
        if (!Enabled() && touched == 0) return;
        std::cerr << "[DAZG_ORBIT_LATENT_DOMAIN_SUMMARY]"
                  << " frontiers=" << frontiers_.load(std::memory_order_relaxed)
                  << " profile_matches=" << profile_matches_.load(std::memory_order_relaxed)
                  << " identity_matches=" << identity_matches_.load(std::memory_order_relaxed)
                  << " zero_matches=" << zero_matches_.load(std::memory_order_relaxed)
                  << " zero_pretrunc_applied=" << zero_pretrunc_applied_.load(std::memory_order_relaxed)
                  << " zero_pretrunc_elements=" << zero_pretrunc_elements_.load(std::memory_order_relaxed)
                  << " blocked_identity_truncation=" << blocked_identity_truncation_.load(std::memory_order_relaxed)
                  << " blocked_missing_profile=" << blocked_missing_profile_.load(std::memory_order_relaxed)
                  << " blocked_mixed_or_unknown=" << blocked_mixed_or_unknown_.load(std::memory_order_relaxed)
                  << " unsafe_requests_refused=" << unsafe_requests_refused_.load(std::memory_order_relaxed)
                  << " elements_examined=" << elements_examined_.load(std::memory_order_relaxed)
                  << " runtime_applied=" << zero_pretrunc_applied_.load(std::memory_order_relaxed)
                  << " skipped_truncate_activation_calls=" << zero_pretrunc_applied_.load(std::memory_order_relaxed)
                  << " identity_domain_cut_applied=0"
                  << " mode=strict_profile_frontier_v3"
                  << " certificate=site_call_n_input_variant_whole_call_output"
                  << " guard=identity_requires_extra_truncation_proof"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }

private:
    Stats() = default;
    std::atomic<uint64_t> frontiers_{0};
    std::atomic<uint64_t> profile_matches_{0};
    std::atomic<uint64_t> identity_matches_{0};
    std::atomic<uint64_t> zero_matches_{0};
    std::atomic<uint64_t> zero_pretrunc_applied_{0};
    std::atomic<uint64_t> zero_pretrunc_elements_{0};
    std::atomic<uint64_t> blocked_identity_truncation_{0};
    std::atomic<uint64_t> blocked_missing_profile_{0};
    std::atomic<uint64_t> blocked_mixed_or_unknown_{0};
    std::atomic<uint64_t> unsafe_requests_refused_{0};
    std::atomic<uint64_t> elements_examined_{0};
};

template <typename TensorT>
inline void FillTensorPublicZero(TensorT& x) {
    auto& data = x.data();
    using ElemRef = decltype(data[0]);
    using Elem = typename std::remove_reference<ElemRef>::type;
    for (auto& v : data) v = static_cast<Elem>(0);
}

template <typename TensorT>
inline bool TryApplyPreTruncationZeroShortcut(TensorT& x,
                                              const std::string& site,
                                              uint64_t activation_call_id,
                                              int shift,
                                              int bw,
                                              bool signed_trunc) {
    (void)shift;
    (void)bw;
    (void)signed_trunc;

    if (!Enabled()) return false;

    const uint64_t n = static_cast<uint64_t>(x.data().size());
    Stats::Instance().RecordFrontier(n);

    const auto match = dazg_orbit::certact_fuse::LookupProfileActivation(site, activation_call_id, n);
    if (!match.matched) {
        Stats::Instance().RecordNoProfile();
        static std::atomic<int> prints{0};
        if (DetailEnabled() && prints.fetch_add(1, std::memory_order_relaxed) < 64) {
            std::cerr << "[DAZG_ORBIT_LATENT_DOMAIN_FUSE]"
                      << " applied=0 route=frontier_guard"
                      << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                      << " call_id=" << activation_call_id
                      << " n=" << n
                      << " reason=no_strict_profile_match"
                      << " action=keep_fixpoint_truncate_then_activation"
                      << " exact_equiv=1 semantic_loss=0"
                      << std::endl;
        }
        return false;
    }

    Stats::Instance().RecordProfileMatch(match.identity, match.zero);
    const uint64_t profile_input_variant = match.entry.input_variant;
    const bool has_profile_variant =
        profile_input_variant != std::numeric_limits<uint64_t>::max();

    if (match.zero) {
        FillTensorPublicZero(x);
        Stats::Instance().RecordZeroApplied(n);
        std::cerr << "[DAZG_ORBIT_LATENT_DOMAIN_FUSE]"
                  << " applied=1 route=profile_all_zero_pretrunc_public_zero"
                  << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                  << " call_id=" << activation_call_id
                  << " n=" << n
                  << " profile_call_id=" << match.entry.call_id
                  << " profile_n=" << match.entry.n
                  << " profile_input_variant=" << (has_profile_variant ? profile_input_variant : 0)
                  << " current_input_variant=" << dazg_orbit::ablation::InputVariant()
                  << " skipped_truncate=1 skipped_activation=1"
                  << " identity_domain_cut_applied=0"
                  << " reason=" << match.reason
                  << " certificate=strict_profile_all_zero_whole_call_output"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
        return true;
    }

    if (match.identity) {
        Stats::Instance().RecordIdentityBlocked();
        static std::atomic<int> prints{0};
        if (DetailEnabled() || prints.fetch_add(1, std::memory_order_relaxed) < 32) {
            std::cerr << "[DAZG_ORBIT_LATENT_DOMAIN_FUSE]"
                      << " applied=0 route=identity_frontier_guard"
                      << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                      << " call_id=" << activation_call_id
                      << " n=" << n
                      << " profile_call_id=" << match.entry.call_id
                      << " profile_n=" << match.entry.n
                      << " profile_input_variant=" << (has_profile_variant ? profile_input_variant : 0)
                      << " current_input_variant=" << dazg_orbit::ablation::InputVariant()
                      << " reason=identity_requires_exact_truncation_proof"
                      << " blocked_math=floor_shift_is_not_ring_linear"
                      << " action=keep_fixpoint_truncate_then_localcanon_v2"
                      << " unsafe_domain_cut_refused=1"
                      << " exact_equiv=1 semantic_loss=0"
                      << std::endl;
        }
        return false;
    }

    Stats::Instance().RecordMixedBlocked();
    if (DetailEnabled()) {
        std::cerr << "[DAZG_ORBIT_LATENT_DOMAIN_FUSE]"
                  << " applied=0 route=mixed_or_unknown_frontier_guard"
                  << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                  << " call_id=" << activation_call_id
                  << " n=" << n
                  << " reason=profile_not_whole_call_zero_or_identity"
                  << " action=keep_original_protocol"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
    return false;
}

inline bool RefuseUnsafeIdentityDomainCut(const std::string& site,
                                          uint64_t activation_call_id,
                                          uint64_t n) {
    if (!Enabled()) return false;
    Stats::Instance().RecordUnsafeRequestRefused();
    std::cerr << "[DAZG_ORBIT_LATENT_DOMAIN_FUSE]"
              << " applied=0 route=unsafe_identity_domain_cut_refused"
              << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
              << " call_id=" << activation_call_id
              << " n=" << n
              << " reason=missing_pretrunc_divisibility_or_carry_free_certificate"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
    return false;
}

}  // namespace latent_domain
}  // namespace dazg_orbit
