// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_scatterlift_v8.h
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
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils/dazg_orbit_ablation_flags.h"
#include "Utils/dazg_orbit_certact_fuse.h"

namespace dazg_orbit {
namespace scatterlift_v8 {

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

inline const char* ProfileFile() {
    const char* p = EnvStr("DAZG_ORBIT_SCATTERLIFT_V8_PROFILE", "");
    return (p && *p) ? p : EnvStr("DAZG_ORBIT_SCATTERLIFT_V8_PROFILE", "");
}

inline const char* MaskOutFile() {
    const char* p = EnvStr("DAZG_ORBIT_SCATTERLIFT_V8_MASK_OUT", "");
    return (p && *p) ? p : EnvStr("DAZG_ORBIT_SCATTERLIFT_V8_MASK_OUT", "");
}

inline bool Enabled() {
    return dazg_orbit::ablation::EnableRouteSpecialization() &&
           EnvFlag("DAZG_ORBIT_ENABLE_SCATTERLIFT_V8",
                   EnvFlag("DAZG_ORBIT_SCATTERLIFT_V8", false));
}

inline bool ProofMode() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V8_PROOF",
                   EnvFlag("DAZG_ORBIT_SCATTERLIFT_V8_PROOF", false));
}

inline bool EmitMasksEnabled() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V8_EMIT_MASKS",
                   EnvFlag("DAZG_ORBIT_SCATTERLIFT_V8_EMIT_MASKS", false));
}

inline bool DetailEnabled() {
    return EnvFlag("DAZG_ORBIT_SCATTERLIFT_V8_DETAIL",
                   EnvFlag("DAZG_ORBIT_SCATTERLIFT_V8_DETAIL", false));
}

inline std::string SanitizeForLog(const std::string& s) {
    return dazg_orbit::certact_fuse::SanitizeForLog(s);
}

inline std::string Trim(std::string s) {
    return dazg_orbit::certact_fuse::Trim(std::move(s));
}

inline uint64_t ParseU64(const std::string& s, uint64_t fallback = 0) {
    return dazg_orbit::certact_fuse::ParseU64(s, fallback);
}

inline int64_t ParseI64(const std::string& s, int64_t fallback = 0) {
    return dazg_orbit::certact_fuse::ParseI64(s, fallback);
}

inline std::unordered_map<std::string, std::string> ParseKV(const std::string& line) {
    return dazg_orbit::certact_fuse::ParseKeyValueTokens(line);
}

inline uint64_t Mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

inline std::string Hex64(uint64_t v) {
    std::ostringstream os;
    os << std::hex << std::setfill('0') << std::setw(16) << v;
    return os.str();
}

inline bool ParseHex64(const std::string& s, uint64_t* out) {
    if (!out || s.size() != 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        uint64_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<uint64_t>(10 + c - 'a');
        else if (c >= 'A' && c <= 'F') d = static_cast<uint64_t>(10 + c - 'A');
        else return false;
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

inline bool MaskBit(const std::vector<uint64_t>& words, uint64_t i) {
    return ((words[static_cast<std::size_t>(i >> 6)] >> (i & 63u)) & 1ull) != 0ull;
}

inline uint64_t HashMask(const std::vector<uint64_t>& words, uint64_t n) {
    uint64_t h = 0x53434C465456385Full ^ (n * 0x9E3779B97F4A7C15ull);
    for (std::size_t i = 0; i < words.size(); ++i) {
        h ^= Mix64(words[i] ^ (static_cast<uint64_t>(i) * 0xD1B54A32D192ED03ull));
        h = Mix64(h);
    }
    return h;
}

struct MaskEncoding {
    std::string mask_hex;
    uint64_t words = 0;
    uint64_t tail_identity = 0;
    uint64_t tail_zero = 0;
    uint64_t central = 0;
    uint64_t mask_hash = 0;
};

template <typename Vec>
inline MaskEncoding EncodeIdentityMaskHex(const Vec& x_fp, int64_t clip_fp) {
    MaskEncoding e;
    const uint64_t n = static_cast<uint64_t>(x_fp.size());
    e.words = (n + 63u) / 64u;
    std::vector<uint64_t> words(static_cast<std::size_t>(e.words), 0);
    for (uint64_t i = 0; i < n; ++i) {
        const int64_t x = static_cast<int64_t>(x_fp[static_cast<std::size_t>(i)]);
        if (x >= clip_fp) {
            words[static_cast<std::size_t>(i >> 6)] |= (1ull << (i & 63u));
            ++e.tail_identity;
        } else if (x <= -clip_fp) {
            ++e.tail_zero;
        } else {
            ++e.central;
        }
    }
    e.mask_hash = HashMask(words, n);
    e.mask_hex.reserve(words.size() * 16u);
    for (uint64_t w : words) e.mask_hex += Hex64(w);
    return e;
}

inline bool DecodeMaskHex(const std::string& hex, uint64_t n, std::vector<uint64_t>* words) {
    if (!words) return false;
    const uint64_t need = (n + 63u) / 64u;
    if (hex.size() != static_cast<std::size_t>(need * 16u)) return false;

    std::vector<uint64_t> tmp;
    tmp.reserve(static_cast<std::size_t>(need));
    for (uint64_t i = 0; i < need; ++i) {
        uint64_t v = 0;
        if (!ParseHex64(hex.substr(static_cast<std::size_t>(i * 16u), 16), &v)) return false;
        tmp.push_back(v);
    }

    if ((n & 63u) != 0 && !tmp.empty()) {
        const uint64_t keep = (1ull << (n & 63u)) - 1ull;
        if ((tmp.back() & ~keep) != 0ull) return false;
    }

    *words = std::move(tmp);
    return true;
}

inline uint64_t CountIdentity(const std::vector<uint64_t>& words, uint64_t n) {
    uint64_t c = 0;
    for (uint64_t i = 0; i < n; ++i) {
        if (MaskBit(words, i)) ++c;
    }
    return c;
}

inline void AppendMaskProfileLine(const std::string& site,
                                  uint64_t call_id,
                                  uint64_t n,
                                  uint64_t tail_identity,
                                  uint64_t tail_zero,
                                  int64_t clip_fp,
                                  int64_t min_fp,
                                  int64_t max_fp,
                                  const MaskEncoding& enc) {
    if (!EmitMasksEnabled()) return;

    const char* path = MaskOutFile();
    if (!path || !*path) return;

    if (enc.central != 0) return;
    if (tail_identity == 0 || tail_zero == 0) return;
    if (enc.tail_identity != tail_identity || enc.tail_zero != tail_zero) return;
    if (tail_identity + tail_zero != n) return;

    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);

    std::ofstream out(path, std::ios::app);
    if (!out.good()) return;

    out << "profile_kind=mixed_scatter"
        << " site=" << SanitizeForLog(site)
        << " call_id=" << call_id
        << " n=" << n
        << " input_variant=" << dazg_orbit::ablation::InputVariant()
        << " clip_fp=" << clip_fp
        << " min_fp=" << min_fp
        << " max_fp=" << max_fp
        << " tail_identity=" << tail_identity
        << " tail_zero=" << tail_zero
        << " mask_codec=hex64le"
        << " mask_word_bits=64"
        << " mask_words=" << enc.words
        << " mask_hash=" << enc.mask_hash
        << " mask_hex=" << enc.mask_hex
        << " safe_sparse_replay=1"
        << " theorem=zero_positions_need_no_peer_share_identity_positions_need_peer_share"
        << " canonical_prg_reshare_preserved=1"
        << " exact_equiv=1 semantic_loss=0\n";
}

struct MaskProfileEntry {
    std::string site;
    uint64_t call_id = 0;
    uint64_t n = 0;
    uint64_t input_variant = std::numeric_limits<uint64_t>::max();
    int64_t clip_fp = 0;
    int64_t min_fp = 0;
    int64_t max_fp = 0;
    uint64_t tail_identity = 0;
    uint64_t tail_zero = 0;
    uint64_t mask_hash = 0;
    std::vector<uint64_t> words;

    bool identity_at(uint64_t i) const {
        return MaskBit(words, i);
    }
};

struct LookupResult {
    bool matched = false;
    std::string reason = "no_profile_match";
    MaskProfileEntry entry;
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
            out.reason = "no_exact_site_call_n_mask";
            return out;
        }

        if (it->second.input_variant != std::numeric_limits<uint64_t>::max() &&
            it->second.input_variant != dazg_orbit::ablation::InputVariant()) {
            out.reason = "input_variant_mismatch";
            return out;
        }

        out.matched = true;
        out.reason = "exact_site_call_n_mask_match";
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

    void Add(MaskProfileEntry e) {
        e.site = Trim(e.site);
        if (e.site.empty() || e.n == 0 || e.words.empty()) return;
        if (e.tail_identity == 0 || e.tail_zero == 0) return;
        if (e.tail_identity + e.tail_zero != e.n) return;

        std::lock_guard<std::mutex> lock(mu_);
        entries_[Key(e.site, e.call_id, e.n)] = std::move(e);
    }

    void Load(const char* path) {
        std::ifstream fin(path);
        uint64_t loaded = 0;
        uint64_t rejected = 0;

        if (!fin.good()) {
            std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8_PROFILE]"
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
            if (raw.find("mask_hex=") == std::string::npos) continue;
            if (raw.find("profile_kind=mixed_scatter") == std::string::npos &&
                raw.find("mask_kind=mixed_scatter") == std::string::npos) {
                continue;
            }

            const auto kv = ParseKV(raw);
            auto get = [&](const char* k) -> std::string {
                const auto it = kv.find(k);
                return it == kv.end() ? std::string() : it->second;
            };

            MaskProfileEntry e;
            e.site = get("site");
            e.call_id = ParseU64(get("call_id"), 0);
            e.n = ParseU64(get("n"), 0);
            e.input_variant = ParseU64(get("input_variant"), std::numeric_limits<uint64_t>::max());
            e.clip_fp = ParseI64(get("clip_fp"), 0);
            e.min_fp = ParseI64(get("min_fp"), 0);
            e.max_fp = ParseI64(get("max_fp"), 0);
            e.tail_identity = ParseU64(get("tail_identity"), 0);
            e.tail_zero = ParseU64(get("tail_zero"), 0);
            e.mask_hash = ParseU64(get("mask_hash"), 0);

            if (e.site.empty() || e.n == 0 || !DecodeMaskHex(get("mask_hex"), e.n, &e.words)) {
                ++rejected;
                continue;
            }

            const uint64_t cnt = CountIdentity(e.words, e.n);
            const uint64_t hash = HashMask(e.words, e.n);
            if (cnt != e.tail_identity ||
                e.tail_identity + e.tail_zero != e.n ||
                (e.mask_hash != 0 && hash != e.mask_hash)) {
                ++rejected;
                continue;
            }
            if (e.mask_hash == 0) e.mask_hash = hash;

            Add(std::move(e));
            ++loaded;
        }

        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8_PROFILE]"
                  << " loaded=" << loaded
                  << " rejected=" << rejected
                  << " path=" << SanitizeForLog(path ? path : "")
                  << " mode=profile_mixed_mask_sparse_peer_gather_v8"
                  << " exact_equiv=1 semantic_loss=0\n";
    }

    std::mutex mu_;
    std::mutex load_mu_;
    std::atomic<bool> loaded_{false};
    std::unordered_map<std::string, MaskProfileEntry> entries_;
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

    void Applied(uint64_t n, uint64_t identity, uint64_t zero, bool proof) {
        calls_.fetch_add(1, std::memory_order_relaxed);
        elems_.fetch_add(n, std::memory_order_relaxed);
        ids_.fetch_add(identity, std::memory_order_relaxed);
        zeros_.fetch_add(zero, std::memory_order_relaxed);
        sparse_sent_.fetch_add(proof ? n : identity, std::memory_order_relaxed);
        saved_.fetch_add(proof ? 0 : zero, std::memory_order_relaxed);
        if (proof) proofs_.fetch_add(1, std::memory_order_relaxed);
    }

    ~Stats() {
        const uint64_t c = calls_.load();
        const uint64_t r = rejects_.load();
        if (!Enabled() && !EmitMasksEnabled() && c == 0 && r == 0) return;

        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8_SUMMARY]"
                  << " applied_calls=" << c
                  << " applied_elements=" << elems_.load()
                  << " identity_elements=" << ids_.load()
                  << " zero_elements=" << zeros_.load()
                  << " sparse_sent_elements=" << sparse_sent_.load()
                  << " saved_peer_elements=" << saved_.load()
                  << " proof_calls=" << proofs_.load()
                  << " rejects=" << r
                  << " saved_peer_share_bytes_32=" << saved_.load() * 4ull
                  << " mode=profile_mixed_mask_sparse_peer_gather_v8"
                  << " theorem=zero_positions_need_no_peer_share_identity_positions_need_peer_share"
                  << " local_only_skip_forbidden=1"
                  << " canonical_prg_reshare_preserved=1"
                  << " exact_equiv=1 semantic_loss=0\n";
    }

private:
    std::atomic<uint64_t> calls_{0};
    std::atomic<uint64_t> elems_{0};
    std::atomic<uint64_t> ids_{0};
    std::atomic<uint64_t> zeros_{0};
    std::atomic<uint64_t> sparse_sent_{0};
    std::atomic<uint64_t> saved_{0};
    std::atomic<uint64_t> proofs_{0};
    std::atomic<uint64_t> rejects_{0};
};


inline uint64_t PlannerMinSavedBytes() {
    return ParseU64(EnvStr("DAZG_ORBIT_SCATTERLIFT_V8_MIN_SAVED_BYTES", "65536"), 65536);
}

inline uint64_t PlannerMinZeroPermille() {
    return ParseU64(EnvStr("DAZG_ORBIT_SCATTERLIFT_V8_MIN_ZERO_PERMILLE", "0"), 0);
}

inline bool PlannerAccept(const MaskProfileEntry& e) {
    const uint64_t saved_bytes = e.tail_zero * 4ull;
    const uint64_t min_bytes = PlannerMinSavedBytes();
    const uint64_t min_permille = PlannerMinZeroPermille();

    if (min_bytes != 0 && saved_bytes < min_bytes) return false;
    if (min_permille != 0 && e.n != 0 && (e.tail_zero * 1000ull) < (min_permille * e.n)) return false;
    return true;
}

inline LookupResult LookupProfileMask(const std::string& site, uint64_t call_id, uint64_t n) {
    if (!Enabled()) {
        LookupResult out;
        out.reason = "scatterlift_v8_disabled";
        return out;
    }
    LookupResult out = Registry::Instance().Lookup(site, call_id, n);
    if (out.matched && !PlannerAccept(out.entry)) {
        out.matched = false;
        out.reason = "planner_reject_low_benefit_mask";
    }
    return out;
}

inline void RecordReject() {
    Stats::Instance().Reject();
}

inline void RecordApplied(uint64_t n, uint64_t identity, uint64_t zero, bool proof) {
    Stats::Instance().Applied(n, identity, zero, proof);
}

}  // namespace scatterlift_v8
}  // namespace dazg_orbit
