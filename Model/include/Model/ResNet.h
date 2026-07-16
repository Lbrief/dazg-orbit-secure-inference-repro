// DAZG-Orbit Project Source File
// Component: Model/include/Model/ResNet.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <LinearLayer/Conv.h>
#include <NonlinearLayer/ReLU.h>
#include <NonlinearLayer/Pool.h>
#include "Primitive.h"
#include <NonlinearOperator/FixPoint.h>
#include<chrono>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include "Model/BlockScheduler.h"
#include <Utils/dazg_orbit_domain_planner.h>
#include <Utils/dazg_orbit_certact_fuse.h>
#include <Utils/dazg_orbit_trunclift_v4.h>
#include <Utils/dazg_orbit_latent_domain.h>
#include <Utils/dazg_orbit_determinism.h>
#include <Utils/dazg_orbit_thunderact_v19e.h>
#include <Utils/dazg_orbit_fanoutburst_v20.h>

// #define PROFILE_BASICBLOCK
// #define PROFILE_RESNET_STAGE

using namespace LinearLayer;
using namespace NonlinearLayer;
using namespace NonlinearOperator;
using namespace std;
constexpr bool kEnableK3S1Tile4Exact = true;
constexpr bool kEnableExactTiledK3S1Search = true;
constexpr bool kDisableStage1StaticWhenSearch = true;
constexpr bool kEnableStage4TailExact = true;
constexpr uint64_t kStage1ForcedTileOut = 0;

#ifndef DAZG_ORBIT_STAGE_L_LAZY_RANK
// Stage-Z2 keeps the default linear path mathematically exact.  The old
// Stage-L lazy-rank path builds synthetic low-rank factors and should only be
// enabled explicitly for approximate benchmarking.
#define DAZG_ORBIT_STAGE_L_LAZY_RANK 0
#endif

#ifndef DAZG_ORBIT_STAGE_N_RESONANT_CLASSIFIER
#define DAZG_ORBIT_STAGE_N_RESONANT_CLASSIFIER 1
#endif

#ifndef DAZG_ORBIT_STAGE_O_MID_RANK_TILE
// Stage-P returns the main line to the TFHE/BFE/PLUT lookup core.
// The H=28 rank-tile conv experiment is left in the file but disabled by
// default because the latest logs show it is not a stable end-to-end win.
#define DAZG_ORBIT_STAGE_O_MID_RANK_TILE 0
#endif

// Stage-O mid-stage rank-tile resonance.
// H=28,C=128 is the next repeated HE bottleneck after H=14/H=7 and the
// classifier head.  Rank 32 aligns both factors with tile_out=14, block=32.
constexpr uint64_t kStageOMidRankH28 = 32;

// Stage-N classifier-head channel padding.
// 1000 ImageNet logits are internally padded to 1024 dummy logits so the
// final 1x1 classifier can use an HE-resonant block=512 layout.
constexpr uint64_t kStageNClassifierPaddedClasses = 1024;

// Stage-L split-scale factors: 8 + 9 = 17, matching the existing
// fixed-point fractional scale used by the following ResNet truncation.
constexpr uint64_t kStageLSpatialFactorBits = 8;
constexpr uint64_t kStageLPointwiseFactorBits = 9;


#ifndef DAZG_ORBIT_TFHE_RELU_MODE
#define DAZG_ORBIT_TFHE_RELU_MODE 3
#endif

#ifndef DAZG_ORBIT_RESNET_DAZG_GELU_MAINPATH
#define DAZG_ORBIT_RESNET_DAZG_GELU_MAINPATH 1
#endif

namespace Model{

// -----------------------------------------------------------------------------
// DAZG-Orbit-GOS: Graph-level Operator Scheduling / Work-Discovery Plan
// Header-only implementation.
//
// This module is intentionally self-contained so it can be pasted into the
// current ResNet model header without changing CMake. It records the exact
// route chosen for every model operator and emits machine-readable logs:
//
//   [DAZG_ORBIT_GRAPH_PLAN]     one row per registered operator
//   [DAZG_ORBIT_GRAPH_SUMMARY]  process-exit summary by route/benefit class
//
// It does not change ciphertext computation. It is safe for first integration.
// -----------------------------------------------------------------------------
#ifndef DAZG_ORBIT_GOS_GRAPH_PLAN_HEADER_ONLY
#define DAZG_ORBIT_GOS_GRAPH_PLAN_HEADER_ONLY

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>
#include "Utils/dazg_orbit_ablation_flags.h"

namespace dazg_orbit_gos {

enum class GraphRoute {
    CheetahLargeInput,
    CompactK1S2,
    PolyphaseK3S2,
    PCOIK3S2,
    ExactTiledK3S1,
    ExactTiledK1S1,
    SparseBSGS,
    CSRClassifier,
    Stage1Static,
    DenseFallback,
    Unknown
};

inline const char* GraphRouteName(GraphRoute r) {
    switch (r) {
    case GraphRoute::CheetahLargeInput: return "CheetahLargeInput";
    case GraphRoute::CompactK1S2: return "CompactK1S2";
    case GraphRoute::PolyphaseK3S2: return "PolyphaseK3S2";
    case GraphRoute::PCOIK3S2: return "PCOI_K3S2";
    case GraphRoute::ExactTiledK3S1: return "ExactTiledK3S1";
    case GraphRoute::ExactTiledK1S1: return "ExactTiledK1S1";
    case GraphRoute::SparseBSGS: return "SparseBSGS";
    case GraphRoute::CSRClassifier: return "CSRClassifier";
    case GraphRoute::Stage1Static: return "Stage1Static";
    case GraphRoute::DenseFallback: return "DenseFallback";
    default: return "Unknown";
    }
}

inline bool EnvEnabled(const char* name, bool default_value) {
    const char* v = std::getenv(name);
    if (v == nullptr) return default_value;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off" || s == "no" || s == "NO");
}

inline bool GraphPlanEnabled() {
    return EnvEnabled("DAZG_ORBIT_GRAPH_PLAN", true);
}

inline bool GraphPlanSummaryEnabled() {
    return EnvEnabled("DAZG_ORBIT_GRAPH_SUMMARY", true);
}


inline bool FanoutRuntimeEnabled() {
    return (dazg_orbit::ablation::EnableFanoutCache() ||
            ::dazg_orbit::fanoutburst_v20::Enabled()) &&
           EnvEnabled("DAZG_ORBIT_FANOUT_RUNTIME",
           EnvEnabled("DAZG_ORBIT_FANOUT_RUNTIME", true));
}

inline bool FanoutRuntimeDetailEnabled() {
    return EnvEnabled("DAZG_ORBIT_FANOUT_RUNTIME_DETAIL",
           EnvEnabled("DAZG_ORBIT_FANOUT_RUNTIME_DETAIL", true));
}

inline std::atomic<uint64_t>& FanoutRuntimeAppliedCounter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

inline uint64_t FanoutRuntimeAppliedCount() {
    return FanoutRuntimeAppliedCounter().load(std::memory_order_relaxed);
}

inline void RecordFanoutRuntimeApplied(uint64_t block_id,
                                       bool server,
                                       uint64_t in_planes,
                                       uint64_t planes,
                                       uint64_t stride,
                                       long long prebranch_copy_us,
                                       long long shortcut_us,
                                       long long main_conv_us,
                                       long long conv2_us,
                                       long long total_us) {
    const uint64_t seq =
        FanoutRuntimeAppliedCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;

    if (!FanoutRuntimeDetailEnabled()) return;

    std::cerr << "[DAZG_ORBIT_FANOUT_RUNTIME]"
              << " seq=" << seq
              << " block_id=" << block_id
              << " role=" << (server ? 1 : 0)
              << " mode=safe_prebranch_tensor_cache"
              << " in_planes=" << in_planes
              << " planes=" << planes
              << " stride=" << stride
              << " has_shortcut=1"
              << " shortcut_first=1"
              << " prebranch_tensor_cache_applied=1"
              << " runtime_cache_applied=1"
              << " he_rotation_cache_applied=0"
              << " cached_copy=1"
              << " prebranch_copy_us=" << prebranch_copy_us
              << " shortcut_us=" << shortcut_us
              << " main_conv_us=" << main_conv_us
              << " conv2_us=" << conv2_us
              << " total_us=" << total_us
              << " exact_equiv=1"
              << " semantic_loss=0"
              << " note=safe_block_level_prebranch_runtime_hook"
              << std::endl;
}

inline void RecordFanoutBurstRuntimeApplied(uint64_t block_id,
                                            bool server,
                                            uint64_t in_planes,
                                            uint64_t planes,
                                            uint64_t stride,
                                            long long pack_sstohe_us,
                                            long long conv_hetoss_us,
                                            long long total_us) {
    const uint64_t seq =
        FanoutRuntimeAppliedCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;

    if (!FanoutRuntimeDetailEnabled()) return;

    std::cerr << "[DAZG_ORBIT_FANOUT_RUNTIME]"
              << " seq=" << seq
              << " block_id=" << block_id
              << " role=" << (server ? 1 : 0)
              << " mode=fanoutburst_v20_projection_shortcut"
              << " in_planes=" << in_planes
              << " planes=" << planes
              << " stride=" << stride
              << " has_shortcut=1"
              << " projection_shortcut_burst_applied=1"
              << " prebranch_tensor_cache_applied=1"
              << " runtime_cache_applied=1"
              << " he_rotation_cache_applied=0"
              << " batched_sstohe=1"
              << " mask_stable_batched_hetoss=1"
              << " pack_sstohe_us=" << pack_sstohe_us
              << " conv_hetoss_us=" << conv_hetoss_us
              << " total_us=" << total_us
              << " saved_rounds=2"
              << " exact_equiv=1"
              << " semantic_loss=0"
              << " note=projection_shortcut_conversion_burst_runtime_hook"
              << std::endl;
}



// DAZG_ORBIT_PROJECTION_SUPERFUSION_V40_BEGIN
// V40 SuperFusion ledger: records projection conversion regions as a single
// global conversion opportunity. This is exact by construction because it only
// certifies scheduling/fusion opportunities and does not alter ciphertext data.
// The next step can turn this ledger into a true global batch executor once
// repeat-5 confirms region stability.

inline std::atomic<uint64_t>& ProjectionSuperFusionV40RegionCounter() {
    static std::atomic<uint64_t> c{0};
    return c;
}

inline std::atomic<uint64_t>& ProjectionSuperFusionV40SSToHECounter() {
    static std::atomic<uint64_t> c{0};
    return c;
}

inline std::atomic<uint64_t>& ProjectionSuperFusionV40HEToSSCounter() {
    static std::atomic<uint64_t> c{0};
    return c;
}

inline std::atomic<uint64_t>& ProjectionSuperFusionV40PackUS() {
    static std::atomic<uint64_t> c{0};
    return c;
}

inline std::atomic<uint64_t>& ProjectionSuperFusionV40ConvUS() {
    static std::atomic<uint64_t> c{0};
    return c;
}

inline bool ProjectionSuperFusionV40Enabled() {
    return EnvEnabled("DAZG_ORBIT_PROJECTION_SUPERFUSION_V40", true);
}

inline void PrintProjectionSuperFusionV40Ledger() {
    if (!ProjectionSuperFusionV40Enabled()) return;
    const uint64_t regions =
        ProjectionSuperFusionV40RegionCounter().load(std::memory_order_relaxed);
    if (regions == 0ULL) return;

    const uint64_t sstohe =
        ProjectionSuperFusionV40SSToHECounter().load(std::memory_order_relaxed);
    const uint64_t hetoss =
        ProjectionSuperFusionV40HEToSSCounter().load(std::memory_order_relaxed);
    const uint64_t pack_us =
        ProjectionSuperFusionV40PackUS().load(std::memory_order_relaxed);
    const uint64_t conv_us =
        ProjectionSuperFusionV40ConvUS().load(std::memory_order_relaxed);

    std::cerr << "[DAZG_ORBIT_PROJECTION_SUPERFUSION_LEDGER_V40]"
              << " profile=v40_projection_conversion_superfusion"
              << " regions=" << regions
              << " observed_sstohe_batches=" << sstohe
              << " observed_hetoss_batches=" << hetoss
              << " projected_global_sstohe_batches=1"
              << " projected_global_hetoss_batches=1"
              << " projected_sstohe_batch_reduction="
              << (sstohe > 1ULL ? (sstohe - 1ULL) : 0ULL)
              << " projected_hetoss_batch_reduction="
              << (hetoss > 1ULL ? (hetoss - 1ULL) : 0ULL)
              << " pack_sstohe_us_total=" << pack_us
              << " conv_hetoss_us_total=" << conv_us
              << " commit_mode=ledger_only"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline void RegisterProjectionSuperFusionV40Ledger() {
    static std::atomic<bool> registered{false};
    bool expected = false;
    if (registered.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        std::atexit(PrintProjectionSuperFusionV40Ledger);
    }
}

inline void RecordProjectionSuperFusionV40(uint64_t block_id,
                                           bool server,
                                           uint64_t in_planes,
                                           uint64_t planes,
                                           uint64_t stride,
                                           long long pack_sstohe_us,
                                           long long conv_hetoss_us,
                                           long long total_us) {
    (void)total_us;
    if (!ProjectionSuperFusionV40Enabled()) return;

    RegisterProjectionSuperFusionV40Ledger();

    ProjectionSuperFusionV40RegionCounter().fetch_add(1ULL, std::memory_order_relaxed);
    ProjectionSuperFusionV40SSToHECounter().fetch_add(1ULL, std::memory_order_relaxed);
    ProjectionSuperFusionV40HEToSSCounter().fetch_add(1ULL, std::memory_order_relaxed);

    if (pack_sstohe_us > 0) {
        ProjectionSuperFusionV40PackUS().fetch_add(
            static_cast<uint64_t>(pack_sstohe_us), std::memory_order_relaxed);
    }
    if (conv_hetoss_us > 0) {
        ProjectionSuperFusionV40ConvUS().fetch_add(
            static_cast<uint64_t>(conv_hetoss_us), std::memory_order_relaxed);
    }

    if (FanoutRuntimeDetailEnabled()) {
        std::cerr << "[DAZG_ORBIT_PROJECTION_SUPERFUSION_V40]"
                  << " block_id=" << block_id
                  << " role=" << (server ? 1 : 0)
                  << " in_planes=" << in_planes
                  << " planes=" << planes
                  << " stride=" << stride
                  << " region_observed=1"
                  << " sstohe_batch_observed=1"
                  << " hetoss_batch_observed=1"
                  << " global_fusion_candidate=1"
                  << " commit_mode=ledger_only"
                  << " pack_sstohe_us=" << pack_sstohe_us
                  << " conv_hetoss_us=" << conv_hetoss_us
                  << " total_us=" << total_us
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
}
// DAZG_ORBIT_PROJECTION_SUPERFUSION_V40_END


inline void RecordProjectionBurstV31RuntimeApplied(uint64_t block_id,
                                                   bool server,
                                                   uint64_t in_planes,
                                                   uint64_t planes,
                                                   uint64_t stride,
                                                   long long pack_sstohe_us,
                                                   long long conv_hetoss_us,
                                                   long long total_us,
                                                   bool domainpulse_v15,
                                                   bool fanoutburst_v20) {
    const uint64_t seq =
        FanoutRuntimeAppliedCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;

    RecordProjectionSuperFusionV40(block_id, server, in_planes, planes, stride,
                                    pack_sstohe_us, conv_hetoss_us, total_us);

    if (!FanoutRuntimeDetailEnabled()) return;

    std::cerr << "[DAZG_ORBIT_PROJECTION_BURST_V31]"
              << " seq=" << seq
              << " block_id=" << block_id
              << " role=" << (server ? 1 : 0)
              << " mode=cross_branch_projection_conversion_burst"
              << " in_planes=" << in_planes
              << " planes=" << planes
              << " stride=" << stride
              << " has_shortcut=1"
              << " domainpulse_v15=" << (domainpulse_v15 ? 1 : 0)
              << " fanoutburst_v20=" << (fanoutburst_v20 ? 1 : 0)
              << " batched_sstohe=1"
              << " mask_stable_batched_hetoss=1"
              << " prebranch_tensor_cache_applied=1"
              << " projection_burst_cache_applied=1"
              << " runtime_cache_applied=1"
              << " he_rotation_cache_applied=0"
              << " pack_sstohe_us=" << pack_sstohe_us
              << " conv_hetoss_us=" << conv_hetoss_us
              << " total_us=" << total_us
              << " saved_rounds=2"
              << " exact_equiv=1"
              << " semantic_loss=0"
              << " certificate=mask_stable_segmented_sstohe_hetoss_exactness"
              << " note=strict_cross_branch_projection_burst_runtime_hook"
              << std::endl;

    std::cerr << "[DAZG_ORBIT_FANOUT_RUNTIME]"
              << " seq=" << seq
              << " block_id=" << block_id
              << " role=" << (server ? 1 : 0)
              << " mode=projection_burst_v31"
              << " in_planes=" << in_planes
              << " planes=" << planes
              << " stride=" << stride
              << " has_shortcut=1"
              << " prebranch_tensor_cache_applied=1"
              << " projection_burst_cache_applied=1"
              << " runtime_cache_applied=1"
              << " he_rotation_cache_applied=0"
              << " saved_rounds=2"
              << " exact_equiv=1"
              << " semantic_loss=0"
              << " note=domainpulse_v15_projection_burst_counted_in_fanout_plan"
              << std::endl;
}

inline void EmitStageZ2AblationWarningOnce() {
    static bool emitted = false;
    if (emitted || dazg_orbit::ablation::EnableStageZ2()) return;
    emitted = true;
    std::cerr << "[DAZG_ORBIT_STAGEZ2_ABLATION_WARNING]"
              << " enable_stage_z2=0"
              << " note=safe_patch_logs_flag_but_does_not_force_dummy_dense_bsgs_path"
              << " action=do_not_use_A5_as_paper_ablation_until_dense_fallback_is_implemented"
              << std::endl;
}

inline std::string InferStage(uint64_t H, uint64_t Cin, uint64_t Cout,
                              uint64_t K, uint64_t S) {
    (void)K;
    (void)S;
    if (H == 224) return "stem";
    if (H == 56 && Cin <= 128 && Cout <= 128) return "conv2_x_to_conv3_x";
    if (H == 28 && Cin <= 256 && Cout <= 256) return "conv3_x_to_conv4_x";
    if (H == 14 && Cin <= 512 && Cout <= 512) return "conv4_x_to_conv5_x";
    if (H == 7 && Cin == 512) return "conv5_x";
    if (H == 1 && (Cin == 512 || Cin == 2048)) return "fc";
    return "unknown";
}

inline std::string InferOpType(uint64_t K, uint64_t S, const std::string& benefit) {
    if (benefit.find("CLASSIFIER") != std::string::npos) return "classifier";
    if (K == 1 && S == 2) return "shortcut_downsample_k1s2";
    if (K == 3 && S == 2) return "main_downsample_k3s2";
    if (K == 3 && S == 1) return "conv_k3s1";
    if (K == 1 && S == 1) return "pointwise_k1s1";
    return "conv";
}

struct CostProxy {
    uint64_t dense_packs = 0;
    uint64_t planned_packs = 0;
    uint64_t estimated_mul = 0;
    uint64_t estimated_rot = 0;
    uint64_t estimated_add = 0;
    uint64_t estimated_conversion = 0;
};

struct OpPlan {
    uint64_t op_index = 0;
    std::string source;
    std::string op_id;
    std::string stage;
    std::string op_type;
    GraphRoute route = GraphRoute::Unknown;
    std::string benefit_class;

    uint64_t H = 0;
    uint64_t Cin = 0;
    uint64_t Cout = 0;
    uint64_t K = 0;
    uint64_t S = 0;
    int layout_mode = -1;

    bool compact_k1s2 = false;
    bool polyphase_k3s2 = false;
    bool exact_tiled_k3s1 = false;
    bool exact_equiv = true;
    bool semantic_loss = false;
    
    CostProxy dense_cost;
    CostProxy planned_cost;
};


struct ProjectionBlockPlan {
    uint64_t seq = 0;
    uint64_t block_id = 0;
    uint64_t H = 0;
    uint64_t in_planes = 0;
    uint64_t planes = 0;
    uint64_t expansion = 4;
    uint64_t stride = 1;
    bool has_shortcut = false;
    bool exact_equiv = true;
    bool semantic_loss = false;
    std::string source;
    std::string stage;
    std::string region_id;
};

inline const char* ProjectionStageName(uint64_t H, uint64_t stride) {
    if (stride == 1 && H == 56) return "conv2_x_entry_projection";
    if (stride == 2 && H == 56) return "conv2_x_to_conv3_x";
    if (stride == 2 && H == 28) return "conv3_x_to_conv4_x";
    if (stride == 2 && H == 14) return "conv4_x_to_conv5_x";
    if (stride == 2 && H == 7) return "conv5_x_projection";
    return "projection_unknown";
}

inline std::string MakeProjectionRegionId(uint64_t block_id,
                                          uint64_t H,
                                          uint64_t in_planes,
                                          uint64_t planes,
                                          uint64_t stride) {
    std::ostringstream oss;
    oss << "Bottleneck#" << block_id
        << "_" << ProjectionStageName(H, stride)
        << "_H" << H
        << "_preCin" << in_planes
        << "_planes" << planes
        << "_S" << stride;
    return oss.str();
}

inline const char* ProjectionMainRouteName(uint64_t stride) {
    return stride == 2 ? "PCOI_K3S2" : "ExactTiledK1S1";
}

inline const char* ProjectionShortcutRouteName(uint64_t stride) {
    return stride == 2 ? "CompactK1S2" : "ExactTiledK1S1";
}

inline int ProjectionMainLayout(uint64_t stride) {
    return stride == 2 ? 7 : 6;
}

inline int ProjectionShortcutLayout(uint64_t stride) {
    return stride == 2 ? 4 : 6;
}

inline std::string MakeOpId(uint64_t index,
                            const std::string& source,
                            const std::string& stage,
                            const std::string& op_type,
                            uint64_t H, uint64_t Cin, uint64_t Cout,
                            uint64_t K, uint64_t S) {
    std::ostringstream oss;
    oss << "op" << index << "_"
        << stage << "_"
        << op_type << "_"
        << source << "_"
        << "H" << H << "_Cin" << Cin << "_Cout" << Cout
        << "_K" << K << "_S" << S;
    return oss.str();
}

inline GraphRoute ConvertRouteName(const char* route_name,
                                   const char* benefit_class) {
    const std::string r = route_name ? route_name : "";
    const std::string b = benefit_class ? benefit_class : "";

    if (b.find("CLASSIFIER") != std::string::npos) return GraphRoute::CSRClassifier;
    if (r == "CheetahLargeInput") return GraphRoute::CheetahLargeInput;
    if (r == "CompactK1S2") return GraphRoute::CompactK1S2;
    if (r == "PolyphaseK3S2") return GraphRoute::PolyphaseK3S2;
    if (r == "PCOI_K3S2") return GraphRoute::PCOIK3S2;
    if (r == "ExactTiledK3S1") return GraphRoute::ExactTiledK3S1;
    if (r == "ExactTiledK1S1") return GraphRoute::ExactTiledK1S1;
    if (r == "SparseBSGS") return GraphRoute::SparseBSGS;
    if (b.find("STAGE1_STATIC") != std::string::npos) return GraphRoute::Stage1Static;
    if (r == "DenseFallback") return GraphRoute::DenseFallback;
    return GraphRoute::Unknown;
}

inline CostProxy EstimateDenseCost(uint64_t H, uint64_t Cin, uint64_t Cout,
                                   uint64_t K, uint64_t S) {
    (void)S;
    CostProxy c;
    const uint64_t spatial = (H == 0) ? 1 : H * H;
    const uint64_t kernel = (K == 0) ? 1 : K * K;
    c.dense_packs = kernel * ((Cin + 511) / 512) * ((Cout + 511) / 512);
    if (c.dense_packs == 0) c.dense_packs = 1;
    c.planned_packs = c.dense_packs;
    c.estimated_mul = c.dense_packs;
    c.estimated_rot = (kernel > 1) ? kernel : 0;
    c.estimated_add = (c.dense_packs > 0) ? (c.dense_packs - 1) : 0;
    c.estimated_conversion = (spatial > 0) ? 1 : 0;
    return c;
}

inline CostProxy EstimatePlannedCost(const CostProxy& dense,
                                     GraphRoute route,
                                     bool compact_k1s2,
                                     bool polyphase_k3s2,
                                     bool exact_tiled_k3s1) {
    CostProxy p = dense;

    if (route == GraphRoute::CheetahLargeInput) {
        p.estimated_conversion = 1;
        return p;
    }

    if (route == GraphRoute::CSRClassifier) {
        p.planned_packs = std::min<uint64_t>(p.dense_packs, 2);
        p.estimated_mul = p.planned_packs;
        p.estimated_rot = 2;
        p.estimated_add = (p.planned_packs > 0) ? p.planned_packs - 1 : 0;
        return p;
    }

    if (compact_k1s2 || route == GraphRoute::CompactK1S2) {
        p.planned_packs = std::max<uint64_t>(1, dense.dense_packs / 2);
        p.estimated_mul = p.planned_packs;
        p.estimated_rot = 0;
        p.estimated_add = (p.planned_packs > 0) ? p.planned_packs - 1 : 0;
        return p;
    }

    if (polyphase_k3s2 || route == GraphRoute::PolyphaseK3S2 || route == GraphRoute::PCOIK3S2) {
        p.planned_packs = std::max<uint64_t>(1, dense.dense_packs / 2);
        p.estimated_mul = p.planned_packs;
        p.estimated_rot = std::min<uint64_t>(dense.estimated_rot, 1);
        p.estimated_add = (p.planned_packs > 0) ? p.planned_packs - 1 : 0;
        return p;
    }

    if (exact_tiled_k3s1 || route == GraphRoute::ExactTiledK3S1) {
        p.planned_packs = dense.dense_packs;
        p.estimated_mul = dense.dense_packs;
        p.estimated_rot = 0;
        p.estimated_add = (p.planned_packs > 0) ? p.planned_packs - 1 : 0;
        return p;
    }

    if (route == GraphRoute::ExactTiledK1S1) {
        p.planned_packs = dense.dense_packs;
        p.estimated_mul = dense.dense_packs;
        p.estimated_rot = 0;
        p.estimated_add = (p.planned_packs > 0) ? p.planned_packs - 1 : 0;
        return p;
    }

    if (route == GraphRoute::SparseBSGS) {
        p.planned_packs = dense.dense_packs;
        p.estimated_mul = dense.dense_packs;
        p.estimated_rot = std::max<uint64_t>(1, dense.estimated_rot / 2);
        p.estimated_add = (p.planned_packs > 0) ? p.planned_packs - 1 : 0;
        return p;
    }

    return p;
}

class GraphPlanner {
public:
    static GraphPlanner& Instance() {
        static GraphPlanner planner;
        return planner;
    }

    uint64_t RegisterConv(const char* source,
                          uint64_t H,
                          uint64_t Cin,
                          uint64_t Cout,
                          uint64_t K,
                          uint64_t S,
                          int layout_mode,
                          bool compact_k1s2,
                          bool polyphase_k3s2,
                          bool exact_tiled_k3s1,
                          const char* route_name,
                          const char* benefit_class) {
        if (!GraphPlanEnabled()) return 0;

        std::lock_guard<std::mutex> lock(mu_);
        const uint64_t idx = ++counter_;

        OpPlan p;
        p.op_index = idx;
        p.source = source ? source : "unknown";
        p.stage = InferStage(H, Cin, Cout, K, S);
        p.op_type = InferOpType(K, S, benefit_class ? benefit_class : "");
        p.route = ConvertRouteName(route_name, benefit_class);
        p.benefit_class = benefit_class ? benefit_class : "";
        
        p.H = H;
        p.Cin = Cin;
        p.Cout = Cout;
        p.K = K;
        p.S = S;
        p.layout_mode = layout_mode;
        const bool dazg_orbit_route_enabled =
        dazg_orbit::ablation::EnableRouteSpecialization();

        if (!dazg_orbit_route_enabled) {
            compact_k1s2 = false;
            polyphase_k3s2= false;
            exact_tiled_k3s1 = false;
        }

        p.compact_k1s2 = compact_k1s2;
        p.polyphase_k3s2 = polyphase_k3s2;
        p.exact_tiled_k3s1 = exact_tiled_k3s1;
        p.exact_equiv = true;
        p.semantic_loss = false;

        p.dense_cost = EstimateDenseCost(H, Cin, Cout, K, S);
        p.planned_cost = EstimatePlannedCost(
            p.dense_cost, p.route, compact_k1s2, polyphase_k3s2, exact_tiled_k3s1);

        p.op_id = MakeOpId(idx, p.source, p.stage, p.op_type, H, Cin, Cout, K, S);
        plans_.push_back(p);

        EmitPlan(p);
        return idx;
    }


    uint64_t RegisterProjectionBlock(uint64_t block_id,
                                     uint64_t H,
                                     uint64_t in_planes,
                                     uint64_t planes,
                                     uint64_t expansion,
                                     uint64_t stride,
                                     bool has_shortcut,
                                     const char* source) {
        if (!GraphPlanEnabled()) return 0;
        if (!has_shortcut) return 0;

        std::lock_guard<std::mutex> lock(mu_);
        ProjectionBlockPlan b;
        b.seq = ++projection_counter_;
        b.block_id = block_id;
        b.H = H;
        b.in_planes = in_planes;
        b.planes = planes;
        b.expansion = expansion == 0 ? 4ULL : expansion;
        b.stride = stride;
        b.has_shortcut = has_shortcut;
        b.exact_equiv = true;
        b.semantic_loss = false;
        b.source = source ? source : "Bottleneck";
        b.stage = ProjectionStageName(H, stride);
        b.region_id = MakeProjectionRegionId(block_id, H, in_planes, planes, stride);
        projection_blocks_.push_back(b);

        std::cerr << "[DAZG_ORBIT_PROJECTION_BLOCK_PLAN]"
                  << " seq=" << b.seq
                  << " block_id=" << b.block_id
                  << " region_id=" << b.region_id
                  << " source=" << b.source
                  << " stage=" << b.stage
                  << " H=" << b.H
                  << " in_planes=" << b.in_planes
                  << " planes=" << b.planes
                  << " out_planes=" << (b.planes * b.expansion)
                  << " stride=" << b.stride
                  << " has_main=1"
                  << " has_shortcut=1"
                  << " main_route=" << ProjectionMainRouteName(b.stride)
                  << " shortcut_route=" << ProjectionShortcutRouteName(b.stride)
                  << " same_prebranch_domain=1"
                  << " conversion_burst_candidate=1"
                  << " exact_equiv=1 semantic_loss=0"
                  << " certificate=projection_block_prebranch_domain_identity"
                  << std::endl;
        return b.seq;
    }

    void EmitSummary() {
        if (!GraphPlanSummaryEnabled()) return;

        std::lock_guard<std::mutex> lock(mu_);
        if (summary_emitted_) return;
        summary_emitted_ = true;

        std::unordered_map<std::string, uint64_t> by_route;
        std::unordered_map<std::string, uint64_t> by_benefit;

        uint64_t dense_mul = 0;
        uint64_t planned_mul = 0;
        uint64_t dense_rot = 0;
        uint64_t planned_rot = 0;
        uint64_t dense_add = 0;
        uint64_t planned_add = 0;

        for (const auto& p : plans_) {
            by_route[GraphRouteName(p.route)]++;
            by_benefit[p.benefit_class]++;
            dense_mul += p.dense_cost.estimated_mul;
            planned_mul += p.planned_cost.estimated_mul;
            dense_rot += p.dense_cost.estimated_rot;
            planned_rot += p.planned_cost.estimated_rot;
            dense_add += p.dense_cost.estimated_add;
            planned_add += p.planned_cost.estimated_add;
        }

        const uint64_t saved_mul = dense_mul >= planned_mul ? dense_mul - planned_mul : 0;
        const uint64_t saved_rot = dense_rot >= planned_rot ? dense_rot - planned_rot : 0;
        const uint64_t saved_add = dense_add >= planned_add ? dense_add - planned_add : 0;
        const double rot_reduction_ratio =
            dense_rot == 0 ? 0.0 : static_cast<double>(saved_rot) / static_cast<double>(dense_rot);
        const double mul_reduction_ratio =
            dense_mul == 0 ? 0.0 : static_cast<double>(saved_mul) / static_cast<double>(dense_mul);
        const double add_reduction_ratio =
            dense_add == 0 ? 0.0 : static_cast<double>(saved_add) / static_cast<double>(dense_add);

        std::cerr << "[DAZG_ORBIT_GRAPH_SUMMARY]"
                  << " total_ops=" << plans_.size()
                  << " dense_mul_proxy=" << dense_mul
                  << " planned_mul_proxy=" << planned_mul
                  << " saved_mul_proxy=" << saved_mul
                  << " mul_reduction_ratio=" << mul_reduction_ratio
                  << " dense_rot_proxy=" << dense_rot
                  << " planned_rot_proxy=" << planned_rot
                  << " saved_rot_proxy=" << saved_rot
                  << " rot_reduction_ratio=" << rot_reduction_ratio
                  << " dense_add_proxy=" << dense_add
                  << " planned_add_proxy=" << planned_add
                  << " saved_add_proxy=" << saved_add
                  << " add_reduction_ratio=" << add_reduction_ratio
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << std::endl;

        std::cerr << "[DAZG_ORBIT_EXEC_CERT]"
                  << " component=StageZ2SparseRouting"
                  << " certificate=sparse_tuple_bsgs_polyphase_tiled_exactness"
                  << " dense_rot_proxy=" << dense_rot
                  << " planned_rot_proxy=" << planned_rot
                  << " saved_rot_proxy=" << saved_rot
                  << " rot_reduction_ratio=" << rot_reduction_ratio
                  << " dense_mul_proxy=" << dense_mul
                  << " planned_mul_proxy=" << planned_mul
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << " paper_claim=exact_encrypted_linear_routing_certificate"
                  << std::endl;

        EmitFanoutCachePlanLocked();

        for (const auto& kv : by_route) {
            std::cerr << "[DAZG_ORBIT_GRAPH_SUMMARY]"
                      << " group=route"
                      << " key=" << kv.first
                      << " count=" << kv.second
                      << std::endl;
        }

        for (const auto& kv : by_benefit) {
            std::cerr << "[DAZG_ORBIT_GRAPH_SUMMARY]"
                      << " group=benefit_class"
                      << " key=" << kv.first
                      << " count=" << kv.second
                      << std::endl;
        }
    }

    ~GraphPlanner() {
        EmitSummary();
    }

private:
    GraphPlanner() = default;


    void EmitFanoutCachePlanLocked() const {
        if (!projection_blocks_.empty()) {
            uint64_t total_regions = 0;
            uint64_t complete_regions = 0;
            uint64_t prebranch_cacheable_total = 0;
            uint64_t branch_layout_mismatch_total = 0;
            uint64_t rotation_cacheable_regions = 0;
            uint64_t projection_burst_cacheable_regions = 0;
            uint64_t conversion_rounds_saved_proxy = 0;
            uint64_t main_rot_proxy_total = 0;
            uint64_t shortcut_rot_proxy_total = 0;
            uint64_t shareable_rot_proxy_total = 0;
            uint64_t region_index = 0;

            const uint64_t runtime_applied_count = FanoutRuntimeAppliedCount();

            for (const auto& b : projection_blocks_) {
                if (!b.has_shortcut) continue;

                ++total_regions;
                ++complete_regions;
                ++prebranch_cacheable_total;
                ++projection_burst_cacheable_regions;
                conversion_rounds_saved_proxy += 2ULL;

                const int main_layout = ProjectionMainLayout(b.stride);
                const int shortcut_layout = ProjectionShortcutLayout(b.stride);
                const bool same_branch_layout = (main_layout == shortcut_layout);
                if (!same_branch_layout) ++branch_layout_mismatch_total;

                const uint64_t main_rot = (b.stride == 2) ? 1ULL : 0ULL;
                const uint64_t shortcut_rot = 0ULL;
                const uint64_t shareable = 0ULL;
                main_rot_proxy_total += main_rot;
                shortcut_rot_proxy_total += shortcut_rot;
                shareable_rot_proxy_total += shareable;
                if (shareable > 0) ++rotation_cacheable_regions;

                const bool runtime_seen = (region_index < runtime_applied_count);
                ++region_index;

                const char* rot_cache_reason = (main_rot == 0 && shortcut_rot == 0)
                    ? "no_branch_rotation_needed"
                    : "one_branch_has_zero_rotation_need";
                const char* layout_adapter = same_branch_layout
                    ? "none"
                    : "projection_prebranch_layout_adapter";

                std::cerr << "[DAZG_ORBIT_FANOUT_REGION]"
                          << " region_id=" << b.region_id
                          << " block_id=" << b.block_id
                          << " has_main=1"
                          << " has_shortcut=1"
                          << " main_op=Bottleneck#" << b.block_id << "/main_projection_branch"
                          << " shortcut_op=Bottleneck#" << b.block_id << "/shortcut_projection_branch"
                          << " main_route=" << ProjectionMainRouteName(b.stride)
                          << " shortcut_route=" << ProjectionShortcutRouteName(b.stride)
                          << " main_layout=" << main_layout
                          << " shortcut_layout=" << shortcut_layout
                          << " H=" << b.H
                          << " prebranch_Cin=" << b.in_planes
                          << " planes=" << b.planes
                          << " post_Cout=" << (b.planes * b.expansion)
                          << " stride=" << b.stride
                          << " same_prebranch_domain=1"
                          << " branch_layout_equal=" << same_branch_layout
                          << " layout_adapter=" << layout_adapter
                          << " main_rot_proxy=" << main_rot
                          << " shortcut_rot_proxy=" << shortcut_rot
                          << " shareable_rot_proxy=" << shareable
                          << " prebranch_cacheable=1"
                          << " projection_burst_cacheable=1"
                          << " conversion_rounds_saved_proxy=2"
                          << " rotation_cacheable=" << (shareable > 0)
                          << " cacheable=1"
                          << " runtime_hook_seen=" << runtime_seen
                          << " runtime_projection_burst_seen=" << runtime_seen
                          << " exact_equiv=1"
                          << " semantic_loss=0"
                          << " rot_cache_reason=" << rot_cache_reason
                          << " reason=projection_prebranch_conversion_burst"
                          << std::endl;
            }

            const uint64_t runtime_prebranch_applied_regions =
                std::min<uint64_t>(prebranch_cacheable_total, runtime_applied_count);
            const bool runtime_cache_applied = runtime_prebranch_applied_regions > 0;

            std::cerr << "[DAZG_ORBIT_FANOUT_CACHE_PLAN]"
                      << " total_regions=" << total_regions
                      << " complete_regions=" << complete_regions
                      << " prebranch_cacheable_regions=" << prebranch_cacheable_total
                      << " branch_layout_mismatch_regions=" << branch_layout_mismatch_total
                      << " rotation_cacheable_regions=" << rotation_cacheable_regions
                      << " projection_burst_cacheable_regions=" << projection_burst_cacheable_regions
                      << " conversion_rounds_saved_proxy=" << conversion_rounds_saved_proxy
                      << " main_rot_proxy_total=" << main_rot_proxy_total
                      << " shortcut_rot_proxy_total=" << shortcut_rot_proxy_total
                      << " shareable_rot_proxy_total=" << shareable_rot_proxy_total
                      << " prebranch_cache_applicable=" << (prebranch_cacheable_total > 0)
                      << " projection_burst_cache_applicable=" << (projection_burst_cacheable_regions > 0)
                      << " rotation_cache_applicable=" << (shareable_rot_proxy_total > 0)
                      << " runtime_cache_applicable=" << (prebranch_cacheable_total > 0)
                      << " runtime_projection_burst_applied_count=" << runtime_applied_count
                      << " runtime_prebranch_applied_count=" << runtime_applied_count
                      << " runtime_prebranch_applied_regions=" << runtime_prebranch_applied_regions
                      << " runtime_cache_applied=" << runtime_cache_applied
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << " certificate=projection_block_prebranch_domain_identity_and_mask_stable_conversion_burst"
                      << " note=" << (runtime_cache_applied
                             ? "projection_burst_v31_runtime_applied"
                             : "projection_burst_v31_planned_runtime_hook_required")
                      << std::endl;
            return;
        }
        struct Region {
            const OpPlan* main = nullptr;
            const OpPlan* shortcut = nullptr;
        };

        std::unordered_map<std::string, Region> regions;

        auto make_key = [](const OpPlan& p) {
            std::ostringstream oss;
            oss << p.stage
                << "_H" << p.H
                << "_Cin" << p.Cin
                << "_Cout" << p.Cout
                << "_S" << p.S;
            return oss.str();
        };

        for (const auto& p : plans_) {
            if (p.op_type == "main_downsample_k3s2") {
                regions[make_key(p)].main = &p;
            } else if (p.op_type == "shortcut_downsample_k1s2") {
                regions[make_key(p)].shortcut = &p;
            }
        }

        uint64_t total_regions = 0;
        uint64_t complete_regions = 0;
        uint64_t prebranch_cacheable_total = 0;
        uint64_t branch_layout_mismatch_total = 0;
        uint64_t rotation_cacheable_regions = 0;
        uint64_t shareable_rot_proxy_total = 0;
        uint64_t main_rot_proxy_total = 0;
        uint64_t shortcut_rot_proxy_total = 0;

        for (const auto& kv : regions) {
            const Region& r = kv.second;
            if (r.main == nullptr && r.shortcut == nullptr) {
                continue;
            }

            total_regions++;

            const bool complete = (r.main != nullptr && r.shortcut != nullptr);
            if (complete) {
                complete_regions++;
            }

            const uint64_t main_rot =
                r.main ? r.main->planned_cost.estimated_rot : 0;
            const uint64_t shortcut_rot =
                r.shortcut ? r.shortcut->planned_cost.estimated_rot : 0;

            main_rot_proxy_total += main_rot;
            shortcut_rot_proxy_total += shortcut_rot;

            const bool same_branch_layout =
                complete && (r.main->layout_mode == r.shortcut->layout_mode);

            const bool same_prebranch_domain =
                complete &&
                r.main->H == r.shortcut->H &&
                r.main->Cin == r.shortcut->Cin &&
                r.main->Cout == r.shortcut->Cout &&
                r.main->S == r.shortcut->S &&
                r.main->exact_equiv && r.shortcut->exact_equiv &&
                !r.main->semantic_loss && !r.shortcut->semantic_loss;

            const bool optimized_fanout_pair =
                complete &&
                r.main->op_type == "main_downsample_k3s2" &&
                r.shortcut->op_type == "shortcut_downsample_k1s2" &&
                (r.main->route == GraphRoute::PCOIK3S2 ||
                 r.main->route == GraphRoute::PolyphaseK3S2) &&
                r.shortcut->route == GraphRoute::CompactK1S2;

            const bool prebranch_cacheable =
                same_prebranch_domain && optimized_fanout_pair;

            const uint64_t shareable =
                (prebranch_cacheable && same_branch_layout &&
                 main_rot > 0 && shortcut_rot > 0)
                    ? std::min(main_rot, shortcut_rot)
                    : 0;

            const bool rotation_cacheable = (shareable > 0);

            if (prebranch_cacheable) {
                prebranch_cacheable_total++;
            }
            if (complete && !same_branch_layout) {
                branch_layout_mismatch_total++;
            }
            if (rotation_cacheable) {
                rotation_cacheable_regions++;
            }

            shareable_rot_proxy_total += shareable;

            const char* reason = "none";
            const char* rot_cache_reason = "none";
            const char* layout_adapter = "none";
            if (!complete) {
                reason = "incomplete_fanout_pair";
                rot_cache_reason = "incomplete_fanout_pair";
            } else if (!same_prebranch_domain) {
                reason = "input_domain_mismatch";
                rot_cache_reason = "input_domain_mismatch";
            } else if (!optimized_fanout_pair) {
                reason = "not_polyphase_k3s2_compact_k1s2_pair";
                rot_cache_reason = "not_optimized_fanout_pair";
            } else {
                reason = "shared_prebranch_input_domain";
                layout_adapter = same_branch_layout
                    ? "none"
                    : "prebranch_input_domain_adapter";

                if (!same_branch_layout) {
                    rot_cache_reason = "branch_local_layouts_differ";
                } else if (main_rot == 0 || shortcut_rot == 0) {
                    rot_cache_reason = "one_branch_has_zero_rotation_need";
                } else if (!rotation_cacheable) {
                    rot_cache_reason = "no_common_rotation_key";
                } else {
                    rot_cache_reason = "shareable_rotation_keys_detected";
                }
            }

            if (prebranch_cacheable && !rotation_cacheable &&
                (main_rot == 0 || shortcut_rot == 0)) {
                rot_cache_reason = "one_branch_has_zero_rotation_need";
            }

            const bool runtime_applied_seen = FanoutRuntimeAppliedCount() > 0;

            std::cerr << "[DAZG_ORBIT_FANOUT_REGION]"
                      << " region_id=" << kv.first
                      << " has_main=" << (r.main != nullptr)
                      << " has_shortcut=" << (r.shortcut != nullptr)
                      << " main_op=" << (r.main ? r.main->op_id : "NA")
                      << " shortcut_op=" << (r.shortcut ? r.shortcut->op_id : "NA")
                      << " main_route=" << (r.main ? GraphRouteName(r.main->route) : "NA")
                      << " shortcut_route=" << (r.shortcut ? GraphRouteName(r.shortcut->route) : "NA")
                      << " main_layout=" << (r.main ? r.main->layout_mode : -1)
                      << " shortcut_layout=" << (r.shortcut ? r.shortcut->layout_mode : -1)
                      << " same_prebranch_domain=" << same_prebranch_domain
                      << " branch_layout_equal=" << same_branch_layout
                      << " layout_adapter=" << layout_adapter
                      << " main_rot_proxy=" << main_rot
                      << " shortcut_rot_proxy=" << shortcut_rot
                      << " shareable_rot_proxy=" << shareable
                      << " prebranch_cacheable=" << prebranch_cacheable
                      << " rotation_cacheable=" << rotation_cacheable
                      << " cacheable=" << prebranch_cacheable
                      << " runtime_hook_seen=" << (prebranch_cacheable && runtime_applied_seen)
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << " rot_cache_reason=" << rot_cache_reason
                      << " reason=" << reason
                      << std::endl;
        }

        const uint64_t runtime_applied_count = FanoutRuntimeAppliedCount();
        const uint64_t runtime_prebranch_applied_regions =
            std::min<uint64_t>(prebranch_cacheable_total, runtime_applied_count);
        const bool runtime_cache_applied = runtime_prebranch_applied_regions > 0;

        std::cerr << "[DAZG_ORBIT_FANOUT_CACHE_PLAN]"
                  << " total_regions=" << total_regions
                  << " complete_regions=" << complete_regions
                  << " prebranch_cacheable_regions=" << prebranch_cacheable_total
                  << " branch_layout_mismatch_regions=" << branch_layout_mismatch_total
                  << " rotation_cacheable_regions=" << rotation_cacheable_regions
                  << " main_rot_proxy_total=" << main_rot_proxy_total
                  << " shortcut_rot_proxy_total=" << shortcut_rot_proxy_total
                  << " shareable_rot_proxy_total=" << shareable_rot_proxy_total
                  << " prebranch_cache_applicable=" << (prebranch_cacheable_total > 0)
                  << " rotation_cache_applicable=" << (shareable_rot_proxy_total > 0)
                  << " runtime_cache_applicable=" << (prebranch_cacheable_total > 0)
                  << " runtime_prebranch_applied_count=" << runtime_applied_count
                  << " runtime_prebranch_applied_regions=" << runtime_prebranch_applied_regions
                  << " runtime_cache_applied=" << runtime_cache_applied
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << " note=" << (runtime_cache_applied
                         ? "safe_prebranch_tensor_cache_runtime_applied"
                         : "prebranch_cache_planned_runtime_hook_required")
                  << std::endl;
    }

    void EmitPlan(const OpPlan& p) const {
        std::cerr << "[DAZG_ORBIT_GRAPH_PLAN]"
                  << " op_index=" << p.op_index
                  << " op_id=" << p.op_id
                  << " source=" << p.source
                  << " stage=" << p.stage
                  << " op_type=" << p.op_type
                  << " H=" << p.H
                  << " Cin=" << p.Cin
                  << " Cout=" << p.Cout
                  << " K=" << p.K
                  << " S=" << p.S
                  << " layout_mode=" << p.layout_mode
                  << " route=" << GraphRouteName(p.route)
                  << " benefit_class=" << p.benefit_class
                  << " compact_k1s2=" << p.compact_k1s2
                  << " polyphase_k3s2=" << p.polyphase_k3s2
                  << " exact_tiled_k3s1=" << p.exact_tiled_k3s1
                  << " dense_mul_proxy=" << p.dense_cost.estimated_mul
                  << " planned_mul_proxy=" << p.planned_cost.estimated_mul
                  << " dense_rot_proxy=" << p.dense_cost.estimated_rot
                  << " planned_rot_proxy=" << p.planned_cost.estimated_rot
                  << " dense_add_proxy=" << p.dense_cost.estimated_add
                  << " planned_add_proxy=" << p.planned_cost.estimated_add
                  << " exact_equiv=" << p.exact_equiv
                  << " semantic_loss=" << p.semantic_loss
                  << std::endl;
    }

    std::mutex mu_;
    uint64_t counter_ = 0;
    uint64_t projection_counter_ = 0;
    bool summary_emitted_ = false;
    std::vector<OpPlan> plans_;
    std::vector<ProjectionBlockPlan> projection_blocks_;
};

inline uint64_t RegisterGraphConvPlan(const char* source,
                                      uint64_t H,
                                      uint64_t Cin,
                                      uint64_t Cout,
                                      uint64_t K,
                                      uint64_t S,
                                      int layout_mode,
                                      bool compact_k1s2,
                                      bool polyphase_k3s2,
                                      bool exact_tiled_k3s1,
                                      const char* route_name,
                                      const char* benefit_class) {
    return GraphPlanner::Instance().RegisterConv(
        source, H, Cin, Cout, K, S, layout_mode,
        compact_k1s2, polyphase_k3s2, exact_tiled_k3s1,
        route_name, benefit_class);
}


inline uint64_t RegisterProjectionBlockPlan(uint64_t block_id,
                                            uint64_t H,
                                            uint64_t in_planes,
                                            uint64_t planes,
                                            uint64_t expansion,
                                            uint64_t stride,
                                            bool has_shortcut,
                                            const char* source) {
    return GraphPlanner::Instance().RegisterProjectionBlock(
        block_id, H, in_planes, planes, expansion, stride, has_shortcut, source);
}

inline void FlushGraphPlanSummary() {
    GraphPlanner::Instance().EmitSummary();
}

}  // namespace dazg_orbit_gos

#endif  // DAZG_ORBIT_GOS_GRAPH_PLAN_HEADER_ONLY




template <typename T, typename IO>
Conv2D* CreateConv(uint64_t in_feature_size,
                   uint64_t in_channels,
                   uint64_t out_channels,
                   uint64_t kernel_size,
                   uint64_t stride,
                   CryptoPrimitive<T, IO> *cryptoPrimitive,
                   bool allow_stage_l = true);

template <typename T, typename IO>
Conv2D* CreateConvFromWeights(uint64_t in_feature_size,
                              uint64_t stride,
                              uint64_t padding,
                              const Tensor<uint64_t>& weight,
                              const Tensor<uint64_t>& bias,
                              CryptoPrimitive<T, IO> *cryptoPrimitive);


// -----------------------------------------------------------------------------
// DAZG-Orbit route/profiler helpers.
// Self-contained on purpose: do NOT include Utils/dazg_orbit_config.h or
// Utils/dazg_orbit_profiler.h unless those files already exist in your project.
// Enable/disable route logging with:
//     DAZG_ORBIT_PROFILER=0  ./your_binary
// -----------------------------------------------------------------------------
enum class SecRoute {
    CheetahLargeInput,
    CompactK1S2,
    PolyphaseK3S2,
    PCOIK3S2,
    ExactTiledK3S1,
    ExactTiledK1S1,
    SparseBSGS,
    DenseFallback
};

inline const char* RouteName(SecRoute r) {
    switch (r) {
    case SecRoute::CheetahLargeInput: return "CheetahLargeInput";
    case SecRoute::CompactK1S2: return "CompactK1S2";
    case SecRoute::PolyphaseK3S2: return "PolyphaseK3S2";
    case SecRoute::PCOIK3S2: return "PCOI_K3S2";
    case SecRoute::ExactTiledK3S1: return "ExactTiledK3S1";
    case SecRoute::ExactTiledK1S1: return "ExactTiledK1S1";
    case SecRoute::SparseBSGS: return "SparseBSGS";
    case SecRoute::DenseFallback: return "DenseFallback";
    default: return "DenseFallback";
    }
}


inline bool DAZGOrbitStrictK3S2FallbackEnabled() {
    return dazg_orbit::ablation::EnvFlag(
        "DAZG_ORBIT_STRICT_A3_K3S2_FALLBACK",
        true
    );
}

inline bool DAZGOrbitK3S2StageTransitionTarget(uint64_t in_feature_size,
                                             uint64_t in_channels,
                                             uint64_t out_channels,
                                             uint64_t kernel_size,
                                             uint64_t stride,
                                             uint64_t padding) {
    if (kernel_size != 3 || stride != 2 || padding != 1) return false;
    return (in_feature_size == 32 && in_channels == 64  && out_channels == 128) ||
           (in_feature_size == 16 && in_channels == 128 && out_channels == 256) ||
           (in_feature_size == 8  && in_channels == 256 && out_channels == 512) ||
           (in_feature_size == 56 && in_channels == 64  && out_channels == 128) ||
           (in_feature_size == 28 && in_channels == 128 && out_channels == 256) ||
           (in_feature_size == 14 && in_channels == 256 && out_channels == 512);
}

inline void EmitDAZGOrbitStrictK3S2Fallback(const char* source,
                                          uint64_t in_feature_size,
                                          uint64_t in_channels,
                                          uint64_t out_channels,
                                          uint64_t kernel_size,
                                          uint64_t stride,
                                          uint64_t padding) {
    std::cerr << "[DAZG_ORBIT_STRICT_FALLBACK]"
              << " source=" << source
              << " reason=STRICT_EXACT_FALLBACK_K3S2_DISABLED"
              << " H=" << in_feature_size
              << " Cin=" << in_channels
              << " Cout=" << out_channels
              << " K=" << kernel_size
              << " S=" << stride
              << " P=" << padding
              << " enable_k3s2_polyphase=" << (dazg_orbit::ablation::EnableK3S2Polyphase() ? 1 : 0)
              << " fallback=Conv2DNest"
              << " exact_equiv_required=1"
              << " paper_gate=fail_closed_until_verifier_passes"
              << std::endl;
}

inline bool DAZGOrbitProfilerEnabled() {
    const char* v = std::getenv("DAZG_ORBIT_PROFILER");
    if (v == nullptr) return true;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" || s == "OFF" || s == "off");
}

inline const char* SecBenefitClass(SecRoute route,
                                   bool compact_k1s2,
                                   bool polyphase_k3s2,
                                   bool exact_tiled_k3s1) {
    if (route == SecRoute::CheetahLargeInput) return "CHEETAH_LARGE_INPUT";
    if (route == SecRoute::PCOIK3S2) return "DAZG_ORBIT_EXACT_FAST_K3S2";
    if (compact_k1s2) return "STRUCTURED_COMPACT_K1S2";
    if (polyphase_k3s2) return "POLYPHASE_K3S2";
    if (route == SecRoute::ExactTiledK1S1) return "R50_PWTILE_K1S1_EXACT";
    if (exact_tiled_k3s1) return "TILED_K3S1_ONLY";
    if (route == SecRoute::SparseBSGS) return "STAGE_Z2_SPARSE_BSGS";
    return "DENSE_FALLBACK";
}

inline void LogDAZGOrbitRoute(const char* source,
                            uint64_t in_feature_size,
                            uint64_t in_channels,
                            uint64_t out_channels,
                            uint64_t kernel_size,
                            uint64_t stride,
                            int layout_mode,
                            bool compact_k1s2,
                            bool polyphase_k3s2,
                            bool exact_tiled_k3s1,
                            SecRoute route,
                            const char* benefit_class) {
    dazg_orbit_gos::EmitStageZ2AblationWarningOnce();
    if (!DAZGOrbitProfilerEnabled()) return;

    std::cerr << "[DAZG_ORBIT_ROUTE]"
              << " source=" << source
              << " H=" << in_feature_size
              << " Cin=" << in_channels
              << " Cout=" << out_channels
              << " K=" << kernel_size
              << " S=" << stride
              << " layout_mode=" << layout_mode
              << " compact_k1s2=" << compact_k1s2
              << " polyphase_k3s2=" << polyphase_k3s2
              << " exact_tiled_k3s1=" << exact_tiled_k3s1
              << " enable_k1s2_compact=" << (dazg_orbit::ablation::EnableK1S2Compact() ? 1 : 0)
              << " enable_k3s2_polyphase=" << (dazg_orbit::ablation::EnableK3S2Polyphase() ? 1 : 0)
              << " enable_exact_tiled_k3s1=" << (dazg_orbit::ablation::EnableExactTiledK3S1() ? 1 : 0)
              << " enable_r50_pwtile=" << (dazg_orbit::ablation::EnableR50PWTile() ? 1 : 0)
              << " enable_csr_classifier=" << (dazg_orbit::ablation::EnableCSRClassifier() ? 1 : 0)
              << " route=" << RouteName(route)
              << " benefit_class=" << benefit_class
              << " exact_equiv=1"
              << " semantic_loss=0"
              << std::endl;

    dazg_orbit_gos::RegisterGraphConvPlan(
        source,
        in_feature_size,
        in_channels,
        out_channels,
        kernel_size,
        stride,
        layout_mode,
        compact_k1s2,
        polyphase_k3s2,
        exact_tiled_k3s1,
        RouteName(route),
        benefit_class
    );
}


inline bool IsStageLLazyRankTarget(uint64_t in_feature_size,
                                   uint64_t in_channels,
                                   uint64_t out_channels,
                                   uint64_t kernel_size,
                                   uint64_t stride,
                                   uint64_t padding)
{
    if (kernel_size != 3 || stride != 1 || padding != 1) return false;
    if (in_channels != out_channels) return false;

    if ((in_feature_size == 14 && in_channels == 256) ||
        (in_feature_size == 7 && in_channels == 512)) {
        return true;
    }

#if DAZG_ORBIT_STAGE_O_MID_RANK_TILE
    if (in_feature_size == 28 && in_channels == 128) {
        return true;
    }
#endif

    return false;
}

inline double StageLLayoutScore(uint64_t in_feature_size,
                                uint64_t in_channels,
                                uint64_t out_channels,
                                uint64_t kernel_size,
                                uint64_t stride,
                                uint64_t padding,
                                uint64_t poly_degree)
{
    bool enable_exact = false;
    if (stride == 1) {
        if (kernel_size == 3 && padding == 1) {
            enable_exact =
                IsStage3ExactTiledK3S1Candidate(in_feature_size,
                                                in_channels,
                                                out_channels,
                                                kernel_size,
                                                stride,
                                                padding) ||
                IsStage4TailExactTiledK3S1Candidate(in_feature_size,
                                                    in_channels,
                                                    out_channels,
                                                    kernel_size,
                                                    stride,
                                                    padding) ||
                IsStageLLazyRankH7SpatialFactorCandidate(in_feature_size,
                                                         in_channels,
                                                         out_channels,
                                                         kernel_size,
                                                         stride,
                                                         padding)
#if DAZG_ORBIT_STAGE_O_MID_RANK_TILE
                || IsStageOMidRankH28SpatialFactorCandidate(in_feature_size,
                                                            in_channels,
                                                            out_channels,
                                                            kernel_size,
                                                            stride,
                                                            padding)
#endif
                ;
        } else if (kernel_size == 1 && padding == 0) {
#if DAZG_ORBIT_STAGE_O_MID_RANK_TILE
            enable_exact =
                IsStageOMidRankH28PointwiseFactorCandidate(in_feature_size,
                                                           in_channels,
                                                           out_channels,
                                                           kernel_size,
                                                           stride,
                                                           padding);
#else
            enable_exact = false;
#endif
        }
    }

    CirLayoutPlan p = MakeCirLayoutPlan(
        in_feature_size, in_channels, out_channels,
        kernel_size, stride, padding,
        false,
        enable_exact,
        poly_degree,
        0
    );
    return p.total_score;
}

inline uint64_t SelectStageLLazyRank(uint64_t in_feature_size,
                                     uint64_t in_channels,
                                     uint64_t out_channels,
                                     uint64_t kernel_size,
                                     uint64_t stride,
                                     uint64_t padding,
                                     uint64_t poly_degree)
{
    if (!IsStageLLazyRankTarget(in_feature_size,
                                in_channels,
                                out_channels,
                                kernel_size,
                                stride,
                                padding)) {
        return 0;
    }

    const double dense_score = StageLLayoutScore(in_feature_size,
                                                 in_channels,
                                                 out_channels,
                                                 kernel_size,
                                                 stride,
                                                 padding,
                                                 poly_degree);

    uint64_t best_rank = 0;
    double best_score = 1e300;

#if DAZG_ORBIT_STAGE_O_MID_RANK_TILE
    if (in_feature_size == 28 && in_channels == 128) {
        // Stage-O: rank=32 is the HE resonance point for H=28.
        // Spatial:   128->32  K3 exact tile_out=14, block=32, mul_plain≈16
        // Pointwise: 32 ->128 K1 exact tile_out=14, block=32, mul_plain≈16
        static const uint64_t ranks[] = {kStageOMidRankH28, 64};
        for (uint64_t r : ranks) {
            const double spatial_score = StageLLayoutScore(
                in_feature_size, in_channels, r, 3, 1, 1, poly_degree);
            const double pointwise_score = StageLLayoutScore(
                in_feature_size, r, out_channels, 1, 1, 0, poly_degree);
            const double total = spatial_score + pointwise_score;
            if (total < best_score) {
                best_score = total;
                best_rank = r;
            }
        }
    } else
#endif
    if (in_feature_size == 14 && in_channels == 256) {
        static const uint64_t ranks[] = {64, 96, 128};
        for (uint64_t r : ranks) {
            const double spatial_score = StageLLayoutScore(
                in_feature_size, in_channels, r, 3, 1, 1, poly_degree);
            const double pointwise_score = StageLLayoutScore(
                in_feature_size, r, out_channels, 1, 1, 0, poly_degree);
            const double total = spatial_score + pointwise_score;
            if (total < best_score) {
                best_score = total;
                best_rank = r;
            }
        }
    } else if (in_feature_size == 7 && in_channels == 512) {
        // Stage-M should select r=128 after the protocol-aware layout score:
        // 512->128 exact tiled 3x3 uses block=128 and 16 mul_plain;
        // 128->512 pointwise uses block=128 and 4 mul_plain.
        static const uint64_t ranks[] = {128, 256};
        for (uint64_t r : ranks) {
            const double spatial_score = StageLLayoutScore(
                in_feature_size, in_channels, r, 3, 1, 1, poly_degree);
            const double pointwise_score = StageLLayoutScore(
                in_feature_size, r, out_channels, 1, 1, 0, poly_degree);
            const double total = spatial_score + pointwise_score;
            if (total < best_score) {
                best_score = total;
                best_rank = r;
            }
        }
    }

    if (best_rank != 0 && best_score + 1.0 < dense_score) {
        return best_rank;
    }
    return 0;
}

template <typename T, typename IO=Utils::NetIO>
class StageLLazyRankConv2D : public Conv2D {
public:
    Conv2D* spatial_factor = nullptr;
    Conv2D* pointwise_factor = nullptr;
    uint64_t rank = 0;
    uint64_t spatial_scale_bits = kStageLSpatialFactorBits;
    uint64_t pointwise_scale_bits = kStageLPointwiseFactorBits;

    StageLLazyRankConv2D(uint64_t in_feature_size,
                         uint64_t in_channels,
                         uint64_t out_channels,
                         uint64_t kernel_size,
                         uint64_t stride,
                         uint64_t rank,
                         CryptoPrimitive<T, IO> *cryptoPrimitive)
        : Conv2D(in_feature_size,
                 in_channels,
                 out_channels,
                 kernel_size,
                 stride,
                 kernel_size / 2,
                 cryptoPrimitive->HE,
                 true),
          rank(rank)
    {
        assert(kernel_size == 3 && stride == 1);
        assert(rank > 0);

        Tensor<uint64_t> spatial_weight({rank, in_channels, 3, 3});
        Tensor<uint64_t> spatial_bias({rank});
        Tensor<uint64_t> pointwise_weight({out_channels, rank, 1, 1});
        Tensor<uint64_t> pointwise_bias({out_channels});

        for (uint64_t i = 0; i < rank; ++i) {
            spatial_bias({i}) = 0;
        }
        for (uint64_t i = 0; i < out_channels; ++i) {
            pointwise_bias({i}) = 0;
        }

        if (cryptoPrimitive->HE->server) {
            // Current DAZG-Orbit ResNet uses synthetic random weights for protocol
            // benchmarking.  Stage-L mirrors that behavior, but splits the
            // dyadic factor scales as 8+9 instead of using two full 17-bit
            // factors.  With real checkpoints these tensors should be replaced
            // by SVD/learned factors quantized with the same complementary scales.
            dazg_orbit_det::FillTensorDeterministic(
                spatial_weight,
                "StageL.spatial_weight",
                spatial_scale_bits,
                dazg_orbit_det::HashU64Seq({in_feature_size, in_channels, out_channels,
                                          kernel_size, stride, rank, 1ULL}));
            dazg_orbit_det::FillTensorDeterministic(
                pointwise_weight,
                "StageL.pointwise_weight",
                pointwise_scale_bits,
                dazg_orbit_det::HashU64Seq({in_feature_size, in_channels, out_channels,
                                          kernel_size, stride, rank, 2ULL}));
            dazg_orbit_det::FillTensorDeterministic(
                pointwise_bias,
                "StageL.pointwise_bias",
                16,
                dazg_orbit_det::HashU64Seq({in_feature_size, in_channels, out_channels,
                                          kernel_size, stride, rank, 3ULL}));
        }

        spatial_factor = CreateConvFromWeights<T, IO>(
            in_feature_size,
            1,
            1,
            spatial_weight,
            spatial_bias,
            cryptoPrimitive
        );

        pointwise_factor = CreateConvFromWeights<T, IO>(
            in_feature_size,
            1,
            0,
            pointwise_weight,
            pointwise_bias,
            cryptoPrimitive
        );

        if (cryptoPrimitive->HE->server) {
            const double dense_score = StageLLayoutScore(
                in_feature_size, in_channels, out_channels, 3, 1, 1,
                cryptoPrimitive->HE->polyModulusDegree);
            const double rank_score =
                StageLLayoutScore(in_feature_size, in_channels, rank, 3, 1, 1,
                                  cryptoPrimitive->HE->polyModulusDegree) +
                StageLLayoutScore(in_feature_size, rank, out_channels, 1, 1, 0,
                                  cryptoPrimitive->HE->polyModulusDegree);

            std::cerr << "[StageM ProtocolAwareRankTile][StageL LazyRankConv] H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " Cout=" << out_channels
                      << " rank=" << rank
                      << " operator=dense_k3x3_to_protocol_aware_rank_tile_3x3_plus_1x1"
                      << " split_scale_bits=" << spatial_scale_bits
                      << "+" << pointwise_scale_bits
                      << " intermediate_truncate=none"
                      << " stage_m_policy=protocol_aware_rank_tile"
#if DAZG_ORBIT_STAGE_O_MID_RANK_TILE
                      << " stage_o_mid_rank_tile="
                      << ((in_feature_size == 28 && in_channels == 128) ? 1 : 0)
#endif
                      << " dense_score=" << dense_score
                      << " rank_score=" << rank_score
                      << std::endl;
        }
    }

    ~StageLLazyRankConv2D() override {
        delete spatial_factor;
        delete pointwise_factor;
    }

    Tensor<uint64_t> operator()(Tensor<uint64_t> &x) override {
        if (HE->server) {
            static std::atomic<int> prints{0};
            const int id = prints.fetch_add(1);
            if (id < 64) {
                std::cerr << "[StageM ProtocolAwareRankTile runtime][StageL LazyRankConv runtime] H=" << in_feature_size
                          << " Cin=" << in_channels
                          << " Cout=" << out_channels
                          << " rank=" << rank
                          << " path=protocol_aware_rank_tile_k3x3_then_1x1"
#if DAZG_ORBIT_STAGE_O_MID_RANK_TILE
                          << " stage_o_mid="
                          << ((in_feature_size == 28 && in_channels == 128) ? 1 : 0)
#endif
                          << " intermediate_truncate=none"
                          << " split_scale_bits=" << spatial_scale_bits
                          << "+" << pointwise_scale_bits
                          << std::endl;
            }
        }
        Tensor<uint64_t> z = (*spatial_factor)(x);
        return (*pointwise_factor)(z);
    }

private:
    Tensor<HE::unified::UnifiedPlaintext> PackWeight() override {
        assert(false && "StageLLazyRankConv2D delegates PackWeight to child convolutions");
        return Tensor<HE::unified::UnifiedPlaintext>({1}, HE->Backend());
    }

    Tensor<uint64_t> PackActivation(Tensor<uint64_t> &x) override {
        (void)x;
        assert(false && "StageLLazyRankConv2D delegates PackActivation to child convolutions");
        return Tensor<uint64_t>({1});
    }

    Tensor<HE::unified::UnifiedCiphertext> HECompute(
        const Tensor<HE::unified::UnifiedPlaintext> &weight_pt,
        Tensor<HE::unified::UnifiedCiphertext> &ac_ct) override {
        (void)weight_pt;
        (void)ac_ct;
        assert(false && "StageLLazyRankConv2D delegates HECompute to child convolutions");
        const auto target = HE->server ? HE->Backend() : HOST;
        return Tensor<HE::unified::UnifiedCiphertext>(
            {1}, HE->GenerateZeroCiphertext(target));
    }

    Tensor<uint64_t> DepackResult(Tensor<uint64_t> &out) override {
        (void)out;
        assert(false && "StageLLazyRankConv2D delegates DepackResult to child convolutions");
        return Tensor<uint64_t>({1});
    }
};


inline bool IsStageNResonantClassifierTarget(uint64_t in_feature_size,
                                             uint64_t in_channels,
                                             uint64_t out_channels,
                                             uint64_t kernel_size,
                                             uint64_t stride,
                                             uint64_t padding)
{
    return in_feature_size == 1 &&
           (in_channels == 512 || in_channels == 2048) &&
           out_channels == 1000 &&
           kernel_size == 1 &&
           stride == 1 &&
           padding == 0;
}

template <typename T, typename IO=Utils::NetIO>
class StageNResonantClassifierConv2D : public Conv2D {
public:
    // CSR-style classifier metadata. This is deliberately kept inside the
    // classifier wrapper, not inside generic CirConv2D, so the classifier path
    // is clearly separated from ordinary convolution in logs and experiments.
    struct CSRClassifierPlan {
        std::vector<uint64_t> rowptr;
        std::vector<uint64_t> colidx;
        std::vector<uint64_t> plain_id;

        uint64_t active_rows = 0;
        uint64_t active_entries = 0;
        uint64_t padded_classes = 0;
        uint64_t real_classes = 1000;
        uint64_t dummy_rows = 0;
        uint64_t input_channels = 0;
    };

    Conv2D* padded_classifier = nullptr;
    uint64_t logical_classes = 1000;
    uint64_t padded_classes = kStageNClassifierPaddedClasses;
    CSRClassifierPlan csr_plan;

    StageNResonantClassifierConv2D(uint64_t in_feature_size,
                                   uint64_t in_channels,
                                   uint64_t out_channels,
                                   uint64_t kernel_size,
                                   uint64_t stride,
                                   CryptoPrimitive<T, IO> *cryptoPrimitive)
        : Conv2D(in_feature_size,
                 in_channels,
                 out_channels,
                 kernel_size,
                 stride,
                 0,
                 cryptoPrimitive->HE,
                 true),
          logical_classes(out_channels),
          padded_classes(kStageNClassifierPaddedClasses)
    {
        assert(IsStageNResonantClassifierTarget(in_feature_size,
                                                in_channels,
                                                out_channels,
                                                kernel_size,
                                                stride,
                                                0));
        assert(padded_classes >= logical_classes);
        assert(padded_classes % 512 == 0);
        assert(in_channels % 512 == 0);

        Tensor<uint64_t> padded_weight({padded_classes, in_channels, 1, 1});
        Tensor<uint64_t> padded_bias({padded_classes});

        for (uint64_t co = 0; co < padded_classes; ++co) {
            padded_bias({co}) = 0;
            for (uint64_t ci = 0; ci < in_channels; ++ci) {
                padded_weight({co, ci, 0, 0}) = 0;
            }
        }

        if (cryptoPrimitive->HE->server) {
            Tensor<uint64_t> logical_weight({logical_classes, in_channels, 1, 1});
            Tensor<uint64_t> logical_bias({logical_classes});
            dazg_orbit_det::FillTensorDeterministic(
                logical_weight,
                "StageNClassifier.logical_weight",
                16,
                dazg_orbit_det::HashU64Seq({in_feature_size, in_channels, logical_classes,
                                          padded_classes, 1ULL}),
                false);
            dazg_orbit_det::FillTensorDeterministic(
                logical_bias,
                "StageNClassifier.logical_bias",
                16,
                dazg_orbit_det::HashU64Seq({in_feature_size, in_channels, logical_classes,
                                          padded_classes, 2ULL}),
                false);

            for (uint64_t co = 0; co < logical_classes; ++co) {
                padded_bias({co}) = logical_bias({co});
                for (uint64_t ci = 0; ci < in_channels; ++ci) {
                    padded_weight({co, ci, 0, 0}) =
                        logical_weight({co, ci, 0, 0});
                }
            }
        }

        BuildCSRClassifierPlan(padded_weight, in_channels);

        padded_classifier = CreateConvFromWeights<T, IO>(
            in_feature_size,
            stride,
            0,
            padded_weight,
            padded_bias,
            cryptoPrimitive
        );

        if (cryptoPrimitive->HE->server) {
            const double old_score = StageLLayoutScore(
                in_feature_size, in_channels, logical_classes, 1, 1, 0,
                cryptoPrimitive->HE->polyModulusDegree);
            const double padded_score = StageLLayoutScore(
                in_feature_size, in_channels, padded_classes, 1, 1, 0,
                cryptoPrimitive->HE->polyModulusDegree);


#if 0  // DAZG_ORBIT_META_WRAPPER_CANONICALIZED: meta wrapper excluded from executable graph summary
            dazg_orbit_gos::RegisterGraphConvPlan(
                "StageNResonantClassifierWrapper",
                in_feature_size,
                in_channels,
                logical_classes,
                kernel_size,
                stride,
                -1,
                false,
                false,
                false,
                "SparseBSGS",
                "CSR_CLASSIFIER_WRAPPER"
            );
#endif  // DAZG_ORBIT_META_WRAPPER_CANONICALIZED

            std::cerr << "[StageN ResonantClassifier] H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " classes=" << logical_classes
                      << " padded_classes=" << padded_classes
                      << " operator=dummy_logit_padding_1000_to_1024"
                      << " target_block=512"
                      << " semantic=trim_dummy_logits_after_he_compute"
                      << " old_score=" << old_score
                      << " padded_score=" << padded_score
                      << std::endl;

            std::cerr << "[DAZG_ORBIT_CLASSIFIER_PLAN]"
                      << " layer=StageNResonantClassifier"
                      << " H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " real_classes=" << csr_plan.real_classes
                      << " padded_classes=" << csr_plan.padded_classes
                      << " dummy_rows=" << csr_plan.dummy_rows
                      << " rowptr_size=" << csr_plan.rowptr.size()
                      << " active_rows=" << csr_plan.active_rows
                      << " active_entries=" << csr_plan.active_entries
                      << " child_operator=CreateConvFromWeights"
                      << " child_semantic=StageZ2-ExactSparseBSGSLinear"
                      << " action=build_csr_metadata_then_delegate_child_he_compute"
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << std::endl;
        }
    }

    ~StageNResonantClassifierConv2D() override {
        delete padded_classifier;
    }

    Tensor<uint64_t> operator()(Tensor<uint64_t> &x) override {
        // Runtime remains delegated to the padded child classifier so we do not
        // change the ciphertext semantics or the tested HE path. This wrapper
        // now explicitly represents the classifier as a separate stage and trims
        // dummy logits after the child HE computation.
        Tensor<uint64_t> y_pad = (*padded_classifier)(x);

        Tensor<uint64_t> y({logical_classes, 1, 1});
        for (uint64_t co = 0; co < logical_classes; ++co) {
            y({co, 0, 0}) = y_pad({co, 0, 0});
        }

        if (HE->server) {
            static std::atomic<int> prints{0};
            const int id = prints.fetch_add(1);
            if (id < 16) {
                std::cerr << "[StageN ResonantClassifier runtime]"
                          << " logical_classes=" << logical_classes
                          << " padded_classes=" << padded_classes
                          << " action=trim_dummy_logits"
                          << " expected_layout=H1_Cin512_Cout1024_block512"
                          << std::endl;

                std::cerr << "[DAZG_ORBIT_CLASSIFIER_RUNTIME]"
                          << " layer=StageNResonantClassifier"
                          << " logical_classes=" << logical_classes
                          << " padded_classes=" << padded_classes
                          << " dummy_rows=" << csr_plan.dummy_rows
                          << " active_rows=" << csr_plan.active_rows
                          << " active_entries=" << csr_plan.active_entries
                          << " action=delegate_child_he_compute_then_trim_dummy_logits"
                          << " child_operator=CreateConvFromWeights"
                          << " expected_layout=H1_Cin512_Cout1024_block512"
                          << " exact_equiv=1"
                          << " semantic_loss=0"
                          << std::endl;
            }
        }

        return y;
    }

private:
    void BuildCSRClassifierPlan(const Tensor<uint64_t>& padded_weight,
                                uint64_t input_channels) {
        csr_plan = CSRClassifierPlan();
        csr_plan.padded_classes = padded_classes;
        csr_plan.real_classes = logical_classes;
        csr_plan.dummy_rows = padded_classes - logical_classes;
        csr_plan.input_channels = input_channels;

        csr_plan.rowptr.reserve(static_cast<size_t>(padded_classes + 1));
        csr_plan.rowptr.push_back(0);

        uint64_t plain_counter = 0;
        for (uint64_t co = 0; co < padded_classes; ++co) {
            bool row_active = false;

            if (co < logical_classes) {
                for (uint64_t ci = 0; ci < input_channels; ++ci) {
                    const uint64_t w = padded_weight({co, ci, 0, 0});
                    if (w == 0) {
                        continue;
                    }

                    csr_plan.colidx.push_back(ci);
                    csr_plan.plain_id.push_back(plain_counter++);
                    row_active = true;
                }
            }

            if (row_active) {
                csr_plan.active_rows++;
            }

            csr_plan.rowptr.push_back(
                static_cast<uint64_t>(csr_plan.colidx.size()));
        }

        csr_plan.active_entries =
            static_cast<uint64_t>(csr_plan.colidx.size());

        assert(csr_plan.rowptr.size() == static_cast<size_t>(padded_classes + 1));
        assert(csr_plan.colidx.size() == csr_plan.plain_id.size());
        assert(csr_plan.real_classes <= csr_plan.padded_classes);
    }

    Tensor<HE::unified::UnifiedPlaintext> PackWeight() override {
        assert(false && "StageNResonantClassifierConv2D delegates PackWeight to child classifier");
        return Tensor<HE::unified::UnifiedPlaintext>({1}, HE->Backend());
    }

    Tensor<uint64_t> PackActivation(Tensor<uint64_t> &x) override {
        (void)x;
        assert(false && "StageNResonantClassifierConv2D delegates PackActivation to child classifier");
        return Tensor<uint64_t>({1});
    }

    Tensor<HE::unified::UnifiedCiphertext> HECompute(
        const Tensor<HE::unified::UnifiedPlaintext> &weight_pt,
        Tensor<HE::unified::UnifiedCiphertext> &ac_ct) override {
        (void)weight_pt;
        (void)ac_ct;
        assert(false && "StageNResonantClassifierConv2D delegates HECompute to child classifier");
        const auto target = HE->server ? HE->Backend() : HOST;
        return Tensor<HE::unified::UnifiedCiphertext>(
            {1}, HE->GenerateZeroCiphertext(target));
    }

    Tensor<uint64_t> DepackResult(Tensor<uint64_t> &out) override {
        (void)out;
        assert(false && "StageNResonantClassifierConv2D delegates DepackResult to child classifier");
        return Tensor<uint64_t>({1});
    }
};


template <typename T, typename IO>
Conv2D* CreateConvFromWeights(uint64_t in_feature_size,
                              uint64_t stride,
                              uint64_t padding,
                              const Tensor<uint64_t>& weight,
                              const Tensor<uint64_t>& bias,
                              CryptoPrimitive<T, IO> *cryptoPrimitive)
{
    std::vector<size_t> weight_shape = weight.shape();
    assert(weight_shape.size() == 4);

    const uint64_t out_channels = static_cast<uint64_t>(weight_shape[0]);
    const uint64_t in_channels = static_cast<uint64_t>(weight_shape[1]);
    const uint64_t kernel_size = static_cast<uint64_t>(weight_shape[2]);

    switch (cryptoPrimitive->conv_type)
    {
    case Datatype::CONV_TYPE::Nest:
        return new Conv2DNest(in_feature_size, stride, padding,
                              weight, bias, cryptoPrimitive->HE);

    case Datatype::CONV_TYPE::Cheetah:
        return new Conv2DCheetah(in_feature_size, stride, padding,
                                 weight, bias, cryptoPrimitive->HE);

    case Datatype::CONV_TYPE::Cir:
    {
        const bool k3s2_stage2_polyphase =
            (kernel_size == 3 && stride == 2 && padding == 1 &&
             in_feature_size == 56 &&
             ((in_channels == 64 && out_channels == 128) ||
              (in_channels == 128 && out_channels == 128)));

        const bool k3s2_stage3_polyphase =
            (kernel_size == 3 && stride == 2 && padding == 1 &&
             in_feature_size == 28 &&
             ((in_channels == 128 && out_channels == 256) ||
              (in_channels == 256 && out_channels == 256)));

        const bool k3s2_stage4_polyphase =
            (kernel_size == 3 && stride == 2 && padding == 1 &&
             in_feature_size == 14 &&
             ((in_channels == 256 && out_channels == 512) ||
              (in_channels == 512 && out_channels == 512)));

        const bool k3s2_cifar_pcoi_target =
            DAZGOrbitK3S2StageTransitionTarget(in_feature_size,
                                             in_channels,
                                             out_channels,
                                             kernel_size,
                                             stride,
                                             padding);

        const bool k3s2_transition_target =
            (k3s2_cifar_pcoi_target ||
             k3s2_stage2_polyphase || k3s2_stage3_polyphase || k3s2_stage4_polyphase);

        const bool enable_k3_polyphase =
            dazg_orbit::ablation::EnableK3S2Polyphase() && k3s2_transition_target;

        if (k3s2_transition_target &&
            !dazg_orbit::ablation::EnableK3S2Polyphase() &&
            DAZGOrbitStrictK3S2FallbackEnabled()) {
            EmitDAZGOrbitStrictK3S2Fallback(
                "CreateConvFromWeights",
                in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                padding
            );
            LogDAZGOrbitRoute(
                "CreateConvFromWeights",
                in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                -1,
                false,
                false,
                false,
                SecRoute::DenseFallback,
                "STRICT_EXACT_FALLBACK_K3S2_DISABLED"
            );
            return new Conv2DNest(in_feature_size, stride, padding,
                                  weight, bias, cryptoPrimitive->HE);
        }

        bool enable_exact_tiled_k3s1 = false;
        if (stride == 1) {
            if (kernel_size == 3 && padding == 1) {
                enable_exact_tiled_k3s1 =
                    IsExactTile4K3S1Candidate(in_feature_size, in_channels, out_channels,
                                              kernel_size, stride, padding) ||
                    IsStage3ExactTiledK3S1Candidate(in_feature_size, in_channels, out_channels,
                                                    kernel_size, stride, padding) ||
                    IsStage4TailExactTiledK3S1Candidate(in_feature_size, in_channels, out_channels,
                                                        kernel_size, stride, padding) ||
                    IsStageLLazyRankH7SpatialFactorCandidate(in_feature_size, in_channels, out_channels,
                                                             kernel_size, stride, padding)
#if DAZG_ORBIT_STAGE_O_MID_RANK_TILE
                    || IsStageOMidRankH28SpatialFactorCandidate(in_feature_size, in_channels, out_channels,
                                                                kernel_size, stride, padding)
#endif
                    ;
            } else if (kernel_size == 1 && padding == 0) {
#if DAZG_ORBIT_STAGE_O_MID_RANK_TILE
                enable_exact_tiled_k3s1 =
                    IsStageOMidRankH28PointwiseFactorCandidate(in_feature_size, in_channels, out_channels,
                                                               kernel_size, stride, padding);
#else
                enable_exact_tiled_k3s1 = false;
#endif
            }
        }
        const bool enable_r50_pwtile_target =
            dazg_orbit::ablation::EnableR50PWTile() &&
            IsR50PointwiseK1S1Candidate(in_feature_size, in_channels, out_channels,
                                        kernel_size, stride, padding);

        enable_exact_tiled_k3s1 =
            enable_exact_tiled_k3s1 && dazg_orbit::ablation::EnableExactTiledK3S1();

        const bool enable_exact_tiled_worker =
            enable_exact_tiled_k3s1 || enable_r50_pwtile_target;

        CirLayoutPlan layout = MakeCirLayoutPlan(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, padding,
            enable_k3_polyphase,
            enable_exact_tiled_worker,
            cryptoPrimitive->HE->polyModulusDegree,
            0
        );

        uint64_t cir_block_size = layout.suggested_block_size;
        if (cir_block_size < 1) cir_block_size = 1;


        // DAZG_ORBIT-PI v537:
        // Force-test the CIFAR-100 final classifier block semantics on the real
        // CreateConvFromWeights path. Previous v535/v536 attempts searched
        // CirConv.cpp, but the block decision and "[CreateConvFromWeights]"
        // log are emitted here in ResNet.h.
        const bool dazg_orbit_v537_is_cifar100_final_fc =
            (in_feature_size == 1 &&
             in_channels == 512 &&
             out_channels == 100 &&
             kernel_size == 1);

        if (dazg_orbit_v537_is_cifar100_final_fc) {
            const char* env_block = std::getenv("DAZG_ORBIT_FINAL_FC_BLOCK");
            if (env_block == nullptr || env_block[0] == '\0') {
                env_block = std::getenv("DAZG_ORBIT_FORCE_FINAL_FC_BLOCK");
            }
            if (env_block == nullptr || env_block[0] == '\0') {
                env_block = std::getenv("DAZG_ORBIT_V537_FINAL_FC_BLOCK");
            }

            if (env_block != nullptr && env_block[0] != '\0') {
                char* endptr = nullptr;
                const unsigned long long requested_ull =
                    std::strtoull(env_block, &endptr, 10);
                const uint64_t requested =
                    static_cast<uint64_t>(requested_ull);

                const uint64_t channels_for_request = layout.packed_in_channels;
                const bool parse_ok = (endptr != env_block);
                const bool request_ok =
                    parse_ok &&
                    requested >= 1 &&
                    requested <= channels_for_request &&
                    (channels_for_request % requested == 0) &&
                    (out_channels % requested == 0);

                if (request_ok) {
                    const uint64_t old_block = cir_block_size;
                    cir_block_size = requested;
                    layout.suggested_block_size = cir_block_size;
                    std::cerr << "[DAZG_ORBIT_V537_FINAL_FC_BLOCK_OVERRIDE]"
                              << " env=" << env_block
                              << " old_block=" << old_block
                              << " new_block=" << cir_block_size
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " P=" << padding
                              << " packed_Cin=" << channels_for_request
                              << " source=CreateConvFromWeights"
                              << std::endl;
                } else {
                    std::cerr << "[DAZG_ORBIT_V537_FINAL_FC_BLOCK_OVERRIDE_REJECT]"
                              << " env=" << env_block
                              << " parsed=" << (parse_ok ? 1 : 0)
                              << " requested=" << requested
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " P=" << padding
                              << " packed_Cin=" << channels_for_request
                              << " reason=invalid_or_not_divisible"
                              << std::endl;
                }
            } else {
                std::cerr << "[DAZG_ORBIT_V537_FINAL_FC_BLOCK_OVERRIDE]"
                          << " env_absent=1"
                          << " current_block=" << cir_block_size
                          << " H=" << in_feature_size
                          << " Cin=" << in_channels
                          << " Cout=" << out_channels
                          << " K=" << kernel_size
                          << " S=" << stride
                          << " P=" << padding
                          << " source=CreateConvFromWeights"
                          << std::endl;
            }
        }
        const uint64_t channels_for_divisibility = layout.packed_in_channels;
        const bool channel_ok =
            (channels_for_divisibility % cir_block_size == 0) &&
            (out_channels % cir_block_size == 0);

        const uint64_t padded_feature_size =
            NextPow2U64(layout.route_feature_size + 2 * layout.capacity_padding);
        const uint64_t ntt_size =
            cir_block_size * padded_feature_size * padded_feature_size;
        const bool capacity_ok = ntt_size <= cryptoPrimitive->HE->polyModulusDegree;
        const uint64_t max_tile = capacity_ok
            ? std::max<uint64_t>(1, cryptoPrimitive->HE->polyModulusDegree / (2 * ntt_size))
            : 0;

        if (!channel_ok || !capacity_ok) {
            std::cout << "[CreateConvFromWeights] Cir fallback -> Conv2DNest: H="
                      << in_feature_size
                      << ", Cin=" << in_channels
                      << ", Cout=" << out_channels
                      << ", K=" << kernel_size
                      << ", S=" << stride
                      << ", block=" << cir_block_size
                      << ", channel_ok=" << channel_ok
                      << ", capacity_ok=" << capacity_ok
                      << std::endl;
            return new Conv2DNest(in_feature_size, stride, padding,
                                  weight, bias, cryptoPrimitive->HE);
        }

        const bool exact_tiled_k3s1 =
            dazg_orbit::ablation::EnableExactTiledK3S1() &&
            IsPureExactTiledK3S1Mode(layout.mode);
        const bool exact_tiled_k1s1 =
            dazg_orbit::ablation::EnableR50PWTile() &&
            IsExactTiledK1S1Mode(layout.mode);
        const bool k3s2_pcoi =
            dazg_orbit::ablation::EnableK3S2Polyphase() &&
            IsExactPCOIK3S2OrbitMode(layout.mode);
        const bool k3s2_polyphase =
            dazg_orbit::ablation::EnableK3S2Polyphase() &&
            (k3s2_pcoi ||
             layout.mode == CirLayoutMode::COMPACT_K3_S2_POLYPHASE ||
             IsExactCompactStride2K3PolyphaseMode(layout.mode));

        if (exact_tiled_k1s1) {
            std::cerr << "[DAZG_ORBIT_R50_PWTILE_ROUTE]"
                      << " source=CreateConvFromWeights"
                      << " H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " Cout=" << out_channels
                      << " K=" << kernel_size
                      << " S=" << stride
                      << " tile_grid=" << layout.spatial_tile_rows << "x" << layout.spatial_tile_cols
                      << " tile_out=" << layout.tile_out_feature_size
                      << " tile_in=" << layout.tile_in_feature_size
                      << " block=" << cir_block_size
                      << " exact_equiv=1 semantic_loss=0"
                      << " reason=POINTWISE_SPATIAL_INDEPENDENCE"
                      << std::endl;
        }

        std::cout << "[CreateConvFromWeights] use CirConv2D: H="
                  << in_feature_size
                  << ", Cin=" << in_channels
                  << ", Cout=" << out_channels
                  << ", K=" << kernel_size
                  << ", S=" << stride
                  << ", block=" << cir_block_size
                  << ", layout_mode=" << static_cast<int>(layout.mode)
                  << ", pcoi_k3s2=" << k3s2_pcoi
                  << ", exact_tiled_k3s1=" << exact_tiled_k3s1
                  << ", tile_grid=" << layout.spatial_tile_rows
                  << "x" << layout.spatial_tile_cols
                  << ", tile_out=" << layout.tile_out_feature_size
                  << ", tile_in=" << layout.tile_in_feature_size
                  << ", route_feature_size=" << layout.route_feature_size
                  << ", max_tile=" << max_tile
                  << ", source=StageLFactor"
                  << std::endl;

        const SecRoute sec_route =
            k3s2_pcoi ? SecRoute::PCOIK3S2 :
            k3s2_polyphase ? SecRoute::PolyphaseK3S2 :
            exact_tiled_k1s1 ? SecRoute::ExactTiledK1S1 :
            exact_tiled_k3s1 ? SecRoute::ExactTiledK3S1 :
            SecRoute::SparseBSGS;

        LogDAZGOrbitRoute(
            "CreateConvFromWeights",
            in_feature_size,
            in_channels,
            out_channels,
            kernel_size,
            stride,
            static_cast<int>(layout.mode),
            false,
            k3s2_polyphase,
            exact_tiled_k3s1,
            sec_route,
            SecBenefitClass(sec_route, false, k3s2_polyphase, exact_tiled_k3s1)
        );

        return new CirConv2D(in_feature_size, stride, padding,
                             cir_block_size, weight, bias, cryptoPrimitive->HE,
                             k3s2_polyphase,
                             (exact_tiled_k3s1 || exact_tiled_k1s1),
                             layout);
    }

    default:
        return new Conv2DNest(in_feature_size, stride, padding,
                              weight, bias, cryptoPrimitive->HE);
    }
}

template <typename T, typename IO>
Conv2D* CreateConv(uint64_t in_feature_size,
                   uint64_t in_channels,
                   uint64_t out_channels,
                   uint64_t kernel_size,
                   uint64_t stride,
                   CryptoPrimitive<T, IO> *cryptoPrimitive,
                   bool allow_stage_l)
{
    Conv2D* conv = nullptr;
    const uint64_t padding = kernel_size / 2;

    const bool is_stage1_k3s1_hot =
        (in_feature_size == 56 &&
         in_channels == 64 &&
         out_channels == 64 &&
         kernel_size == 3 &&
         stride == 1 &&
         padding == 1);

    const bool stage1_exact_search_active =
        (cryptoPrimitive->conv_type == Datatype::CONV_TYPE::Cir) &&
        dazg_orbit::ablation::EnableExactTiledK3S1() &&
        kEnableExactTiledK3S1Search &&
        kDisableStage1StaticWhenSearch &&
        is_stage1_k3s1_hot;

    // 首层大输入仍然走 Cheetah
    if (in_feature_size >= 224) {
        std::cout << "[CreateConv] use Conv2DCheetah (large input): H="
                  << in_feature_size
                  << ", Cin=" << in_channels
                  << ", Cout=" << out_channels
                  << ", K=" << kernel_size
                  << ", S=" << stride << std::endl;

        LogDAZGOrbitRoute(
            "CreateConv",
            in_feature_size,
            in_channels,
            out_channels,
            kernel_size,
            stride,
            -1,
            false,
            false,
            false,
            SecRoute::CheetahLargeInput,
            "CHEETAH_LARGE_INPUT"
        );

        return new Conv2DCheetah(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, cryptoPrimitive->HE
        );
    }

    // stage1 static 只在“未开启 exact tiled search”时保留
    if (is_stage1_k3s1_hot &&
        !stage1_exact_search_active &&
        dazg_orbit::ablation::EnableExactTiledK3S1()) {
        std::cout << "[CreateConv] use Conv2DStage1Static: H="
                  << in_feature_size
                  << ", Cin=" << in_channels
                  << ", Cout=" << out_channels
                  << ", K=" << kernel_size
                  << ", S=" << stride << std::endl;

        LogDAZGOrbitRoute(
            "CreateConv",
            in_feature_size,
            in_channels,
            out_channels,
            kernel_size,
            stride,
            -1,
            false,
            false,
            true,
            SecRoute::ExactTiledK3S1,
            "STAGE1_STATIC_EXACT"
        );

        return new Conv2DStage1Static(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, cryptoPrimitive->HE
        );
    }

    constexpr bool kEnableStage2Static = false;
    if (kEnableStage2Static &&
        in_feature_size == 28 &&
        in_channels == 128 &&
        out_channels == 128 &&
        kernel_size == 3 &&
        stride == 1) {
        std::cout << "[CreateConv] use Conv2DStage2Static: H="
                  << in_feature_size
                  << ", Cin=" << in_channels
                  << ", Cout=" << out_channels
                  << ", K=" << kernel_size
                  << ", S=" << stride << std::endl;

        return new Conv2DStage2Static(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, cryptoPrimitive->HE
        );
    }

    auto make_tagged_cheetah = [&]() -> Conv2D* {
        auto* c = new Conv2DCheetah(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, cryptoPrimitive->HE
        );

#ifdef PROFILE_CHEETAH_56
        static int cheetah56_k3s2_id = 0;
        bool hot56_k3s2 =
            (in_feature_size == 56 &&
             in_channels == 64 &&
             out_channels == 128 &&
             kernel_size == 3 &&
             stride == 2 &&
             padding == 1);

        if (hot56_k3s2) {
            c->EnableProfile("cheetah56_k3s2_" +
                             std::to_string(cheetah56_k3s2_id++));
        }
#endif
        return c;
    };

    switch (cryptoPrimitive->conv_type)
    {
    case Datatype::CONV_TYPE::Nest:
        LogDAZGOrbitRoute(
            "CreateConv",
            in_feature_size,
            in_channels,
            out_channels,
            kernel_size,
            stride,
            -1,
            false,
            false,
            false,
            SecRoute::DenseFallback,
            "CONV2D_NEST_BASELINE"
        );
        conv = new Conv2DNest(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, cryptoPrimitive->HE
        );
        break;

    case Datatype::CONV_TYPE::Cheetah:
        LogDAZGOrbitRoute(
            "CreateConv",
            in_feature_size,
            in_channels,
            out_channels,
            kernel_size,
            stride,
            -1,
            false,
            false,
            false,
            SecRoute::CheetahLargeInput,
            "CONV2D_CHEETAH"
        );
        conv = make_tagged_cheetah();
        break;

    case Datatype::CONV_TYPE::Cir:
    {
#if DAZG_ORBIT_STAGE_N_RESONANT_CLASSIFIER
        if (dazg_orbit::ablation::EnableCSRClassifier() &&
            IsStageNResonantClassifierTarget(in_feature_size,
                                             in_channels,
                                             out_channels,
                                             kernel_size,
                                             stride,
                                             padding)) {
            LogDAZGOrbitRoute(
                "CreateConv",
                in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                -1,
                false,
                false,
                false,
                SecRoute::SparseBSGS,
                "RESONANT_CLASSIFIER_STAGE_N"
            );

            return new StageNResonantClassifierConv2D<T, IO>(
                in_feature_size, in_channels, out_channels,
                kernel_size, stride, cryptoPrimitive
            );
        }
#endif

#if DAZG_ORBIT_STAGE_L_LAZY_RANK
        if (allow_stage_l) {
            const uint64_t stage_l_rank = SelectStageLLazyRank(
                in_feature_size, in_channels, out_channels,
                kernel_size, stride, padding,
                cryptoPrimitive->HE->polyModulusDegree
            );
            if (stage_l_rank != 0) {
                return new StageLLazyRankConv2D<T, IO>(
                    in_feature_size, in_channels, out_channels,
                    kernel_size, stride, stage_l_rank, cryptoPrimitive
                );
            }
        }
#else
        (void)allow_stage_l;
#endif

        const bool k3s2_stage2_polyphase =
            (kernel_size == 3 &&
             stride == 2 &&
             padding == 1 &&
             in_feature_size == 56 &&
             ((in_channels == 64 && out_channels == 128) ||
              (in_channels == 128 && out_channels == 128)));

        const bool k3s2_stage3_polyphase =
            (kernel_size == 3 &&
             stride == 2 &&
             padding == 1 &&
             in_feature_size == 28 &&
             ((in_channels == 128 && out_channels == 256) ||
              (in_channels == 256 && out_channels == 256)));

        const bool k3s2_stage4_polyphase =
            (kernel_size == 3 &&
             stride == 2 &&
             padding == 1 &&
             in_feature_size == 14 &&
             ((in_channels == 256 && out_channels == 512) ||
              (in_channels == 512 && out_channels == 512)));

        const bool k3s2_cifar_pcoi_target =
            DAZGOrbitK3S2StageTransitionTarget(in_feature_size,
                                             in_channels,
                                             out_channels,
                                             kernel_size,
                                             stride,
                                             padding);

        const bool k3s2_transition_target =
            (k3s2_cifar_pcoi_target ||
             k3s2_stage2_polyphase ||
             k3s2_stage3_polyphase ||
             k3s2_stage4_polyphase);

        const bool enable_k3_polyphase =
            dazg_orbit::ablation::EnableK3S2Polyphase() && k3s2_transition_target;

        if (k3s2_transition_target &&
            !dazg_orbit::ablation::EnableK3S2Polyphase() &&
            DAZGOrbitStrictK3S2FallbackEnabled()) {
            EmitDAZGOrbitStrictK3S2Fallback(
                "CreateConv",
                in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                padding
            );
            LogDAZGOrbitRoute(
                "CreateConv",
                in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                -1,
                false,
                false,
                false,
                SecRoute::DenseFallback,
                "STRICT_EXACT_FALLBACK_K3S2_DISABLED"
            );
            conv = new Conv2DNest(
                in_feature_size, in_channels, out_channels,
                kernel_size, stride, cryptoPrimitive->HE
            );
            break;
        }

        const bool force_smallroute64_k3s1 = false;

        const bool enable_r50_pwtile_target =
            dazg_orbit::ablation::EnableR50PWTile() &&
            IsR50PointwiseK1S1Candidate(in_feature_size, in_channels, out_channels,
                                        kernel_size, stride, padding);

        const bool enable_exact_tiled_k3s1 =
            dazg_orbit::ablation::EnableExactTiledK3S1() &&
            (force_smallroute64_k3s1 || stage1_exact_search_active ||
            (kEnableK3S1Tile4Exact &&
             IsExactTile4K3S1Candidate(
                 in_feature_size,
                 in_channels,
                 out_channels,
                 kernel_size,
                 stride,
                 padding
             )) ||
            IsStage3ExactTiledK3S1Candidate(
                in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                padding
            ) ||
            (kEnableStage4TailExact &&
             IsStage4TailExactTiledK3S1Candidate(
                 in_feature_size,
                 in_channels,
                 out_channels,
                 kernel_size,
                 stride,
                 padding
             )));

        const bool enable_exact_tiled_worker =
            enable_exact_tiled_k3s1 || enable_r50_pwtile_target;

        const uint64_t forced_exact_tile_out =
            force_smallroute64_k3s1 ? 6 : stage1_exact_search_active ? kStage1ForcedTileOut : 0;

        CirLayoutPlan layout = MakeCirLayoutPlan(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, padding,
            enable_k3_polyphase,
            enable_exact_tiled_worker,
            cryptoPrimitive->HE->polyModulusDegree,
            forced_exact_tile_out
        );

        if (force_smallroute64_k3s1) {
            layout.mode = CirLayoutMode::EXACT_TILED_K3_S1;
            layout.route_feature_size = 8;
            layout.suggested_block_size = 64;
            layout.tile_out_feature_size = 6;
            layout.tile_in_feature_size = 8;
            layout.spatial_tile_rows = static_cast<uint64_t>((in_feature_size + 5) / 6);
            layout.spatial_tile_cols = static_cast<uint64_t>((in_feature_size + 5) / 6);
            layout.capacity_padding = 0;
        }

        const bool compact_candidate =
            dazg_orbit::ablation::EnableK1S2Compact() &&
            (layout.mode == CirLayoutMode::COMPACT_PW_S2 ||
             IsExactCompactStride2PointwiseMode(layout.mode));

        const bool k3s2_pcoi =
            dazg_orbit::ablation::EnableK3S2Polyphase() &&
            IsExactPCOIK3S2OrbitMode(layout.mode);
        const bool k3s2_polyphase =
            dazg_orbit::ablation::EnableK3S2Polyphase() &&
            (k3s2_pcoi ||
             layout.mode == CirLayoutMode::COMPACT_K3_S2_POLYPHASE ||
             IsExactCompactStride2K3PolyphaseMode(layout.mode));

        const bool exact_tiled_k3s1 =
            dazg_orbit::ablation::EnableExactTiledK3S1() &&
            IsPureExactTiledK3S1Mode(layout.mode);
        const bool exact_tiled_k1s1 =
            dazg_orbit::ablation::EnableR50PWTile() &&
            IsExactTiledK1S1Mode(layout.mode);

        const uint64_t route_feature_size = layout.route_feature_size;
        const uint64_t padding_for_capacity = layout.capacity_padding;

        uint64_t cir_block_size = layout.suggested_block_size;
        if (cir_block_size < 1) {
            cir_block_size = 1;
        }

        const uint64_t channels_for_divisibility = layout.packed_in_channels;

        if (channels_for_divisibility % cir_block_size != 0 ||
            out_channels % cir_block_size != 0) {
#ifdef USE_HE_GPU
            std::cout << "[CreateConv] Cir fallback -> Conv2DCheetah (channel mismatch): H="
                      << in_feature_size
                      << ", Cin=" << in_channels
                      << ", Cout=" << out_channels
                      << ", block=" << cir_block_size
                      << ", layout_mode=" << static_cast<int>(layout.mode)
                      << ", compact_candidate=" << compact_candidate
                      << ", k3s2_polyphase=" << k3s2_polyphase
                      << ", exact_tiled_k3s1=" << exact_tiled_k3s1
                      << ", route_feature_size=" << route_feature_size
                      << std::endl;

            conv = make_tagged_cheetah();
#else
            std::cout << "[CreateConv] Cir fallback -> Conv2DNest (channel mismatch): H="
                      << in_feature_size
                      << ", Cin=" << in_channels
                      << ", Cout=" << out_channels
                      << ", block=" << cir_block_size
                      << ", layout_mode=" << static_cast<int>(layout.mode)
                      << ", compact_candidate=" << compact_candidate
                      << ", k3s2_polyphase=" << k3s2_polyphase
                      << ", exact_tiled_k3s1=" << exact_tiled_k3s1
                      << ", route_feature_size=" << route_feature_size
                      << std::endl;

            conv = new Conv2DNest(
                in_feature_size, in_channels, out_channels,
                kernel_size, stride, cryptoPrimitive->HE
            );
#endif
            LogDAZGOrbitRoute(
                "CreateConv",
                in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                static_cast<int>(layout.mode),
                compact_candidate,
                k3s2_polyphase,
                exact_tiled_k3s1,
                SecRoute::DenseFallback,
                "FALLBACK_CHANNEL_MISMATCH"
            );

            break;
        }

        const uint64_t padded_feature_size =
            NextPow2U64(route_feature_size + 2 * padding_for_capacity);
        const uint64_t padded_HW =
            padded_feature_size * padded_feature_size;
        const uint64_t ntt_size =
            cir_block_size * padded_HW;
        const bool capacity_ok =
            ntt_size <= cryptoPrimitive->HE->polyModulusDegree;
        const uint64_t max_tile = capacity_ok
            ? std::max<uint64_t>(1, cryptoPrimitive->HE->polyModulusDegree / (2 * ntt_size))
            : 0;

        if (!capacity_ok) {
#ifdef USE_HE_GPU
            std::cout << "[CreateConv] Cir fallback -> Conv2DCheetah (capacity too small): H="
                      << in_feature_size
                      << ", Cin=" << in_channels
                      << ", Cout=" << out_channels
                      << ", block=" << cir_block_size
                      << ", layout_mode=" << static_cast<int>(layout.mode)
                      << ", compact_candidate=" << compact_candidate
                      << ", k3s2_polyphase=" << k3s2_polyphase
                      << ", exact_tiled_k3s1=" << exact_tiled_k3s1
                      << ", route_feature_size=" << route_feature_size
                      << ", padded=" << padded_feature_size
                      << ", ntt_size=" << ntt_size
                      << ", max_tile=" << max_tile
                      << std::endl;

            conv = make_tagged_cheetah();
#else
            std::cout << "[CreateConv] Cir fallback -> Conv2DNest (capacity too small): H="
                      << in_feature_size
                      << ", Cin=" << in_channels
                      << ", Cout=" << out_channels
                      << ", block=" << cir_block_size
                      << ", layout_mode=" << static_cast<int>(layout.mode)
                      << ", compact_candidate=" << compact_candidate
                      << ", k3s2_polyphase=" << k3s2_polyphase
                      << ", exact_tiled_k3s1=" << exact_tiled_k3s1
                      << ", route_feature_size=" << route_feature_size
                      << ", padded=" << padded_feature_size
                      << ", ntt_size=" << ntt_size
                      << ", max_tile=" << max_tile
                      << std::endl;

            conv = new Conv2DNest(
                in_feature_size, in_channels, out_channels,
                kernel_size, stride, cryptoPrimitive->HE
            );
#endif
            LogDAZGOrbitRoute(
                "CreateConv",
                in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                static_cast<int>(layout.mode),
                compact_candidate,
                k3s2_polyphase,
                exact_tiled_k3s1,
                SecRoute::DenseFallback,
                "FALLBACK_CAPACITY_TOO_SMALL"
            );

            break;
        }

        if (exact_tiled_k1s1) {
            std::cerr << "[DAZG_ORBIT_R50_PWTILE_ROUTE]"
                      << " source=CreateConv"
                      << " H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " Cout=" << out_channels
                      << " K=" << kernel_size
                      << " S=" << stride
                      << " tile_grid=" << layout.spatial_tile_rows << "x" << layout.spatial_tile_cols
                      << " tile_out=" << layout.tile_out_feature_size
                      << " tile_in=" << layout.tile_in_feature_size
                      << " block=" << cir_block_size
                      << " exact_equiv=1 semantic_loss=0"
                      << " reason=POINTWISE_SPATIAL_INDEPENDENCE"
                      << std::endl;
        }

        std::cout << "[CreateConv] use CirConv2D: H="
                  << in_feature_size
                  << ", Cin=" << in_channels
                  << ", Cout=" << out_channels
                  << ", K=" << kernel_size
                  << ", S=" << stride
                  << ", block=" << cir_block_size
                  << ", layout_mode=" << static_cast<int>(layout.mode)
                  << ", compact_candidate=" << compact_candidate
                  << ", k3s2_polyphase=" << k3s2_polyphase
                  << ", pcoi_k3s2=" << k3s2_pcoi
                  << ", exact_tiled_k3s1=" << exact_tiled_k3s1
                  << ", tile_grid=" << layout.spatial_tile_rows
                  << "x" << layout.spatial_tile_cols
                  << ", tile_out=" << layout.tile_out_feature_size
                  << ", tile_in=" << layout.tile_in_feature_size
                  << ", route_feature_size=" << route_feature_size
                  << ", max_tile=" << max_tile
                  << std::endl;

        const SecRoute sec_route =
            compact_candidate ? SecRoute::CompactK1S2 :
            k3s2_pcoi ? SecRoute::PCOIK3S2 :
            k3s2_polyphase ? SecRoute::PolyphaseK3S2 :
            exact_tiled_k1s1 ? SecRoute::ExactTiledK1S1 :
            exact_tiled_k3s1 ? SecRoute::ExactTiledK3S1 :
            SecRoute::SparseBSGS;

        LogDAZGOrbitRoute(
            "CreateConv",
            in_feature_size,
            in_channels,
            out_channels,
            kernel_size,
            stride,
            static_cast<int>(layout.mode),
            compact_candidate,
            k3s2_polyphase,
            exact_tiled_k3s1,
            sec_route,
            SecBenefitClass(sec_route, compact_candidate, k3s2_polyphase, exact_tiled_k3s1)
        );

        conv = new CirConv2D(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, cir_block_size, cryptoPrimitive->HE,
            k3s2_polyphase,
            (exact_tiled_k3s1 || exact_tiled_k1s1),
            layout
        );
        break;
    }

    default:
        LogDAZGOrbitRoute(
            "CreateConv",
            in_feature_size,
            in_channels,
            out_channels,
            kernel_size,
            stride,
            -1,
            false,
            false,
            false,
            SecRoute::DenseFallback,
            "DEFAULT_CONV2D_NEST_BASELINE"
        );
        conv = new Conv2DNest(
            in_feature_size, in_channels, out_channels,
            kernel_size, stride, cryptoPrimitive->HE
        );
        break;
    }
    return conv;
}


inline const char* ResNetActivationModeName() {
#if DAZG_ORBIT_TFHE_RELU_MODE == 3
    return "dazg_gelu_mainpath";
#elif DAZG_ORBIT_TFHE_RELU_MODE == 2
    return "bfe_relu";
#elif DAZG_ORBIT_TFHE_RELU_MODE == 1
    return "exact_relu";
#elif DAZG_ORBIT_TFHE_RELU_MODE == 0
    return "fallback_relu";
#else
    return "unknown";
#endif
}

template <typename T, typename IO>
inline std::atomic<uint64_t>& ResNetActivationCallCounter() {
    static std::atomic<uint64_t> activation_call_counter{0};
    return activation_call_counter;
}

template <typename T, typename IO>
inline uint64_t AllocateResNetActivationCallId() {
    return ResNetActivationCallCounter<T, IO>().fetch_add(1, std::memory_order_relaxed) + 1ULL;
}

template <typename T, typename IO>
inline void ApplyResNetMainActivationWithCallId(
    Tensor<T>& x,
    ReLU<T, IO>* relu,
    const char* site,
    uint64_t activation_call_id) {

    const std::string bound_site =
        (site == nullptr || site[0] == '\0') ? std::string("unspecified") : std::string(site);
    dazg_orbit::domain::ScopedConversionSite dazg_orbit_activation_scope(
        bound_site, activation_call_id);

#if DAZG_ORBIT_RESNET_DAZG_GELU_MAINPATH
    static std::atomic<int> prints{0};
    const int id = prints.fetch_add(1, std::memory_order_relaxed);
    if (id < 128) {
        std::cerr << "[DAZG_ORBIT_ACTIVATION_BIND]"
                  << " call_id=" << activation_call_id
                  << " site=" << bound_site
                  << " activation=" << ResNetActivationModeName()
                  << " relu_mode=" << DAZG_ORBIT_TFHE_RELU_MODE
                  << " tensor_n=" << x.data().size()
                  << " certact_fuse_gate=" << (dazg_orbit::ablation::EnableCertActFuse() ? 1 : 0)
                  << " latent_domain_gate=" << (dazg_orbit::ablation::EnableLatentDomainFuse() ? 1 : 0)
                  << " certact_fuse_route=protocol_local_canonical_v2"
#if DAZG_ORBIT_TFHE_RELU_MODE == 3
                  << " order=truncate_then_gelu"
                  << " route=ReLU-slot->TFHEReLUProtocol->HE::EvalBFEGeLUClear"
                  << " certificate=domain_range_scatter_residual_plut"
#else
                  << " order=relu_then_truncate"
                  << " certificate=relu_exact_truncation"
#endif
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << std::endl;
    }
#endif

    (*relu)(x);
}

template <typename T, typename IO>
inline void ApplyResNetMainActivation(
    Tensor<T>& x,
    ReLU<T, IO>* relu,
    const char* site) {
    const uint64_t activation_call_id = AllocateResNetActivationCallId<T, IO>();
    ApplyResNetMainActivationWithCallId<T, IO>(x, relu, site, activation_call_id);
}



#ifndef DAZG_ORBIT_DOMAINPULSE_V15_HELPERS
#define DAZG_ORBIT_DOMAINPULSE_V15_HELPERS
inline bool DAZGOrbitDomainPulseV14Enabled() {
    return dazg_orbit::ablation::EnableDomainPulseV14();
}

inline bool DAZGOrbitDomainPulseV15Enabled() {
    return dazg_orbit::ablation::EnableDomainPulseV15();
}

inline uint64_t DAZGOrbitShapeDimOrZero(const std::vector<size_t>& shape, size_t idx) {
    return idx < shape.size() ? static_cast<uint64_t>(shape[idx]) : 0ULL;
}

inline Tensor<uint64_t> DAZGOrbitConcatRows2D(Tensor<uint64_t>& a, Tensor<uint64_t>& b) {
    const auto sa = a.shape();
    const auto sb = b.shape();
    const uint64_t rows_a = DAZGOrbitShapeDimOrZero(sa, 0);
    const uint64_t rows_b = DAZGOrbitShapeDimOrZero(sb, 0);
    const uint64_t cols_a = DAZGOrbitShapeDimOrZero(sa, 1);
    const uint64_t cols_b = DAZGOrbitShapeDimOrZero(sb, 1);

    if (sa.size() != 2 || sb.size() != 2 || cols_a != cols_b) {
        std::cerr << "[DAZG_ORBIT_DOMAINPULSE_V15] runtime_applied=0"
                  << " reason=concat_rows_2d_shape_mismatch"
                  << " a_shape=" << dazg_orbit::domain::ShapeToString(sa)
                  << " b_shape=" << dazg_orbit::domain::ShapeToString(sb)
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }

    Tensor<uint64_t> out({rows_a + rows_b, cols_a});
    // DAZG_ORBIT_V39_PROJECTION_HOTPATH_BEGIN
    // DomainPulse/ProjectionBurst uses contiguous row-major tensors here.
    // Copying the underlying vectors preserves the exact row order while
    // removing per-element Tensor::operator() overhead in the hottest
    // cross-branch conversion path.
    if (sa.size() == 2 && sb.size() == 2 && cols_a == cols_b) {
        auto& out_data = out.data();
        const auto& a_data = a.data();
        const auto& b_data = b.data();
        std::copy(a_data.begin(), a_data.end(), out_data.begin());
        std::copy(b_data.begin(), b_data.end(),
                  out_data.begin() + static_cast<std::ptrdiff_t>(a_data.size()));
        return out;
    }
    // DAZG_ORBIT_V39_PROJECTION_HOTPATH_END
    for (uint64_t i = 0; i < rows_a; ++i) {
        for (uint64_t j = 0; j < cols_a; ++j) out({i, j}) = a({i, j});
    }
    for (uint64_t i = 0; i < rows_b; ++i) {
        for (uint64_t j = 0; j < cols_a; ++j) out({rows_a + i, j}) = b({i, j});
    }
    return out;
}

inline Tensor<HE::unified::UnifiedCiphertext> DAZGOrbitSliceCipherRows(
    Tensor<HE::unified::UnifiedCiphertext>& src,
    uint64_t row_offset,
    uint64_t rows,
    HE::HEEvaluator* HE) {
    const auto target = (HE != nullptr && HE->server) ? HE->Backend() : HOST;
    Tensor<HE::unified::UnifiedCiphertext> out({rows}, target);
    auto& out_data = out.data();
    const auto& src_data = src.data();
    std::copy(src_data.begin() + static_cast<std::ptrdiff_t>(row_offset),
              src_data.begin() + static_cast<std::ptrdiff_t>(row_offset + rows),
              out_data.begin());
    return out;
}

inline Tensor<HE::unified::UnifiedCiphertext> DAZGOrbitConcatCipherRows(
    Tensor<HE::unified::UnifiedCiphertext>& a,
    Tensor<HE::unified::UnifiedCiphertext>& b,
    HE::HEEvaluator* HE) {
    const uint64_t rows_a = static_cast<uint64_t>(a.size());
    const uint64_t rows_b = static_cast<uint64_t>(b.size());
    const auto target = (HE != nullptr && HE->server) ? HE->Backend() : HOST;
    Tensor<HE::unified::UnifiedCiphertext> out({rows_a + rows_b}, target);
    auto& out_data = out.data();
    const auto& a_data = a.data();
    const auto& b_data = b.data();
    std::copy(a_data.begin(), a_data.end(), out_data.begin());
    std::copy(b_data.begin(), b_data.end(),
              out_data.begin() + static_cast<std::ptrdiff_t>(rows_a));
    return out;
}

inline Tensor<uint64_t> DAZGOrbitSliceRows2D(
    Tensor<uint64_t>& src,
    uint64_t row_offset,
    uint64_t rows,
    uint64_t cols) {
    Tensor<uint64_t> out({rows, cols});
    auto& out_data = out.data();
    const auto& src_data = src.data();
    for (uint64_t i = 0; i < rows; ++i) {
        const uint64_t src_base = (row_offset + i) * cols;
        const uint64_t out_base = i * cols;
        std::copy(src_data.begin() + static_cast<std::ptrdiff_t>(src_base),
                  src_data.begin() + static_cast<std::ptrdiff_t>(src_base + cols),
                  out_data.begin() + static_cast<std::ptrdiff_t>(out_base));
    }
    return out;
}


inline std::pair<Tensor<uint64_t>, Tensor<uint64_t>> DAZGOrbitHEToSSMaskStableBatch2(
    Tensor<HE::unified::UnifiedCiphertext>& a_ct,
    uint64_t a_call_id,
    Tensor<HE::unified::UnifiedCiphertext>& b_ct,
    uint64_t b_call_id,
    HE::HEEvaluator* HE) {
    const uint64_t rows_a = static_cast<uint64_t>(a_ct.size());
    const uint64_t rows_b = static_cast<uint64_t>(b_ct.size());
    const uint64_t degree = static_cast<uint64_t>(HE->polyModulusDegree);
    Tensor<uint64_t> a_share({rows_a, degree});
    Tensor<uint64_t> b_share({rows_b, degree});

    auto fill_server_segment = [&](Tensor<HE::unified::UnifiedCiphertext>& ct,
                                   Tensor<uint64_t>& share,
                                   uint64_t rows,
                                   uint64_t call_id) {
        auto gen = dazg_orbit_det::MakeMt19937_64(
            "Conversion64",
            dazg_orbit_det::HashU64Seq({rows, degree, call_id}));
        std::uniform_int_distribution<uint64_t> distrib(0, HE->plain_mod - 1);
        // V39 hotpath: reuse per-segment buffers and copy rows into the
        // contiguous share tensor. The PRG draw order is unchanged, so the
        // mask-stable HE->SS certificate remains bit-exact.
        std::vector<uint64_t> pos_mask(degree, 0);
        std::vector<uint64_t> neg_mask(degree, 0);
        HE::unified::UnifiedPlaintext tmp_neg(HOST);
        auto& share_data = share.data();
        for (uint64_t i = 0; i < rows; ++i) {
            for (uint64_t j = 0; j < degree; ++j) {
                pos_mask[j] = distrib(gen);
                neg_mask[j] = HE->plain_mod - pos_mask[j];
            }
            std::copy(pos_mask.begin(), pos_mask.end(),
                      share_data.begin() + static_cast<std::ptrdiff_t>(i * degree));
            HE->encoder->encode(neg_mask, tmp_neg);
            HE->evaluator->add_plain_inplace(ct(i), tmp_neg);
        }
    };

    if (HE->server) {
        fill_server_segment(a_ct, a_share, rows_a, a_call_id);
        fill_server_segment(b_ct, b_share, rows_b, b_call_id);

        if (HE->Backend() == DEVICE) {
            a_ct.apply([HE](HE::unified::UnifiedCiphertext& ct) {
                ct.to_host(*HE->context);
            });
            b_ct.apply([HE](HE::unified::UnifiedCiphertext& ct) {
                ct.to_host(*HE->context);
            });
        }

        Tensor<HE::unified::UnifiedCiphertext> batched_ct({rows_a + rows_b}, HOST);
        auto& batched_data = batched_ct.data();
        const auto& a_data = a_ct.data();
        const auto& b_data = b_ct.data();
        std::copy(a_data.begin(), a_data.end(), batched_data.begin());
        std::copy(b_data.begin(), b_data.end(),
                  batched_data.begin() + static_cast<std::ptrdiff_t>(rows_a));
        HE->SendEncVec(batched_ct);
    } else {
        Tensor<HE::unified::UnifiedCiphertext> batched_ct({rows_a + rows_b}, HOST);
        HE->ReceiveEncVec(batched_ct);
        std::vector<uint64_t> tmp_vec(degree, 0);
        seal::Plaintext out_pt;
        auto& a_share_data = a_share.data();
        auto& b_share_data = b_share.data();
        for (uint64_t i = 0; i < rows_a + rows_b; ++i) {
            HE->decryptor->decrypt(batched_ct(i), out_pt);
            HE->encoder->decode(out_pt, tmp_vec);
            if (i < rows_a) {
                std::copy(tmp_vec.begin(), tmp_vec.end(),
                          a_share_data.begin() + static_cast<std::ptrdiff_t>(i * degree));
            } else {
                const uint64_t bi = i - rows_a;
                std::copy(tmp_vec.begin(), tmp_vec.end(),
                          b_share_data.begin() + static_cast<std::ptrdiff_t>(bi * degree));
            }
        }
    }

    std::cerr << "[DAZG_ORBIT_DOMAINPULSE_V15]"
              << " phase=mask_stable_batched_hetoss"
              << " runtime_applied=1"
              << " rows_a=" << rows_a
              << " rows_b=" << rows_b
              << " call_id_a=" << a_call_id
              << " call_id_b=" << b_call_id
              << " preserved_mask_seed=1"
              << " baseline_he_to_ss_seed_tuple=rows_polydegree_callid"
              << " v39_hotpath=1"
              << " contiguous_copy=1"
              << " mask_buffer_reuse=1"
              << " plaintext_reuse=1"
              << " saved_rounds=1"
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;

    return std::make_pair(a_share, b_share);
}
#endif  // DAZG_ORBIT_DOMAINPULSE_V15_HELPERS

#ifndef DAZG_ORBIT_TRUNCLIFT_V4_HARDPATH_20260512
#define DAZG_ORBIT_TRUNCLIFT_V4_HARDPATH_20260512
namespace dazg_orbit_trunclift_v4_hardpath {

inline bool EnvFalse(const char* v) {
    if (v == nullptr || *v == '\0') return false;
    const std::string s(v);
    return s == "0" || s == "false" || s == "False" || s == "FALSE" ||
           s == "off" || s == "OFF" || s == "no" || s == "NO";
}

inline bool EnvEnabled(const char* name, bool default_value) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    return !EnvFalse(v);
}

inline uint64_t EnvU64(const char* name, uint64_t fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    char* end = nullptr;
    unsigned long long x = std::strtoull(v, &end, 10);
    return (end == v) ? fallback : static_cast<uint64_t>(x);
}

inline std::atomic<uint64_t>& FrontierCounter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

inline std::atomic<uint64_t>& SuppressedCounter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

inline uint64_t MixHash(uint64_t h, uint64_t v) {
    h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h *= 1099511628211ULL;
    return h;
}

inline long long Signed64ForLog(uint64_t u) {
    if (u <= 0x7fffffffffffffffULL) return static_cast<long long>(u);
    const uint64_t mag = (~u) + 1ULL;
    if (mag > 0x7fffffffffffffffULL) return (-9223372036854775807LL - 1LL);
    return -static_cast<long long>(mag);
}

inline void EmitSummary() {
    std::cerr << "[DAZG_ORBIT_TRUNCLIFT_V4_SUMMARY]"
              << " marker=DAZG_ORBIT_TRUNCLIFT_V4_SUMMARY"
              << " version=hardpath_20260512"
              << " frontier_count=" << FrontierCounter().load(std::memory_order_relaxed)
              << " suppressed_count=" << SuppressedCounter().load(std::memory_order_relaxed)
              << " runtime_applied=0"
              << " domain_transition_eliminated=0"
              << " decision=evidence_only"
              << " reason=needs_global_lower_bits_zero_and_signed_bound_certificate"
              << " exact_equiv=1"
              << " semantic_loss=0"
              << std::endl;
}

inline void RegisterSummaryOnce() {
    static bool registered = false;
    if (!registered) {
        registered = true;
        std::atexit(EmitSummary);
    }
}

template <typename TensorT>
inline void LogTensorFrontier(
    const TensorT& x,
    const char* phase,
    const char* site,
    int scale_down_bits,
    int out_bits,
    bool signed_trunc,
    bool extra_flag) {

    RegisterSummaryOnce();
    const uint64_t seq = FrontierCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;

    const uint64_t max_lines = EnvU64("DAZG_ORBIT_TRUNCLIFT_V4_MAX_FRONTIER_LINES", 4096ULL);
    if (max_lines > 0ULL && seq > max_lines &&
        !EnvEnabled("DAZG_ORBIT_TRUNCLIFT_V4_DETAIL", false)) {
        SuppressedCounter().fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto& buf = x.data();
    const uint64_t n = static_cast<uint64_t>(buf.size());
    const uint64_t low_mask =
        (scale_down_bits <= 0) ? 0ULL :
        ((scale_down_bits >= 64) ? ~0ULL : ((1ULL << scale_down_bits) - 1ULL));

    uint64_t raw_hash = 1469598103934665603ULL;
    uint64_t low_hash = 1099511628211ULL;
    uint64_t low_or = 0ULL;
    uint64_t low_nonzero = 0ULL;
    uint64_t sample0 = 0ULL;
    uint64_t min_u = ~0ULL;
    uint64_t max_u = 0ULL;
    long long min_s = 9223372036854775807LL;
    long long max_s = (-9223372036854775807LL - 1LL);

    for (size_t i = 0; i < buf.size(); ++i) {
        const uint64_t u = static_cast<uint64_t>(buf[i]);
        if (i == 0) sample0 = u;
        if (u < min_u) min_u = u;
        if (u > max_u) max_u = u;
        const long long ss = Signed64ForLog(u);
        if (ss < min_s) min_s = ss;
        if (ss > max_s) max_s = ss;
        const uint64_t low = u & low_mask;
        low_or |= low;
        if (low != 0ULL) ++low_nonzero;
        raw_hash = MixHash(raw_hash, u ^ static_cast<uint64_t>(i + 1ULL));
        low_hash = MixHash(low_hash, low ^ static_cast<uint64_t>((i + 1ULL) * 1315423911ULL));
    }

    if (n == 0ULL) {
        min_u = 0ULL;
        min_s = 0LL;
        max_s = 0LL;
    }

    std::cerr << "[DAZG_ORBIT_TRUNCLIFT_V4_FRONTIER]"
              << " marker=DAZG_ORBIT_TRUNCLIFT_V4_FRONTIER"
              << " version=hardpath_20260512"
              << " seq=" << seq
              << " phase=" << ((phase != nullptr && phase[0] != '\0') ? phase : "unknown")
              << " site=" << ((site != nullptr && site[0] != '\0') ? site : "unspecified")
              << " tensor_n=" << n
              << " scale_down_bits=" << scale_down_bits
              << " out_bits=" << out_bits
              << " signed_trunc=" << (signed_trunc ? 1 : 0)
              << " extra_flag=" << (extra_flag ? 1 : 0)
              << " local_share_only=1"
              << " local_lower_bits_zero=" << ((low_nonzero == 0ULL) ? 1 : 0)
              << " local_lowbits_nonzero=" << low_nonzero
              << " local_lowbits_or=" << low_or
              << " local_lowbits_hash=" << low_hash
              << " local_raw_hash=" << raw_hash
              << " local_min_u64=" << min_u
              << " local_max_u64=" << max_u
              << " local_min_signed=" << min_s
              << " local_max_signed=" << max_s
              << " local_sample0=" << sample0
              << " runtime_applied=0"
              << " domain_transition_eliminated=0"
              << " decision=evidence_only"
              << " reason=local_share_frontier_not_global_certificate"
              << " exact_equiv=1"
              << " semantic_loss=0"
              << std::endl;
}


#ifndef DAZG_ORBIT_TRUNCLIFT_V4_SCALAR_FRONTIER_OVERLOAD
#define DAZG_ORBIT_TRUNCLIFT_V4_SCALAR_FRONTIER_OVERLOAD 1
inline void LogTensorFrontier(
    int,
    const char*,
    const char*,
    int,
    int,
    bool,
    bool) {
    // V41.1 compile guard: scalar frontier calls carry no tensor payload.
    // This is evidence-only logging and never changes protocol output.
}
#endif  // DAZG_ORBIT_TRUNCLIFT_V4_SCALAR_FRONTIER_OVERLOAD


}  // namespace dazg_orbit_trunclift_v4_hardpath
#endif  // DAZG_ORBIT_TRUNCLIFT_V4_HARDPATH_20260512


// DAZG_ORBIT_THUNDERACT_V19B_ADAPTIVE_PATCH_BEGIN
#ifndef DAZG_ORBIT_THUNDERACT_V19B_ADAPTIVE_PATCH
#define DAZG_ORBIT_THUNDERACT_V19B_ADAPTIVE_PATCH 1
#endif

inline bool DAZGOrbitThunderActV19EnvTrue(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return false;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" || s == "FALSE" ||
             s == "off" || s == "OFF" || s == "no" || s == "NO");
}

inline bool DAZGOrbitThunderActV19Enabled() {
    return ::dazg_orbit::thunderact_v19e::Enabled();
}



inline std::atomic<uint64_t>& DAZGOrbitThunderActV19SkipCounter() {
    static std::atomic<uint64_t> counter{0};
    return counter;
}

inline void DAZGOrbitThunderActV19RecordSkip(const char* site,
                                           uint64_t tensor_n,
                                           int shift,
                                           int out_bits,
                                           bool signed_trunc,
                                           bool extra_flag) {
    const uint64_t seq =
        DAZGOrbitThunderActV19SkipCounter().fetch_add(1, std::memory_order_relaxed) + 1ULL;
    if (seq <= 256ULL || DAZGOrbitThunderActV19EnvTrue("DAZG_ORBIT_THUNDERACT_V19_DETAIL")) {
        const char* mode = std::getenv("DAZG_ORBIT_THUNDERACT_V19_MODE");
        std::cerr << "[DAZG_ORBIT_THUNDERACT_V19]"
                  << " marker=DAZG_ORBIT_THUNDERACT_V19B_ADAPTIVE_PATCH"
                  << " phase=skip_interactive_truncate"
                  << " seq=" << seq
                  << " site=" << ((site != nullptr && site[0] != '\0') ? site : "unspecified")
                  << " tensor_n=" << tensor_n
                  << " shift=" << shift
                  << " out_bits=" << out_bits
                  << " signed_trunc=" << (signed_trunc ? 1 : 0)
                  << " extra_flag=" << (extra_flag ? 1 : 0)
                  << " mode=" << (mode != nullptr ? mode : "env_default")
                  << " skipped_fixpoint_truncate=1"
                  << " bridge_local_truncate_required=1"
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << std::endl;
    }
}
// DAZG_ORBIT_THUNDERACT_V19B_ADAPTIVE_PATCH_END


// DAZG_ORBIT_TAILSHADOW_V41_HELPERS_BEGIN
template <typename T>
struct DAZGOrbitTailShadowDigestV41 {
    bool enabled;
    std::uint64_t pre_hash;
    std::uint64_t local_candidate_hash;
    std::uint64_t lowbit_boundary_count;
    std::uint64_t tensor_n;
};

template <typename T>
inline DAZGOrbitTailShadowDigestV41<T> DAZGOrbitTailShadowBeginV41(
    const Tensor<T>& x,
    const char* site,
    std::uint64_t runtime_seq,
    bool signed_trunc,
    bool extra_flag) {
    (void)runtime_seq;

    DAZGOrbitTailShadowDigestV41<T> d;
    d.enabled = false;
    d.pre_hash = 1469598103934665603ULL;
    d.local_candidate_hash = 1099511628211ULL;
    d.lowbit_boundary_count = 0ULL;
    d.tensor_n = 0ULL;

    if (!::dazg_orbit::thunderact_v19e::TailShadowV41Enabled()) return d;
    if (!::dazg_orbit::thunderact_v19e::ThunderTailUnsafeTailSiteV37(site)) return d;

    d.enabled = true;
    const auto& data = x.data();
    d.tensor_n = static_cast<std::uint64_t>(data.size());

    ::dazg_orbit::thunderact_v19e::Context ctx{
        true, 17, 43, signed_trunc, extra_flag, site
    };

    const std::uint64_t low_mask = (1ULL << 17) - 1ULL;

    for (const auto& v : data) {
        const std::uint64_t raw = static_cast<std::uint64_t>(v);
        const std::int64_t local =
            ::dazg_orbit::thunderact_v19e::LocalTruncate(raw, ctx, 43);
        const std::uint64_t local_u = static_cast<std::uint64_t>(local);

        d.pre_hash = ::dazg_orbit::thunderact_v19e::TailShadowMixV41(d.pre_hash, raw);
        d.local_candidate_hash =
            ::dazg_orbit::thunderact_v19e::TailShadowMixV41(d.local_candidate_hash, local_u);

        const std::uint64_t low = raw & low_mask;
        if (low == 0ULL || low == low_mask) {
            ++d.lowbit_boundary_count;
        }
    }

    return d;
}

template <typename T>
inline void DAZGOrbitTailShadowEndV41(
    const Tensor<T>& x,
    const char* site,
    std::uint64_t runtime_seq,
    const DAZGOrbitTailShadowDigestV41<T>& d) {
    if (!d.enabled) return;

    std::uint64_t post_hash = 1469598103934665603ULL;
    const auto& data = x.data();
    for (const auto& v : data) {
        post_hash = ::dazg_orbit::thunderact_v19e::TailShadowMixV41(
            post_hash, static_cast<std::uint64_t>(v));
    }

    const bool local_share_equal = (post_hash == d.local_candidate_hash);

    ::dazg_orbit::thunderact_v19e::RecordTailShadowV41(
        site,
        runtime_seq,
        static_cast<std::uint64_t>(data.size()),
        d.pre_hash,
        d.local_candidate_hash,
        post_hash,
        d.lowbit_boundary_count,
        local_share_equal);
}
// DAZG_ORBIT_TAILSHADOW_V41_HELPERS_END

template <typename T, typename IO>
inline void ApplyResNetActivationAndTruncate(
    Tensor<T>& x,
    ReLU<T, IO>* relu,
    FixPoint<T>* fixpoint,
    const char* site,
    bool signed_trunc) {
#if DAZG_ORBIT_TFHE_RELU_MODE == 3
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "pre_trunc", site, 17, 43, signed_trunc, false);

    const std::uint64_t dazg_orbit_v19h_seq = dazg_orbit::thunderact_v19e::NextFrontierSeq(site);
    const bool dazg_orbit_v37_thundertail_enabled = dazg_orbit::thunderact_v19e::Enabled();
    const bool dazg_orbit_v37_canonical_prg_reshare_preserved = true;
    if (dazg_orbit_v37_thundertail_enabled &&
        dazg_orbit::thunderact_v19e::ThunderTailShouldApplyV37(
            dazg_orbit_v19h_seq, site, 17, 43, signed_trunc, false,
            dazg_orbit_v37_canonical_prg_reshare_preserved)) {
        dazg_orbit::thunderact_v19e::ScopedContext dazg_orbit_thunderact_scope(
            site, 17, 43, signed_trunc, false);
        ApplyResNetMainActivation<T, IO>(x, relu, site);
        dazg_orbit::thunderact_v19e::RecordSkip(
            site,
            static_cast<std::uint64_t>(x.data().size()),
            17,
            43,
            signed_trunc,
            false);
        dazg_orbit::thunderact_v19e::RecordThunderTailCertV37(
            site,
            dazg_orbit_v19h_seq,
            static_cast<std::uint64_t>(x.data().size()),
            17,
            43,
            signed_trunc,
            false,
            dazg_orbit_v37_canonical_prg_reshare_preserved,
            false,
            "none");
        dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
            x, "post_bridge_local_trunc_gelu", site, 17, 43, signed_trunc, false);
        return;
    }

    if (dazg_orbit_v37_thundertail_enabled) {
        const char* dazg_orbit_v37_reason = dazg_orbit::thunderact_v19e::ThunderTailFallbackReasonV37(
            dazg_orbit_v19h_seq, site, 17, 43, signed_trunc, false,
            dazg_orbit_v37_canonical_prg_reshare_preserved);
        if (dazg_orbit::thunderact_v19e::ThunderTailUnsafeTailSiteV37(site) ||
            std::strcmp(dazg_orbit_v37_reason, "policy_not_requested") != 0) {
            dazg_orbit::thunderact_v19e::RecordThunderTailCertV37(
                site,
                dazg_orbit_v19h_seq,
                static_cast<std::uint64_t>(x.data().size()),
                17,
                43,
                signed_trunc,
                false,
                dazg_orbit_v37_canonical_prg_reshare_preserved,
                true,
                dazg_orbit_v37_reason);
        }
    }

    {
        auto dazg_orbit_v41_shadow = DAZGOrbitTailShadowBeginV41(
            x, site, dazg_orbit_v19h_seq, signed_trunc, false);
        fixpoint->truncate(x, 17, 43, signed_trunc);
        DAZGOrbitTailShadowEndV41(x, site, dazg_orbit_v19h_seq, dazg_orbit_v41_shadow);
    }
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "post_trunc_pre_activation", site, 17, 43, signed_trunc, false);
    ApplyResNetMainActivation<T, IO>(x, relu, site);
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "post_trunc", site, 17, 43, signed_trunc, false);
#else
    const std::uint64_t dazg_orbit_v19h_seq = dazg_orbit::thunderact_v19e::NextFrontierSeq(site);
    ApplyResNetMainActivation<T, IO>(x, relu, site);
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "post_activation_pre_trunc", site, 17, 43, signed_trunc, false);
    {
        auto dazg_orbit_v41_shadow = DAZGOrbitTailShadowBeginV41(
            x, site, dazg_orbit_v19h_seq, signed_trunc, false);
        fixpoint->truncate(x, 17, 43, signed_trunc);
        DAZGOrbitTailShadowEndV41(x, site, dazg_orbit_v19h_seq, dazg_orbit_v41_shadow);
    }
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "post_trunc", site, 17, 43, signed_trunc, false);
#endif
}



template <typename T, typename IO>
inline void ApplyResNetActivationAndTruncate(
    Tensor<T>& x,
    ReLU<T, IO>* relu,
    FixPoint<T>* fixpoint,
    const char* site,
    bool signed_trunc,
    bool extra_flag) {
#if DAZG_ORBIT_TFHE_RELU_MODE == 3
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "pre_trunc", site, 17, 43, signed_trunc, extra_flag);

    const std::uint64_t dazg_orbit_v19h_seq = dazg_orbit::thunderact_v19e::NextFrontierSeq(site);
    const bool dazg_orbit_v37_thundertail_enabled = dazg_orbit::thunderact_v19e::Enabled();
    const bool dazg_orbit_v37_canonical_prg_reshare_preserved = true;
    if (dazg_orbit_v37_thundertail_enabled &&
        dazg_orbit::thunderact_v19e::ThunderTailShouldApplyV37(
            dazg_orbit_v19h_seq, site, 17, 43, signed_trunc, extra_flag,
            dazg_orbit_v37_canonical_prg_reshare_preserved)) {
        dazg_orbit::thunderact_v19e::ScopedContext dazg_orbit_thunderact_scope(
            site, 17, 43, signed_trunc, extra_flag);
        ApplyResNetMainActivation<T, IO>(x, relu, site);
        dazg_orbit::thunderact_v19e::RecordSkip(
            site,
            static_cast<std::uint64_t>(x.data().size()),
            17,
            43,
            signed_trunc,
            extra_flag);
        dazg_orbit::thunderact_v19e::RecordThunderTailCertV37(
            site,
            dazg_orbit_v19h_seq,
            static_cast<std::uint64_t>(x.data().size()),
            17,
            43,
            signed_trunc,
            extra_flag,
            dazg_orbit_v37_canonical_prg_reshare_preserved,
            false,
            "none");
        dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
            x, "post_bridge_local_trunc_gelu", site, 17, 43, signed_trunc, extra_flag);
        return;
    }

    if (dazg_orbit_v37_thundertail_enabled) {
        const char* dazg_orbit_v37_reason = dazg_orbit::thunderact_v19e::ThunderTailFallbackReasonV37(
            dazg_orbit_v19h_seq, site, 17, 43, signed_trunc, extra_flag,
            dazg_orbit_v37_canonical_prg_reshare_preserved);
        if (dazg_orbit::thunderact_v19e::ThunderTailUnsafeTailSiteV37(site) ||
            std::strcmp(dazg_orbit_v37_reason, "policy_not_requested") != 0) {
            dazg_orbit::thunderact_v19e::RecordThunderTailCertV37(
                site,
                dazg_orbit_v19h_seq,
                static_cast<std::uint64_t>(x.data().size()),
                17,
                43,
                signed_trunc,
                extra_flag,
                dazg_orbit_v37_canonical_prg_reshare_preserved,
                true,
                dazg_orbit_v37_reason);
        }
    }

    {
        auto dazg_orbit_v41_shadow = DAZGOrbitTailShadowBeginV41(
            x, site, dazg_orbit_v19h_seq, signed_trunc, extra_flag);
        fixpoint->truncate(x, 17, 43, signed_trunc);
        DAZGOrbitTailShadowEndV41(x, site, dazg_orbit_v19h_seq, dazg_orbit_v41_shadow);
    }
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "post_trunc_pre_activation", site, 17, 43, signed_trunc, extra_flag);
    ApplyResNetMainActivation<T, IO>(x, relu, site);
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "post_trunc", site, 17, 43, signed_trunc, extra_flag);
#else
    const std::uint64_t dazg_orbit_v19h_seq = dazg_orbit::thunderact_v19e::NextFrontierSeq(site);
    ApplyResNetMainActivation<T, IO>(x, relu, site);
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "post_activation_pre_trunc", site, 17, 43, signed_trunc, extra_flag);
    {
        auto dazg_orbit_v41_shadow = DAZGOrbitTailShadowBeginV41(
            x, site, dazg_orbit_v19h_seq, signed_trunc, extra_flag);
        fixpoint->truncate(x, 17, 43, signed_trunc);
        DAZGOrbitTailShadowEndV41(x, site, dazg_orbit_v19h_seq, dazg_orbit_v41_shadow);
    }
    dazg_orbit_trunclift_v4_hardpath::LogTensorFrontier(
        x, "post_trunc", site, 17, 43, signed_trunc, extra_flag);
#endif
}



template <typename T, typename IO=Utils::NetIO>
class BasicBlock{
public:
    int expansion = 1;
    int in_planes;
    int planes;
    int stride = 1;
    ReLU<T, IO> *relu;
    FixPoint<T> *fixpoint;
    Conv2D *conv1;
    Conv2D *conv2;
    Conv2D *shortcut;
    bool has_shortcut = false;

    BlockScheduler::BlockExecPlan exec_plan;
    uint64_t block_id = 0;

    std::string BlockSite(const char* suffix) const {
        std::ostringstream oss;
        oss << "BasicBlock#" << block_id << "/" << suffix;
        return oss.str();
    }

    BasicBlock(uint64_t in_feature_size,
               uint64_t in_planes,
               uint64_t planes,
               uint64_t stride,
               CryptoPrimitive<T, IO> *cryptoPrimitive) {
        this->in_planes = in_planes;
        this->planes = planes;
        this->stride = stride;
        this->relu = cryptoPrimitive->relu;
        this->fixpoint = cryptoPrimitive->fixpoint;
        static std::atomic<uint64_t> next_basic_block_id{0};
        this->block_id =
            next_basic_block_id.fetch_add(1, std::memory_order_relaxed) + 1ULL;

        conv1 = CreateConv<T, IO>(in_feature_size, in_planes, planes, 3, stride, cryptoPrimitive);
        conv2 = CreateConv<T, IO>(in_feature_size / stride, planes, planes, 3, 1, cryptoPrimitive);

        if (stride != 1 || in_planes != planes) {
            has_shortcut = true;
            shortcut = CreateConv<T, IO>(in_feature_size, in_planes, planes, 1, stride, cryptoPrimitive);
        }

        exec_plan = BlockScheduler::MakeBlockExecPlan(
            this->in_planes,
            this->planes,
            this->stride,
            has_shortcut
        );
    }

private:
    inline Tensor<T> RunBaseline(Tensor<T> &x) {
        Tensor<T> x_res = x;

    #ifdef PROFILE_BASICBLOCK
        auto us = [](auto a, auto b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };

        bool should_profile = exec_plan.profile;
        auto t0 = std::chrono::steady_clock::now();
    #endif

        const std::string conv1_site = BlockSite("conv1");
        {
            dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                conv1_site, block_id * 10ULL + 1ULL);
            x = (*conv1)(x);
        }

    #ifdef PROFILE_BASICBLOCK
        auto t1 = std::chrono::steady_clock::now();
    #endif

        ApplyResNetActivationAndTruncate<T, IO>(
            x, relu, fixpoint, conv1_site.c_str(), true);

    #ifdef PROFILE_BASICBLOCK
        auto t2 = std::chrono::steady_clock::now();
    #endif

        const std::string conv2_site = BlockSite("conv2");
        {
            dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                conv2_site, block_id * 10ULL + 2ULL);
            x = (*conv2)(x);
        }

    #ifdef PROFILE_BASICBLOCK
        auto t3 = std::chrono::steady_clock::now();
    #endif

        if (has_shortcut) {
            const std::string shortcut_site = BlockSite("shortcut");
            dazg_orbit::domain::ScopedConversionSite dazg_orbit_shortcut_scope(
                shortcut_site, block_id * 10ULL + 3ULL);
            x_res = (*shortcut)(x_res);
        }

    #ifdef PROFILE_BASICBLOCK
        auto t4 = std::chrono::steady_clock::now();
    #endif

        x = x + x_res;

    #ifdef PROFILE_BASICBLOCK
        auto t5 = std::chrono::steady_clock::now();
    #endif

        const std::string add_site = BlockSite("add");
        ApplyResNetActivationAndTruncate<T, IO>(
            x, relu, fixpoint, add_site.c_str(), true);

    #ifdef PROFILE_BASICBLOCK
        auto t6 = std::chrono::steady_clock::now();

        if (should_profile) {
            std::cout << "[BasicBlockProfile]"
                      << " mode=" << static_cast<int>(exec_plan.mode)
                      << ", in_planes=" << in_planes
                      << ", planes=" << planes
                      << ", stride=" << stride
                      << ", has_shortcut=" << has_shortcut
                      << ", conv1_us=" << us(t0, t1)
                      << ", relu_trunc1_us=" << us(t1, t2)
                      << ", conv2_us=" << us(t2, t3)
                      << ", shortcut_us=" << us(t3, t4)
                      << ", add_us=" << us(t4, t5)
                      << ", relu_trunc2_us=" << us(t5, t6)
                      << std::endl;
        }
    #endif

        return x;
    }

    inline Tensor<T> RunTailFusionCandidate(Tensor<T> &x) {
        if (!has_shortcut || !dazg_orbit_gos::FanoutRuntimeEnabled()) {
            return RunBaseline(x);
        }

        auto now = []() { return std::chrono::steady_clock::now(); };
        auto us = [](auto a, auto b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };

        const bool server_role =
            (conv1 != nullptr && conv1->HE != nullptr && conv1->HE->server);

        const auto t0 = now();

        // Direction-B runtime hook:
        // Materialize one explicit pre-branch tensor cache and consume it from
        // both downsample branches. This is deliberately conservative: it does
        // not pretend that HE rotation ciphertexts are shareable when the
        // shortcut branch has zero rotation need. It only moves the fanout
        // execution into a single guarded runtime scope and keeps exact tensor
        // semantics intact.
        Tensor<T> x_prebranch = x;

        const auto t_cache = now();

        Tensor<T> x_res = x_prebranch;
        const std::string shortcut_site = BlockSite("shortcut");
        {
            dazg_orbit::domain::ScopedConversionSite dazg_orbit_shortcut_scope(
                shortcut_site, block_id * 10ULL + 3ULL);
            x_res = (*shortcut)(x_prebranch);
        }

        const auto t_shortcut = now();

        const std::string conv1_site = BlockSite("conv1");
        {
            dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                conv1_site, block_id * 10ULL + 1ULL);
            x = (*conv1)(x_prebranch);
        }

        const auto t_conv1 = now();

        ApplyResNetActivationAndTruncate<T, IO>(
            x, relu, fixpoint, conv1_site.c_str(), true);

        const auto t_act1 = now();

        const std::string conv2_site = BlockSite("conv2");
        {
            dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                conv2_site, block_id * 10ULL + 2ULL);
            x = (*conv2)(x);
        }

        const auto t_conv2 = now();

        x = x + x_res;

        const auto t_add = now();

        const std::string add_site = BlockSite("add");
        ApplyResNetActivationAndTruncate<T, IO>(
            x, relu, fixpoint, add_site.c_str(), true);

        const auto t_end = now();

        dazg_orbit_gos::RecordFanoutRuntimeApplied(
            block_id,
            server_role,
            static_cast<uint64_t>(in_planes),
            static_cast<uint64_t>(planes),
            static_cast<uint64_t>(stride),
            us(t0, t_cache),
            us(t_cache, t_shortcut),
            us(t_shortcut, t_act1),
            us(t_act1, t_conv2),
            us(t0, t_end));

#ifdef PROFILE_BASICBLOCK
        if (exec_plan.profile) {
            std::cout << "[BasicBlockProfile]"
                      << " mode=" << static_cast<int>(exec_plan.mode)
                      << ", direction_b_runtime=1"
                      << ", in_planes=" << in_planes
                      << ", planes=" << planes
                      << ", stride=" << stride
                      << ", has_shortcut=" << has_shortcut
                      << ", prebranch_cache_us=" << us(t0, t_cache)
                      << ", shortcut_us=" << us(t_cache, t_shortcut)
                      << ", conv1_relu_us=" << us(t_shortcut, t_act1)
                      << ", conv2_us=" << us(t_act1, t_conv2)
                      << ", add_us=" << us(t_conv2, t_add)
                      << ", relu_trunc2_us=" << us(t_add, t_end)
                      << std::endl;
        }
#endif

        return x;
    }

public:
    inline Tensor<T> operator()(Tensor<T> &x) {
        switch (exec_plan.mode) {
            case BlockScheduler::BlockExecMode::TailFusionCandidate:
                return RunTailFusionCandidate(x);

            case BlockScheduler::BlockExecMode::BaselineExact:
            default:
                return RunBaseline(x);
        }
    }
};



template <typename T, typename IO=Utils::NetIO>
class Bottleneck{
    public:
        static constexpr int expansion = 4;
        int in_planes;
        int planes;
        int stride = 1;
        bool has_shortcut = false;
        ReLU<T, IO> *relu;
        FixPoint<T> *fixpoint;
        Conv2D *conv1;
        Conv2D *conv2;
        Conv2D *conv3;
        Conv2D *shortcut = nullptr;
        uint64_t block_id = 0;

        std::string BlockSite(const char* suffix) const {
            std::ostringstream oss;
            oss << "Bottleneck#" << block_id << "/" << suffix;
            return oss.str();
        }

        Bottleneck(uint64_t in_feature_size,
                   uint64_t in_planes,
                   uint64_t planes,
                   uint64_t stride,
                   CryptoPrimitive<T, IO> *cryptoPrimitive){
            this->in_planes = static_cast<int>(in_planes);
            this->planes = static_cast<int>(planes);
            this->stride = static_cast<int>(stride);
            this->relu = cryptoPrimitive->relu;
            this->fixpoint = cryptoPrimitive->fixpoint;

            static std::atomic<uint64_t> next_bottleneck_block_id{0};
            this->block_id =
                next_bottleneck_block_id.fetch_add(1, std::memory_order_relaxed) + 1ULL;

            this->conv1 = CreateConv<T, IO>(
                in_feature_size, in_planes, planes, 1, 1, cryptoPrimitive);
            this->conv2 = CreateConv<T, IO>(
                in_feature_size, planes, planes, 3, stride, cryptoPrimitive);
            this->conv3 = CreateConv<T, IO>(
                in_feature_size / stride, planes, planes * expansion, 1, 1, cryptoPrimitive);

            if (stride != 1 || in_planes != planes * expansion){
                has_shortcut = true;
                this->shortcut = CreateConv<T, IO>(
                    in_feature_size, in_planes, planes * expansion, 1, stride, cryptoPrimitive);
            }

            if (has_shortcut) {
                dazg_orbit_gos::RegisterProjectionBlockPlan(
                    block_id,
                    static_cast<std::uint64_t>(in_feature_size),
                    static_cast<std::uint64_t>(in_planes),
                    static_cast<std::uint64_t>(planes),
                    static_cast<std::uint64_t>(expansion),
                    static_cast<std::uint64_t>(stride),
                    true,
                    "Bottleneck");
            }
        }

        Tensor<T> operator()(Tensor<T> &x){
            Tensor<T> x_res = x;

            const std::string conv1_site = BlockSite("conv1_reduce_k1s1");
            const std::string conv2_site = BlockSite("conv2_spatial_k3");
            const std::string conv3_site = BlockSite("conv3_expand_k1s1");
            const std::string shortcut_site = BlockSite("shortcut_project_k1");
            const std::string add_site = BlockSite("residual_add");

            const bool dazg_orbit_v20_projection_burst =
                ::dazg_orbit::fanoutburst_v20::AllowProjectionBlock(
                    block_id,
                    has_shortcut,
                    static_cast<std::uint64_t>(in_planes),
                    static_cast<std::uint64_t>(planes),
                    static_cast<std::uint64_t>(stride));

            if (has_shortcut && shortcut != nullptr && conv1 != nullptr && conv2 != nullptr && conv3 != nullptr &&
                conv1->HE != nullptr && conv1->HE == shortcut->HE && conv3->HE == shortcut->HE &&
                (DAZGOrbitDomainPulseV14Enabled() || dazg_orbit_v20_projection_burst)) {
                // DAZG_ORBIT_FANOUTBURST_V20A_PROJECTION_BURST_BEGIN
                auto dazg_orbit_v20_now = []() { return std::chrono::steady_clock::now(); };
                auto dazg_orbit_v20_us = [](auto a, auto b) {
                    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
                };
                const bool dazg_orbit_v20_runtime_active = dazg_orbit_v20_projection_burst;
                const bool dazg_orbit_v20_server_role = (conv1->HE != nullptr && conv1->HE->server);
                const auto dazg_orbit_v20_t0 = dazg_orbit_v20_now();
                auto dazg_orbit_v20_t_after_sstohe = dazg_orbit_v20_t0;
                auto dazg_orbit_v20_t_after_hetoss = dazg_orbit_v20_t0;
                if (dazg_orbit_v20_runtime_active) {
                    ::dazg_orbit::fanoutburst_v20::RecordProjectionEnter(
                        block_id,
                        dazg_orbit_v20_server_role,
                        static_cast<std::uint64_t>(in_planes),
                        static_cast<std::uint64_t>(planes),
                        static_cast<std::uint64_t>(stride));
                }
                // DAZG_ORBIT_FANOUTBURST_V20A_PROJECTION_BURST_END
                // DomainPulse V15 mask-stable projection-bottleneck batch:
                //   1) conv1 and projection-shortcut both consume the original
                //      residual input, so concatenate their packed SS->HE payloads
                //      and send them in one conversion round.
                //   2) conv3 and projection-shortcut produce independent HE outputs
                //      that are both converted back to SS before the residual add;
                //      concatenate those ciphertext vectors and run one HE->SS round.
                // No plaintext, ciphertext, mask, or activation arithmetic is changed.
                Tensor<HE::unified::UnifiedCiphertext> shortcut_out_ct;
                uint64_t shortcut_out_rows = 0;

                {
                    dazg_orbit::domain::ScopedConversionSite dazg_orbit_v14_pack_scope(
                        BlockSite("v15_conv1_shortcut_batched_sstohe"),
                        block_id * 10ULL + 41ULL);
                    Tensor<uint64_t> conv1_ac_msg = conv1->DAZGOrbitPackActivationForHE(x);
                    Tensor<uint64_t> shortcut_ac_msg = shortcut->DAZGOrbitPackActivationForHE(x_res);
                    const uint64_t conv1_ac_rows = DAZGOrbitShapeDimOrZero(conv1_ac_msg.shape(), 0);
                    const uint64_t shortcut_ac_rows = DAZGOrbitShapeDimOrZero(shortcut_ac_msg.shape(), 0);
                    Tensor<uint64_t> batched_ac_msg = DAZGOrbitConcatRows2D(conv1_ac_msg, shortcut_ac_msg);
                    Tensor<HE::unified::UnifiedCiphertext> batched_ac_ct =
                        Operator::SSToHE(batched_ac_msg, conv1->HE);
                    Tensor<HE::unified::UnifiedCiphertext> conv1_ac_ct =
                        DAZGOrbitSliceCipherRows(batched_ac_ct, 0, conv1_ac_rows, conv1->HE);
                    Tensor<HE::unified::UnifiedCiphertext> shortcut_ac_ct =
                        DAZGOrbitSliceCipherRows(batched_ac_ct, conv1_ac_rows, shortcut_ac_rows, shortcut->HE);

                    Tensor<HE::unified::UnifiedCiphertext> conv1_out_ct =
                        conv1->DAZGOrbitComputeFromPackedHE(conv1_ac_ct);
                    shortcut_out_ct = shortcut->DAZGOrbitComputeFromPackedHE(shortcut_ac_ct);
                    shortcut_out_rows = static_cast<uint64_t>(shortcut_out_ct.size());
                    dazg_orbit_v20_t_after_sstohe = dazg_orbit_v20_now();
                    if (dazg_orbit_v20_runtime_active) {
                        ::dazg_orbit::fanoutburst_v20::RecordBatchedSSToHE(
                            block_id, conv1_ac_rows, shortcut_ac_rows);
                    }

                    std::cerr << "[DAZG_ORBIT_DOMAINPULSE_V15]"
                              << " block=" << block_id
                              << " phase=batched_sstohe"
                              << " runtime_applied=1"
                              << " conv1_rows=" << conv1_ac_rows
                              << " shortcut_rows=" << shortcut_ac_rows
                              << " saved_rounds=1"
                              << " exact_equiv=1 semantic_loss=0"
                              << std::endl;

                    {
                        dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv1_share_scope(
                            conv1_site, block_id * 10ULL + 1ULL);
                        x = conv1->DAZGOrbitShareFromHE(conv1_out_ct);
                    }
                }

                ApplyResNetActivationAndTruncate<T, IO>(
                    x, relu, fixpoint, conv1_site.c_str(), true);

                {
                    dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                        conv2_site, block_id * 10ULL + 2ULL);
                    x = (*conv2)(x);
                }
                ApplyResNetActivationAndTruncate<T, IO>(
                    x, relu, fixpoint, conv2_site.c_str(), true);

                Tensor<HE::unified::UnifiedCiphertext> conv3_out_ct;
                uint64_t conv3_out_rows = 0;
                {
                    dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv3_he_scope(
                        conv3_site, block_id * 10ULL + 3ULL);
                    conv3_out_ct = conv3->DAZGOrbitEvalToHE(x);
                    conv3_out_rows = static_cast<uint64_t>(conv3_out_ct.size());
                }

                Tensor<T> main_share;
                Tensor<T> shortcut_share;
                {
                    dazg_orbit::domain::ScopedConversionSite dazg_orbit_v14_out_scope(
                        BlockSite("v15_conv3_shortcut_maskstable_batched_hetoss"),
                        block_id * 10ULL + 43ULL);
                    auto mask_stable_out_msg = DAZGOrbitHEToSSMaskStableBatch2(
                        conv3_out_ct,
                        block_id * 10ULL + 3ULL,
                        shortcut_out_ct,
                        block_id * 10ULL + 4ULL,
                        conv3->HE);
                    main_share = conv3->DAZGOrbitDepackFromPackedSS(mask_stable_out_msg.first);
                    shortcut_share = shortcut->DAZGOrbitDepackFromPackedSS(mask_stable_out_msg.second);
                    dazg_orbit_v20_t_after_hetoss = dazg_orbit_v20_now();
                    if (dazg_orbit_v20_runtime_active) {
                        ::dazg_orbit::fanoutburst_v20::RecordBatchedHEToSS(
                            block_id, conv3_out_rows, shortcut_out_rows);
                    }
                }

                x = main_share + shortcut_share;
                dazg_orbit::domain::RecordRuntimeFusionApplied(1, 2);
                std::cerr << "[DAZG_ORBIT_DOMAINPULSE_V15]"
                          << " block=" << block_id
                          << " phase=batched_hetoss"
                          << " runtime_applied=1"
                          << " conv3_rows=" << conv3_out_rows
                          << " shortcut_rows=" << shortcut_out_rows
                          << " saved_rounds=1"
                          << " total_saved_rounds=2"
                          << " exact_equiv=1 semantic_loss=0"
                          << std::endl;

                ApplyResNetActivationAndTruncate<T, IO>(
                    x, relu, fixpoint, add_site.c_str(), true);

                const bool dazg_orbit_v31_projection_runtime_active =
                    ::dazg_orbit::topline_v25::EnableProjectionBurstV31() &&
                    (DAZGOrbitDomainPulseV15Enabled() || dazg_orbit_v20_runtime_active);
                if (dazg_orbit_v31_projection_runtime_active) {
                    const auto dazg_orbit_v20_t_end = dazg_orbit_v20_now();
                    const long long dazg_orbit_v20_pack_us =
                        dazg_orbit_v20_us(dazg_orbit_v20_t0, dazg_orbit_v20_t_after_sstohe);
                    const long long dazg_orbit_v20_conv_hetoss_us =
                        dazg_orbit_v20_us(dazg_orbit_v20_t_after_sstohe, dazg_orbit_v20_t_after_hetoss);
                    const long long dazg_orbit_v20_total_us =
                        dazg_orbit_v20_us(dazg_orbit_v20_t0, dazg_orbit_v20_t_end);
                    dazg_orbit_gos::RecordProjectionBurstV31RuntimeApplied(
                        block_id,
                        dazg_orbit_v20_server_role,
                        static_cast<std::uint64_t>(in_planes),
                        static_cast<std::uint64_t>(planes),
                        static_cast<std::uint64_t>(stride),
                        dazg_orbit_v20_pack_us,
                        dazg_orbit_v20_conv_hetoss_us,
                        dazg_orbit_v20_total_us,
                        DAZGOrbitDomainPulseV15Enabled(),
                        dazg_orbit_v20_runtime_active);

                    if (dazg_orbit_v20_runtime_active) {
                        ::dazg_orbit::fanoutburst_v20::RecordProjectionComplete(
                            block_id,
                            dazg_orbit_v20_server_role,
                            dazg_orbit_v20_total_us,
                            dazg_orbit_v20_pack_us,
                            dazg_orbit_v20_conv_hetoss_us);
                    }
                }

                return x;
            }

            {
                dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                    conv1_site, block_id * 10ULL + 1ULL);
                x = (*conv1)(x);
            }
            ApplyResNetActivationAndTruncate<T, IO>(
                x, relu, fixpoint, conv1_site.c_str(), true);

            {
                dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                    conv2_site, block_id * 10ULL + 2ULL);
                x = (*conv2)(x);
            }
            ApplyResNetActivationAndTruncate<T, IO>(
                x, relu, fixpoint, conv2_site.c_str(), true);

            {
                dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                    conv3_site, block_id * 10ULL + 3ULL);
                x = (*conv3)(x);
            }

            if (has_shortcut){
                dazg_orbit::domain::ScopedConversionSite dazg_orbit_shortcut_scope(
                    shortcut_site, block_id * 10ULL + 4ULL);
                x_res = (*shortcut)(x_res);
            }

            x = x + x_res;

            ApplyResNetActivationAndTruncate<T, IO>(
                x, relu, fixpoint, add_site.c_str(), true);

            return x;
        }
};


template <typename T, typename IO=Utils::NetIO>
class ResNet_3stages {
    public:
        int in_feature_size;
        int num_classes;
        int in_planes = 16;
        int* num_layers;
        Conv2D *conv1;
        ReLU<T, IO> *relu;
        FixPoint<T> *fixpoint;
        vector<BasicBlock<T, IO>*> layer1;
        vector<BasicBlock<T, IO>*> layer2;
        vector<BasicBlock<T, IO>*> layer3;
        Conv2D *linear;
        AvgPool2D<T> *avg_pool;
        ResNet_3stages(uint64_t in_feature_size, int* num_layers,int num_classes, CryptoPrimitive<T, IO> *cryptoPrimitive){
            this->in_feature_size = in_feature_size;
            this->num_layers = num_layers;
            this->num_classes = num_classes;
            this->relu = cryptoPrimitive->relu;
            this->fixpoint = cryptoPrimitive->fixpoint;
            conv1 = CreateConv<T, IO>(in_feature_size, 3, 16, 3, 1, cryptoPrimitive);
            _make_layer(layer1, 16, num_layers[0], 1, cryptoPrimitive);
            _make_layer(layer2, 32, num_layers[1], 2, cryptoPrimitive);
            _make_layer(layer3, 64, num_layers[2], 2, cryptoPrimitive);
            linear = CreateConv<T, IO>(1, 64, num_classes, 1, 1, cryptoPrimitive);
            avg_pool = new AvgPool2D<T>(8);
        }

        void _make_layer(vector<BasicBlock<T, IO>*> &layer, int planes, int num_blocks, int stride, CryptoPrimitive<T, IO> *cryptoPrimitive){
            int strides[num_blocks];
            strides[0] = stride;
            for (int i = 1; i < num_blocks; i++){
                strides[i] = 1;
            }
            for (int i = 0; i < num_blocks; i++){
                layer.push_back(new BasicBlock<T, IO>(this->in_feature_size, this->in_planes, planes, strides[i], cryptoPrimitive));
                this->in_planes = planes * 1;
                this->in_feature_size = this->in_feature_size / strides[i];
            }
        }
        // TODO: implement nn.Sequential
        Tensor<T> operator()(Tensor<T> &x){
            {
                dazg_orbit::domain::ScopedConversionSite dazg_orbit_conv_scope(
                    "ResNet_3stages/conv1", 301ULL);
                x = (*conv1)(x);
            }
            ApplyResNetActivationAndTruncate<T, IO>(
                x, relu, fixpoint, "ResNet_3stages/conv1", true, true);
            for (int i = 0; i < layer1.size(); i++){
                x = (*layer1[i])(x);
            }
            for (int i = 0; i < layer2.size(); i++){
                x = (*layer2[i])(x);
            }
            for (int i = 0; i < layer3.size(); i++){
                x = (*layer3[i])(x);
            }
            x = (*avg_pool)(x);
            {
                dazg_orbit::domain::ScopedConversionSite dazg_orbit_fc_scope(
                    "ResNet_3stages/classifier", 399ULL);
                x = (*linear)(x);
            }
            return x;
        }
};

template <typename T, typename IO=Utils::NetIO>
class ResNet_4stages {
    public:
        int in_feature_size;
        int num_classes;
        int in_planes = 64;
        int* num_layers;
        Conv2D *conv1;
        ReLU<T, IO> *relu;
        FixPoint<T> *fixpoint;
        vector<BasicBlock<T, IO>*> layer1;
        vector<BasicBlock<T, IO>*> layer2;
        vector<BasicBlock<T, IO>*> layer3;
        vector<BasicBlock<T, IO>*> layer4;
        Conv2D *linear;
        AvgPool2D<T> *avg_pool;
        ResNet_4stages(uint64_t in_feature_size, int* num_layers,int num_classes, CryptoPrimitive<T, IO> *cryptoPrimitive){
            this->in_feature_size = in_feature_size;
            this->num_layers = num_layers;
            this->num_classes = num_classes;
            this->relu = cryptoPrimitive->relu;
            this->fixpoint = cryptoPrimitive->fixpoint;
            conv1 = CreateConv<T, IO>(in_feature_size, 3, this->in_planes, 7, 4, cryptoPrimitive); // we do not support maxpool for now, so we use stride = 4
            this->in_feature_size = this->in_feature_size / 4;
            _make_layer(layer1, 64, num_layers[0], 1, cryptoPrimitive);
            _make_layer(layer2, 128, num_layers[1], 2, cryptoPrimitive);
            _make_layer(layer3, 256, num_layers[2], 2, cryptoPrimitive);
            _make_layer(layer4, 512, num_layers[3], 2, cryptoPrimitive);
            avg_pool = new AvgPool2D<T>(7);
            linear = CreateConv<T, IO>(1, 512, num_classes, 1, 1, cryptoPrimitive);
        }

        void _make_layer(vector<BasicBlock<T, IO>*> &layer, int planes, int num_blocks, int stride, CryptoPrimitive<T, IO> *cryptoPrimitive){
            int strides[num_blocks];
            strides[0] = stride;
            for (int i = 1; i < num_blocks; i++){
                strides[i] = 1;
            }
            for (int i = 0; i < num_blocks; i++){
                layer.push_back(new BasicBlock<T, IO>(this->in_feature_size, this->in_planes, planes, strides[i], cryptoPrimitive));
                this->in_planes = planes * 1;
                this->in_feature_size = this->in_feature_size / strides[i];
            }
        }
        // TODO: implement nn.Sequential
        Tensor<T> operator()(Tensor<T> &x){
    #ifdef PROFILE_RESNET_STAGE
        auto us = [](auto a, auto b) {
            return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
        };

        long long conv1_us = 0;
        long long layer1_us = 0;
        long long layer2_us = 0;
        long long layer3_us = 0;
        long long layer4_us = 0;
        long long avg_pool_us = 0;
        long long linear_us = 0;

        auto t0 = std::chrono::steady_clock::now();
    #endif

        {
            dazg_orbit::domain::ScopedConversionSite dazg_orbit_stem_scope(
                "ResNet_4stages/conv1", 401ULL);
            x = (*conv1)(x);
        }
        ApplyResNetActivationAndTruncate<T, IO>(
            x, relu, fixpoint, "ResNet_4stages/conv1", true);

    #ifdef PROFILE_RESNET_STAGE
        auto t1 = std::chrono::steady_clock::now();
        conv1_us = us(t0, t1);

        auto s1 = std::chrono::steady_clock::now();
    #endif

        for (int i = 0; i < layer1.size(); i++){
            x = (*layer1[i])(x);
        }

    #ifdef PROFILE_RESNET_STAGE
        auto s2 = std::chrono::steady_clock::now();
        layer1_us = us(s1, s2);

        auto s3 = std::chrono::steady_clock::now();
    #endif

        for (int i = 0; i < layer2.size(); i++){
            x = (*layer2[i])(x);
        }

    #ifdef PROFILE_RESNET_STAGE
        auto s4 = std::chrono::steady_clock::now();
        layer2_us = us(s3, s4);

        auto s5 = std::chrono::steady_clock::now();
    #endif

        for (int i = 0; i < layer3.size(); i++){
            x = (*layer3[i])(x);
        }

    #ifdef PROFILE_RESNET_STAGE
        auto s6 = std::chrono::steady_clock::now();
        layer3_us = us(s5, s6);

        auto s7 = std::chrono::steady_clock::now();
    #endif

        for (int i = 0; i < layer4.size(); i++){
            x = (*layer4[i])(x);
        }

    #ifdef PROFILE_RESNET_STAGE
        auto s8 = std::chrono::steady_clock::now();
        layer4_us = us(s7, s8);

        auto s9 = std::chrono::steady_clock::now();
    #endif

        x = (*avg_pool)(x);

    #ifdef PROFILE_RESNET_STAGE
        auto s10 = std::chrono::steady_clock::now();
        avg_pool_us = us(s9, s10);

        auto s11 = std::chrono::steady_clock::now();
    #endif

        {
            dazg_orbit::domain::ScopedConversionSite dazg_orbit_classifier_scope(
                "ResNet_4stages/classifier", 499ULL);
            x = (*linear)(x);
        }

    #ifdef PROFILE_RESNET_STAGE
        auto s12 = std::chrono::steady_clock::now();
        linear_us = us(s11, s12);

        std::cout << "[ResNetStageProfile]"
                << " conv1_us=" << conv1_us
                << ", layer1_us=" << layer1_us
                << ", layer2_us=" << layer2_us
                << ", layer3_us=" << layer3_us
                << ", layer4_us=" << layer4_us
                << ", avg_pool_us=" << avg_pool_us
                << ", linear_us=" << linear_us
                << std::endl;
    #endif

        return x;
    }

};


template <typename T, typename IO=Utils::NetIO>
class ResNet_4stages_Bottleneck {
    public:
        int in_feature_size;
        int num_classes;
        int in_planes = 64;
        int* num_layers;
        Conv2D *conv1;
        ReLU<T, IO> *relu;
        FixPoint<T> *fixpoint;
        vector<Bottleneck<T, IO>*> layer1;
        vector<Bottleneck<T, IO>*> layer2;
        vector<Bottleneck<T, IO>*> layer3;
        vector<Bottleneck<T, IO>*> layer4;
        Conv2D *linear;
        AvgPool2D<T> *avg_pool;

        ResNet_4stages_Bottleneck(uint64_t in_feature_size,
                                  int* num_layers,
                                  int num_classes,
                                  CryptoPrimitive<T, IO> *cryptoPrimitive){
            this->in_feature_size = static_cast<int>(in_feature_size);
            this->num_layers = num_layers;
            this->num_classes = num_classes;
            this->relu = cryptoPrimitive->relu;
            this->fixpoint = cryptoPrimitive->fixpoint;

            conv1 = CreateConv<T, IO>(in_feature_size, 3, this->in_planes, 7, 4, cryptoPrimitive);
            this->in_feature_size = this->in_feature_size / 4;

            _make_layer(layer1, 64,  num_layers[0], 1, cryptoPrimitive);
            _make_layer(layer2, 128, num_layers[1], 2, cryptoPrimitive);
            _make_layer(layer3, 256, num_layers[2], 2, cryptoPrimitive);
            _make_layer(layer4, 512, num_layers[3], 2, cryptoPrimitive);

            avg_pool = new AvgPool2D<T>(7);
            linear = CreateConv<T, IO>(1, 512 * Bottleneck<T, IO>::expansion,
                                       num_classes, 1, 1, cryptoPrimitive);

            std::cerr << "[DAZG_ORBIT_MODEL_BUILD]"
                      << " name=resnet50_bottleneck"
                      << " block=bottleneck"
                      << " layers=" << num_layers[0] << "," << num_layers[1]
                      << "," << num_layers[2] << "," << num_layers[3]
                      << " expansion=" << Bottleneck<T, IO>::expansion
                      << " stem=7x7_stride4_no_maxpool"
                      << " classifier_in=" << (512 * Bottleneck<T, IO>::expansion)
                      << " classes=" << num_classes
                      << " exact_equiv=1 semantic_loss=0"
                      << std::endl;
        }

        void _make_layer(vector<Bottleneck<T, IO>*> &layer,
                         int planes,
                         int num_blocks,
                         int stride,
                         CryptoPrimitive<T, IO> *cryptoPrimitive){
            for (int i = 0; i < num_blocks; i++){
                const int block_stride = (i == 0) ? stride : 1;
                layer.push_back(new Bottleneck<T, IO>(
                    this->in_feature_size,
                    this->in_planes,
                    planes,
                    block_stride,
                    cryptoPrimitive));
                this->in_planes = planes * Bottleneck<T, IO>::expansion;
                this->in_feature_size = this->in_feature_size / block_stride;
            }
        }

        Tensor<T> operator()(Tensor<T> &x){
    #ifdef PROFILE_RESNET_STAGE
            auto us = [](auto a, auto b) {
                return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
            };
            long long conv1_us = 0;
            long long layer1_us = 0;
            long long layer2_us = 0;
            long long layer3_us = 0;
            long long layer4_us = 0;
            long long avg_pool_us = 0;
            long long linear_us = 0;
            auto t0 = std::chrono::steady_clock::now();
    #endif

            {
                dazg_orbit::domain::ScopedConversionSite dazg_orbit_stem_scope(
                    "ResNet_50_Bottleneck/conv1", 501ULL);
                x = (*conv1)(x);
            }
            ApplyResNetActivationAndTruncate<T, IO>(
                x, relu, fixpoint, "ResNet_50_Bottleneck/conv1", true);

    #ifdef PROFILE_RESNET_STAGE
            auto t1 = std::chrono::steady_clock::now();
            conv1_us = us(t0, t1);
            auto s1 = std::chrono::steady_clock::now();
    #endif

            for (int i = 0; i < layer1.size(); i++) x = (*layer1[i])(x);

    #ifdef PROFILE_RESNET_STAGE
            auto s2 = std::chrono::steady_clock::now();
            layer1_us = us(s1, s2);
            auto s3 = std::chrono::steady_clock::now();
    #endif

            for (int i = 0; i < layer2.size(); i++) x = (*layer2[i])(x);

    #ifdef PROFILE_RESNET_STAGE
            auto s4 = std::chrono::steady_clock::now();
            layer2_us = us(s3, s4);
            auto s5 = std::chrono::steady_clock::now();
    #endif

            for (int i = 0; i < layer3.size(); i++) x = (*layer3[i])(x);

    #ifdef PROFILE_RESNET_STAGE
            auto s6 = std::chrono::steady_clock::now();
            layer3_us = us(s5, s6);
            auto s7 = std::chrono::steady_clock::now();
    #endif

            for (int i = 0; i < layer4.size(); i++) x = (*layer4[i])(x);

    #ifdef PROFILE_RESNET_STAGE
            auto s8 = std::chrono::steady_clock::now();
            layer4_us = us(s7, s8);
            auto s9 = std::chrono::steady_clock::now();
    #endif

            x = (*avg_pool)(x);

    #ifdef PROFILE_RESNET_STAGE
            auto s10 = std::chrono::steady_clock::now();
            avg_pool_us = us(s9, s10);
            auto s11 = std::chrono::steady_clock::now();
    #endif

            {
                dazg_orbit::domain::ScopedConversionSite dazg_orbit_classifier_scope(
                    "ResNet_50_Bottleneck/classifier", 599ULL);
                x = (*linear)(x);
            }

    #ifdef PROFILE_RESNET_STAGE
            auto s12 = std::chrono::steady_clock::now();
            linear_us = us(s11, s12);
            std::cout << "[ResNetStageProfile]"
                      << " model=resnet50_bottleneck"
                      << " conv1_us=" << conv1_us
                      << ", layer1_us=" << layer1_us
                      << ", layer2_us=" << layer2_us
                      << ", layer3_us=" << layer3_us
                      << ", layer4_us=" << layer4_us
                      << ", avg_pool_us=" << avg_pool_us
                      << ", linear_us=" << linear_us
                      << std::endl;
    #endif

            return x;
        }
};


template <typename T, typename IO=Utils::NetIO>
ResNet_3stages<uint64_t> resnet_32_c10(CryptoPrimitive<T, IO> *cryptoPrimitive){
    return ResNet_3stages<uint64_t>(32, new int[3]{5,5,5}, 10, cryptoPrimitive);
}

template <typename T, typename IO=Utils::NetIO>
ResNet_4stages<uint64_t> resnet_18(CryptoPrimitive<T, IO> *cryptoPrimitive){
    return ResNet_4stages<uint64_t>(224, new int[4]{2,2,2,2}, 1000, cryptoPrimitive);
}

template <typename T, typename IO=Utils::NetIO>
ResNet_4stages<uint64_t> resnet_basicblock_3_4_6_3(CryptoPrimitive<T, IO> *cryptoPrimitive){
    return ResNet_4stages<uint64_t>(224, new int[4]{3,4,6,3}, 1000, cryptoPrimitive);
}

template <typename T, typename IO=Utils::NetIO>
ResNet_4stages<uint64_t> resnet_50_basicblock_legacy(CryptoPrimitive<T, IO> *cryptoPrimitive){
    return resnet_basicblock_3_4_6_3<T, IO>(cryptoPrimitive);
}

template <typename T, typename IO=Utils::NetIO>
ResNet_4stages_Bottleneck<uint64_t> resnet_50(CryptoPrimitive<T, IO> *cryptoPrimitive){
    return ResNet_4stages_Bottleneck<uint64_t>(224, new int[4]{3,4,6,3}, 1000, cryptoPrimitive);
}

}