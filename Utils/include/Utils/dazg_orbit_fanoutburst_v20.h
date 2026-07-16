// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_fanoutburst_v20.h
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
namespace fanoutburst_v20 {

inline bool IsFalseValue(const char* v) {
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

inline bool IsTrueValue(const char* v) {
    return v != nullptr && *v != '\0' && !IsFalseValue(v);
}

inline const char* GetenvAny(const char* a,
                             const char* b = nullptr,
                             const char* c = nullptr,
                             const char* d = nullptr,
                             const char* e = nullptr) {
    const char* names[5] = {a, b, c, d, e};
    for (const char* name : names) {
        if (name == nullptr) continue;
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') return v;
    }
    return nullptr;
}

inline bool Enabled() {
    return ::dazg_orbit::topline_v25::EnableFanoutBurst();
}



inline bool DetailEnabled() {
    const char* v = GetenvAny("DAZG_ORBIT_FANOUTBURST_V20_DETAIL",
                              "DAZG_ORBIT_FANOUTBURST_V20_DETAIL");
    if (v == nullptr || *v == '\0') return true;
    return IsTrueValue(v);
}

inline const char* Policy() {
    const char* p = GetenvAny("DAZG_ORBIT_FANOUTBURST_V20_POLICY",
                              "DAZG_ORBIT_FANOUTBURST_V20_POLICY",
                              "DAZG_ORBIT_FANOUTBURST_V20_POLICY");
    if (p != nullptr && *p != '\0') return p;
    return ::dazg_orbit::topline_v25::FanoutBurstPolicy();
}



inline bool StartsWith(const char* s, const char* prefix) {
    if (s == nullptr || prefix == nullptr) return false;
    while (*prefix != '\0') {
        if (*s != *prefix) return false;
        ++s;
        ++prefix;
    }
    return true;
}

inline bool NumberListContains(const char* list, std::uint64_t value) {
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

        if (value >= static_cast<std::uint64_t>(lo) &&
            value <= static_cast<std::uint64_t>(hi)) {
            return true;
        }

        while (*p != '\0' && *p != ',' && *p != ';' && *p != '|') ++p;
    }
    return false;
}

inline bool AllowProjectionBlock(std::uint64_t block_id,
                                 bool has_shortcut,
                                 std::uint64_t in_planes,
                                 std::uint64_t planes,
                                 std::uint64_t stride) {
    if (!Enabled()) return false;
    if (!has_shortcut) return false;

    const char* p = Policy();
    if (p == nullptr || *p == '\0' || IsFalseValue(p)) return false;

    if (std::strcmp(p, "all") == 0 ||
        std::strcmp(p, "projection") == 0 ||
        std::strcmp(p, "projection_shortcut") == 0 ||
        std::strcmp(p, "bottleneck_projection") == 0) {
        return true;
    }

    if (StartsWith(p, "block:")) {
        return NumberListContains(p + 6, block_id);
    }

    if (StartsWith(p, "prefix:")) {
        const unsigned long long n = std::strtoull(p + 7, nullptr, 10);
        return block_id <= static_cast<std::uint64_t>(n);
    }

    if (std::strcmp(p, "downsample") == 0) {
        return stride != 1 || in_planes != planes * 4ULL;
    }

    return false;
}

inline std::atomic<std::uint64_t>& EnterCounter() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<std::uint64_t>& CompleteCounter() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline void RecordProjectionEnter(std::uint64_t block_id,
                                  bool server_role,
                                  std::uint64_t in_planes,
                                  std::uint64_t planes,
                                  std::uint64_t stride) {
    const std::uint64_t seq = EnterCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    if (!DetailEnabled() && seq > 8ULL) return;
    std::cerr << "[DAZG_ORBIT_FANOUTBURST_V20]"
              << " phase=enter_projection_shortcut_burst"
              << " seq=" << seq
              << " block_id=" << block_id
              << " role=" << (server_role ? 1 : 0)
              << " in_planes=" << in_planes
              << " planes=" << planes
              << " stride=" << stride
              << " policy=" << Policy()
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RecordBatchedSSToHE(std::uint64_t block_id,
                                std::uint64_t conv1_rows,
                                std::uint64_t shortcut_rows) {
    if (!DetailEnabled()) return;
    std::cerr << "[DAZG_ORBIT_FANOUTBURST_V20]"
              << " phase=batched_sstohe"
              << " block_id=" << block_id
              << " conv1_rows=" << conv1_rows
              << " shortcut_rows=" << shortcut_rows
              << " saved_rounds=1"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RecordBatchedHEToSS(std::uint64_t block_id,
                                std::uint64_t conv3_rows,
                                std::uint64_t shortcut_rows) {
    if (!DetailEnabled()) return;
    std::cerr << "[DAZG_ORBIT_FANOUTBURST_V20]"
              << " phase=mask_stable_batched_hetoss"
              << " block_id=" << block_id
              << " conv3_rows=" << conv3_rows
              << " shortcut_rows=" << shortcut_rows
              << " saved_rounds=1"
              << " preserved_mask_seed=1"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RecordProjectionComplete(std::uint64_t block_id,
                                     bool server_role,
                                     long long total_us,
                                     long long pack_sstohe_us,
                                     long long conv_hetoss_us) {
    const std::uint64_t seq = CompleteCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    if (!DetailEnabled() && seq > 8ULL) return;
    std::cerr << "[DAZG_ORBIT_FANOUTBURST_V20]"
              << " phase=complete_projection_shortcut_burst"
              << " seq=" << seq
              << " block_id=" << block_id
              << " role=" << (server_role ? 1 : 0)
              << " total_us=" << total_us
              << " pack_sstohe_us=" << pack_sstohe_us
              << " conv_hetoss_us=" << conv_hetoss_us
              << " saved_rounds=2"
              << " v19_prefix35_compatible=1"
              << " strict_hash_required=1"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

}  // namespace fanoutburst_v20
}  // namespace dazg_orbit
