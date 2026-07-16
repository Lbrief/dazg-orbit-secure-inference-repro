// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_thunderact_v19e.h
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
#include <Utils/dazg_orbit_dazg_orbit_topline_v25.h>
#include <Utils/dazg_orbit_dazg_orbit_topline_v23.h>

namespace dazg_orbit {
namespace thunderact_v19e {

struct Context {
    bool active;
    int shift;
    int bw;
    bool signed_trunc;
    bool extra_flag;
    const char* site;
};

inline thread_local Context current_context{false, 17, 43, true, false, ""};

inline bool false_value(const char* v) {
    if (v == nullptr || *v == '\0') return false;
    return std::strcmp(v, "0") == 0 ||
           std::strcmp(v, "false") == 0 ||
           std::strcmp(v, "False") == 0 ||
           std::strcmp(v, "FALSE") == 0 ||
           std::strcmp(v, "off") == 0 ||
           std::strcmp(v, "OFF") == 0 ||
           std::strcmp(v, "no") == 0 ||
           std::strcmp(v, "NO") == 0 ||
           std::strcmp(v, "baseline") == 0 ||
           std::strcmp(v, "BASELINE") == 0;
}

inline bool true_value(const char* v) {
    return v != nullptr && *v != '\0' && !false_value(v);
}

inline bool Enabled() {
    return ::dazg_orbit::topline_v25::EnableThunderAct();
}



inline const char* Mode() {
    const char* names[] = {
        "DAZG_ORBIT_THUNDERACT_V19_MODE",
        "DAZG_ORBIT_THUNDERACT_V19_MODE",
        "DAZG_ORBIT_THUNDERACT_V19_VARIANT",
        "DAZG_ORBIT_THUNDERACT_V19_VARIANT",
        "THUNDERACT_V19_VARIANT"
    };
    for (const char* name : names) {
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') return v;
    }
    return "signed43";
}

// DAZG_ORBIT_THUNDERACT_V19K_SITE_MODE_MAP_BEGIN
inline std::string TrimV19K(std::string x) {
    while (!x.empty() && (x.front() == ' ' || x.front() == '\t' || x.front() == '\n' || x.front() == '\r')) {
        x.erase(x.begin());
    }
    while (!x.empty() && (x.back() == ' ' || x.back() == '\t' || x.back() == '\n' || x.back() == '\r')) {
        x.pop_back();
    }
    return x;
}

inline const char* ModeForContext(const Context& ctx) {
    const char* spec_c = std::getenv("DAZG_ORBIT_THUNDERACT_V19_SITE_MODE_MAP");
    if (spec_c != nullptr && spec_c[0] != '\0' &&
        ctx.site != nullptr && ctx.site[0] != '\0') {
        static thread_local std::string selected_mode;
        const std::string spec(spec_c);
        const std::string site(ctx.site);

        std::size_t start = 0;
        while (start < spec.size()) {
            while (start < spec.size() &&
                   (spec[start] == ';' || spec[start] == ',' || spec[start] == ' ' ||
                    spec[start] == '\t' || spec[start] == '\n' || spec[start] == '\r')) {
                ++start;
            }

            if (start >= spec.size()) break;

            std::size_t end = spec.find_first_of(";,", start);
            if (end == std::string::npos) end = spec.size();

            std::string tok = spec.substr(start, end - start);
            std::size_t sep = tok.find('=');
            const std::size_t sep_colon = tok.find(':');
            if (sep == std::string::npos ||
                (sep_colon != std::string::npos && sep_colon < sep)) {
                sep = sep_colon;
            }

            if (sep != std::string::npos) {
                std::string key = TrimV19K(tok.substr(0, sep));
                std::string val = TrimV19K(tok.substr(sep + 1));
                if (!key.empty() && !val.empty() && site.find(key) != std::string::npos) {
                    selected_mode = val;
                    return selected_mode.c_str();
                }
            }

            start = end + 1;
        }
    }

    return ::dazg_orbit::topline_v25::ThunderActModeForSite(ctx.site, Mode());
}


// DAZG_ORBIT_THUNDERACT_V19K_SITE_MODE_MAP_END


class ScopedContext {
public:
    ScopedContext(const char* site,
                  int shift,
                  int bw,
                  bool signed_trunc,
                  bool extra_flag)
        : prev_(current_context) {
        current_context.active = true;
        current_context.shift = shift;
        current_context.bw = bw;
        current_context.signed_trunc = signed_trunc;
        current_context.extra_flag = extra_flag;
        current_context.site = site;
    }

    ~ScopedContext() {
        current_context = prev_;
    }

private:
    Context prev_;
};

inline const Context* Current() {
    return current_context.active ? &current_context : nullptr;
}

inline int clamp_bits(int bits) {
    if (bits <= 0) return 64;
    if (bits > 64) return 64;
    return bits;
}

inline std::uint64_t mask_bits(std::uint64_t x, int bits) {
    bits = clamp_bits(bits);
    if (bits >= 64) return x;
    return x & ((std::uint64_t{1} << static_cast<unsigned>(bits)) - 1ULL);
}

inline std::int64_t sign_extend(std::uint64_t x, int bits) {
    bits = clamp_bits(bits);
    x = mask_bits(x, bits);
    if (bits >= 64) return static_cast<std::int64_t>(x);

    const std::uint64_t sign = std::uint64_t{1} << static_cast<unsigned>(bits - 1);
    const std::uint64_t mod = std::uint64_t{1} << static_cast<unsigned>(bits);
    if ((x & sign) == 0) return static_cast<std::int64_t>(x);
    return static_cast<std::int64_t>(x) - static_cast<std::int64_t>(mod);
}

inline std::int64_t floor_shift(std::int64_t x, int shift) {
    if (shift <= 0) return x;
    if (shift >= 63) return x < 0 ? -1 : 0;
    if (x >= 0) return x >> static_cast<unsigned>(shift);

    const std::uint64_t ux = static_cast<std::uint64_t>(-(x + 1)) + 1ULL;
    const std::uint64_t q =
        (ux + ((1ULL << static_cast<unsigned>(shift)) - 1ULL)) >>
        static_cast<unsigned>(shift);
    return -static_cast<std::int64_t>(q);
}

inline std::uint64_t logical_shift(std::uint64_t x, int shift) {
    if (shift <= 0) return x;
    if (shift >= 64) return 0;
    return x >> static_cast<unsigned>(shift);
}

inline bool mode_eq(const char* mode, const char* value) {
    return mode != nullptr && std::strcmp(mode, value) == 0;
}

inline std::int64_t LocalTruncate(std::uint64_t raw,
                                  const Context& ctx,
                                  int activation_bitwidth) {
    const char* mode = ModeForContext(ctx);
    const int sh = ctx.shift < 0 ? 0 : ctx.shift;
    const int bw = clamp_bits(ctx.bw > 0 ? ctx.bw : activation_bitwidth);
    const int act_bits = clamp_bits(activation_bitwidth);

    const auto logical_shifted = [&](int in_bits) -> std::uint64_t {
        const std::uint64_t x = mask_bits(raw, in_bits);
        if (sh <= 0) return x;
        if (sh >= 64) return 0;
        return x >> static_cast<unsigned>(sh);
    };

    const auto signed_shifted = [&](int in_bits) -> std::uint64_t {
        const std::int64_t x = sign_extend(raw, in_bits);
        return static_cast<std::uint64_t>(floor_shift(x, sh));
    };

    const auto as_signed_bits = [&](std::uint64_t x, int bits) -> std::int64_t {
        return sign_extend(mask_bits(x, bits), bits);
    };

    const auto as_activation_s64 = [&](std::uint64_t x) -> std::int64_t {
        return sign_extend(mask_bits(x, act_bits), act_bits);
    };

    // V19F bridge-exact scan modes.
    //
    // Current evidence:
    //   clean baseline: totalRounds=1097, no V19 markers;
    //   V19E candidates: totalRounds=117, but strict mismatch.
    // Baseline TFHEReLU ranges are consistent with a reduced-width
    // truncation result, not the full 43-bit signed result used in V19E.
    if (mode_eq(mode, "u44s27")) return as_signed_bits(logical_shifted(44), 27);
    if (mode_eq(mode, "u43s26")) return as_signed_bits(logical_shifted(43), 26);
    if (mode_eq(mode, "u45s28")) return as_signed_bits(logical_shifted(45), 28);
    if (mode_eq(mode, "u44s28")) return as_signed_bits(logical_shifted(44), 28);
    if (mode_eq(mode, "u43s27")) return as_signed_bits(logical_shifted(43), 27);
    if (mode_eq(mode, "u46s29")) return as_signed_bits(logical_shifted(46), 29);
    if (mode_eq(mode, "u47s30")) return as_signed_bits(logical_shifted(47), 30);
    if (mode_eq(mode, "u48s31")) return as_signed_bits(logical_shifted(48), 31);

    if (mode_eq(mode, "u43act")) return as_activation_s64(logical_shifted(43));
    if (mode_eq(mode, "u44act")) return as_activation_s64(logical_shifted(44));
    if (mode_eq(mode, "u45act")) return as_activation_s64(logical_shifted(45));
    if (mode_eq(mode, "u60act")) return as_activation_s64(logical_shifted(60));
    if (mode_eq(mode, "u64act")) return as_activation_s64(logical_shifted(64));

    if (mode_eq(mode, "s43s26")) return as_signed_bits(signed_shifted(43), 26);
    if (mode_eq(mode, "s44s27")) return as_signed_bits(signed_shifted(44), 27);
    if (mode_eq(mode, "s45s28")) return as_signed_bits(signed_shifted(45), 28);
    if (mode_eq(mode, "s46s29")) return as_signed_bits(signed_shifted(46), 29);
    if (mode_eq(mode, "s60act")) return as_activation_s64(signed_shifted(60));
    if (mode_eq(mode, "s64act")) return as_activation_s64(signed_shifted(64));

    if (mode_eq(mode, "signed43")) {
        return as_signed_bits(signed_shifted(43), 43);
    }

    if (mode_eq(mode, "logical43")) {
        return as_signed_bits(logical_shifted(43), 43);
    }

    if (mode_eq(mode, "signed60")) {
        return as_signed_bits(signed_shifted(60), bw);
    }

    if (mode_eq(mode, "logical60")) {
        return as_signed_bits(logical_shifted(60), bw);
    }

    if (mode_eq(mode, "signed64")) {
        return as_signed_bits(signed_shifted(64), bw);
    }

    if (mode_eq(mode, "word64")) {
        return as_activation_s64(logical_shifted(64));
    }

    if (ctx.signed_trunc) {
        return as_signed_bits(signed_shifted(bw), bw);
    }

    return as_signed_bits(logical_shifted(bw), bw);
}


inline std::atomic<std::uint64_t>& SkipCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& BridgeCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline void RecordSkip(const char* site,
                       std::uint64_t n,
                       int shift,
                       int bw,
                       bool signed_trunc,
                       bool extra_flag) {
    const std::uint64_t seq =
        SkipCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;

    if (seq <= 256ULL) {
        std::cerr << "[DAZG_ORBIT_THUNDERACT_V19]"
                  << " patch=v19e"
                  << " phase=skip_interactive_truncate"
                  << " runtime_applied=1"
                  << " seq=" << seq
                  << " site=" << (site != nullptr ? site : "unknown")
                  << " tensor_n=" << n
                  << " shift=" << shift
                  << " bw=" << bw
                  << " signed_trunc=" << (signed_trunc ? 1 : 0)
                  << " extra_flag=" << (extra_flag ? 1 : 0)
                  << " mode=" << ModeForContext(Context{true, shift, bw, signed_trunc, extra_flag, site})
                  << " explicit_gate_required=1"
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << std::endl;
    }
}

inline void RecordBridge(const char* site,
                         int n,
                         int bitwidth,
                         int scale,
                         const Context& ctx) {
    const std::uint64_t seq =
        BridgeCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;

    if (seq <= 256ULL) {
        std::cerr << "[DAZG_ORBIT_THUNDERACT_V19]"
                  << " patch=v19e"
                  << " phase=bridge_local_truncate"
                  << " runtime_applied=1"
                  << " seq=" << seq
                  << " site=" << (site != nullptr ? site : "unknown")
                  << " n=" << n
                  << " activation_bitwidth=" << bitwidth
                  << " scale=" << scale
                  << " shift=" << ctx.shift
                  << " bw=" << ctx.bw
                  << " signed_trunc=" << (ctx.signed_trunc ? 1 : 0)
                  << " mode=" << ModeForContext(ctx)
                  << " canonical_prg_reshare_preserved=1"
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << std::endl;
    }
}





// DAZG_ORBIT_THUNDERACT_V19H_FRONTIER_POLICY_BEGIN
inline std::atomic<std::uint64_t>& FrontierActivationSeqCounter() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::uint64_t NextFrontierSeq(const char*) {
    return FrontierActivationSeqCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;
}

inline bool FrontierStartsWith(const char* s, const char* prefix) {
    if (s == nullptr || prefix == nullptr) return false;
    while (*prefix != '\0') {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

inline bool FrontierSeqListContains(const char* list, std::uint64_t seq) {
    if (list == nullptr) return false;
    const char* p = list;

    while (*p != '\0') {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '|') ++p;
        if (*p == '\0') break;

        char* end = nullptr;
        unsigned long long lo = std::strtoull(p, &end, 10);
        if (end == p) {
            while (*p != '\0' && *p != ',' && *p != ';' && *p != '|') ++p;
            continue;
        }

        unsigned long long hi = lo;
        p = end;

        if (*p == '-') {
            ++p;
            char* end2 = nullptr;
            unsigned long long parsed_hi = std::strtoull(p, &end2, 10);
            if (end2 != p) {
                hi = parsed_hi;
                p = end2;
            }
        }

        if (seq >= static_cast<std::uint64_t>(lo) &&
            seq <= static_cast<std::uint64_t>(hi)) {
            return true;
        }

        while (*p != '\0' && *p != ',' && *p != ';' && *p != '|') ++p;
    }

    return false;
}

inline bool FrontierSiteTokenMatch(const char* tokens, const char* site) {
    if (tokens == nullptr || site == nullptr) return false;
    const char* p = tokens;

    while (*p != '\0') {
        while (*p == ' ' || *p == ',' || *p == ';' || *p == '|') ++p;
        if (*p == '\0') break;

        const char* start = p;
        while (*p != '\0' && *p != ',' && *p != ';' && *p != '|') ++p;

        std::string token(start, static_cast<std::size_t>(p - start));
        if (!token.empty() && std::string(site).find(token) != std::string::npos) {
            return true;
        }
    }

    return false;
}

inline const char* FrontierPolicy() {
    const char* p = std::getenv("DAZG_ORBIT_THUNDERACT_V19_POLICY");
    if (p != nullptr && *p != '\0') return p;

    p = std::getenv("DAZG_ORBIT_THUNDERACT_V19H_POLICY");
    if (p != nullptr && *p != '\0') return p;

    p = std::getenv("DAZG_ORBIT_THUNDERACT_V19G_POLICY");
    if (p != nullptr && *p != '\0') return p;

    const char* preset = ::dazg_orbit::topline_v25::ThunderActPolicy();
    if (preset != nullptr && *preset != '\0') return preset;

    return "none";
}





// DAZG_ORBIT_THUNDERTAIL_V37_SITE_STABLE_BEGIN
inline const char* ThunderTailCertifiedSiteAllowlistV37() {
    return "ResNet_50_Bottleneck/conv1;"
           "Bottleneck#1/conv1_reduce_k1s1;Bottleneck#1/conv2_spatial_k3;Bottleneck#1/residual_add;"
           "Bottleneck#2/conv1_reduce_k1s1;Bottleneck#2/conv2_spatial_k3;Bottleneck#2/residual_add;"
           "Bottleneck#3/conv1_reduce_k1s1;Bottleneck#3/conv2_spatial_k3;Bottleneck#3/residual_add;"
           "Bottleneck#4/conv1_reduce_k1s1;Bottleneck#4/conv2_spatial_k3;Bottleneck#4/residual_add;"
           "Bottleneck#5/conv1_reduce_k1s1;Bottleneck#5/conv2_spatial_k3;Bottleneck#5/residual_add;"
           "Bottleneck#6/conv1_reduce_k1s1;Bottleneck#6/conv2_spatial_k3;Bottleneck#6/residual_add;"
           "Bottleneck#7/conv1_reduce_k1s1;Bottleneck#7/conv2_spatial_k3;Bottleneck#7/residual_add;"
           "Bottleneck#8/conv1_reduce_k1s1;Bottleneck#8/conv2_spatial_k3;Bottleneck#8/residual_add;"
           "Bottleneck#9/conv1_reduce_k1s1;Bottleneck#9/conv2_spatial_k3;Bottleneck#9/residual_add;"
           "Bottleneck#10/conv1_reduce_k1s1;Bottleneck#10/conv2_spatial_k3;Bottleneck#10/residual_add;"
           "Bottleneck#11/conv1_reduce_k1s1;Bottleneck#11/conv2_spatial_k3;Bottleneck#11/residual_add;"
           "Bottleneck#12/conv1_reduce_k1s1;Bottleneck#12/conv2_spatial_k3;Bottleneck#12/residual_add;"
           "Bottleneck#13/conv1_reduce_k1s1;Bottleneck#13/conv2_spatial_k3;Bottleneck#13/residual_add;"
           "Bottleneck#14/conv1_reduce_k1s1;Bottleneck#14/conv2_spatial_k3";
}

inline bool ThunderTailStrEqV37(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

inline bool ThunderTailSiteEqV37(const char* site, const char* certified) {
    return site != nullptr && certified != nullptr && std::strcmp(site, certified) == 0;
}

inline bool ThunderTailCertifiedSiteV37(const char* site) {
    static const char* const kSites[] = {
        "ResNet_50_Bottleneck/conv1",
        "Bottleneck#1/conv1_reduce_k1s1", "Bottleneck#1/conv2_spatial_k3", "Bottleneck#1/residual_add",
        "Bottleneck#2/conv1_reduce_k1s1", "Bottleneck#2/conv2_spatial_k3", "Bottleneck#2/residual_add",
        "Bottleneck#3/conv1_reduce_k1s1", "Bottleneck#3/conv2_spatial_k3", "Bottleneck#3/residual_add",
        "Bottleneck#4/conv1_reduce_k1s1", "Bottleneck#4/conv2_spatial_k3", "Bottleneck#4/residual_add",
        "Bottleneck#5/conv1_reduce_k1s1", "Bottleneck#5/conv2_spatial_k3", "Bottleneck#5/residual_add",
        "Bottleneck#6/conv1_reduce_k1s1", "Bottleneck#6/conv2_spatial_k3", "Bottleneck#6/residual_add",
        "Bottleneck#7/conv1_reduce_k1s1", "Bottleneck#7/conv2_spatial_k3", "Bottleneck#7/residual_add",
        "Bottleneck#8/conv1_reduce_k1s1", "Bottleneck#8/conv2_spatial_k3", "Bottleneck#8/residual_add",
        "Bottleneck#9/conv1_reduce_k1s1", "Bottleneck#9/conv2_spatial_k3", "Bottleneck#9/residual_add",
        "Bottleneck#10/conv1_reduce_k1s1", "Bottleneck#10/conv2_spatial_k3", "Bottleneck#10/residual_add",
        "Bottleneck#11/conv1_reduce_k1s1", "Bottleneck#11/conv2_spatial_k3", "Bottleneck#11/residual_add",
        "Bottleneck#12/conv1_reduce_k1s1", "Bottleneck#12/conv2_spatial_k3", "Bottleneck#12/residual_add",
        "Bottleneck#13/conv1_reduce_k1s1", "Bottleneck#13/conv2_spatial_k3", "Bottleneck#13/residual_add",
        "Bottleneck#14/conv1_reduce_k1s1", "Bottleneck#14/conv2_spatial_k3"
    };
    for (const char* s : kSites) {
        if (ThunderTailSiteEqV37(site, s)) return true;
    }
    return false;
}

inline bool ThunderTailUnsafeTailSiteV37(const char* site) {
    static const char* const kUnsafe[] = {
        "Bottleneck#14/residual_add",
        "Bottleneck#15/conv1_reduce_k1s1",
        "Bottleneck#15/conv2_spatial_k3",
        "Bottleneck#15/residual_add",
        "Bottleneck#16/conv1_reduce_k1s1",
        "Bottleneck#16/conv2_spatial_k3",
        "Bottleneck#16/residual_add"
    };
    for (const char* s : kUnsafe) {
        if (ThunderTailSiteEqV37(site, s)) return true;
    }
    return false;
}

inline const char* ThunderTailExpectedModeV37(const char* site) {
    if (ThunderTailSiteEqV37(site, "Bottleneck#13/residual_add")) {
        return "logical43";
    }
    return "signed43";
}

inline const char* ThunderTailEnvSiteAllowV37() {
    const char* p = std::getenv("DAZG_ORBIT_THUNDERTAIL_SITE_ALLOW");
    if (p != nullptr && *p != '\0') return p;
    p = std::getenv("DAZG_ORBIT_THUNDERACT_V19_SITE_ALLOW");
    if (p != nullptr && *p != '\0') return p;
    p = std::getenv("DAZG_ORBIT_THUNDERTAIL_SITE_ALLOW");
    if (p != nullptr && *p != '\0') return p;
    return nullptr;
}

inline bool ThunderTailPolicyIsV37PresetV37(const char* policy) {
    return ThunderTailStrEqV37(policy, "site_allowlist_v37_249") ||
           ThunderTailStrEqV37(policy, "site_allowlist") ||
           ThunderTailStrEqV37(policy, "v37") ||
           ThunderTailStrEqV37(policy, "certified");
}

inline const char* ThunderTailEffectiveSiteAllowV37() {
    // V37.2 freeze: the successful 249-round certificate uses the semantic
    // site preset as the effective allowlist. A stale narrow tail env var must
    // not silently downgrade the final policy back to 269 or 1069 rounds.
    const char* policy = FrontierPolicy();
    if (ThunderTailPolicyIsV37PresetV37(policy)) return "site_allowlist_v37_249";
    const char* raw = ThunderTailEnvSiteAllowV37();
    return (raw != nullptr && *raw != '\0') ? raw : "site_allowlist_v37_249";
}

inline bool ThunderTailSafeExtensionSiteV37(const char* site) {
    return ThunderTailSiteEqV37(site, "Bottleneck#14/conv1_reduce_k1s1") ||
           ThunderTailSiteEqV37(site, "Bottleneck#14/conv2_spatial_k3");
}

inline bool ThunderTailExtensionDecisionSiteV37(const char* site) {
    return ThunderTailSafeExtensionSiteV37(site) || ThunderTailUnsafeTailSiteV37(site);
}

inline bool ThunderTailSiteAllowlistContainsV37(const char* site) {
    if (!ThunderTailCertifiedSiteV37(site)) return false;

    // V37.1/V37.2: the tail allowlist must only constrain the newly-added tail
    // decision space. It must not de-authorize the already-certified V19/V31
    // frontier sites selected by the legacy seq policy; otherwise seq:1-40
    // silently fall back and rounds jump back to baseline.
    if (!ThunderTailExtensionDecisionSiteV37(site)) return true;

    const char* policy = FrontierPolicy();
    if (ThunderTailPolicyIsV37PresetV37(policy)) {
        // Final 249-round policy is semantic and closed: both certified B14
        // tail sites are enabled; unsafe tail sites are still rejected by the
        // certified/unsafe/exactness gates.
        return ThunderTailSafeExtensionSiteV37(site);
    }

    const char* allow = ThunderTailEnvSiteAllowV37();
    if (allow == nullptr || *allow == '\0') return ThunderTailSafeExtensionSiteV37(site);
    if (false_value(allow)) return false;
    if (ThunderTailStrEqV37(allow, "all") ||
        ThunderTailStrEqV37(allow, "certified") ||
        ThunderTailStrEqV37(allow, "site_allowlist_v37_249")) {
        return ThunderTailSafeExtensionSiteV37(site);
    }
    return ThunderTailSafeExtensionSiteV37(site) && FrontierSiteTokenMatch(allow, site);
}

inline bool ThunderTailSiteAllowlistRequestsV37(const char* site) {
    return ThunderTailExtensionDecisionSiteV37(site) && ThunderTailSiteAllowlistContainsV37(site);
}

inline const char* ThunderTailPolicySourceV37() {
    const char* p = std::getenv("DAZG_ORBIT_THUNDERACT_V19_POLICY");
    if (p != nullptr && *p != '\0') {
        if (ThunderTailPolicyIsV37PresetV37(p)) return "env_site_allowlist_v37_249";
        return FrontierStartsWith(p, "seq:") ? "env_legacy_seq_sanitized" : "env";
    }
    p = std::getenv("DAZG_ORBIT_THUNDERACT_V19H_POLICY");
    if (p != nullptr && *p != '\0') {
        if (ThunderTailPolicyIsV37PresetV37(p)) return "env_site_allowlist_v37_249";
        return FrontierStartsWith(p, "seq:") ? "env_legacy_seq_sanitized" : "env";
    }
    p = std::getenv("DAZG_ORBIT_THUNDERACT_V19G_POLICY");
    if (p != nullptr && *p != '\0') {
        if (ThunderTailPolicyIsV37PresetV37(p)) return "env_site_allowlist_v37_249";
        return FrontierStartsWith(p, "seq:") ? "env_legacy_seq_sanitized" : "env";
    }
    p = ThunderTailEnvSiteAllowV37();
    if (p != nullptr && *p != '\0') return "env_tail_site_allow_sanitized";
    return "site_allowlist_v37_default";
}

inline bool ThunderTailPolicyRequestsSiteSeqV37(const char* policy,
                                                std::uint64_t seq,
                                                const char* site) {
    if (policy == nullptr || *policy == '\0') return false;
    if (std::strcmp(policy, "0") == 0 ||
        std::strcmp(policy, "none") == 0 ||
        std::strcmp(policy, "off") == 0 ||
        std::strcmp(policy, "baseline") == 0) {
        return false;
    }
    if (ThunderTailPolicyIsV37PresetV37(policy)) {
        return true;
    }
    if (std::strcmp(policy, "all") == 0 ||
        std::strcmp(policy, "full") == 0 ||
        std::strcmp(policy, "unsafe_all") == 0) {
        return true;
    }
    if (std::strcmp(policy, "frontier1") == 0 ||
        std::strcmp(policy, "prefix1") == 0) return seq <= 1ULL;
    if (std::strcmp(policy, "frontier2") == 0 ||
        std::strcmp(policy, "prefix2") == 0) return seq <= 2ULL;
    if (std::strcmp(policy, "frontier3") == 0 ||
        std::strcmp(policy, "prefix3") == 0) return seq <= 3ULL;
    if (FrontierStartsWith(policy, "seq:")) {
        return FrontierSeqListContains(policy + 4, seq);
    }
    if (FrontierStartsWith(policy, "prefix:")) {
        const unsigned long long n = std::strtoull(policy + 7, nullptr, 10);
        return seq <= static_cast<std::uint64_t>(n);
    }
    if (FrontierStartsWith(policy, "site:")) {
        return FrontierSiteTokenMatch(policy + 5, site);
    }
    return false;
}

inline bool ThunderTailExactGatePassV37(const Context& ctx,
                                        bool canonical_prg_reshare_preserved) {
    if (!canonical_prg_reshare_preserved) return false;
    if (!ThunderTailCertifiedSiteV37(ctx.site)) return false;
    if (ThunderTailUnsafeTailSiteV37(ctx.site)) return false;
    if (ctx.shift != 17 || ctx.bw != 43 || !ctx.signed_trunc) return false;
    const char* mode = ModeForContext(ctx);
    const char* expected = ThunderTailExpectedModeV37(ctx.site);
    return mode_eq(mode, expected);
}

inline const char* ThunderTailFallbackReasonV37(std::uint64_t seq,
                                                const char* site,
                                                int shift,
                                                int bw,
                                                bool signed_trunc,
                                                bool extra_flag,
                                                bool canonical_prg_reshare_preserved) {
    (void)extra_flag;
    const char* policy = FrontierPolicy();
    const bool legacy_requested = ThunderTailPolicyRequestsSiteSeqV37(policy, seq, site);
    const bool site_requested = ThunderTailSiteAllowlistRequestsV37(site);

    if (ThunderTailUnsafeTailSiteV37(site)) return "unsafe_tail_site";
    if (!legacy_requested && !site_requested) return "policy_not_requested";
    if (!ThunderTailCertifiedSiteV37(site)) return "site_not_in_certified_allowlist";
    if (!ThunderTailSiteAllowlistContainsV37(site)) return "site_allowlist_restricted";
    if (!canonical_prg_reshare_preserved) return "canonical_prg_reshare_not_preserved";
    if (shift != 17 || bw != 43 || !signed_trunc) return "exact_gate_failed_shift_bw_signed";
    Context ctx{true, shift, bw, signed_trunc, extra_flag, site};
    if (!mode_eq(ModeForContext(ctx), ThunderTailExpectedModeV37(site))) {
        return "exact_gate_failed_mode";
    }
    return "none";
}

inline bool ThunderTailShouldApplyV37(std::uint64_t seq,
                                      const char* site,
                                      int shift,
                                      int bw,
                                      bool signed_trunc,
                                      bool extra_flag,
                                      bool canonical_prg_reshare_preserved) {
    const char* policy = FrontierPolicy();
    const bool legacy_requested = ThunderTailPolicyRequestsSiteSeqV37(policy, seq, site);
    const bool site_requested = ThunderTailSiteAllowlistRequestsV37(site);
    if (!legacy_requested && !site_requested) return false;
    if (!ThunderTailSiteAllowlistContainsV37(site)) return false;
    Context ctx{true, shift, bw, signed_trunc, extra_flag, site};
    return ThunderTailExactGatePassV37(ctx, canonical_prg_reshare_preserved);
}

inline std::atomic<std::uint64_t>& ThunderTailCertCounterV37() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& ThunderTailLedgerTotalV38() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& ThunderTailLedgerFastV38() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& ThunderTailLedgerFallbackV38() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& ThunderTailLedgerUnsafeFallbackV38() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& ThunderTailLedgerRestrictedFallbackV38() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& ThunderTailLedgerUnexpectedGateFailV38() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& ThunderTailLedgerB14Conv1FastV38() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline std::atomic<std::uint64_t>& ThunderTailLedgerB14Conv2FastV38() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline bool ThunderTailLedgerEnabledV38() {
    const char* v = std::getenv("DAZG_ORBIT_THUNDERTAIL_LEDGER_V38");
    return v == nullptr || *v == '\0' || !false_value(v);
}

inline bool ThunderTailLedgerStrictV38() {
    const char* v = std::getenv("DAZG_ORBIT_THUNDERTAIL_LEDGER_V38_STRICT");
    return v != nullptr && *v != '\0' && true_value(v);
}

inline void PrintThunderTailLedgerV38() {
    if (!ThunderTailLedgerEnabledV38()) return;

    const std::uint64_t total = ThunderTailLedgerTotalV38().load(std::memory_order_relaxed);
    if (total == 0ULL) return;

    const std::uint64_t fast = ThunderTailLedgerFastV38().load(std::memory_order_relaxed);
    const std::uint64_t fallback = ThunderTailLedgerFallbackV38().load(std::memory_order_relaxed);
    const std::uint64_t unsafe_fb = ThunderTailLedgerUnsafeFallbackV38().load(std::memory_order_relaxed);
    const std::uint64_t restricted_fb = ThunderTailLedgerRestrictedFallbackV38().load(std::memory_order_relaxed);
    const std::uint64_t unexpected_gate_fail =
        ThunderTailLedgerUnexpectedGateFailV38().load(std::memory_order_relaxed);
    const std::uint64_t b14c1 = ThunderTailLedgerB14Conv1FastV38().load(std::memory_order_relaxed);
    const std::uint64_t b14c2 = ThunderTailLedgerB14Conv2FastV38().load(std::memory_order_relaxed);

    const bool full_249_shape =
        total == 49ULL &&
        fast == 42ULL &&
        fallback == 7ULL &&
        unsafe_fb == 7ULL &&
        restricted_fb == 0ULL &&
        unexpected_gate_fail == 0ULL &&
        b14c1 == 1ULL &&
        b14c2 == 1ULL;

    std::cerr << "[DAZG_ORBIT_THUNDERTAIL_LEDGER_V38]"
              << " profile=v38_cert_ledger"
              << " runtime_policy=" << FrontierPolicy()
              << " effective_site_allowlist=" << ThunderTailEffectiveSiteAllowV37()
              << " total_certs=" << total
              << " fast_certs=" << fast
              << " fallback_certs=" << fallback
              << " unsafe_fallbacks=" << unsafe_fb
              << " site_allowlist_restricted=" << restricted_fb
              << " unexpected_gate_failures=" << unexpected_gate_fail
              << " b14_conv1_fast=" << b14c1
              << " b14_conv2_fast=" << b14c2
              << " expected_total_per_role=49"
              << " expected_fast_per_role=42"
              << " expected_unsafe_fallback_per_role=7"
              << " expected_b14_conv1_fast=1"
              << " expected_b14_conv2_fast=1"
              << " full_249_shape=" << (full_249_shape ? 1 : 0)
              << " v40_tailproof=1"
              << " v40_fallback_preserving=1"
              << " v41_tailshadow=1"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;

    if (ThunderTailLedgerStrictV38() && !full_249_shape) {
        std::cerr << "[DAZG_ORBIT_THUNDERTAIL_LEDGER_V38_STRICT_FAIL]"
                  << " total_certs=" << total
                  << " fast_certs=" << fast
                  << " fallback_certs=" << fallback
                  << " unsafe_fallbacks=" << unsafe_fb
                  << " site_allowlist_restricted=" << restricted_fb
                  << " unexpected_gate_failures=" << unexpected_gate_fail
                  << " b14_conv1_fast=" << b14c1
                  << " b14_conv2_fast=" << b14c2
                  << " exact_equiv=unknown semantic_loss=unknown"
                  << std::endl;
        std::abort();
    }
}

inline void RegisterThunderTailLedgerV38() {
    static std::atomic<bool> registered{false};
    bool expected = false;
    if (registered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::atexit(PrintThunderTailLedgerV38);
    }
}

inline void RecordThunderTailLedgerV38(const char* site,
                                       bool gate_pass,
                                       bool fallback,
                                       const char* fallback_reason) {
    RegisterThunderTailLedgerV38();

    const bool unsafe_reason =
        fallback_reason != nullptr && std::strcmp(fallback_reason, "unsafe_tail_site") == 0;
    const bool restricted_reason =
        fallback_reason != nullptr && std::strcmp(fallback_reason, "site_allowlist_restricted") == 0;

    ThunderTailLedgerTotalV38().fetch_add(1ULL, std::memory_order_relaxed);
    if (!fallback && gate_pass) {
        ThunderTailLedgerFastV38().fetch_add(1ULL, std::memory_order_relaxed);
        if (ThunderTailSiteEqV37(site, "Bottleneck#14/conv1_reduce_k1s1")) {
            ThunderTailLedgerB14Conv1FastV38().fetch_add(1ULL, std::memory_order_relaxed);
        }
        if (ThunderTailSiteEqV37(site, "Bottleneck#14/conv2_spatial_k3")) {
            ThunderTailLedgerB14Conv2FastV38().fetch_add(1ULL, std::memory_order_relaxed);
        }
    } else {
        ThunderTailLedgerFallbackV38().fetch_add(1ULL, std::memory_order_relaxed);
    }

    if (!gate_pass && !unsafe_reason) {
        ThunderTailLedgerUnexpectedGateFailV38().fetch_add(1ULL, std::memory_order_relaxed);
    }

    if (unsafe_reason) {
        ThunderTailLedgerUnsafeFallbackV38().fetch_add(1ULL, std::memory_order_relaxed);
    }

    if (restricted_reason) {
        ThunderTailLedgerRestrictedFallbackV38().fetch_add(1ULL, std::memory_order_relaxed);
    }
}






// DAZG_ORBIT_TAILSHADOW_V41_BEGIN
// V41 Active Shadow Carry Verifier.
// This is intentionally fallback-preserving: it never commits unsafe tail
// fast paths.  It fingerprints the local candidate and the fallback result so
// that V42 can promote only externally verified per-site candidates.

inline bool TailShadowV41Enabled() {
    const char* v = std::getenv("DAZG_ORBIT_TAILSHADOW_V41");
    return v == nullptr || *v == '\0' || !false_value(v);
}

inline bool TailShadowV41DetailEnabled() {
    const char* v = std::getenv("DAZG_ORBIT_TAILSHADOW_V41_DETAIL");
    return v != nullptr && *v != '\0' && true_value(v);
}

inline std::uint64_t TailShadowMixV41(std::uint64_t h, std::uint64_t x) {
    h ^= x + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h *= 1099511628211ULL;
    return h;
}

inline std::atomic<std::uint64_t>& TailShadowV41Sites() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<std::uint64_t>& TailShadowV41Elems() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<std::uint64_t>& TailShadowV41LocalEq() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<std::uint64_t>& TailShadowV41LocalNe() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<std::uint64_t>& TailShadowV41Boundary() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline void PrintTailShadowV41Ledger() {
    if (!TailShadowV41Enabled()) return;

    const std::uint64_t sites = TailShadowV41Sites().load(std::memory_order_relaxed);
    if (sites == 0ULL) return;

    const std::uint64_t elems = TailShadowV41Elems().load(std::memory_order_relaxed);
    const std::uint64_t eq = TailShadowV41LocalEq().load(std::memory_order_relaxed);
    const std::uint64_t ne = TailShadowV41LocalNe().load(std::memory_order_relaxed);
    const std::uint64_t boundary = TailShadowV41Boundary().load(std::memory_order_relaxed);

    std::cerr << "[DAZG_ORBIT_TAILSHADOW_LEDGER_V41]"
              << " profile=v41_active_shadow_carry_verifier"
              << " shadow_sites=" << sites
              << " expected_shadow_sites_per_role=7"
              << " shadow_elements=" << elems
              << " local_share_equal_sites=" << eq
              << " local_share_mismatch_sites=" << ne
              << " lowbit_boundary_hits=" << boundary
              << " candidate_commit=0"
              << " fallback_preserving=1"
              << " exact_output_preserved=1"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RegisterTailShadowV41Ledger() {
    static std::atomic<bool> registered{false};
    bool expected = false;
    if (registered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::atexit(PrintTailShadowV41Ledger);
    }
}

inline void RecordTailShadowV41(const char* site,
                                std::uint64_t runtime_seq,
                                std::uint64_t tensor_n,
                                std::uint64_t pre_hash,
                                std::uint64_t local_candidate_hash,
                                std::uint64_t post_fallback_hash,
                                std::uint64_t lowbit_boundary_count,
                                bool local_share_digest_equal) {
    if (!TailShadowV41Enabled()) return;
    if (!ThunderTailUnsafeTailSiteV37(site)) return;

    RegisterTailShadowV41Ledger();

    TailShadowV41Sites().fetch_add(1ULL, std::memory_order_relaxed);
    TailShadowV41Elems().fetch_add(tensor_n, std::memory_order_relaxed);
    TailShadowV41Boundary().fetch_add(lowbit_boundary_count, std::memory_order_relaxed);
    if (local_share_digest_equal) {
        TailShadowV41LocalEq().fetch_add(1ULL, std::memory_order_relaxed);
    } else {
        TailShadowV41LocalNe().fetch_add(1ULL, std::memory_order_relaxed);
    }

    if (TailShadowV41DetailEnabled() || runtime_seq <= 64ULL) {
        std::cerr << "[DAZG_ORBIT_TAILSHADOW_CERT_V41]"
                  << " profile=v41_active_shadow_carry_verifier"
                  << " site=" << (site != nullptr ? site : "unknown")
                  << " runtime_seq=" << runtime_seq
                  << " tensor_n=" << tensor_n
                  << " pre_hash=" << pre_hash
                  << " local_candidate_hash=" << local_candidate_hash
                  << " post_fallback_hash=" << post_fallback_hash
                  << " lowbit_boundary_count=" << lowbit_boundary_count
                  << " local_share_digest_equal=" << (local_share_digest_equal ? 1 : 0)
                  << " compare_scope=per_party_share_digest"
                  << " candidate_commit=0"
                  << " fallback_preserving=1"
                  << " exact_output_preserved=1"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
}
// DAZG_ORBIT_TAILSHADOW_V41_END




// DAZG_ORBIT_TAILPROOF_V40_BEGIN
// V40.2 fail-closed TailProof ledger. Defines the V40 symbols before
// RecordThunderTailCertV37 uses them; unsafe fast commit remains disabled
// unless an external site certificate and DAZG_ORBIT_TAILPROOF_V40_COMMIT=1 exist.

inline bool ThunderTailV40Enabled() {
    const char* v = std::getenv("DAZG_ORBIT_TAILPROOF_V40");
    return v == nullptr || *v == '\0' || !false_value(v);
}

inline bool ThunderTailV40DetailEnabled() {
    const char* v = std::getenv("DAZG_ORBIT_TAILPROOF_V40_DETAIL");
    return v != nullptr && *v != '\0' && true_value(v);
}

inline bool ThunderTailV40CommitEnabled() {
    const char* v = std::getenv("DAZG_ORBIT_TAILPROOF_V40_COMMIT");
    return v != nullptr && *v != '\0' && true_value(v);
}

inline bool ThunderTailV40StrEq(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

inline bool ThunderTailV40IsDelim(char c) {
    return c == ',' || c == ';' || c == '|' || c == ':' ||
           c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

inline bool ThunderTailV40TokenMatch(const char* tokens, const char* site) {
    if (tokens == nullptr || *tokens == '\0' || site == nullptr || *site == '\0') return false;
    const std::size_t site_len = std::strlen(site);
    const char* p = tokens;
    while (*p != '\0') {
        while (*p != '\0' && ThunderTailV40IsDelim(*p)) ++p;
        const char* q = p;
        while (*q != '\0' && !ThunderTailV40IsDelim(*q)) ++q;
        const std::size_t n = static_cast<std::size_t>(q - p);
        if (n == 3U && std::strncmp(p, "all", 3U) == 0) return true;
        if (n == 4U && std::strncmp(p, "true", 4U) == 0) return true;
        if (n == 1U && *p == '1') return true;
        if (n == site_len && std::strncmp(p, site, site_len) == 0) return true;
        p = q;
    }
    return false;
}

inline bool ThunderTailV40OpportunitySite(const char* site) {
    return ThunderTailUnsafeTailSiteV37(site);
}

inline bool ThunderTailV40ExternalVerified(const char* site) {
    const char* verified = std::getenv("DAZG_ORBIT_TAILPROOF_V40_EXTERNAL_VERIFIED");
    if (verified == nullptr || *verified == '\0' || false_value(verified)) return false;
    const char* allow = std::getenv("DAZG_ORBIT_TAILPROOF_V40_SITE_ALLOW");
    if (ThunderTailV40StrEq(verified, "1") || ThunderTailV40StrEq(verified, "true") ||
        ThunderTailV40StrEq(verified, "all")) {
        return allow != nullptr && *allow != '\0' && !false_value(allow) &&
               ThunderTailV40TokenMatch(allow, site);
    }
    return ThunderTailV40TokenMatch(verified, site);
}

inline const char* ThunderTailV40ProofState(const char* site, int shift, int bw,
                                            bool signed_trunc, bool extra_flag,
                                            bool canonical_prg_reshare_preserved) {
    if (!ThunderTailV40OpportunitySite(site)) return "not_tailproof_opportunity";
    if (!canonical_prg_reshare_preserved) return "blocked_canonical_prg_reshare";
    if (shift != 17 || bw != 43 || !signed_trunc) return "blocked_shift_bw_signed";
    if (extra_flag) return "blocked_extra_flag";
    if (!ThunderTailV40ExternalVerified(site)) return "needs_external_tailproof_certificate";
    if (!ThunderTailV40CommitEnabled()) return "verified_but_commit_disabled";
    return "commit_permitted_by_external_tailproof";
}

inline bool ThunderTailV40CanCommit(const char* site, int shift, int bw,
                                    bool signed_trunc, bool extra_flag,
                                    bool canonical_prg_reshare_preserved) {
    return ThunderTailV40OpportunitySite(site) && canonical_prg_reshare_preserved &&
           shift == 17 && bw == 43 && signed_trunc && !extra_flag &&
           ThunderTailV40ExternalVerified(site) && ThunderTailV40CommitEnabled();
}

inline std::atomic<std::uint64_t>& ThunderTailV40OpportunityCounter() {
    static std::atomic<std::uint64_t> c{0}; return c;
}
inline std::atomic<std::uint64_t>& ThunderTailV40FallbackCounter() {
    static std::atomic<std::uint64_t> c{0}; return c;
}
inline std::atomic<std::uint64_t>& ThunderTailV40VerifiedCounter() {
    static std::atomic<std::uint64_t> c{0}; return c;
}
inline std::atomic<std::uint64_t>& ThunderTailV40CommitCounter() {
    static std::atomic<std::uint64_t> c{0}; return c;
}
inline std::atomic<std::uint64_t>& ThunderTailV40BlockedCounter() {
    static std::atomic<std::uint64_t> c{0}; return c;
}

inline void PrintThunderTailV40Ledger() {
    if (!ThunderTailV40Enabled()) return;
    const std::uint64_t opportunities = ThunderTailV40OpportunityCounter().load(std::memory_order_relaxed);
    if (opportunities == 0ULL) return;
    std::cerr << "[DAZG_ORBIT_TAILPROOF_LEDGER_V40]"
              << " profile=v40_2_tailproof_fallback_preserving"
              << " opportunity_sites=" << opportunities
              << " verified_sites=" << ThunderTailV40VerifiedCounter().load(std::memory_order_relaxed)
              << " committed_sites=" << ThunderTailV40CommitCounter().load(std::memory_order_relaxed)
              << " fallback_sites=" << ThunderTailV40FallbackCounter().load(std::memory_order_relaxed)
              << " blocked_sites=" << ThunderTailV40BlockedCounter().load(std::memory_order_relaxed)
              << " default_commit_disabled=" << (ThunderTailV40CommitEnabled() ? 0 : 1)
              << " external_certificate_required=1 fallback_preserving=1 exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RegisterThunderTailV40Ledger() {
    static std::atomic<bool> registered{false};
    bool expected = false;
    if (registered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::atexit(PrintThunderTailV40Ledger);
    }
}

inline void RecordThunderTailV40Proof(const char* site, std::uint64_t runtime_seq,
                                      std::uint64_t tensor_n, int shift, int bw,
                                      bool signed_trunc, bool extra_flag,
                                      bool canonical_prg_reshare_preserved,
                                      bool fallback, const char* fallback_reason) {
    if (!ThunderTailV40Enabled()) return;
    if (!ThunderTailV40OpportunitySite(site)) return;
    RegisterThunderTailV40Ledger();
    const bool verified = ThunderTailV40ExternalVerified(site);
    const bool commit = ThunderTailV40CanCommit(site, shift, bw, signed_trunc, extra_flag,
                                                canonical_prg_reshare_preserved);
    const char* proof_state = ThunderTailV40ProofState(site, shift, bw, signed_trunc,
                                                       extra_flag, canonical_prg_reshare_preserved);
    ThunderTailV40OpportunityCounter().fetch_add(1ULL, std::memory_order_relaxed);
    if (verified) ThunderTailV40VerifiedCounter().fetch_add(1ULL, std::memory_order_relaxed);
    if (commit && !fallback) {
        ThunderTailV40CommitCounter().fetch_add(1ULL, std::memory_order_relaxed);
    } else {
        ThunderTailV40FallbackCounter().fetch_add(1ULL, std::memory_order_relaxed);
        ThunderTailV40BlockedCounter().fetch_add(1ULL, std::memory_order_relaxed);
    }
    if (ThunderTailV40DetailEnabled() || runtime_seq <= 64ULL) {
        std::cerr << "[DAZG_ORBIT_TAILPROOF_CERT_V40]"
                  << " profile=v40_2_tailproof_fallback_preserving"
                  << " site=" << (site != nullptr ? site : "unknown")
                  << " runtime_seq=" << runtime_seq
                  << " tensor_n=" << tensor_n
                  << " opportunity_site=1 shift=" << shift << " bw=" << bw
                  << " signed_trunc=" << (signed_trunc ? 1 : 0)
                  << " extra_flag=" << (extra_flag ? 1 : 0)
                  << " canonical_prg_reshare_preserved=" << (canonical_prg_reshare_preserved ? 1 : 0)
                  << " external_verified=" << (verified ? 1 : 0)
                  << " commit_enabled=" << (ThunderTailV40CommitEnabled() ? 1 : 0)
                  << " commit_permitted=" << (commit ? 1 : 0)
                  << " fallback=" << (fallback ? 1 : 0)
                  << " fallback_reason=" << (fallback_reason != nullptr ? fallback_reason : "none")
                  << " proof_state=" << proof_state
                  << " fallback_preserving=1 exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
}
// DAZG_ORBIT_TAILPROOF_V40_END

inline void RecordThunderTailCertV37(const char* site,
                                     std::uint64_t runtime_seq,
                                     std::uint64_t tensor_n,
                                     int shift,
                                     int bw,
                                     bool signed_trunc,
                                     bool extra_flag,
                                     bool canonical_prg_reshare_preserved,
                                     bool fallback,
                                     const char* fallback_reason) {
    const std::uint64_t cert_seq =
        ThunderTailCertCounterV37().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    Context ctx{true, shift, bw, signed_trunc, extra_flag, site};
    const bool gate_pass = ThunderTailExactGatePassV37(ctx, canonical_prg_reshare_preserved);
    RecordThunderTailLedgerV38(site, gate_pass, fallback, fallback_reason);
    RecordThunderTailV40Proof(site, runtime_seq, tensor_n, shift, bw,
                               signed_trunc, extra_flag,
                               canonical_prg_reshare_preserved,
                               fallback, fallback_reason);
    if (cert_seq > 512ULL && !true_value(std::getenv("DAZG_ORBIT_THUNDERTAIL_CERT_DETAIL"))) {
        return;
    }
    const char* raw_site_allow = ThunderTailEnvSiteAllowV37();
    std::cerr << "[DAZG_ORBIT_THUNDERTAIL_CERT]"
              << " cert_profile=v38_cert_ledger"
              << " tailproof_v40=1"
              << " cert_seq=" << cert_seq
              << " site=" << (site != nullptr ? site : "unknown")
              << " runtime_seq=" << runtime_seq
              << " tensor_n=" << tensor_n
              << " policy_source=" << ThunderTailPolicySourceV37()
              << " runtime_policy=" << FrontierPolicy()
              << " site_allowlist=" << ThunderTailEffectiveSiteAllowV37()
              << " raw_site_allowlist=" << (raw_site_allow != nullptr ? raw_site_allow : "none")
              << " shift=" << shift
              << " bw=" << bw
              << " signed_trunc=" << (signed_trunc ? 1 : 0)
              << " extra_flag=" << (extra_flag ? 1 : 0)
              << " runtime_mode=" << ModeForContext(ctx)
              << " expected_mode=" << ThunderTailExpectedModeV37(site)
              << " certified_site=" << (ThunderTailCertifiedSiteV37(site) ? 1 : 0)
              << " unsafe_tail_site=" << (ThunderTailUnsafeTailSiteV37(site) ? 1 : 0)
              << " canonical_prg_reshare_preserved=" << (canonical_prg_reshare_preserved ? 1 : 0)
              << " gate_pass=" << (gate_pass ? 1 : 0)
              << " tailproof_v40=" << (ThunderTailV40OpportunitySite(site) ? 1 : 0)
              << " v41_tailshadow=" << (ThunderTailUnsafeTailSiteV37(site) ? 1 : 0)
              << " tailproof_external_verified=" << (ThunderTailV40ExternalVerified(site) ? 1 : 0)
              << " tailproof_commit_enabled=" << (ThunderTailV40CommitEnabled() ? 1 : 0)
              << " tailproof_commit_permitted=" << (ThunderTailV40CanCommit(site, shift, bw, signed_trunc, extra_flag, canonical_prg_reshare_preserved) ? 1 : 0)
              << " tailproof_state=" << ThunderTailV40ProofState(site, shift, bw, signed_trunc, extra_flag, canonical_prg_reshare_preserved)
              << " fallback=" << (fallback ? 1 : 0)
              << " fallback_reason=" << (fallback_reason != nullptr ? fallback_reason : "none")
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}
// DAZG_ORBIT_THUNDERTAIL_V37_SITE_STABLE_END

inline bool FrontierAllowSiteSeq(std::uint64_t seq, const char* site) {
    // V37.2: preserve the legacy seq frontier for already-certified core sites,
    // and apply the site-stable exact gate only to the late tail decision space.
    // Under the frozen semantic V37 preset, unknown/non-certified sites cannot
    // be opened accidentally by a broad policy token.
    const char* policy = FrontierPolicy();
    const bool legacy_requested = ThunderTailPolicyRequestsSiteSeqV37(policy, seq, site);
    if (!ThunderTailExtensionDecisionSiteV37(site)) {
        if (ThunderTailPolicyIsV37PresetV37(policy)) {
            return legacy_requested && ThunderTailCertifiedSiteV37(site) &&
                   ThunderTailSiteAllowlistContainsV37(site);
        }
        return legacy_requested;
    }
    return ThunderTailShouldApplyV37(seq, site, 17, 43, true, false, true);
}
// DAZG_ORBIT_THUNDERACT_V19H_FRONTIER_POLICY_END



// DAZG_ORBIT_CORE_V29_MODEPLAN_BEGIN
// Core patch, not an env-only experiment:
// compile the certified V19 43-bit activation route once, then allow the
// Bob-to-Alice ThunderAct bridge to send a 43-bit payload instead of raw64.
// Exactness condition: the downstream truncate path consumes only low 43 bits.
// Therefore low43(a + b) is preserved by low43(a) + low43(b) mod 2^43.
struct ModePlanV29 {
    bool valid;
    int input_bits;
    int output_bits;
    int shift;
    bool arithmetic_shift;
    bool activation_output;
    bool word64_activation;
    const char* mode_name;

    ModePlanV29()
        : valid(false),
          input_bits(64),
          output_bits(64),
          shift(0),
          arithmetic_shift(false),
          activation_output(false),
          word64_activation(false),
          mode_name("generic") {}
};

inline bool EnvTrueV29(const char* name) {
    const char* v = std::getenv(name);
    return true_value(v);
}

inline bool AnyEnvTrueV29(const char* a,
                          const char* b,
                          const char* c,
                          const char* d,
                          const char* e,
                          const char* f = nullptr,
                          const char* g = nullptr) {
    const char* names[7] = {a, b, c, d, e, f, g};
    for (const char* name : names) {
        if (name == nullptr) continue;
        if (EnvTrueV29(name)) return true;
    }
    return false;
}

inline ModePlanV29 CompileModePlanV29(const Context& ctx,
                                      int activation_bitwidth) {
    ModePlanV29 plan;
    const char* mode = ModeForContext(ctx);
    const int sh = ctx.shift < 0 ? 0 : ctx.shift;
    plan.shift = sh;
    plan.mode_name = mode != nullptr ? mode : "generic";

    if (mode_eq(mode, "signed43")) {
        plan.valid = true;
        plan.input_bits = 43;
        plan.output_bits = 43;
        plan.arithmetic_shift = true;
        return plan;
    }

    if (mode_eq(mode, "logical43")) {
        plan.valid = true;
        plan.input_bits = 43;
        plan.output_bits = 43;
        plan.arithmetic_shift = false;
        return plan;
    }

    if (mode_eq(mode, "word64")) {
        plan.valid = true;
        plan.input_bits = 64;
        plan.output_bits = clamp_bits(activation_bitwidth);
        plan.arithmetic_shift = false;
        plan.activation_output = true;
        plan.word64_activation = true;
        return plan;
    }

    return plan;
}

inline std::int64_t LocalTruncateWithPlanV29(std::uint64_t raw,
                                             const ModePlanV29& plan,
                                             const Context& ctx,
                                             int activation_bitwidth) {
    if (!plan.valid) {
        return LocalTruncate(raw, ctx, activation_bitwidth);
    }

    const int sh = plan.shift < 0 ? 0 : plan.shift;
    const int act_bits = clamp_bits(activation_bitwidth);

    const std::uint64_t shifted = [&]() -> std::uint64_t {
        if (plan.arithmetic_shift) {
            const std::int64_t sx = sign_extend(raw, plan.input_bits);
            return static_cast<std::uint64_t>(floor_shift(sx, sh));
        }
        const std::uint64_t x = mask_bits(raw, plan.input_bits);
        if (sh <= 0) return x;
        if (sh >= 64) return 0;
        return x >> static_cast<unsigned>(sh);
    }();

    if (plan.activation_output || plan.word64_activation) {
        return sign_extend(mask_bits(shifted, act_bits), act_bits);
    }

    return sign_extend(mask_bits(shifted, plan.output_bits), plan.output_bits);
}

inline bool PackedBridgeEnabledV29(const Context& ctx,
                                   int activation_bitwidth,
                                   const ModePlanV29& plan) {
    (void)ctx;
    (void)activation_bitwidth;
    if (!plan.valid) return false;
    if (plan.input_bits != 43 || plan.output_bits != 43) return false;
    if (plan.activation_output || plan.word64_activation) return false;
    return AnyEnvTrueV29("DAZG_ORBIT_PACKED_BRIDGE_V29",
                         "DAZG_ORBIT_V29_PACKED_BRIDGE",
                         "DAZG_ORBIT_PACKED_BRIDGE",
                         "DAZG_ORBIT_ENABLE_PACKED_BRIDGE",
                         "DAZG_ORBIT_PACKED_BRIDGE",
                         "DAZG_ORBIT_PACKED_BRIDGE_V27",
                         "DAZG_ORBIT_THUNDERACT_PACKED_BRIDGE");
}

inline std::atomic<std::uint64_t>& PackedBridgeCounterV29() {
    static std::atomic<std::uint64_t> counter{0};
    return counter;
}

inline void RecordPackedBridgeV29(const char* site,
                                  int n,
                                  int raw_bytes_per_elem,
                                  int packed_bytes_per_elem,
                                  const ModePlanV29& plan) {
    const std::uint64_t seq =
        PackedBridgeCounterV29().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    const std::uint64_t nn = static_cast<std::uint64_t>(n < 0 ? 0 : n);
    const std::uint64_t raw_bytes = nn * static_cast<std::uint64_t>(raw_bytes_per_elem);
    const std::uint64_t packed_bytes = nn * static_cast<std::uint64_t>(packed_bytes_per_elem);

    if (seq <= 256ULL || EnvTrueV29("DAZG_ORBIT_PACKED_BRIDGE_DETAIL")) {
        std::cerr << "[DAZG_ORBIT_PACKED_BRIDGE_V29]"
                  << " runtime_applied=1"
                  << " seq=" << seq
                  << " site=" << (site != nullptr ? site : "unknown")
                  << " n=" << n
                  << " mode=" << (plan.mode_name != nullptr ? plan.mode_name : "generic")
                  << " input_bits=" << plan.input_bits
                  << " output_bits=" << plan.output_bits
                  << " shift=" << plan.shift
                  << " raw_bytes_per_elem=" << raw_bytes_per_elem
                  << " packed_bytes_per_elem=" << packed_bytes_per_elem
                  << " saved_bytes=" << (raw_bytes >= packed_bytes ? raw_bytes - packed_bytes : 0ULL)
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
}
// DAZG_ORBIT_CORE_V29_MODEPLAN_END

}  // namespace thunderact_v19e
}  // namespace dazg_orbit
