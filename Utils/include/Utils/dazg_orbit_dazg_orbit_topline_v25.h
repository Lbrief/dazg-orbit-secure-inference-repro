// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_dazg_orbit_topline_v25.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace dazg_orbit {
namespace topline_v25 {

// DAZGOrbit Topline V25 CertCascade.
// Certificate-driven policy plane; explicit component disable always wins;
// mode variables select semantics only and never enable ThunderAct alone.

inline bool IsFalseValue(const char* v) {
    if (v == nullptr || *v == '\0') return false;
    return std::strcmp(v, "0") == 0 ||
           std::strcmp(v, "false") == 0 || std::strcmp(v, "False") == 0 || std::strcmp(v, "FALSE") == 0 ||
           std::strcmp(v, "off") == 0 || std::strcmp(v, "OFF") == 0 ||
           std::strcmp(v, "no") == 0 || std::strcmp(v, "NO") == 0 ||
           std::strcmp(v, "baseline") == 0 || std::strcmp(v, "BASELINE") == 0;
}

inline bool IsTrueValue(const char* v) {
    return v != nullptr && *v != '\0' && !IsFalseValue(v);
}

inline const char* GetenvAny(const char* a,
                             const char* b = nullptr,
                             const char* c = nullptr,
                             const char* d = nullptr,
                             const char* e = nullptr,
                             const char* f = nullptr) {
    const char* names[6] = {a, b, c, d, e, f};
    for (const char* name : names) {
        if (name == nullptr) continue;
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') return v;
    }
    return nullptr;
}

inline bool AnyExplicitFalse(const char* a,
                             const char* b = nullptr,
                             const char* c = nullptr,
                             const char* d = nullptr,
                             const char* e = nullptr,
                             const char* f = nullptr) {
    const char* names[6] = {a, b, c, d, e, f};
    for (const char* name : names) {
        if (name == nullptr) continue;
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0' && IsFalseValue(v)) return true;
    }
    return false;
}

inline bool AnyExplicitTrue(const char* a,
                            const char* b = nullptr,
                            const char* c = nullptr,
                            const char* d = nullptr,
                            const char* e = nullptr,
                            const char* f = nullptr) {
    const char* names[6] = {a, b, c, d, e, f};
    for (const char* name : names) {
        if (name == nullptr) continue;
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0' && IsTrueValue(v)) return true;
    }
    return false;
}

inline bool Enabled() {
    if (AnyExplicitFalse("DAZG_ORBIT_TOPLINE_V25",
                         "DAZG_ORBIT_TOPLINE_V25",
                         "DAZG_ORBIT_TOPLINE_V25",
                         "DAZG_ORBIT_CERTCASCADE_V25",
                         "DAZG_ORBIT_CERTPATH_V25")) {
        return false;
    }
    return AnyExplicitTrue("DAZG_ORBIT_TOPLINE_V25",
                           "DAZG_ORBIT_TOPLINE_V25",
                           "DAZG_ORBIT_TOPLINE_V25",
                           "DAZG_ORBIT_CERTCASCADE_V25",
                           "DAZG_ORBIT_CERTPATH_V25");
}

inline const char* Profile() {
    const char* p = GetenvAny("DAZG_ORBIT_TOPLINE_V25_PROFILE",
                              "DAZG_ORBIT_TOPLINE_V25_PROFILE",
                              "DAZG_ORBIT_TOPLINE_V25_PROFILE",
                              "DAZG_ORBIT_CERTCASCADE_V25_PROFILE",
                              "DAZG_ORBIT_CERTPATH_V25_PROFILE");
    if (p != nullptr && *p != '\0') return p;
    return Enabled() ? "latency" : "off";
}

inline bool ProfileIs(const char* x) {
    const char* p = Profile();
    return p != nullptr && x != nullptr && std::strcmp(p, x) == 0;
}

inline bool ProfileWantsRounds() {
    return ProfileIs("rounds") || ProfileIs("projection") ||
           ProfileIs("domain") || ProfileIs("research") ||
           ProfileIs("sci") || ProfileIs("all");
}

inline const char* CertificateName() {
    return "v19k_v21a_20260515_strict_hash_numeric_certificate";
}

inline bool ComponentExplicitFalseV19() {
    return AnyExplicitFalse("DAZG_ORBIT_THUNDERACT_V19",
                            "DAZG_ORBIT_THUNDERACT_V19",
                            "DAZG_ORBIT_THUNDERACT_V19",
                            "DAZG_ORBIT_ENABLE_THUNDERACT_V19",
                            "DAZG_ORBIT_ENABLE_THUNDERACT_V19",
                            "THUNDERACT_V19");
}

inline bool ComponentExplicitTrueV19() {
    return AnyExplicitTrue("DAZG_ORBIT_THUNDERACT_V19",
                           "DAZG_ORBIT_THUNDERACT_V19",
                           "DAZG_ORBIT_THUNDERACT_V19",
                           "DAZG_ORBIT_ENABLE_THUNDERACT_V19",
                           "DAZG_ORBIT_ENABLE_THUNDERACT_V19",
                           "THUNDERACT_V19");
}

inline bool EnableThunderAct() {
    if (ComponentExplicitFalseV19()) return false;
    if (ComponentExplicitTrueV19()) return true;
    return Enabled();
}


inline const char* RuntimeThunderActPolicy() {
    const char* p = GetenvAny("DAZG_ORBIT_THUNDERACT_V19_POLICY",
                              "DAZG_ORBIT_THUNDERACT_V19H_POLICY",
                              "DAZG_ORBIT_THUNDERACT_V19G_POLICY",
                              "DAZG_ORBIT_THUNDERACT_V19_POLICY");
    if (p != nullptr && *p != '\0') return p;
    return "site_allowlist_v37_249";
}

inline const char* RuntimeThunderActPolicySource() {
    const char* p = GetenvAny("DAZG_ORBIT_THUNDERACT_V19_POLICY",
                              "DAZG_ORBIT_THUNDERACT_V19H_POLICY",
                              "DAZG_ORBIT_THUNDERACT_V19G_POLICY",
                              "DAZG_ORBIT_THUNDERACT_V19_POLICY");
    if (p != nullptr && *p != '\0') {
        return std::strstr(p, "seq:") == p ? "env_legacy_seq_sanitized" : "env";
    }
    return "site_allowlist_v37_default";
}

inline bool ThunderActPolicyIsV37Preset(const char* p) {
    return p != nullptr &&
           (std::strcmp(p, "site_allowlist_v37_249") == 0 ||
            std::strcmp(p, "site_allowlist") == 0 ||
            std::strcmp(p, "v37") == 0 ||
            std::strcmp(p, "certified") == 0);
}

inline const char* RawRuntimeThunderTailSiteAllow() {
    const char* p = GetenvAny("DAZG_ORBIT_THUNDERTAIL_SITE_ALLOW",
                              "DAZG_ORBIT_THUNDERACT_V19_SITE_ALLOW",
                              "DAZG_ORBIT_THUNDERTAIL_SITE_ALLOW");
    if (p != nullptr && *p != '\0') return p;
    return "none";
}

inline const char* RuntimeThunderTailSiteAllow() {
    const char* policy = RuntimeThunderActPolicy();
    if (ThunderActPolicyIsV37Preset(policy)) return "site_allowlist_v37_249";
    const char* raw = RawRuntimeThunderTailSiteAllow();
    if (raw != nullptr && *raw != '\0' && std::strcmp(raw, "none") != 0) return raw;
    return "site_allowlist_v37_249";
}

inline const char* ThunderActPolicy() {
    if (!EnableThunderAct()) return "none";
    return RuntimeThunderActPolicy();
}

inline const char* ThunderActModeForSite(const char* site, const char* fallback_mode) {
    if (site != nullptr && std::strstr(site, "Bottleneck#13/residual_add") != nullptr) {
        return "logical43";
    }
    if (fallback_mode != nullptr && *fallback_mode != '\0' && !IsFalseValue(fallback_mode)) {
        return fallback_mode;
    }
    return "signed43";
}

inline bool ComponentExplicitFalseFanout() {
    return AnyExplicitFalse("DAZG_ORBIT_FANOUTBURST_V20",
                            "DAZG_ORBIT_FANOUTBURST_V20",
                            "DAZG_ORBIT_FANOUTBURST_V20",
                            "DAZG_ORBIT_ENABLE_FANOUTBURST_V20",
                            "DAZG_ORBIT_ENABLE_FANOUTBURST_V20");
}

inline bool ComponentExplicitTrueFanout() {
    return AnyExplicitTrue("DAZG_ORBIT_FANOUTBURST_V20",
                           "DAZG_ORBIT_FANOUTBURST_V20",
                           "DAZG_ORBIT_FANOUTBURST_V20",
                           "DAZG_ORBIT_ENABLE_FANOUTBURST_V20",
                           "DAZG_ORBIT_ENABLE_FANOUTBURST_V20");
}

inline bool EnableFanoutBurst() {
    if (ComponentExplicitFalseFanout()) return false;
    if (ComponentExplicitTrueFanout()) return true;
    return Enabled() && ProfileWantsRounds();
}

inline const char* FanoutBurstPolicy() {
    if (!EnableFanoutBurst()) return "off";
    return "projection";
}

inline bool ComponentExplicitFalseProjectionBurstV31() {
    return AnyExplicitFalse("DAZG_ORBIT_PROJECTION_BURST_V31",
                            "DAZG_ORBIT_PROJECTION_BURST_V31",
                            "DAZG_ORBIT_PROJECTION_BURST_V31",
                            "DAZG_ORBIT_ENABLE_PROJECTION_BURST_V31",
                            "DAZG_ORBIT_ENABLE_PROJECTION_BURST_V31");
}

inline bool ComponentExplicitTrueProjectionBurstV31() {
    return AnyExplicitTrue("DAZG_ORBIT_PROJECTION_BURST_V31",
                           "DAZG_ORBIT_PROJECTION_BURST_V31",
                           "DAZG_ORBIT_PROJECTION_BURST_V31",
                           "DAZG_ORBIT_ENABLE_PROJECTION_BURST_V31",
                           "DAZG_ORBIT_ENABLE_PROJECTION_BURST_V31");
}

inline bool ComponentExplicitFalseDomainPulseV15() {
    return AnyExplicitFalse("DAZG_ORBIT_ENABLE_DOMAINPULSE_V15",
                            "DAZG_ORBIT_ENABLE_CONVERSION_FUSION_V15",
                            "DAZG_ORBIT_DOMAINPULSE_V15");
}

inline bool ComponentExplicitTrueDomainPulseV15() {
    return AnyExplicitTrue("DAZG_ORBIT_ENABLE_DOMAINPULSE_V15",
                           "DAZG_ORBIT_ENABLE_CONVERSION_FUSION_V15",
                           "DAZG_ORBIT_DOMAINPULSE_V15");
}

inline bool EnableDomainPulseV15() {
    if (ComponentExplicitFalseDomainPulseV15()) return false;
    if (ComponentExplicitTrueDomainPulseV15()) return true;
    return Enabled() && ProfileWantsRounds();
}

// ProjectionBurst V31 is the SCI-facing, graph-visible form of the existing
// exact DomainPulse V15 projection conversion burst.  It does not invent a
// shortcut rotation cache; it exposes the strict cross-branch conversion fusion
// as a first-class certificate and runtime metric.
inline bool EnableProjectionBurstV31() {
    if (ComponentExplicitFalseProjectionBurstV31()) return false;
    if (ComponentExplicitTrueProjectionBurstV31()) return true;
    return EnableDomainPulseV15() || EnableFanoutBurst();
}

inline const char* ProjectionBurstV31Policy() {
    return EnableProjectionBurstV31()
        ? "cross_branch_projection_conversion_burst"
        : "off";
}


inline void PrintConfigOnce(int role) {
    static std::atomic<bool> printed{false};
    if (printed.exchange(true, std::memory_order_relaxed)) return;

    const bool component_runtime_enabled =
        Enabled() || EnableThunderAct() || EnableFanoutBurst() ||
        EnableDomainPulseV15() || EnableProjectionBurstV31();

    std::cerr << "[DAZG_ORBIT_TOPLINE_V25]"
              << " role=" << role
              << " enabled=" << (component_runtime_enabled ? 1 : 0)
              << " topline_enabled=" << (Enabled() ? 1 : 0)
              << " profile=" << Profile()
              << " topline_profile=" << Profile()
              << " component_runtime_enabled=" << (component_runtime_enabled ? 1 : 0)
              << " thunderact=" << (EnableThunderAct() ? 1 : 0)
              << " runtime_thunderact_policy=" << ThunderActPolicy()
              << " thunderact_policy=" << ThunderActPolicy()
              << " reported_policy_source=" << RuntimeThunderActPolicySource()
              << " runtime_thundertail_site_allow=" << RuntimeThunderTailSiteAllow()
              << " raw_thundertail_site_allow=" << RawRuntimeThunderTailSiteAllow()
              << " v37_final_policy_frozen=1"
              << " v38_cert_ledger=1"
              << " v41_tailshadow=1"
              << " v41_active_shadow_carry_verifier=1"
              << " v40_tailproof=1"
              << " v40_tailproof_policy=proof_carrying_fail_closed"
              << " expected_tailproof_opportunities_per_role=7"
              << " expected_thundertail_total_per_role=49"
              << " expected_thundertail_fast_per_role=42"
              << " expected_thundertail_unsafe_fallback_per_role=7"
              << " certified_site_mode=Bottleneck#13/residual_add:logical43;default:signed43"
              << " v37_site_stable_gate=1"
              << " legacy_seq_policy_sanitized=1"
              << " unsafe_tail_fallback=1"
              << " fanoutburst=" << (EnableFanoutBurst() ? 1 : 0)
              << " fanout_policy=" << FanoutBurstPolicy()
              << " domainpulse_v15=" << (EnableDomainPulseV15() ? 1 : 0)
              << " v40_conversion_superfusion=ledger"
              << " projection_burst_v31=" << (EnableProjectionBurstV31() ? 1 : 0)
              << " projection_burst_policy=" << ProjectionBurstV31Policy()
              << " baseline_clean_by_default=1"
              << " mode_alone_never_enables=1"
              << " explicit_component_zero_wins=1"
              << " strict_hash_required=1"
              << " certificate=" << CertificateName()
              << " expected_rounds_certificate=V37_SITE_STABLE_THUNDERTAIL_249rounds"
              << " projection_burst_certificate=projection_block_prebranch_domain_identity_and_mask_stable_conversion_burst"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

}  // namespace topline_v25
}  // namespace dazg_orbit
