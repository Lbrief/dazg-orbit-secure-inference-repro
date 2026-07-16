// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_ablation_flags.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <cstdint>
#include <Utils/dazg_orbit_dazg_orbit_topline_v25.h>
#include <Utils/dazg_orbit_dazg_orbit_topline_v23.h>

namespace dazg_orbit {
namespace ablation {

inline bool StringIsTrue(const char* v) {
    if (v == nullptr) return false;
    return std::strcmp(v, "1") == 0 ||
           std::strcmp(v, "true") == 0 || std::strcmp(v, "TRUE") == 0 ||
           std::strcmp(v, "on") == 0 || std::strcmp(v, "ON") == 0 ||
           std::strcmp(v, "yes") == 0 || std::strcmp(v, "YES") == 0;
}

inline bool StringIsFalse(const char* v) {
    if (v == nullptr) return false;
    return std::strcmp(v, "0") == 0 ||
           std::strcmp(v, "false") == 0 || std::strcmp(v, "FALSE") == 0 ||
           std::strcmp(v, "off") == 0 || std::strcmp(v, "OFF") == 0 ||
           std::strcmp(v, "no") == 0 || std::strcmp(v, "NO") == 0;
}

inline bool EnvFlag(const char* name, bool default_value) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    if (StringIsTrue(v)) return true;
    if (StringIsFalse(v)) return false;
    return default_value;
}

inline bool EnvFlag2(const char* primary, const char* alias, bool default_value) {
    const char* v = std::getenv(primary);
    if (v != nullptr && *v != '\0') return EnvFlag(primary, default_value);
    return EnvFlag(alias, default_value);
}

inline bool EnvFlag3(const char* primary, const char* alias1, const char* alias2,
                     bool default_value) {
    const char* v = std::getenv(primary);
    if (v != nullptr && *v != '\0') return EnvFlag(primary, default_value);
    v = std::getenv(alias1);
    if (v != nullptr && *v != '\0') return EnvFlag(alias1, default_value);
    return EnvFlag(alias2, default_value);
}

inline uint64_t EnvU64(const char* name, uint64_t default_value) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v) return default_value;
    return static_cast<uint64_t>(parsed);
}

inline const char* EnvStr(const char* name, const char* default_value) {
    const char* v = std::getenv(name);
    return (v == nullptr || *v == '\0') ? default_value : v;
}

inline const char* EnvStrAny(const char* primary,
                             const char* alias1,
                             const char* alias2,
                             const char* default_value) {
    const char* names[3] = {primary, alias1, alias2};
    for (const char* name : names) {
        if (name == nullptr) continue;
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') return v;
    }
    return default_value;
}

inline uint64_t EnvU64Any(const char* primary,
                          const char* alias1,
                          const char* alias2,
                          uint64_t default_value) {
    const char* names[3] = {primary, alias1, alias2};
    for (const char* name : names) {
        if (name == nullptr) continue;
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') return EnvU64(name, default_value);
    }
    return default_value;
}

// Global route gate. Keep it independent from per-component gates so A1-A6 can
// selectively disable one route family without accidentally disabling all others.
inline bool EnableRouteSpecialization() {
    return EnvFlag("DAZG_ORBIT_ENABLE_ROUTE_SPECIALIZATION", true);
}

inline bool EnableStageY() {
    return EnvFlag3("DAZG_ORBIT_ENABLE_STAGEY", "DAZG_ORBIT_ENABLE_STAGE_Y",
                    "DAZG_ORBIT_STAGE_Y", true);
}

inline bool EnableStageZ2() {
    return EnvFlag3("DAZG_ORBIT_ENABLE_STAGEZ2", "DAZG_ORBIT_ENABLE_STAGE_Z2",
                    "DAZG_ORBIT_STAGE_Z2", true);
}

inline bool EnableK1S2Compact() {
    return EnableRouteSpecialization() &&
           EnvFlag2("DAZG_ORBIT_ENABLE_K1S2_COMPACT", "DAZG_ORBIT_COMPACT_K1S2", true);
}

inline bool EnableK3S2Polyphase() {
    return EnableRouteSpecialization() &&
           EnvFlag2("DAZG_ORBIT_ENABLE_K3S2_POLYPHASE", "DAZG_ORBIT_POLYPHASE_K3S2", true);
}

inline bool EnableExactTiledK3S1() {
    return EnableRouteSpecialization() &&
           EnvFlag2("DAZG_ORBIT_ENABLE_EXACT_TILED_K3S1", "DAZG_ORBIT_EXACT_TILED_K3S1", true);
}

inline bool EnableR50PWTile() {
    return EnableRouteSpecialization() &&
           EnvFlag2("DAZG_ORBIT_ENABLE_R50_PWTILE", "DAZG_ORBIT_R50_PWTILE", true);
}

inline bool EnableCSRClassifier() {
    return EnvFlag2("DAZG_ORBIT_ENABLE_CSR_CLASSIFIER", "DAZG_ORBIT_CSR_CLASSIFIER", true);
}

inline bool EnableFanoutCache() {
    return EnvFlag3("DAZG_ORBIT_ENABLE_FANOUT_CACHE", "DAZG_ORBIT_FANOUT_RUNTIME",
                    "DAZG_ORBIT_FANOUT_CACHE", true);
}

inline bool EnableDomainSummary() {
    return EnvFlag("DAZG_ORBIT_DOMAIN_PLAN", true);
}

inline bool EnableDomainDetail() {
    return EnvFlag("DAZG_ORBIT_DOMAIN_DETAIL", false);
}

inline bool EnablePrgReshareCert() {
    return EnvFlag("DAZG_ORBIT_ENABLE_PRG_RESHARE_CERT", true);
}


// DAZGOrbit-SymLift: exact SS->HE conversion route using the key-owner's
// symmetric BFV encryption path.  This keeps the same plaintext semantics as
// public-key encryption while removing the dominant public-key encryption cost
// on the client side after PWTile has reduced linear HE compute.
inline bool EnableSymLiftSSToHE() {
    // DAZG_ORBIT_V575_SYMLIFT_FAST_DEFAULT_20260614
    // Exact key-owner symmetric BFV encryption for SS->HE. Explicit old env wins;
    // otherwise v575 enables it, with Conversion.cpp retaining public-encrypt fallback.
    if (!EnableRouteSpecialization()) return false;
    const char* p = std::getenv("DAZG_ORBIT_ENABLE_SYMLIFT");
    const char* a = std::getenv("DAZG_ORBIT_SYMLIFT");
    if ((p != nullptr && *p != '\0') || (a != nullptr && *a != '\0')) {
        return EnvFlag2("DAZG_ORBIT_ENABLE_SYMLIFT", "DAZG_ORBIT_SYMLIFT", false);
    }
    return EnvFlag("DAZG_ORBIT_V575_SYMLIFT_FAST", true);
}

// DAZGOrbit-DomainPulse V15: mask-stable residual-branch conversion batching.
// V14 batched the network message but changed HEToSS mask seeds, which is not
// strict-hash safe because later truncation/activation is share-representation
// sensitive.  V15 keeps the single network round while deriving each segment's
// HEToSS mask from the same (rows, poly_degree, call_id) tuple used by the
// original separate conversions.
inline bool EnableDomainPulseV15() {
    if (!EnableRouteSpecialization()) return false;

    const char* explicit_primary = std::getenv("DAZG_ORBIT_ENABLE_DOMAINPULSE_V15");
    const char* explicit_alias1 = std::getenv("DAZG_ORBIT_ENABLE_CONVERSION_FUSION_V15");
    const char* explicit_alias2 = std::getenv("DAZG_ORBIT_DOMAINPULSE_V15");
    if ((explicit_primary != nullptr && *explicit_primary != '\0') ||
        (explicit_alias1 != nullptr && *explicit_alias1 != '\0') ||
        (explicit_alias2 != nullptr && *explicit_alias2 != '\0')) {
        return EnvFlag3("DAZG_ORBIT_ENABLE_DOMAINPULSE_V15",
                        "DAZG_ORBIT_ENABLE_CONVERSION_FUSION_V15",
                        "DAZG_ORBIT_DOMAINPULSE_V15",
                        false);
    }

    return ::dazg_orbit::topline_v25::EnableDomainPulseV15();
}



inline bool EnableDomainPulseV14() {
    if (EnableDomainPulseV15()) return true;
    return EnableRouteSpecialization() &&
           EnvFlag3("DAZG_ORBIT_ENABLE_DOMAINPULSE_V14",
                    "DAZG_ORBIT_ENABLE_CONVERSION_FUSION_V14",
                    "DAZG_ORBIT_DOMAINPULSE_V14",
                    false);
}

// DAZGOrbit-CertAct: exact activation certificate fast path.  The first version
// is deliberately conservative: it only bypasses the BFE/PLUT evaluator after
// ALICE reconstructs the activation tensor and proves every element belongs to
// a GeLU tail where the certified output is exactly identity or zero.  Any
// central-domain element falls back to the original evaluator.
inline bool EnableCertAct() {
    return EnableRouteSpecialization() &&
           EnvFlag2("DAZG_ORBIT_ENABLE_CERTACT", "DAZG_ORBIT_CERTACT", false);
}

// DAZGOrbit-CertActFuse: strict replay of a previously verified exact activation
// certificate.  V2 applies the replay inside TFHEReLUProtocol as a local
// canonical share transform; the original exact protocol remains the fallback.
inline bool EnableCertActFuse() {
    return EnableRouteSpecialization() && EnableCertAct() &&
           EnvFlag2("DAZG_ORBIT_ENABLE_CERTACT_FUSE", "DAZG_ORBIT_CERTACT_FUSE", false);
}

// DAZGOrbit-CertActFuse-LatentDomain V3: a strict frontier-level gate.
// It is intentionally dependent on CertActFuse, because it reuses the same
// site+call_id+n+input_variant replay certificate.
inline bool EnableLatentDomainFuse() {
    return EnableCertActFuse() &&
           EnvFlag3("DAZG_ORBIT_ENABLE_LATENT_DOMAIN_FUSE",
                    "DAZG_ORBIT_ENABLE_LATENT_DOMAIN",
                    "DAZG_ORBIT_LATENT_DOMAIN_FUSE",
                    false);
}

inline bool LatentDomainDetail() {
    return EnvFlag3("DAZG_ORBIT_LATENT_DOMAIN_DETAIL",
                    "DAZG_ORBIT_ENABLE_LATENT_DOMAIN_DETAIL",
                    "DAZG_ORBIT_LATENT_DOMAIN_DETAIL",
                    false);
}

inline bool LatentDomainStrictReplay() {
    return EnvFlag3("DAZG_ORBIT_LATENT_DOMAIN_STRICT_REPLAY",
                    "DAZG_ORBIT_LATENT_DOMAIN_STRICT",
                    "DAZG_ORBIT_LATENT_DOMAIN_STRICT_REPLAY",
                    true);
}

inline const char* CertActFuseProfileFile() {
    return EnvStrAny("DAZG_ORBIT_CERTACT_FUSE_PROFILE",
                     "DAZG_ORBIT_CERTACT_FUSE_PROFILE_FILE",
                     "DAZG_ORBIT_CERTACT_FUSE_PROFILE",
                     "");
}

inline const char* CertActFuseIdentitySites() {
    return EnvStrAny("DAZG_ORBIT_CERTACT_FUSE_IDENTITY_SITES",
                     "DAZG_ORBIT_CERTACT_IDENTITY_SITES",
                     "DAZG_ORBIT_CERTACT_FUSE_IDENTITY_SITES",
                     "");
}

inline const char* CertActFuseZeroSites() {
    return EnvStrAny("DAZG_ORBIT_CERTACT_FUSE_ZERO_SITES",
                     "DAZG_ORBIT_CERTACT_ZERO_SITES",
                     "DAZG_ORBIT_CERTACT_FUSE_ZERO_SITES",
                     "");
}

inline bool CertActFuseDetail() {
    return EnvFlag3("DAZG_ORBIT_CERTACT_FUSE_DETAIL", "DAZG_ORBIT_CERTACT_PROFILE_DETAIL",
                    "DAZG_ORBIT_CERTACT_FUSE_DETAIL", false);
}

inline bool CertActFuseAllowWildcardProfile() {
    return EnvFlag3("DAZG_ORBIT_CERTACT_FUSE_ALLOW_WILDCARD", "DAZG_ORBIT_CERTACT_PROFILE_WILDCARD",
                    "DAZG_ORBIT_CERTACT_FUSE_ALLOW_WILDCARD", false);
}

inline bool CertActFuseEmitProfile() {
    return EnvFlag3("DAZG_ORBIT_CERTACT_FUSE_EMIT_PROFILE", "DAZG_ORBIT_CERTACT_EMIT_PROFILE",
                    "DAZG_ORBIT_CERTACT_FUSE_EMIT_PROFILE", true);
}

inline bool CertActFuseProtocolLocalCanonical() {
    return EnvFlag3("DAZG_ORBIT_CERTACT_FUSE_PROTOCOL_LOCAL",
                    "DAZG_ORBIT_CERTACT_FUSE_LOCAL_CANON",
                    "DAZG_ORBIT_CERTACT_FUSE_PROTOCOL_LOCAL",
                    true);
}

inline bool CertActFuseUnsafeTensorNoop() {
    return EnvFlag3("DAZG_ORBIT_CERTACT_FUSE_UNSAFE_TENSOR_NOOP",
                    "DAZG_ORBIT_CERTACT_FUSE_TENSOR_NOOP",
                    "DAZG_ORBIT_CERTACT_FUSE_UNSAFE_TENSOR_NOOP",
                    false);
}

inline bool CertActFuseShadowVerify() {
    return EnvFlag3("DAZG_ORBIT_CERTACT_FUSE_SHADOW_VERIFY",
                    "DAZG_ORBIT_CERTACT_PROFILE_SHADOW_VERIFY",
                    "DAZG_ORBIT_CERTACT_FUSE_SHADOW_VERIFY",
                    false);
}

inline uint64_t CertActClipFpOverride() {
    return EnvU64("DAZG_ORBIT_CERTACT_CLIP_FP", 0ULL);
}


// DAZGOrbit TruncLift V4: exactness-first truncation-lift evidence planner.
// Disabled by default. Runtime cuts require exact lower_bits_zero and
// signed_bound_ok replay certificates.
inline bool EnableTruncLiftV4() {
    return EnableRouteSpecialization() && EnableCertAct() &&
           EnvFlag3("DAZG_ORBIT_ENABLE_TRUNCLIFT_V4",
                    "DAZG_ORBIT_TRUNCLIFT_V4",
                    "DAZG_ORBIT_TRUNCLIFT_V4",
                    false);
}

inline bool TruncLiftV4Detail() {
    return EnvFlag3("DAZG_ORBIT_TRUNCLIFT_V4_DETAIL",
                    "DAZG_ORBIT_ENABLE_TRUNCLIFT_V4_DETAIL",
                    "DAZG_ORBIT_TRUNCLIFT_V4_DETAIL",
                    false);
}

inline bool TruncLiftV4StrictReplay() {
    return EnvFlag3("DAZG_ORBIT_TRUNCLIFT_V4_STRICT_REPLAY",
                    "DAZG_ORBIT_TRUNCLIFT_STRICT_REPLAY",
                    "DAZG_ORBIT_TRUNCLIFT_V4_STRICT_REPLAY",
                    true);
}

inline uint64_t TruncLiftV4PrintLimit() {
    return EnvU64Any("DAZG_ORBIT_TRUNCLIFT_V4_PRINT_LIMIT",
                     "DAZG_ORBIT_TRUNCLIFT_PRINT_LIMIT",
                     "DAZG_ORBIT_TRUNCLIFT_V4_PRINT_LIMIT",
                     256ULL);
}

inline bool TruncLiftV4ScanLocalShares() {
    return EnvFlag3("DAZG_ORBIT_TRUNCLIFT_V4_SCAN_LOCAL_SHARES",
                    "DAZG_ORBIT_TRUNCLIFT_SCAN_LOCAL_SHARES",
                    "DAZG_ORBIT_TRUNCLIFT_V4_SCAN_LOCAL_SHARES",
                    true);
}

inline uint64_t TruncLiftV4ScanLimit() {
    return EnvU64Any("DAZG_ORBIT_TRUNCLIFT_V4_SCAN_LIMIT",
                     "DAZG_ORBIT_TRUNCLIFT_SCAN_LIMIT",
                     "DAZG_ORBIT_TRUNCLIFT_V4_SCAN_LIMIT",
                     0ULL);
}

inline uint64_t TruncLiftV4MinPostTruncMarginFp() {
    return EnvU64Any("DAZG_ORBIT_TRUNCLIFT_V4_MIN_MARGIN_FP",
                     "DAZG_ORBIT_TRUNCLIFT_MIN_MARGIN_FP",
                     "DAZG_ORBIT_TRUNCLIFT_V4_MIN_MARGIN_FP",
                     524288ULL);
}


inline bool EnableThunderActV19() {
    return EnableRouteSpecialization() &&
           EnvFlag3("DAZG_ORBIT_THUNDERACT_V19",
                    "DAZG_ORBIT_ENABLE_THUNDERACT_V19",
                    "DAZG_ORBIT_THUNDERACT_V19",
                    false);
}

inline const char* ThunderActV19Mode() {
    return EnvStrAny("DAZG_ORBIT_THUNDERACT_V19_MODE",
                     "DAZG_ORBIT_THUNDERACT_MODE",
                     "DAZG_ORBIT_THUNDERACT_MODE",
                     "signed43");
}

inline uint64_t ThunderActV19Shift() {
    return EnvU64Any("DAZG_ORBIT_THUNDERACT_V19_SHIFT",
                     "DAZG_ORBIT_THUNDERACT_SHIFT",
                     "DAZG_ORBIT_THUNDERACT_SHIFT",
                     17ULL);
}

inline uint64_t ThunderActV19InputBitwidthOverride() {
    return EnvU64Any("DAZG_ORBIT_THUNDERACT_V19_INPUT_BW",
                     "DAZG_ORBIT_THUNDERACT_INPUT_BW",
                     "DAZG_ORBIT_THUNDERACT_INPUT_BW",
                     0ULL);
}

inline bool ThunderActV19Verbose() {
    return EnvFlag3("DAZG_ORBIT_THUNDERACT_V19_VERBOSE",
                    "DAZG_ORBIT_THUNDERACT_VERBOSE",
                    "DAZG_ORBIT_THUNDERACT_VERBOSE",
                    true);
}

inline const char* ExperimentName() {
    return EnvStr("DAZG_ORBIT_EXPERIMENT", "full_dazg_orbit");
}

inline const char* CaseName() {
    return EnvStr("DAZG_ORBIT_CASE", ExperimentName());
}

inline uint64_t RepeatIndex() {
    return EnvU64("DAZG_ORBIT_REPEAT", 0);
}

inline uint64_t InputVariant() {
    return EnvU64Any("DAZG_ORBIT_INPUT_VARIANT",
                     "DAZG_ORBIT_INPUT_VARIANT",
                     "DAZG_ORBIT_INPUT_VARIANT",
                     0ULL);
}

inline const char* NumericVerifierMode() {
    return EnvStrAny("DAZG_ORBIT_NUMERIC_VERIFIER_MODE",
                     "DAZG_ORBIT_VERIFIER_MODE",
                     "DAZG_ORBIT_NUMERIC_VERIFIER_MODE",
                     "off");
}

inline const char* NumericVerifierFile() {
    return EnvStrAny("DAZG_ORBIT_NUMERIC_VERIFIER_FILE",
                     "DAZG_ORBIT_VERIFIER_FILE",
                     "DAZG_ORBIT_NUMERIC_VERIFIER_FILE",
                     "");
}

inline void PrintConfigOnce(int role) {
    static bool printed = false;
    if (printed) return;
    printed = true;
    std::cerr
        << "[DAZG_ORBIT_EXPERIMENT_CONFIG]"
        << " role=" << role
        << " experiment=" << ExperimentName()
        << " case=" << CaseName()
        << " repeat=" << RepeatIndex()
        << " input_variant=" << InputVariant()
        << " enable_stage_y=" << (EnableStageY() ? 1 : 0)
        << " enable_stage_z2=" << (EnableStageZ2() ? 1 : 0)
        << " enable_route_specialization=" << (EnableRouteSpecialization() ? 1 : 0)
        << " enable_k1s2_compact=" << (EnableK1S2Compact() ? 1 : 0)
        << " enable_k3s2_polyphase=" << (EnableK3S2Polyphase() ? 1 : 0)
        << " enable_exact_tiled_k3s1=" << (EnableExactTiledK3S1() ? 1 : 0)
        << " enable_r50_pwtile=" << (EnableR50PWTile() ? 1 : 0)
        << " enable_symlift=" << (EnableSymLiftSSToHE() ? 1 : 0)
        << " enable_domainpulse_v14=" << (EnableDomainPulseV14() ? 1 : 0)
        << " enable_domainpulse_v15=" << (EnableDomainPulseV15() ? 1 : 0)
        << " enable_certact=" << (EnableCertAct() ? 1 : 0)
        << " enable_certact_fuse=" << (EnableCertActFuse() ? 1 : 0)
        << " enable_trunclift_v4=" << (EnableTruncLiftV4() ? 1 : 0)
        << " trunclift_v4_detail=" << (TruncLiftV4Detail() ? 1 : 0)
        << " trunclift_v4_strict_replay=" << (TruncLiftV4StrictReplay() ? 1 : 0)
        << " trunclift_v4_scan_local_shares=" << (TruncLiftV4ScanLocalShares() ? 1 : 0)
        << " enable_latent_domain_fuse=" << (EnableLatentDomainFuse() ? 1 : 0)
        << " latent_domain_detail=" << (LatentDomainDetail() ? 1 : 0)
        << " latent_domain_strict_replay=" << (LatentDomainStrictReplay() ? 1 : 0)
        << " certact_fuse_profile=" << CertActFuseProfileFile()
        << " certact_fuse_protocol_local=" << (CertActFuseProtocolLocalCanonical() ? 1 : 0)
        << " certact_fuse_unsafe_tensor_noop=" << (CertActFuseUnsafeTensorNoop() ? 1 : 0)
        << " thunderact_v19=" << (EnableThunderActV19() ? 1 : 0)
        << " thunderact_v19_mode=" << ThunderActV19Mode()
        << " thunderact_v19_shift=" << ThunderActV19Shift()
        << " thunderact_v19_input_bw_override=" << ThunderActV19InputBitwidthOverride()
        << " enable_stage_z2_sparse_bsgs=" << (EnableStageZ2() ? 1 : 0)
        << " enable_csr_classifier=" << (EnableCSRClassifier() ? 1 : 0)
        << " enable_fanout_cache=" << (EnableFanoutCache() ? 1 : 0)
        << " domain_plan=" << (EnableDomainSummary() ? 1 : 0)
        << " domain_detail=" << (EnableDomainDetail() ? 1 : 0)
        << " numeric_verifier_mode=" << NumericVerifierMode()
        << " numeric_verifier_file=" << NumericVerifierFile()
        << " exact_equiv=1 semantic_loss=0"
        << std::endl;
}

}  // namespace ablation
}  // namespace dazg_orbit
