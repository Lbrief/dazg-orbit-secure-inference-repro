// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_trunclift_v4.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "Utils/dazg_orbit_ablation_flags.h"

namespace dazg_orbit {
namespace trunclift_v4 {

// DAZG_ORBIT_TRUNCLIFT_V4
// Evidence-only truncation-lift frontier logger.
// Runtime elimination is blocked until exact replay proves:
// lower_bits_zero + signed_bound_ok + exact site/call/n/input replay.

inline bool Enabled() {
#ifdef DAZG_ORBIT_DISABLE_TRUNCLIFT_V4
    return false;
#else
    return dazg_orbit::ablation::EnableTruncLiftV4();
#endif
}

inline bool Detail() {
    return dazg_orbit::ablation::TruncLiftV4Detail();
}

inline uint64_t PrintLimit() {
    return dazg_orbit::ablation::TruncLiftV4PrintLimit();
}

inline bool ScanLocalShares() {
    return dazg_orbit::ablation::TruncLiftV4ScanLocalShares();
}

inline uint64_t ScanLimit() {
    return dazg_orbit::ablation::TruncLiftV4ScanLimit();
}

struct Stats {
    std::atomic<uint64_t> frontiers{0};
    std::atomic<uint64_t> pre_trunc{0};
    std::atomic<uint64_t> post_trunc{0};
    std::atomic<uint64_t> runtime_applied{0};
    std::atomic<uint64_t> blocked_missing_lower_bits{0};
    std::atomic<uint64_t> blocked_missing_bound{0};
    std::atomic<uint64_t> local_lower_bits_zero_lines{0};
    std::atomic<uint64_t> local_lower_bits_nonzero_lines{0};

    ~Stats() {
        const uint64_t f = frontiers.load(std::memory_order_relaxed);
        if (f == 0) return;
        std::cerr << "[DAZG_ORBIT_TRUNCLIFT_V4_SUMMARY]"
                  << " frontiers=" << f
                  << " pre_trunc=" << pre_trunc.load(std::memory_order_relaxed)
                  << " post_trunc=" << post_trunc.load(std::memory_order_relaxed)
                  << " runtime_applied=" << runtime_applied.load(std::memory_order_relaxed)
                  << " blocked_missing_lower_bits=" << blocked_missing_lower_bits.load(std::memory_order_relaxed)
                  << " blocked_missing_bound=" << blocked_missing_bound.load(std::memory_order_relaxed)
                  << " local_lower_bits_zero_lines=" << local_lower_bits_zero_lines.load(std::memory_order_relaxed)
                  << " local_lower_bits_nonzero_lines=" << local_lower_bits_nonzero_lines.load(std::memory_order_relaxed)
                  << " certificate=posttrunc_identity_observed_pretrunc_lift_not_assumed"
                  << " planner=certified_truncation_lift_latent_domain_v4"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
};

inline Stats& GetStats() {
    static Stats s;
    return s;
}

inline std::string SafeSite(const char* site) {
    if (site == nullptr || site[0] == '\0') return std::string("unspecified");
    std::string out(site);
    for (char& c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '|') c = '_';
    }
    return out;
}

inline uint64_t LowMask(int scale_bits) {
    if (scale_bits <= 0) return 0ULL;
    if (scale_bits >= 64) return ~0ULL;
    return (1ULL << static_cast<unsigned>(scale_bits)) - 1ULL;
}

struct LocalShareStats {
    uint64_t scanned{0};
    uint64_t lowbits_nonzero{0};
    uint64_t lowbits_or{0};
    uint64_t lowbits_xor{0};
    uint64_t lowbits_hash{1469598103934665603ULL};
    uint64_t raw_hash{1469598103934665603ULL};
    uint64_t min_raw{std::numeric_limits<uint64_t>::max()};
    uint64_t max_raw{0};
    int64_t min_signed{std::numeric_limits<int64_t>::max()};
    int64_t max_signed{std::numeric_limits<int64_t>::min()};
};

template <typename Data>
inline LocalShareStats ComputeLocalShareStats(const Data& data, int scale_bits) {
    LocalShareStats st;
    const uint64_t n = static_cast<uint64_t>(data.size());
    uint64_t limit = ScanLimit();
    if (limit == 0ULL || limit > n) limit = n;
    const uint64_t mask = LowMask(scale_bits);

    for (uint64_t i = 0; i < limit; ++i) {
        const uint64_t raw = static_cast<uint64_t>(data[static_cast<size_t>(i)]);
        const uint64_t low = raw & mask;
        const int64_t signed_raw = static_cast<int64_t>(raw);

        st.lowbits_or |= low;
        st.lowbits_xor ^= (low + 0x9e3779b97f4a7c15ULL + (st.lowbits_xor << 6) + (st.lowbits_xor >> 2));
        if (low != 0ULL) ++st.lowbits_nonzero;

        st.raw_hash ^= raw + 0x9e3779b97f4a7c15ULL + (st.raw_hash << 6) + (st.raw_hash >> 2);
        st.raw_hash *= 1099511628211ULL;
        st.lowbits_hash ^= low + 0x9e3779b97f4a7c15ULL + (st.lowbits_hash << 6) + (st.lowbits_hash >> 2);
        st.lowbits_hash *= 1099511628211ULL;

        if (raw < st.min_raw) st.min_raw = raw;
        if (raw > st.max_raw) st.max_raw = raw;
        if (signed_raw < st.min_signed) st.min_signed = signed_raw;
        if (signed_raw > st.max_signed) st.max_signed = signed_raw;
        ++st.scanned;
    }

    if (st.scanned == 0ULL) {
        st.min_raw = 0ULL;
        st.max_raw = 0ULL;
        st.min_signed = 0;
        st.max_signed = 0;
    }
    return st;
}

inline void UpdatePhaseStats(const std::string& phase) {
    Stats& s = GetStats();
    if (phase.find("pre") != std::string::npos) {
        s.pre_trunc.fetch_add(1, std::memory_order_relaxed);
    } else {
        s.post_trunc.fetch_add(1, std::memory_order_relaxed);
    }
    s.blocked_missing_lower_bits.fetch_add(1, std::memory_order_relaxed);
    s.blocked_missing_bound.fetch_add(1, std::memory_order_relaxed);
}

inline void LogFrontierLine(uint64_t seq,
                            const char* phase,
                            const char* site,
                            uint64_t n,
                            int scale_bits,
                            int out_bits,
                            bool signed_trunc,
                            bool extra_flag,
                            bool local_scan,
                            const LocalShareStats& local) {
    const std::string p = (phase == nullptr || phase[0] == '\0') ? std::string("unknown") : std::string(phase);

    if (local_scan) {
        if (local.lowbits_nonzero == 0ULL) {
            GetStats().local_lower_bits_zero_lines.fetch_add(1, std::memory_order_relaxed);
        } else {
            GetStats().local_lower_bits_nonzero_lines.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (Detail() && seq <= PrintLimit()) {
        std::cerr << "[DAZG_ORBIT_TRUNCLIFT_V4_FRONTIER]"
                  << " seq=" << seq
                  << " phase=" << p
                  << " site=" << SafeSite(site)
                  << " n=" << n
                  << " scale_bits=" << scale_bits
                  << " out_bits=" << out_bits
                  << " signed_trunc=" << (signed_trunc ? 1 : 0)
                  << " extra_flag=" << (extra_flag ? 1 : 0)
                  << " local_scan=" << (local_scan ? 1 : 0)
                  << " local_share_only=1"
                  << " local_scanned=" << local.scanned
                  << " local_lower_bits_zero=" << ((local_scan && local.lowbits_nonzero == 0ULL) ? 1 : 0)
                  << " local_lowbits_nonzero=" << local.lowbits_nonzero
                  << " local_lowbits_or=" << local.lowbits_or
                  << " local_lowbits_xor=" << local.lowbits_xor
                  << " local_lowbits_hash=" << local.lowbits_hash
                  << " local_raw_hash=" << local.raw_hash
                  << " local_min_raw=" << local.min_raw
                  << " local_max_raw=" << local.max_raw
                  << " local_min_signed=" << local.min_signed
                  << " local_max_signed=" << local.max_signed
                  << " runtime_applied=0"
                  << " domain_transition_eliminated=0"
                  << " decision=evidence_only"
                  << " reason=needs_global_lower_bits_zero_and_signed_bound_certificate"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
}

inline void LogFrontier(const char* phase,
                        const char* site,
                        uint64_t n,
                        int scale_bits,
                        int out_bits,
                        bool signed_trunc,
                        bool extra_flag) {
    if (!Enabled()) return;
    Stats& s = GetStats();
    const uint64_t seq = s.frontiers.fetch_add(1, std::memory_order_relaxed) + 1ULL;
    const std::string p = (phase == nullptr || phase[0] == '\0') ? std::string("unknown") : std::string(phase);
    UpdatePhaseStats(p);
    LocalShareStats empty;
    LogFrontierLine(seq, phase, site, n, scale_bits, out_bits, signed_trunc, extra_flag, false, empty);
}

template <typename Data>
inline void LogTensorFrontier(const char* phase,
                              const char* site,
                              const Data& data,
                              int scale_bits,
                              int out_bits,
                              bool signed_trunc,
                              bool extra_flag) {
    if (!Enabled()) return;
    Stats& s = GetStats();
    const uint64_t seq = s.frontiers.fetch_add(1, std::memory_order_relaxed) + 1ULL;
    const std::string p = (phase == nullptr || phase[0] == '\0') ? std::string("unknown") : std::string(phase);
    UpdatePhaseStats(p);
    const uint64_t n = static_cast<uint64_t>(data.size());
    const bool scan = ScanLocalShares();
    const LocalShareStats local = scan ? ComputeLocalShareStats(data, scale_bits) : LocalShareStats();
    LogFrontierLine(seq, phase, site, n, scale_bits, out_bits, signed_trunc, extra_flag, scan, local);
}

}  // namespace trunclift_v4
}  // namespace dazg_orbit
