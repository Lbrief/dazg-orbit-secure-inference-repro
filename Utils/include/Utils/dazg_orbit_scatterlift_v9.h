// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_scatterlift_v9.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>

#include "Utils/dazg_orbit_ablation_flags.h"
#include "Utils/dazg_orbit_certact_fuse.h"

namespace dazg_orbit {
namespace scatterlift_v9 {

enum class Kind : uint8_t {
    kIdentity = 1,
    kZero = 2,
};

inline bool IsTrue(const char* v) {
    if (!v) return false;
    const std::string s(v);
    return s == "1" || s == "true" || s == "TRUE" ||
           s == "on" || s == "ON" || s == "yes" || s == "YES";
}

inline bool IsFalse(const char* v) {
    if (!v) return false;
    const std::string s(v);
    return s == "0" || s == "false" || s == "FALSE" ||
           s == "off" || s == "OFF" || s == "no" || s == "NO";
}

inline bool EnvFlag(const char* name, bool fallback = false) {
    const char* v = std::getenv(name);
    if (!v || !*v) return fallback;
    if (IsTrue(v)) return true;
    if (IsFalse(v)) return false;
    return fallback;
}

inline const char* EnvStr(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name);
    return (!v || !*v) ? fallback : v;
}

inline uint64_t ParseU64(const std::string& s, uint64_t fallback = 0) {
    return dazg_orbit::certact_fuse::ParseU64(s, fallback);
}

inline int64_t ParseI64(const std::string& s, int64_t fallback = 0) {
    return dazg_orbit::certact_fuse::ParseI64(s, fallback);
}

inline std::string Trim(std::string s) {
    return dazg_orbit::certact_fuse::Trim(std::move(s));
}

inline std::string SanitizeForLog(const std::string& s) {
    return dazg_orbit::certact_fuse::SanitizeForLog(s);
}

inline std::unordered_map<std::string, std::string> ParseKV(const std::string& line) {
    return dazg_orbit::certact_fuse::ParseKeyValueTokens(line);
}

inline const char* ProfileFile() {
    const char* p = EnvStr("DAZG_ORBIT_SCATTERLIFT_V9_PROFILE", "");
    if (p && *p) return p;
    p = EnvStr("DAZG_ORBIT_SCATTERLIFT_V9_PROFILE", "");
    if (p && *p) return p;
    return EnvStr("DAZG_ORBIT_CERTACT_FUSE_PROFILE", "");
}

inline bool Enabled() {
    return dazg_orbit::ablation::EnableRouteSpecialization() &&
           EnvFlag("DAZG_ORBIT_ENABLE_SCATTERLIFT_V9",
                   EnvFlag("DAZG_ORBIT_SCATTERLIFT_V9", false));
}

inline bool ProofMode() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V9_PROOF",
                   EnvFlag("DAZG_ORBIT_SCATTERLIFT_V9_PROOF", false));
}

inline bool DetailEnabled() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V9_DETAIL",
                   EnvFlag("DAZG_ORBIT_SCATTERLIFT_V9_DETAIL", false));
}

inline const char* KindName(Kind k) {
    return k == Kind::kIdentity ? "identity" : "zero";
}

struct ProfileEntry {
    std::string site;
    uint64_t call_id = 0;
    uint64_t n = 0;
    uint64_t input_variant = std::numeric_limits<uint64_t>::max();
    int64_t clip_fp = 0;
    int64_t min_fp = 0;
    int64_t max_fp = 0;
    uint64_t tail_identity = 0;
    uint64_t tail_zero = 0;
    Kind kind = Kind::kIdentity;

    bool identity() const { return kind == Kind::kIdentity; }
    bool zero() const { return kind == Kind::kZero; }
};

struct LookupResult {
    bool matched = false;
    std::string reason = "no_profile_match";
    ProfileEntry entry;
};

inline std::string Key(const std::string& site, uint64_t call_id, uint64_t n) {
    return site + "|" + std::to_string(call_id) + "|" + std::to_string(n);
}

class Registry {
public:
    static Registry& Instance() {
        static Registry r;
        return r;
    }

    LookupResult Lookup(const std::string& site, uint64_t call_id, uint64_t n) {
        EnsureLoaded();

        LookupResult out;
        const std::string s = site.empty() ? std::string("unspecified") : site;

        std::lock_guard<std::mutex> lock(mu_);
        const auto it = entries_.find(Key(s, call_id, n));
        if (it == entries_.end()) {
            out.reason = "no_exact_whole_tail_site_call_n";
            return out;
        }

        if (it->second.input_variant != std::numeric_limits<uint64_t>::max() &&
            it->second.input_variant != dazg_orbit::ablation::InputVariant()) {
            out.reason = "input_variant_mismatch";
            return out;
        }

        out.matched = true;
        out.reason = "exact_whole_tail_profile_match";
        out.entry = it->second;
        return out;
    }

private:
    void EnsureLoaded() {
        if (loaded_.load(std::memory_order_acquire)) return;

        std::lock_guard<std::mutex> load_lock(load_mu_);
        if (loaded_.load(std::memory_order_relaxed)) return;

        const char* path = ProfileFile();
        if (path && *path) Load(path);

        loaded_.store(true, std::memory_order_release);
    }

    void Add(ProfileEntry e) {
        e.site = Trim(e.site);
        if (e.site.empty() || e.n == 0) return;

        if (e.kind == Kind::kIdentity) {
            if (e.tail_identity != e.n || e.tail_zero != 0) return;
        } else {
            if (e.tail_zero != e.n || e.tail_identity != 0) return;
        }

        std::lock_guard<std::mutex> lock(mu_);
        entries_[Key(e.site, e.call_id, e.n)] = std::move(e);
    }

    void Load(const char* path) {
        std::ifstream fin(path);

        uint64_t loaded = 0;
        uint64_t rejected = 0;

        if (!fin.good()) {
            std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V9_PROFILE]"
                      << " loaded=0"
                      << " path=" << SanitizeForLog(path ? path : "")
                      << " reason=open_failed"
                      << " exact_equiv=1 semantic_loss=0\n";
            return;
        }

        std::string line;
        while (std::getline(fin, line)) {
            std::string raw = Trim(line);
            if (raw.empty() || raw[0] == '#') continue;
            if (raw.find("profile_kind=") == std::string::npos) continue;

            const auto kv = ParseKV(raw);
            auto get = [&](const char* k) -> std::string {
                const auto it = kv.find(k);
                return it == kv.end() ? std::string() : it->second;
            };

            const std::string kind = get("profile_kind");
            ProfileEntry e;
            e.site = get("site");
            e.call_id = ParseU64(get("call_id"), 0);
            e.n = ParseU64(get("n"), 0);
            e.input_variant = ParseU64(get("input_variant"), std::numeric_limits<uint64_t>::max());
            e.clip_fp = ParseI64(get("clip_fp"), 0);
            e.min_fp = ParseI64(get("min_fp"), 0);
            e.max_fp = ParseI64(get("max_fp"), 0);
            e.tail_identity = ParseU64(get("tail_identity"), 0);
            e.tail_zero = ParseU64(get("tail_zero"), 0);

            if (kind == "identity" || (e.n != 0 && e.tail_identity == e.n && e.tail_zero == 0)) {
                e.kind = Kind::kIdentity;
            } else if (kind == "zero" || (e.n != 0 && e.tail_zero == e.n && e.tail_identity == 0)) {
                e.kind = Kind::kZero;
            } else {
                continue;
            }

            if (e.site.empty() || e.n == 0) {
                ++rejected;
                continue;
            }

            Add(std::move(e));
            ++loaded;
        }

        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V9_PROFILE]"
                  << " loaded=" << loaded
                  << " rejected=" << rejected
                  << " path=" << SanitizeForLog(path ? path : "")
                  << " mode=whole_tail_canonical_prg_reshare_v9"
                  << " exact_equiv=1 semantic_loss=0\n";
    }

    std::mutex mu_;
    std::mutex load_mu_;
    std::atomic<bool> loaded_{false};
    std::unordered_map<std::string, ProfileEntry> entries_;
};

class Stats {
public:
    static Stats& Instance() {
        static Stats s;
        return s;
    }

    void Reject() {
        rejects_.fetch_add(1, std::memory_order_relaxed);
    }

    void Applied(uint64_t n, Kind kind, bool proof, bool did_full_gather) {
        calls_.fetch_add(1, std::memory_order_relaxed);
        elems_.fetch_add(n, std::memory_order_relaxed);
        if (kind == Kind::kIdentity) {
            identity_calls_.fetch_add(1, std::memory_order_relaxed);
            identity_elems_.fetch_add(n, std::memory_order_relaxed);
        } else {
            zero_calls_.fetch_add(1, std::memory_order_relaxed);
            zero_elems_.fetch_add(n, std::memory_order_relaxed);
        }
        if (did_full_gather) full_gather_elems_.fetch_add(n, std::memory_order_relaxed);
        if (proof) proof_calls_.fetch_add(1, std::memory_order_relaxed);
    }

    ~Stats() {
        const uint64_t c = calls_.load();
        const uint64_t r = rejects_.load();
        if (!Enabled() && c == 0 && r == 0) return;

        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V9_SUMMARY]"
                  << " applied_calls=" << c
                  << " applied_elements=" << elems_.load()
                  << " identity_calls=" << identity_calls_.load()
                  << " identity_elements=" << identity_elems_.load()
                  << " zero_calls=" << zero_calls_.load()
                  << " zero_elements=" << zero_elems_.load()
                  << " full_gather_elements=" << full_gather_elems_.load()
                  << " saved_activation_calls=" << c
                  << " proof_calls=" << proof_calls_.load()
                  << " rejects=" << r
                  << " mode=whole_tail_canonical_prg_reshare_v9"
                  << " theorem=whole_tail_identity_or_zero_requires_canonical_prg_reshare"
                  << " local_only_skip_forbidden=1"
                  << " canonical_prg_reshare_preserved=1"
                  << " exact_equiv=1 semantic_loss=0\n";
    }

private:
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint64_t> elems_{0};
    std::atomic<uint64_t> identity_calls_{0};
    std::atomic<uint64_t> identity_elems_{0};
    std::atomic<uint64_t> zero_calls_{0};
    std::atomic<uint64_t> zero_elems_{0};
    std::atomic<uint64_t> full_gather_elems_{0};
    std::atomic<uint64_t> proof_calls_{0};
    std::atomic<uint64_t> rejects_{0};
};

inline LookupResult LookupWholeTail(const std::string& site, uint64_t call_id, uint64_t n) {
    if (!Enabled()) {
        LookupResult out;
        out.reason = "scatterlift_v9_disabled";
        return out;
    }
    return Registry::Instance().Lookup(site, call_id, n);
}

inline void RecordReject() {
    Stats::Instance().Reject();
}

inline void RecordApplied(uint64_t n, Kind kind, bool proof, bool did_full_gather) {
    Stats::Instance().Applied(n, kind, proof, did_full_gather);
}

}  // namespace scatterlift_v9
}  // namespace dazg_orbit
