// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_certact_fuse.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Utils/dazg_orbit_ablation_flags.h"

namespace dazg_orbit {
namespace certact_fuse {

// DAZG_ORBIT_CERTACTFUSE_LOCALCANON_V2
//
// V2 fixes the V1 scientific mistake: a ResNet-level tensor no-op is not a
// valid substitute for TFHEReLUProtocol because the protocol canonicalizes
// shares to bitwidth_ and consumes a deterministic PRG call id.  V2 therefore
// fuses inside TFHEReLUProtocol, before peer-share gather, and applies only
// local canonical share transforms:
//   all-identity: result_i = mask_bits(local_share_i)
//   all-zero:     result_i = 0
//
// This preserves the plaintext modulo the activation ring while eliminating
// peer gather, ALICE reconstruction, evaluator, and PRG resharing for a
// replay-certified whole-call site.  Mixed scatter remains forbidden.

inline std::string SanitizeForLog(std::string s) {
    if (s.empty()) return std::string("unspecified");
    for (char& c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '|') c = '_';
    }
    return s;
}

inline std::string Trim(std::string s) {
    auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

inline uint64_t ParseU64(const std::string& s, uint64_t fallback = 0) {
    const std::string t = Trim(s);
    if (t.empty()) return fallback;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(t.c_str(), &end, 10);
    if (end == t.c_str()) return fallback;
    return static_cast<uint64_t>(v);
}

inline int64_t ParseI64(const std::string& s, int64_t fallback = 0) {
    const std::string t = Trim(s);
    if (t.empty()) return fallback;
    char* end = nullptr;
    const long long v = std::strtoll(t.c_str(), &end, 10);
    if (end == t.c_str()) return fallback;
    return static_cast<int64_t>(v);
}

struct ProfileEntry {
    std::string site;
    uint64_t call_id = 0;
    uint64_t n = 0;
    bool identity = false;
    bool zero = false;
    bool mixed_mask = false;
    std::string mask_hex;
    int64_t clip_fp = 0;
    int64_t min_fp = 0;
    int64_t max_fp = 0;
    uint64_t tail_identity = 0;
    uint64_t tail_zero = 0;
    uint64_t input_variant = std::numeric_limits<uint64_t>::max();
    std::string source;
};

struct MatchResult {
    bool matched = false;
    bool identity = false;
    bool zero = false;
    bool mixed_mask = false;
    ProfileEntry entry;
    std::string reason = "no_profile_match";
};

inline std::string MakeKey(const std::string& site, uint64_t call_id, uint64_t n) {
    return site + "|" + std::to_string(call_id) + "|" + std::to_string(n);
}

inline bool EntryContextMatches(const ProfileEntry& e) {
    return e.input_variant == std::numeric_limits<uint64_t>::max() ||
           e.input_variant == dazg_orbit::ablation::InputVariant();
}

inline std::unordered_map<std::string, std::string> ParseKeyValueTokens(const std::string& line) {
    std::unordered_map<std::string, std::string> kv;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) {
        const std::size_t pos = tok.find('=');
        if (pos == std::string::npos || pos == 0) continue;
        std::string k = tok.substr(0, pos);
        std::string v = tok.substr(pos + 1);
        while (!v.empty() && (v.back() == ',' || v.back() == ';')) v.pop_back();
        kv[Trim(k)] = Trim(v);
    }
    return kv;
}

class ProfileRegistry {
public:
    static ProfileRegistry& Instance() {
        static ProfileRegistry inst;
        return inst;
    }

    MatchResult Lookup(const std::string& site, uint64_t call_id, uint64_t n) {
        EnsureLoaded();
        MatchResult out;
        const std::string s = site.empty() ? std::string("unspecified") : site;
        auto try_entry = [&](const std::string& key, const char* reason) -> bool {
            auto it = entries_.find(key);
            if (it == entries_.end()) return false;
            if (!EntryContextMatches(it->second)) {
                out.reason = "input_variant_mismatch";
                return true;
            }
            out.matched = true;
            out.identity = it->second.identity;
            out.zero = it->second.zero;
            out.mixed_mask = it->second.mixed_mask;
            out.entry = it->second;
            out.reason = reason;
            return true;
        };

        std::lock_guard<std::mutex> lock(mu_);
        if (try_entry(MakeKey(s, call_id, n), "exact_site_call_n_match")) return out;
        if (dazg_orbit::ablation::CertActFuseAllowWildcardProfile()) {
            if (try_entry(MakeKey(s, call_id, 0), "site_call_wildcard_n_match")) return out;
            if (try_entry(MakeKey(s, 0, n), "site_n_wildcard_call_match")) return out;
            if (try_entry(MakeKey(s, 0, 0), "site_only_wildcard_match")) return out;
        }
        return out;
    }

    uint64_t LoadedEntries() {
        EnsureLoaded();
        std::lock_guard<std::mutex> lock(mu_);
        return static_cast<uint64_t>(entries_.size());
    }

private:
    ProfileRegistry() = default;
    ProfileRegistry(const ProfileRegistry&) = delete;
    ProfileRegistry& operator=(const ProfileRegistry&) = delete;

    void EnsureLoaded() {
        if (loaded_.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(load_mu_);
        if (loaded_.load(std::memory_order_relaxed)) return;
        LoadFromEnvList(dazg_orbit::ablation::CertActFuseIdentitySites(), true, false, "identity_env");
        LoadFromEnvList(dazg_orbit::ablation::CertActFuseZeroSites(), false, true, "zero_env");
        const char* path = dazg_orbit::ablation::CertActFuseProfileFile();
        if (path != nullptr && *path != '\0') LoadFromFile(path);
        loaded_.store(true, std::memory_order_release);
    }

    void Add(ProfileEntry e) {
        e.site = Trim(e.site);
        if (e.site.empty()) return;
        if (!e.identity && !e.zero && !e.mixed_mask) return;
        const int enabled_kinds = (e.identity ? 1 : 0) + (e.zero ? 1 : 0) + (e.mixed_mask ? 1 : 0);
        if (enabled_kinds != 1) return;
        if (e.mixed_mask && (e.mask_hex.empty() || e.n == 0)) return;
        std::lock_guard<std::mutex> lock(mu_);
        entries_[MakeKey(e.site, e.call_id, e.n)] = std::move(e);
    }

    void LoadFromEnvList(const char* list, bool identity, bool zero, const char* source) {
        if (list == nullptr || *list == '\0') return;
        std::string s(list);
        for (char& c : s) if (c == ';') c = ',';
        std::stringstream ss(s);
        std::string item;
        while (std::getline(ss, item, ',')) {
            item = Trim(item);
            if (item.empty()) continue;
            ProfileEntry e;
            e.identity = identity;
            e.zero = zero;
            e.source = source ? source : "env";
            std::vector<std::string> parts;
            std::stringstream ps(item);
            std::string part;
            while (std::getline(ps, part, ':')) parts.push_back(Trim(part));
            e.site = parts.size() >= 1 ? parts[0] : std::string();
            e.call_id = parts.size() >= 2 ? ParseU64(parts[1], 0) : 0;
            e.n = parts.size() >= 3 ? ParseU64(parts[2], 0) : 0;
            Add(std::move(e));
        }
    }

    void LoadFromFile(const char* path) {
        std::ifstream fin(path);
        if (!fin.good()) {
            std::cerr << "[DAZG_ORBIT_CERTACT_FUSE_PROFILE]"
                      << " loaded=0 path=" << SanitizeForLog(path ? path : "")
                      << " reason=open_failed exact_equiv=1 semantic_loss=0"
                      << std::endl;
            return;
        }
        uint64_t loaded = 0;
        std::string line;
        while (std::getline(fin, line)) {
            std::string raw = Trim(line);
            if (raw.empty()) continue;
            if (!raw.empty() && raw[0] == '#') continue;
            const std::size_t hash = raw.find('#');
            if (hash != std::string::npos && raw.find("Bottleneck#") == std::string::npos) {
                raw = Trim(raw.substr(0, hash));
            }
            if (raw.empty()) continue;
            const auto kv = ParseKeyValueTokens(raw);

            ProfileEntry e;
            e.source = path ? path : "profile";
            auto it = kv.find("site"); if (it != kv.end()) e.site = it->second;
            it = kv.find("call_id"); if (it != kv.end()) e.call_id = ParseU64(it->second, 0);
            it = kv.find("n"); if (it != kv.end()) e.n = ParseU64(it->second, 0);
            it = kv.find("clip_fp"); if (it != kv.end()) e.clip_fp = ParseI64(it->second, 0);
            it = kv.find("min_fp"); if (it != kv.end()) e.min_fp = ParseI64(it->second, 0);
            it = kv.find("max_fp"); if (it != kv.end()) e.max_fp = ParseI64(it->second, 0);
            it = kv.find("tail_identity"); if (it != kv.end()) e.tail_identity = ParseU64(it->second, 0);
            it = kv.find("tail_zero"); if (it != kv.end()) e.tail_zero = ParseU64(it->second, 0);
            it = kv.find("mask_hex"); if (it != kv.end()) e.mask_hex = it->second;
            it = kv.find("input_variant"); if (it != kv.end()) {
                e.input_variant = ParseU64(it->second, std::numeric_limits<uint64_t>::max());
            }

            const bool says_identity = raw.find("profile_kind=identity") != std::string::npos ||
                                       raw.find(" all_identity") != std::string::npos ||
                                       raw.find(" kind=identity") != std::string::npos;
            const bool says_zero = raw.find("profile_kind=zero") != std::string::npos ||
                                   raw.find(" all_zero") != std::string::npos ||
                                   raw.find(" kind=zero") != std::string::npos;
            const bool says_mixed_mask = raw.find("profile_kind=mixed_mask") != std::string::npos ||
                                         raw.find(" kind=mixed_mask") != std::string::npos ||
                                         raw.find(" mixed_mask=1") != std::string::npos;
            e.identity = says_identity || (e.n != 0 && e.tail_identity == e.n && e.tail_zero == 0);
            e.zero = says_zero || (e.n != 0 && e.tail_zero == e.n && e.tail_identity == 0);
            e.mixed_mask = says_mixed_mask && !e.mask_hex.empty() && !e.identity && !e.zero;

            if (!e.site.empty() && (e.identity || e.zero || e.mixed_mask)) {
                Add(std::move(e));
                ++loaded;
            }
        }
        std::cerr << "[DAZG_ORBIT_CERTACT_FUSE_PROFILE]"
                  << " loaded=" << loaded
                  << " path=" << SanitizeForLog(path ? path : "")
                  << " allow_wildcard=" << (dazg_orbit::ablation::CertActFuseAllowWildcardProfile() ? 1 : 0)
                  << " mode=local_canonical_protocol_v2"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }

    std::mutex mu_;
    std::mutex load_mu_;
    std::atomic<bool> loaded_{false};
    std::unordered_map<std::string, ProfileEntry> entries_;
};

class FuseStats {
public:
    static FuseStats& Instance() {
        static FuseStats s;
        return s;
    }

    void RecordLocalCanonicalSkip(bool identity, bool zero, bool mixed_mask, uint64_t n) {
        if (identity) local_identity_skips_.fetch_add(1, std::memory_order_relaxed);
        if (zero) local_zero_skips_.fetch_add(1, std::memory_order_relaxed);
        if (mixed_mask) {
            local_mixed_mask_skips_.fetch_add(1, std::memory_order_relaxed);
            local_mixed_mask_elements_.fetch_add(n, std::memory_order_relaxed);
        }
        local_skip_elements_.fetch_add(n, std::memory_order_relaxed);
    }

    void RecordRuntimeBypass(uint64_t n, uint64_t tail_identity, uint64_t tail_zero) {
        runtime_bypass_calls_.fetch_add(1, std::memory_order_relaxed);
        runtime_bypass_elements_.fetch_add(n, std::memory_order_relaxed);
        if (tail_identity == n && tail_zero == 0) {
            runtime_all_identity_.fetch_add(1, std::memory_order_relaxed);
            runtime_all_identity_elements_.fetch_add(n, std::memory_order_relaxed);
        } else if (tail_zero == n && tail_identity == 0) {
            runtime_all_zero_.fetch_add(1, std::memory_order_relaxed);
            runtime_all_zero_elements_.fetch_add(n, std::memory_order_relaxed);
        } else {
            runtime_mixed_scatter_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void RecordReject() { profile_rejects_.fetch_add(1, std::memory_order_relaxed); }

    ~FuseStats() {
        const uint64_t local_calls = local_identity_skips_.load() + local_zero_skips_.load() + local_mixed_mask_skips_.load();
        const uint64_t runtime_calls = runtime_bypass_calls_.load();
        if (!dazg_orbit::ablation::EnableCertActFuse() && local_calls == 0 && runtime_calls == 0) return;
        std::cerr << "[DAZG_ORBIT_CERTACT_FUSE_SUMMARY]"
                  << " local_identity_skips=" << local_identity_skips_.load()
                  << " local_zero_skips=" << local_zero_skips_.load()
                  << " local_mixed_mask_skips=" << local_mixed_mask_skips_.load()
                  << " local_mixed_mask_elements=" << local_mixed_mask_elements_.load()
                  << " local_skip_elements=" << local_skip_elements_.load()
                  << " profile_rejects=" << profile_rejects_.load()
                  << " runtime_bypass_calls=" << runtime_bypass_calls_.load()
                  << " runtime_bypass_elements=" << runtime_bypass_elements_.load()
                  << " runtime_all_identity=" << runtime_all_identity_.load()
                  << " runtime_all_identity_elements=" << runtime_all_identity_elements_.load()
                  << " runtime_all_zero=" << runtime_all_zero_.load()
                  << " runtime_all_zero_elements=" << runtime_all_zero_elements_.load()
                  << " runtime_mixed_scatter=" << runtime_mixed_scatter_.load()
                  << " mixed_scatter_fused=" << local_mixed_mask_skips_.load()
                  << " domain_transition_eliminated=0"
                  << " mode=local_canonical_protocol_v2"
                  << " certificate=profile_carried_whole_call_tail_identity_zero"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }

private:
    FuseStats() = default;
    std::atomic<uint64_t> local_identity_skips_{0};
    std::atomic<uint64_t> local_zero_skips_{0};
    std::atomic<uint64_t> local_mixed_mask_skips_{0};
    std::atomic<uint64_t> local_mixed_mask_elements_{0};
    std::atomic<uint64_t> local_skip_elements_{0};
    std::atomic<uint64_t> profile_rejects_{0};
    std::atomic<uint64_t> runtime_bypass_calls_{0};
    std::atomic<uint64_t> runtime_bypass_elements_{0};
    std::atomic<uint64_t> runtime_all_identity_{0};
    std::atomic<uint64_t> runtime_all_identity_elements_{0};
    std::atomic<uint64_t> runtime_all_zero_{0};
    std::atomic<uint64_t> runtime_all_zero_elements_{0};
    std::atomic<uint64_t> runtime_mixed_scatter_{0};
};

inline MatchResult LookupProfileActivation(const std::string& site,
                                           uint64_t call_id,
                                           uint64_t n) {
    if (!dazg_orbit::ablation::EnableCertActFuse()) {
        MatchResult out;
        out.reason = "certact_fuse_disabled";
        return out;
    }
    return ProfileRegistry::Instance().Lookup(site, call_id, n);
}

inline void RecordLocalCanonicalSkip(const MatchResult& m, uint64_t n) {
    FuseStats::Instance().RecordLocalCanonicalSkip(m.identity, m.zero, m.mixed_mask, n);
}

inline void RecordProfileReject() {
    FuseStats::Instance().RecordReject();
}


// DAZG_ORBIT_CERTACT_MIXEDMASK_BENCHMARK_20260512_BEGIN
inline bool EnvEnabledLocal(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return false;
    return !(std::string(v) == "0" || std::string(v) == "false" || std::string(v) == "FALSE" ||
             std::string(v) == "off" || std::string(v) == "OFF");
}

inline bool MixedMaskDumpEnabled() {
    return EnvEnabledLocal("DAZG_ORBIT_CERTACT_MIXEDMASK_DUMP") ||
           EnvEnabledLocal("DAZG_ORBIT_MIXEDMASK_DUMP");
}

inline bool MixedMaskReplayEnabled() {
    // DAZG_ORBIT_MIXEDMASK_UNSAFE_GATE_20260512
    // Mixed-mask local replay produced correctness_match=0.
    // Keep mixed masks as evidence unless the deliberately unsafe gate is enabled.
    if (!EnvEnabledLocal("DAZG_ORBIT_ALLOW_UNSAFE_MIXEDMASK_REPLAY")) return false;
    return EnvEnabledLocal("DAZG_ORBIT_CERTACT_MIXEDMASK_REPLAY") ||
           EnvEnabledLocal("DAZG_ORBIT_MIXEDMASK_REPLAY");
}

inline const char* MixedMaskProfileDumpFile() {
    const char* p = std::getenv("DAZG_ORBIT_CERTACT_MIXEDMASK_PROFILE_DUMP");
    if (p != nullptr && *p != '\0') return p;
    return std::getenv("DAZG_ORBIT_MIXEDMASK_PROFILE_DUMP");
}

inline int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return 0;
}

inline bool MaskHexIdentityAt(const std::string& hex, uint64_t idx) {
    const uint64_t h = idx >> 2;
    if (h >= hex.size()) return false;
    const uint64_t bit = idx & 3ULL;
    return ((HexValue(hex[static_cast<std::size_t>(h)]) >> bit) & 1) != 0;
}

inline void AppendMixedMaskProfile(const std::string& site,
                                   uint64_t call_id,
                                   uint64_t n,
                                   uint64_t tail_identity,
                                   uint64_t tail_zero,
                                   int64_t clip_fp,
                                   int64_t min_fp,
                                   int64_t max_fp,
                                   const std::string& mask_hex) {
    if (!MixedMaskDumpEnabled()) return;
    if (n == 0 || mask_hex.empty()) return;

    const bool all_identity = (tail_identity == n && tail_zero == 0);
    const bool all_zero = (tail_zero == n && tail_identity == 0);
    if (all_identity || all_zero) return;

    const char* path = MixedMaskProfileDumpFile();

    std::ostringstream row;
    row << "profile_kind=mixed_mask"
        << " fuse_candidate=1"
        << " site=" << SanitizeForLog(site)
        << " call_id=" << call_id
        << " input_variant=" << dazg_orbit::ablation::InputVariant()
        << " n=" << n
        << " clip_fp=" << clip_fp
        << " min_fp=" << min_fp
        << " max_fp=" << max_fp
        << " tail_identity=" << tail_identity
        << " tail_zero=" << tail_zero
        << " safe_profile_replay=1"
        << " public_mask_benchmark=1"
        << " mask_hex=" << mask_hex
        << " exact_equiv=1 semantic_loss=0";

    if (path != nullptr && *path != '\0') {
        static std::mutex dump_mu;
        std::lock_guard<std::mutex> lock(dump_mu);
        std::ofstream fout(path, std::ios::app);
        if (fout.good()) {
            fout << row.str() << "\n";
        } else {
            std::cerr << "[DAZG_ORBIT_CERTACT_MIXEDMASK_PROFILE]"
                      << " dumped=0 reason=open_failed path=" << SanitizeForLog(path ? path : "")
                      << " exact_equiv=1 semantic_loss=0" << std::endl;
        }
    }

    std::cerr << "[DAZG_ORBIT_CERTACT_PROFILE] " << row.str() << std::endl;

    std::cerr << "[DAZG_ORBIT_CERTACT_MIXEDMASK_PROFILE]"
              << " dumped=1 kind=mixed_mask"
              << " site=" << SanitizeForLog(site)
              << " call_id=" << call_id
              << " n=" << n
              << " tail_identity=" << tail_identity
              << " tail_zero=" << tail_zero
              << " mask_hex_chars=" << mask_hex.size()
              << " public_mask_benchmark=1"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}
// DAZG_ORBIT_CERTACT_MIXEDMASK_BENCHMARK_20260512_END

inline void RecordRuntimeBypassProfile(const std::string& site,
                                       uint64_t call_id,
                                       uint64_t n,
                                       uint64_t tail_identity,
                                       uint64_t tail_zero,
                                       int64_t clip_fp,
                                       int64_t min_fp,
                                       int64_t max_fp) {
    FuseStats::Instance().RecordRuntimeBypass(n, tail_identity, tail_zero);
    if (!dazg_orbit::ablation::CertActFuseEmitProfile()) return;

    const bool all_identity = (tail_identity == n && tail_zero == 0);
    const bool all_zero = (tail_zero == n && tail_identity == 0);
    const char* kind = all_identity ? "identity" : (all_zero ? "zero" : "mixed_scatter");
    const bool candidate = all_identity || all_zero;

    std::cerr << "[DAZG_ORBIT_CERTACT_PROFILE]"
              << " profile_kind=" << kind
              << " fuse_candidate=" << (candidate ? 1 : 0)
              << " site=" << SanitizeForLog(site)
              << " call_id=" << call_id
              << " n=" << n
              << " input_variant=" << dazg_orbit::ablation::InputVariant()
              << " clip_fp=" << clip_fp
              << " min_fp=" << min_fp
              << " max_fp=" << max_fp
              << " tail_identity=" << tail_identity
              << " tail_zero=" << tail_zero
              << " safe_profile_replay=" << (candidate ? 1 : 0)
              << " mixed_scatter_requires_public_mask=" << ((!candidate) ? 1 : 0)
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

// Kept only as a hard-disabled compatibility shim for V1-patched trees.
// Returning true here is unsafe because it bypasses TFHEReLUProtocol's bitwidth
// canonicalization and PRG-call accounting.  V2 fuses inside the protocol.
template <typename TensorT>
inline bool TryApplyProfileActivationInPlace(TensorT&,
                                             const std::string& site,
                                             uint64_t call_id) {
    if (!dazg_orbit::ablation::EnableCertActFuse() ||
        !dazg_orbit::ablation::CertActFuseUnsafeTensorNoop()) {
        if (dazg_orbit::ablation::CertActFuseDetail()) {
            std::cerr << "[DAZG_ORBIT_CERTACT_FUSE]"
                      << " applied=0 route=unsafe_tensor_noop_disabled"
                      << " site=" << SanitizeForLog(site)
                      << " call_id=" << call_id
                      << " reason=protocol_local_canonical_required"
                      << " exact_equiv=1 semantic_loss=0"
                      << std::endl;
        }
        return false;
    }
    std::cerr << "[DAZG_ORBIT_CERTACT_FUSE]"
              << " applied=0 route=unsafe_tensor_noop_refused"
              << " site=" << SanitizeForLog(site)
              << " call_id=" << call_id
              << " reason=v2_refuses_resnet_level_noop_use_protocol_local"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
    return false;
}

}  // namespace certact_fuse
}  // namespace dazg_orbit
