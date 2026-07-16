// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_profiler.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once
#include <iostream>
#include <string>

namespace dazg_orbit {

inline void LogRoute(
    const std::string& layer_id,
    const std::string& stage,
    int H, int Cin, int Cout, int K, int S,
    int layout_mode,
    bool compact_k1s2,
    bool polyphase_k3s2,
    bool exact_tiled_k3s1,
    const std::string& route,
    const std::string& benefit_class) 
{
    std::cerr
        << "[DAZG_ORBIT_ROUTE]"
        << " layer=" << layer_id
        << " stage=" << stage
        << " H=" << H
        << " Cin=" << Cin
        << " Cout=" << Cout
        << " K=" << K
        << " S=" << S
        << " layout_mode=" << layout_mode
        << " compact_k1s2=" << compact_k1s2
        << " polyphase_k3s2=" << polyphase_k3s2
        << " exact_tiled_k3s1=" << exact_tiled_k3s1
        << " route=" << route
        << " benefit_class=" << benefit_class
        << std::endl;
}

inline void LogPlan(
    const std::string& layer_id,
    int dense_packs,
    int zero_packs,
    int active_packs,
    int sparse_entries,
    int rotation_slots,
    int bsgs_groups,
    int nonempty_out_packs,
    long packweight_us) 
{
    std::cerr
        << "[DAZG_ORBIT_PLAN]"
        << " layer=" << layer_id
        << " dense_packs=" << dense_packs
        << " zero_packs=" << zero_packs
        << " active_packs=" << active_packs
        << " sparse_entries=" << sparse_entries
        << " rotation_slots=" << rotation_slots
        << " bsgs_groups=" << bsgs_groups
        << " nonempty_out_packs=" << nonempty_out_packs
        << " packweight_us=" << packweight_us
        << std::endl;
}

inline void LogRuntime(
    const std::string& layer_id,
    long hecompute_us,
    int mul_plain,
    int rotate_rows,
    int add_inplace,
    int tile_count,
    int first_real_product_init,
    int skipped_zero_plain,
    const std::string& schedule) 
{
    std::cerr
        << "[DAZG_ORBIT_RUNTIME]"
        << " layer=" << layer_id
        << " hecompute_us=" << hecompute_us
        << " mul_plain=" << mul_plain
        << " rotate_rows=" << rotate_rows
        << " add_inplace=" << add_inplace
        << " tile_count=" << tile_count
        << " first_real_product_init=" << first_real_product_init
        << " skipped_zero_plain=" << skipped_zero_plain
        << " schedule=" << schedule
        << std::endl;
}

inline void LogActivation(
    const std::string& site,
    int n,
    int pbs_calls,
    int pbs_saved,
    int bucket_bins,
    int bucket_reused,
    double active_ratio,
    double reuse_ratio) 
{
    std::cerr
        << "[DAZG_ORBIT_ACTIVATION]"
        << " site=" << site
        << " n=" << n
        << " pbs_calls=" << pbs_calls
        << " pbs_saved=" << pbs_saved
        << " bucket_bins=" << bucket_bins
        << " bucket_reused=" << bucket_reused
        << " active_ratio=" << active_ratio
        << " reuse_ratio=" << reuse_ratio
        << std::endl;
}

}  // namespace dazg_orbit
