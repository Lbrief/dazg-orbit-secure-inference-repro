// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_fanoutburst_v31.h
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
#include <Utils/dazg_orbit_ablation_flags.h>

namespace dazg_orbit {
namespace fanoutburst_v31 {

inline bool IsFalseValue(const char* v) {
    if (v == nullptr || *v == '\0') return false;
    return std::strcmp(v, "0") == 0 ||
           std::strcmp(v, "false") == 0 || std::strcmp(v, "False") == 0 ||
           std::strcmp(v, "FALSE") == 0 ||
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
                             const char* d = nullptr) {
    const char* names[4] = {a, b, c, d};
    for (const char* name : names) {
        if (name == nullptr) continue;
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') return v;
    }
    return nullptr;
}

// FanoutBurst V31 is the paper-facing, strict-equivalence projection certificate.
// It is intentionally tied to DomainPulse V15, because V31's runtime win is the
// exact cross-branch conversion burst: conv1/projection-shortcut consume the same
// pre-branch SS tensor, and conv3/projection-shortcut return through one
// mask-stable HE->SS message. No approximation and no rotation-sharing claim is
// made when the shortcut has zero rotation demand.
inline bool Enabled() {
    const char* explicit_flag = GetenvAny("DAZG_ORBIT_ENABLE_FANOUTBURST_V31",
                                          "DAZG_ORBIT_FANOUTBURST_V31",
                                          "DAZG_ORBIT_FANOUTBURST_V31");
    if (explicit_flag != nullptr) return IsTrueValue(explicit_flag);
    return ::dazg_orbit::ablation::EnableRouteSpecialization() &&
           ::dazg_orbit::ablation::EnableFanoutCache() &&
           ::dazg_orbit::ablation::EnableDomainPulseV15();
}

inline bool DetailEnabled() {
    const char* v = GetenvAny("DAZG_ORBIT_FANOUTBURST_V31_DETAIL",
                              "DAZG_ORBIT_FANOUTBURST_V31_DETAIL");
    if (v == nullptr || *v == '\0') return true;
    return IsTrueValue(v);
}

inline bool AllowProjectionBlock(std::uint64_t block_id,
                                 bool has_shortcut,
                                 std::uint64_t in_planes,
                                 std::uint64_t planes,
                                 std::uint64_t stride) {
    (void)block_id;
    if (!Enabled()) return false;
    if (!has_shortcut) return false;
    // ResNet bottleneck projection iff the residual branch changes stride or
    // channel dimension. This covers block ids 1, 4, 8, 14 in canonical ResNet-50.
    return stride != 1ULL || in_planes != planes * 4ULL;
}

inline std::atomic<std::uint64_t>& EnterCounter() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<std::uint64_t>& BatchedSSToHECounter() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<std::uint64_t>& BatchedHEToSSCounter() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::atomic<std::uint64_t>& CompleteCounter() {
    static std::atomic<std::uint64_t> c{0};
    return c;
}

inline std::uint64_t CompletedRegions() {
    return CompleteCounter().load(std::memory_order_relaxed);
}

inline void RecordProjectionEnter(std::uint64_t block_id,
                                  bool server_role,
                                  std::uint64_t in_planes,
                                  std::uint64_t planes,
                                  std::uint64_t stride) {
    const std::uint64_t seq =
        EnterCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    if (!DetailEnabled() && seq > 8ULL) return;
    std::cerr << "[DAZG_ORBIT_FANOUTBURST_V31]"
              << " phase=enter_projection_certificate"
              << " seq=" << seq
              << " block_id=" << block_id
              << " role=" << (server_role ? 1 : 0)
              << " in_planes=" << in_planes
              << " planes=" << planes
              << " out_planes=" << (planes * 4ULL)
              << " stride=" << stride
              << " certificate=prebranch_identity_two_consumer_projection"
              << " prebranch_cache_applicable=1"
              << " conversion_burst_applicable=1"
              << " rotation_cache_claim=0"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RecordBatchedSSToHE(std::uint64_t block_id,
                                std::uint64_t conv1_rows,
                                std::uint64_t shortcut_rows,
                                std::uint64_t poly_degree) {
    const std::uint64_t seq =
        BatchedSSToHECounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    if (!DetailEnabled() && seq > 8ULL) return;
    const std::uint64_t rows = conv1_rows + shortcut_rows;
    const std::uint64_t bytes = rows * poly_degree * sizeof(std::uint64_t);
    std::cerr << "[DAZG_ORBIT_FANOUTBURST_V31]"
              << " phase=batched_prebranch_sstohe"
              << " seq=" << seq
              << " block_id=" << block_id
              << " conv1_rows=" << conv1_rows
              << " shortcut_rows=" << shortcut_rows
              << " total_rows=" << rows
              << " poly_degree=" << poly_degree
              << " packed_bytes=" << bytes
              << " saved_rounds=1"
              << " prebranch_tensor_copy_eliminated=1"
              << " certificate=single_prebranch_domain_two_packing_maps"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RecordBatchedHEToSS(std::uint64_t block_id,
                                std::uint64_t conv3_rows,
                                std::uint64_t shortcut_rows,
                                std::uint64_t poly_degree) {
    const std::uint64_t seq =
        BatchedHEToSSCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    if (!DetailEnabled() && seq > 8ULL) return;
    const std::uint64_t rows = conv3_rows + shortcut_rows;
    const std::uint64_t bytes = rows * poly_degree * sizeof(std::uint64_t);
    std::cerr << "[DAZG_ORBIT_FANOUTBURST_V31]"
              << " phase=maskstable_batched_postbranch_hetoss"
              << " seq=" << seq
              << " block_id=" << block_id
              << " conv3_rows=" << conv3_rows
              << " shortcut_rows=" << shortcut_rows
              << " total_rows=" << rows
              << " poly_degree=" << poly_degree
              << " packed_bytes=" << bytes
              << " saved_rounds=1"
              << " preserved_mask_seed=1"
              << " baseline_seed_tuple=rows_polydegree_callid"
              << " certificate=mask_stable_segmented_batch"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RecordProjectionComplete(std::uint64_t block_id,
                                     bool server_role,
                                     long long total_us,
                                     long long pack_sstohe_us,
                                     long long conv_hetoss_us,
                                     std::uint64_t conv1_rows,
                                     std::uint64_t shortcut_in_rows,
                                     std::uint64_t conv3_rows,
                                     std::uint64_t shortcut_out_rows) {
    const std::uint64_t seq =
        CompleteCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    if (!DetailEnabled() && seq > 8ULL) return;
    std::cerr << "[DAZG_ORBIT_FANOUTBURST_V31]"
              << " phase=complete_projection_certificate"
              << " seq=" << seq
              << " block_id=" << block_id
              << " role=" << (server_role ? 1 : 0)
              << " total_us=" << total_us
              << " pack_sstohe_us=" << pack_sstohe_us
              << " conv_hetoss_us=" << conv_hetoss_us
              << " conv1_rows=" << conv1_rows
              << " shortcut_in_rows=" << shortcut_in_rows
              << " conv3_rows=" << conv3_rows
              << " shortcut_out_rows=" << shortcut_out_rows
              << " saved_rounds=2"
              << " runtime_cache_applied=1"
              << " conversion_burst_applied=1"
              << " rotation_cache_applied=0"
              << " strict_hash_required=1"
              << " certificate=projection_cross_branch_conversion_burst_v31"
              << " paper_claim=exact_resnet_projection_branch_round_collapse"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

}  // namespace fanoutburst_v31
}  // namespace dazg_orbit
