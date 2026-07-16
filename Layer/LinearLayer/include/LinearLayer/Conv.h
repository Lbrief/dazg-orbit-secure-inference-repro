// DAZG-Orbit Project Source File
// Component: Layer/LinearLayer/include/LinearLayer/Conv.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <seal/seal.h>
#include <hexl/hexl.hpp>
#include <Datatype/Tensor.h>
#include <HE/HE.h>
#include <LinearOperator/Conversion.h>
#include "../../../Layer/Module.h"
#include <string>
#include <vector>
#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <iostream>

using namespace seal;
using namespace Datatype;
using namespace HE;
using namespace HE::unified;
using std::vector;

namespace LinearLayer {

// =========================
// CirConv layout plan
// =========================

enum class CirLayoutMode {
    FULL = 0,
    COMPACT_PW_S2 = 1,
    COMPACT_K3_S2_POLYPHASE = 2,
    EXACT_TILED_K3_S1 = 3,
    EXACT_COMPACT_PW_S2 = 4,
    EXACT_COMPACT_K3_S2_POLYPHASE = 5,
    EXACT_TILED_K1_S1 = 6,
    EXACT_PCOI_K3_S2_ORBIT = 7,
    // Q-CHAR + ARHC-L: exact K3/S1 quotient-character tiling.
    // This is deliberately separated from the historical EXACT_TILED_K3_S1
    // so experiments can attribute speed/rotation changes without claiming
    // block-circulant, BSGS, or generic halo tiling as new.
    EXACT_QCHAR_ARHCL_K3_S1 = 8,
    EXACT_TILE4_K3_S1 = EXACT_TILED_K3_S1
};

inline bool IsExactCompactStride2PointwiseMode(CirLayoutMode mode)
{
    return mode == CirLayoutMode::EXACT_COMPACT_PW_S2;
}

inline bool IsExactCompactStride2K3PolyphaseMode(CirLayoutMode mode)
{
    return mode == CirLayoutMode::EXACT_COMPACT_K3_S2_POLYPHASE;
}

inline bool IsExactPCOIK3S2OrbitMode(CirLayoutMode mode)
{
    return mode == CirLayoutMode::EXACT_PCOI_K3_S2_ORBIT;
}

inline bool IsExactTiledK1S1Mode(CirLayoutMode mode)
{
    return mode == CirLayoutMode::EXACT_TILED_K1_S1;
}

inline bool IsExactQCharARHCLK3S1Mode(CirLayoutMode mode)
{
    return mode == CirLayoutMode::EXACT_QCHAR_ARHCL_K3_S1;
}

inline bool IsExactTileK3S1Mode(CirLayoutMode mode)
{
    return mode == CirLayoutMode::EXACT_TILED_K3_S1 ||
           IsExactQCharARHCLK3S1Mode(mode) ||
           IsExactTiledK1S1Mode(mode) ||
           IsExactCompactStride2PointwiseMode(mode) ||
           IsExactCompactStride2K3PolyphaseMode(mode) ||
           IsExactPCOIK3S2OrbitMode(mode);
}

// Strict helper used by DAZGOrbit ablation gating: true only for the
// pure K3/S1 tiled route, not for the compact stride-2 exact routes.
inline bool IsPureExactTiledK3S1Mode(CirLayoutMode mode)
{
    return mode == CirLayoutMode::EXACT_TILED_K3_S1 ||
           IsExactQCharARHCLK3S1Mode(mode);
}

struct CirLayoutPlan {
    CirLayoutMode mode = CirLayoutMode::FULL;

    // 原始卷积输出边长
    uint64_t out_feature_size = 0;

    // 工厂路由/容量估计使用
    uint64_t route_feature_size = 0;
    uint64_t capacity_padding = 0;

    // 真正用于 packed 几何 / HE 参数
    uint64_t effective_feature_size = 0;
    uint64_t packed_in_channels = 0;
    uint64_t packed_kernel_size = 0;
    uint64_t packed_stride = 0;
    uint64_t packed_padding = 0;

    // 兼容旧字段名：现在它表示“是否进入 exact tiled k3s1 mode”
    bool exact_tile4_k3s1 = false;

    // 逻辑输出尺寸：对 exact tiled mode 仍然是原始 out_feature_size
    uint64_t logical_out_feature_size = 0;

    // 内部 packed worker 的输出尺寸：exact tiled mode 下是单 tile 的输出边长
    uint64_t packed_out_feature_size = 0;

    // tile 参数
    uint64_t spatial_tile_rows = 1;
    uint64_t spatial_tile_cols = 1;
    uint64_t tile_out_feature_size = 0;
    uint64_t tile_in_feature_size = 0;
    uint64_t halo_size = 0;

    // Q-CHAR + ARHC-L certificate metadata.
    // qchar_* describes the quotient-character residue lattice used to select
    // exact local HE workers; arhcl_* describes the anchor-residue halo carry
    // needed for K3/S1 recovery at tile boundaries.
    bool qchar_arhcl_enabled = false;
    uint64_t qchar_modulus_rows = 1;
    uint64_t qchar_modulus_cols = 1;
    uint64_t qchar_residue_count = 1;
    uint64_t qchar_anchor_stride = 0;
    uint64_t arhcl_halo_left = 0;
    uint64_t arhcl_halo_right = 0;
    uint64_t arhcl_halo_carry_rows = 0;
    uint64_t arhcl_halo_carry_cols = 0;
    uint64_t qchar_exact_recovery = 0;

    // V598 real-enable evidence: these fields are not new substrates.
    // They certify that Q-CHAR quotient buckets and ARHC-L halo ownership
    // are actually selected by the runtime plan, rather than only existing
    // as dormant bridge metadata.
    uint64_t qchar_phase_bucket_count = 1;
    uint64_t qchar_orbit_rotation_saved_est = 0;
    uint64_t arhcl_terminal_rotation_saved_est = 0;
    uint64_t arhcl_anchor_digest = 0;

    // compact / polyphase 模式元信息
    uint64_t phase_count = 1;

    // DAZG-ORBIT / PCOI-K3S2: spatial phase orbit stays external to the
    // learned channel-circulant orbit.  phase_count remains one for the packed
    // channel layout; orbit_phase_count is the number of external HE passes.
    uint64_t orbit_phase_count = 1;
    uint64_t orbit_channel_block_size = 0;

    // plan 驱动的 block 选择与成本代理
    uint64_t suggested_block_size = 1;
    uint64_t pass_count = 1;
    double cost_proxy_compute = 0.0;
    double cost_proxy_round = 0.0;
    double total_score = 0.0;
};

inline uint64_t NextPow2U64(uint64_t x)
{
    if (x <= 1) return 1;
    x--;
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    return x + 1;
}
inline uint64_t CeilDivU64(uint64_t a, uint64_t b)
{
    return (a + b - 1) / b;
}

inline uint64_t CeilSqrtU64(uint64_t x)
{
    uint64_t r = 1;
    while (r * r < x) ++r;
    return r;
}

struct CirHeOpEstimate {
    uint64_t padded_feature_size = 0;
    uint64_t tile_size = 1;
    uint64_t tiled_in_channels = 0;
    uint64_t tiled_out_channels = 0;
    uint64_t input_rot = 1;

    double mul_plain = 0.0;
    double rotate_rows = 0.0;
    double add_inplace = 0.0;
    double ct_groups = 0.0;
};

inline CirHeOpEstimate EstimateCirHeOpCounts(
    uint64_t poly_degree,
    uint64_t packed_in_channels,
    uint64_t out_channels,
    uint64_t block_size,
    uint64_t effective_feature_size,
    uint64_t pass_count)
{
    CirHeOpEstimate e;

    e.padded_feature_size = NextPow2U64(effective_feature_size);
    const uint64_t ntt_size =
        block_size * e.padded_feature_size * e.padded_feature_size;

    // Stage-L layout rule: when one packed block already fills the polynomial
    // (ntt_size <= N but 2*ntt_size > N), use the single-half saturated layout.
    // The implementation only mirrors into the second half when tile_size > 1,
    // so tile_size=1 is valid with ntt_size == N.
    if (ntt_size > poly_degree) {
        e.tile_size = 0;
        return e;
    }
    e.tile_size = poly_degree / (2 * ntt_size);
    if (e.tile_size < 1) e.tile_size = 1;

    const uint64_t num_blocks_in = packed_in_channels / block_size;
    const uint64_t num_blocks_out = out_channels / block_size;

    e.tiled_in_channels = CeilDivU64(num_blocks_in, e.tile_size);
    e.tiled_out_channels = CeilDivU64(num_blocks_out, e.tile_size);

    if (e.tile_size == 1) {
        e.mul_plain =
            static_cast<double>(pass_count) *
            e.tiled_in_channels * e.tiled_out_channels;

        e.rotate_rows = 0.0;

        e.add_inplace =
            static_cast<double>(pass_count) *
            e.tiled_out_channels * (e.tiled_in_channels - 1);
    } else {
        e.input_rot = CeilSqrtU64(e.tile_size);
        const uint64_t group_cnt = CeilDivU64(e.tile_size, e.input_rot);

        e.mul_plain =
            static_cast<double>(pass_count) *
            e.tiled_in_channels * e.tiled_out_channels * e.tile_size;

        e.rotate_rows =
            static_cast<double>(pass_count) *
            (e.tiled_in_channels * (e.input_rot - 1) +
             e.tiled_out_channels * (group_cnt - 1));

        e.add_inplace =
            static_cast<double>(pass_count) *
            e.tiled_out_channels *
            ((e.tiled_in_channels - 1) * e.tile_size + e.tile_size - 1);
    }

    e.ct_groups =
        static_cast<double>(pass_count) *
        (e.tiled_in_channels + e.tiled_out_channels);

    return e;
}

inline bool IsExactTile4K3S1Candidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    return raw_in_feature_size == 28 &&
           in_channels == 128 &&
           out_channels == 128 &&
           kernel_size == 3 &&
           stride == 1 &&
           padding == 1;
}


inline bool IsStage3ExactTiledK3S1Candidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    return raw_in_feature_size == 14 &&
           in_channels == 256 &&
           out_channels == 256 &&
           kernel_size == 3 &&
           stride == 1 &&
           padding == 1;
}

inline bool IsStage1ExactTiledK3S1SearchCandidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    return raw_in_feature_size == 56 &&
           in_channels == 64 &&
           out_channels == 64 &&
           kernel_size == 3 &&
           stride == 1 &&
           padding == 1;
}

inline bool IsStage4TailExactTiledK3S1Candidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    return raw_in_feature_size == 7 &&
           in_channels == 512 &&
           out_channels == 512 &&
           kernel_size == 3 &&
           stride == 1 &&
           padding == 1;
}


inline bool DAZGOrbitQCharARHCLStringEnabled(const char* v, bool default_value)
{
    if (v == nullptr || *v == '\0') return default_value;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off" || s == "no" || s == "NO");
}

// DAZG_ORBIT_V599_QCHAR_ARHCL_POLICY_RESIDUE_20260616_BEGIN
// V599 keeps the substrate honest: BSGS/sparse packing remain borrowed
// implementation machinery. The policy decision here is only the new
// quotient-character residue ownership + anchor-halo layout selection.
inline bool DAZGOrbitV599QCharARHCLRealGateEnabled()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V614_RSOF_N10_PROMOTE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V613_RESIDUAL_FORCE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V612_RESIDUAL_COMM_MATRIX"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V611_H8_FUSED_PROMOTE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V610_FUSED_ANCHOR_ORBIT"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V610_QCHAR_ARHCL_FUSED_ORBIT"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V609_ACTIVE_MATRIX"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V609_QCHAR_ARHCL_ACTIVE_MATRIX"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V608_BENEFIT_GUARD"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V608_QCHAR_ARHCL_BENEFIT_GUARD"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V607_H4_ANCHOR_ACTIVE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V607_QCHAR_ARHCL_BOUNDED_ACTIVE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V606_MCA_TERMINAL_FUSE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V606_QCHAR_ARHCL_TERMINAL_FUSE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V605_SCALE_GATEFIX"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V604_DEEP_TURBO"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V603_ACCEL_PREFLIGHT"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V602_STATUS_FUSION_PREFLIGHT"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V602_QCHAR_ARHCL_FUSION_PREFLIGHT"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V602_QCHAR_ARHCL_REAL_GATE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V601_QCHAR_ARHCL_FUSED_SHADOW"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V601_QCHAR_ARHCL_REAL_GATE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V601_QCHAR_ARHCL"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V600_QCHAR_ARHCL_REAL_GATE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V600_QCHAR_ARHCL"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V599_QCHAR_ARHCL_REAL_GATE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V599_QCHAR_ARHCL"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V598_QCHAR_ARHCL_REAL_GATE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V598_QCHAR_ARHCL"), false);
}

inline bool DAZGOrbitV598QCharARHCLRealGateEnabled()
{
    return DAZGOrbitV599QCharARHCLRealGateEnabled();
}

inline bool DAZGOrbitV599QCharARHCLPolicySignal()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_QCHAR_ARHCL_POLICY_FORCE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V518_POLICY_DERIVED_FGBUCKET"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V518_POLICY_INDUCED_ACTIVE_MASK"), false);
}

inline bool DAZGOrbitV599QCharARHCLPolicyOnly()
{
    if (std::getenv("DAZG_ORBIT_V599_QCHAR_ARHCL_POLICY_ONLY") != nullptr) {
        return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V599_QCHAR_ARHCL_POLICY_ONLY"), true);
    }
    if (std::getenv("DAZG_ORBIT_QCHAR_ARHCL_POLICY_ONLY") != nullptr) {
        return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_QCHAR_ARHCL_POLICY_ONLY"), true);
    }
    // Default policy-only keeps the baseline as the old exact network and makes
    // the Q-CHAR/ARHC-L route earn its exactness through hash equality.
    return true;
}

inline bool DAZGOrbitV599QCharARHCLAllowBaseline()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V599_QCHAR_ARHCL_BASELINE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_QCHAR_ARHCL_BASELINE"), false);
}

inline bool DAZGOrbitV599QCharARHCLAllowOffScope()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V599_QCHAR_ARHCL_ALLOW_OFF"), false);
}

inline const char* DAZGOrbitV599QCharARHCLDefaultScope()
{
    const char* s = std::getenv("DAZG_ORBIT_V599_QCHAR_ARHCL_SCOPE_DEFAULT");
    if (s != nullptr && *s != '\0') return s;
    const char* s2 = std::getenv("V599_SCOPE");
    if (s2 != nullptr && *s2 != '\0') return s2;
    return "safe_h16_h8";
}

inline bool DAZGOrbitV599QCharARHCLScopeStringAllows(const std::string& s,
                                                   uint64_t raw_in_feature_size)
{
    if (s == "safe" || s == "safe_h16_h8" || s == "residue" || s == "residue2") {
        return raw_in_feature_size == 16 || raw_in_feature_size == 8;
    }
    if (s == "policy_h8" || s == "h8") return raw_in_feature_size == 8;
    if (s == "policy_h16" || s == "h16") return raw_in_feature_size == 16;
    if (s == "h32") return raw_in_feature_size == 32;
    if (s == "h4") return raw_in_feature_size == 4;
    if (s == "h32_h16") return raw_in_feature_size == 32 || raw_in_feature_size == 16;
    if (s == "h16_h8") return raw_in_feature_size == 16 || raw_in_feature_size == 8;
    if (s == "h32_h16_h8") return raw_in_feature_size == 32 || raw_in_feature_size == 16 || raw_in_feature_size == 8;
    if (s == "all" || s == "k3s1_cifar" || s == "cifar") return true;
    return raw_in_feature_size == 16 || raw_in_feature_size == 8;
}

inline uint64_t DAZGOrbitV599QCharARHCLAnchorDigestU64(
    uint64_t H,
    uint64_t C,
    uint64_t tile_out,
    uint64_t tile_in,
    uint64_t rows,
    uint64_t cols,
    uint64_t policy_only,
    uint64_t policy_signal)
{
    uint64_t h = 1469598103934665603ULL;
    const uint64_t vals[] = {H, C, tile_out, tile_in, rows, cols,
                             policy_only, policy_signal, 0xA59920260616ULL};
    for (uint64_t v : vals) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 1099511628211ULL;
    }
    return h;
}

// DAZG_ORBIT_V600_QCHAR_ARHCL_DOMINANCE_GUARD_20260616_BEGIN
inline uint64_t DAZGOrbitV600EnvU64(const char* key, uint64_t default_value)
{
    const char* env = std::getenv(key);
    if (env == nullptr || *env == '\0') return default_value;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(env, &end, 10);
    if (end == env) return default_value;
    return static_cast<uint64_t>(v);
}

inline bool DAZGOrbitV600QCharARHCLDominanceGuardEnabled()
{
    if (std::getenv("DAZG_ORBIT_V600_QCHAR_ARHCL_DOMINANCE_GUARD") != nullptr) {
        return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V600_QCHAR_ARHCL_DOMINANCE_GUARD"), true);
    }
    if (std::getenv("DAZG_ORBIT_QCHAR_ARHCL_DOMINANCE_GUARD") != nullptr) {
        return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_QCHAR_ARHCL_DOMINANCE_GUARD"), true);
    }
    // Default-on under the real gate: v599 proved exactness, but the naive
    // residue tile loop dominates rotations.  The next algorithmic step must
    // explicitly prove a fused residue route before it can execute by default.
    return DAZGOrbitV599QCharARHCLRealGateEnabled();
}

inline bool DAZGOrbitV600QCharARHCLForceActive()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V600_QCHAR_ARHCL_FORCE_ACTIVE"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_QCHAR_ARHCL_FORCE_ACTIVE"), false);
}

inline bool DAZGOrbitV600QCharARHCLLogEnabled()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V600_QCHAR_ARHCL_LOG"), true);
}

inline uint64_t DAZGOrbitV600QCharARHCLDenseTileLoopPenalty(const CirLayoutPlan& p)
{
    // This is intentionally a dominance guard, not a novelty claim: it detects
    // the already-measured failure mode where exact halo tiles are evaluated as
    // independent BSGS calls.  Q-CHAR/ARHC-L may execute only when a later fused
    // residue schedule proves that the tile loop no longer multiplies rotations.
    return p.pass_count * p.qchar_phase_bucket_count;
}

// DAZG_ORBIT_V601_QCHAR_ARHCL_FUSED_SHADOW_20260616_BEGIN
inline bool DAZGOrbitV601QCharARHCLFusedShadowEnabled()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V601_QCHAR_ARHCL_FUSED_SHADOW"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V601_QCHAR_ARHCL_REAL_GATE"), false);
}

inline bool DAZGOrbitV601QCharARHCLUniqueDominanceLog()
{
    if (std::getenv("DAZG_ORBIT_V601_QCHAR_ARHCL_UNIQUE_LOG") != nullptr) {
        return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V601_QCHAR_ARHCL_UNIQUE_LOG"), true);
    }
    return DAZGOrbitV601QCharARHCLFusedShadowEnabled();
}

inline uint64_t DAZGOrbitV601QCharARHCLFusedAnchorOrbitProxy(const CirLayoutPlan& p)
{
    // This is a lower-bound certificate for the next active algorithm, not
    // an execution count.  A correct residue-fused tilebatch must share one
    // anchor-domain BSGS orbit across residue classes, so its orbit proxy is
    // bounded by the larger of the spatial-pass count and the phase-bucket
    // count, not their product.
    uint64_t a = p.pass_count;
    uint64_t b = p.qchar_phase_bucket_count;
    if (a < 1) a = 1;
    if (b < 1) b = 1;
    return (a > b) ? a : b;
}

inline uint64_t DAZGOrbitV601QCharARHCLFusedSavingProxy(const CirLayoutPlan& p)
{
    const uint64_t naive = DAZGOrbitV600QCharARHCLDenseTileLoopPenalty(p);
    const uint64_t fused = DAZGOrbitV601QCharARHCLFusedAnchorOrbitProxy(p);
    return naive > fused ? (naive - fused) : 0ULL;
}

inline bool DAZGOrbitV601QCharARHCLShouldEmitDominanceLog(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    const char* decision)
{
    if (!DAZGOrbitV601QCharARHCLUniqueDominanceLog()) return true;

    static std::vector<uint64_t> seen;
    uint64_t h = 1469598103934665603ULL;
    const uint64_t vals[] = {
        raw_in_feature_size,
        in_channels,
        out_channels,
        (decision != nullptr && decision[0] == 'e') ? 1ULL : 0ULL,
        0xA60120260616ULL
    };
    for (uint64_t v : vals) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
        h *= 1099511628211ULL;
    }
    for (uint64_t v : seen) {
        if (v == h) return false;
    }
    seen.push_back(h);
    return true;
}
// DAZG_ORBIT_V601_QCHAR_ARHCL_FUSED_SHADOW_20260616_END

// DAZG_ORBIT_V602_STATUS_FUSION_PREFLIGHT_20260616_BEGIN
inline bool DAZGOrbitV602StatusFusionPreflightEnabled()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V602_STATUS_FUSION_PREFLIGHT"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V602_QCHAR_ARHCL_FUSION_PREFLIGHT"), false);
}
// DAZG_ORBIT_V602_STATUS_FUSION_PREFLIGHT_20260616_END

// DAZG_ORBIT_V610_FUSED_ANCHOR_ORBIT_20260616_BEGIN
inline bool DAZGOrbitV610FusedAnchorOrbitEnabled()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V610_FUSED_ANCHOR_ORBIT"), false) ||
           DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V610_QCHAR_ARHCL_FUSED_ORBIT"), false);
}

inline bool DAZGOrbitV610FusedAnchorScopeAllows(uint64_t raw_in_feature_size)
{
    const char* env = std::getenv("DAZG_ORBIT_V610_FUSED_ANCHOR_SCOPE");
    if (env == nullptr || *env == '\0') return true;
    const std::string s(env);
    if (s == "all" || s == "cifar" || s == "k3s1") return true;
    if (s == "h32") return raw_in_feature_size == 32;
    if (s == "h16") return raw_in_feature_size == 16;
    if (s == "h8")  return raw_in_feature_size == 8;
    if (s == "h4")  return raw_in_feature_size == 4;
    if (s == "h16_h8") return raw_in_feature_size == 16 || raw_in_feature_size == 8;
    return true;
}

inline uint64_t DAZGOrbitV610FusedAnchorPhaseBuckets(uint64_t logical_h)
{
    if (logical_h >= 32) return 16ULL;
    if (logical_h == 16) return 16ULL;
    if (logical_h == 8) return 4ULL;
    if (logical_h == 4) return 1ULL;
    return 1ULL;
}

inline bool DAZGOrbitV610IsFusedSingleAnchorPlan(const CirLayoutPlan& p)
{
    return DAZGOrbitV610FusedAnchorOrbitEnabled() &&
           p.qchar_arhcl_enabled &&
           p.pass_count == 1 &&
           p.spatial_tile_rows == 1 &&
           p.spatial_tile_cols == 1;
}
// DAZG_ORBIT_V610_FUSED_ANCHOR_ORBIT_20260616_END

// DAZG_ORBIT_V608_BENEFIT_GUARD_20260616_BEGIN
inline bool DAZGOrbitV608BenefitGuardEnabled()
{
    if (std::getenv("DAZG_ORBIT_V608_ACTIVE_REQUIRE_POSITIVE_SAVINGS") != nullptr) {
        return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V608_ACTIVE_REQUIRE_POSITIVE_SAVINGS"), true);
    }
    if (std::getenv("DAZG_ORBIT_V608_BENEFIT_GUARD") != nullptr) {
        return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V608_BENEFIT_GUARD"), true);
    }
    return false;
}

inline bool DAZGOrbitV608AllowZeroSavingsActive()
{
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_V608_ALLOW_ZERO_SAVINGS_ACTIVE"), false);
}

inline uint64_t DAZGOrbitV608QCharARHCLBenefitScore(const CirLayoutPlan& p)
{
    return p.qchar_orbit_rotation_saved_est +
           p.arhcl_terminal_rotation_saved_est +
           DAZGOrbitV601QCharARHCLFusedSavingProxy(p);
}

inline bool DAZGOrbitV608QCharARHCLHasPositiveBenefit(const CirLayoutPlan& p)
{
    return DAZGOrbitV608QCharARHCLBenefitScore(p) > 0;
}
// DAZG_ORBIT_V608_BENEFIT_GUARD_20260616_END

inline bool DAZGOrbitV600QCharARHCLAdmitsExecution(const CirLayoutPlan& p)
{
    const uint64_t max_pass =
        DAZGOrbitV600EnvU64("DAZG_ORBIT_V600_QCHAR_ARHCL_MAX_ACTIVE_PASS", 1);
    const uint64_t max_residue =
        DAZGOrbitV600EnvU64("DAZG_ORBIT_V600_QCHAR_ARHCL_MAX_ACTIVE_RESIDUE", 1);
    const uint64_t max_penalty =
        DAZGOrbitV600EnvU64("DAZG_ORBIT_V600_QCHAR_ARHCL_MAX_TILELOOP_PENALTY", 1);

    if (DAZGOrbitV608BenefitGuardEnabled() &&
        !DAZGOrbitV608AllowZeroSavingsActive() &&
        !DAZGOrbitV608QCharARHCLHasPositiveBenefit(p)) {
        return false;
    }

    return p.pass_count <= max_pass &&
           p.qchar_residue_count <= max_residue &&
           DAZGOrbitV600QCharARHCLDenseTileLoopPenalty(p) <= max_penalty;
}

inline void DAZGOrbitV600LogQCharARHCLDominance(
    const CirLayoutPlan& p,
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    const char* decision,
    const char* reason)
{
    if (!DAZGOrbitV600QCharARHCLLogEnabled()) return;
    if (!DAZGOrbitV601QCharARHCLShouldEmitDominanceLog(
            raw_in_feature_size, in_channels, out_channels, decision)) {
        return;
    }
    std::cout << "[DAZG_ORBIT_V600_QCHAR_ARHCL_DOMINANCE]"
              << " marker=DAZG_ORBIT_V600_QCHAR_ARHCL_DOMINANCE_GUARD_20260616"
              << " decision=" << decision
              << " reason=" << reason
              << " H=" << raw_in_feature_size
              << " Cin=" << in_channels
              << " Cout=" << out_channels
              << " tile_out=" << p.tile_out_feature_size
              << " tile_in=" << p.tile_in_feature_size
              << " pass_count=" << p.pass_count
              << " qchar_residue_count=" << p.qchar_residue_count
              << " qchar_phase_bucket_count=" << p.qchar_phase_bucket_count
              << " dense_tile_loop_penalty=" << DAZGOrbitV600QCharARHCLDenseTileLoopPenalty(p)
              << " qchar_orbit_rotation_saved_est=" << p.qchar_orbit_rotation_saved_est
              << " arhcl_terminal_rotation_saved_est=" << p.arhcl_terminal_rotation_saved_est
              << " fused_anchor_orbit_proxy=" << DAZGOrbitV601QCharARHCLFusedAnchorOrbitProxy(p)
              << " fusion_saving_proxy=" << DAZGOrbitV601QCharARHCLFusedSavingProxy(p)
              << " v608_benefit_score=" << DAZGOrbitV608QCharARHCLBenefitScore(p)
              << " v608_positive_benefit=" << (DAZGOrbitV608QCharARHCLHasPositiveBenefit(p) ? 1 : 0)
              << " v608_benefit_guard=" << (DAZGOrbitV608BenefitGuardEnabled() ? 1 : 0)
              << " active_execution=" << (std::string(decision) == "execute" ? 1 : 0)
              << " force_active=" << (DAZGOrbitV600QCharARHCLForceActive() ? 1 : 0)
              << " v601_marker=DAZG_ORBIT_V601_QCHAR_ARHCL_FUSED_SHADOW_20260616"
              << " v602_marker=DAZG_ORBIT_V602_STATUS_FUSION_PREFLIGHT_20260616"
              << " v603_marker=DAZG_ORBIT_V603_ACCEL_PREFLIGHT_20260616"
              << " v604_marker=DAZG_ORBIT_V604_DEEP_TURBO_20260616"
              << " v605_marker=DAZG_ORBIT_V605_SCALE_GATEFIX_20260616"
              << " v606_marker=DAZG_ORBIT_V606_MCA_TERMINAL_FUSE_20260616"
              << " v607_marker=DAZG_ORBIT_V607_H4_ANCHOR_ACTIVE_20260616"
              << " v608_marker=DAZG_ORBIT_V608_BENEFIT_GUARD_20260616"
              << " v609_marker=DAZG_ORBIT_V609_ACTIVE_MATRIX_20260616"
              << " v610_marker=DAZG_ORBIT_V610_FUSED_ANCHOR_ORBIT_20260616"
              << " v611_marker=DAZG_ORBIT_V611_H8_FUSED_PROMOTE_20260616"
              << " v612_marker=DAZG_ORBIT_V612_RESIDUAL_COMM_MATRIX_20260616"
              << " v613_marker=DAZG_ORBIT_V613_RESIDUAL_FORCE_20260616"
              << " v614_marker=DAZG_ORBIT_V614_RSOF_N10_PROMOTE_20260616"
              << " fused_single_anchor=" << (DAZGOrbitV610IsFusedSingleAnchorPlan(p) ? 1 : 0)
              << " bounded_active_guard=pass1_residue1_only"
              << " zero_savings_active_blocked=1"
              << " shadow_only=1"
              << " status_role=canonical_report_only"
              << " log_unique=" << (DAZGOrbitV601QCharARHCLUniqueDominanceLog() ? 1 : 0)
              << " borrowed_substrate=bsgs_sparse_packing"
              << " innovation_scope=residue_fused_anchor_orbit_certificate_not_bsgs"
              << " next_active_route=residue_fused_tilebatch"
              << std::endl;
}
// DAZG_ORBIT_V600_QCHAR_ARHCL_DOMINANCE_GUARD_20260616_END

inline uint64_t DAZGOrbitV598QCharARHCLAnchorDigestU64(
    uint64_t H,
    uint64_t C,
    uint64_t tile_out,
    uint64_t tile_in,
    uint64_t rows,
    uint64_t cols)
{
    return DAZGOrbitV599QCharARHCLAnchorDigestU64(
        H, C, tile_out, tile_in, rows, cols,
        DAZGOrbitV599QCharARHCLPolicyOnly() ? 1ULL : 0ULL,
        DAZGOrbitV599QCharARHCLPolicySignal() ? 1ULL : 0ULL);
}
// DAZG_ORBIT_V599_QCHAR_ARHCL_POLICY_RESIDUE_20260616_END

inline bool DAZGOrbitQCharARHCLEnabled()
{
    if (DAZGOrbitV599QCharARHCLRealGateEnabled()) {
        if (DAZGOrbitV599QCharARHCLPolicyOnly() &&
            !DAZGOrbitV599QCharARHCLPolicySignal() &&
            !DAZGOrbitV599QCharARHCLAllowBaseline()) {
            return false;
        }
        return true;
    }
    if (DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_DISABLE_QCHAR_ARHCL"), false)) {
        return false;
    }
    return DAZGOrbitQCharARHCLStringEnabled(std::getenv("DAZG_ORBIT_ENABLE_QCHAR_ARHCL"), false);
}

inline bool DAZGOrbitQCharARHCLScopeAllows(uint64_t raw_in_feature_size)
{
    const char* env = std::getenv("DAZG_ORBIT_QCHAR_ARHCL_SCOPE");
    const bool real_gate = DAZGOrbitV599QCharARHCLRealGateEnabled();
    std::string s = (env == nullptr || *env == '\0') ? std::string() : std::string(env);

    // Legacy v589/v597 runners force scope=off. In v599 real-gate mode that
    // value means "old wrapper default", not a user ablation request. Treat it
    // as safe_h16_h8 unless the caller explicitly opts back into off.
    if (real_gate && (s.empty() || s == "off" || s == "none" || s == "0") &&
        !DAZGOrbitV599QCharARHCLAllowOffScope()) {
        s = DAZGOrbitV599QCharARHCLDefaultScope();
    }

    if (s.empty()) return true;
    if (s == "off" || s == "none" || s == "0") return false;
    return DAZGOrbitV599QCharARHCLScopeStringAllows(s, raw_in_feature_size);
}

inline bool IsQCharARHCLK3S1Candidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    if (!DAZGOrbitQCharARHCLEnabled()) return false;
    if (!DAZGOrbitQCharARHCLScopeAllows(raw_in_feature_size)) return false;
    if (!(kernel_size == 3 && stride == 1 && padding == 1)) return false;
    if (in_channels != out_channels) return false;

    // CIFAR-100 ResNet18 BasicBlock K3/S1 residual bodies.  The stem and
    // classifier are intentionally excluded; stride-2 transitions are already
    // guarded by the existing exact PCOI path and are not re-labelled here.
    if (raw_in_feature_size == 32 && in_channels == 64) return true;
    if (raw_in_feature_size == 16 && in_channels == 128) return true;
    if (raw_in_feature_size == 8  && in_channels == 256) return true;
    if (raw_in_feature_size == 4  && in_channels == 512) return true;
    return false;
}

inline void PopulateQCharARHCLMetadata(CirLayoutPlan& p)
{
    p.qchar_arhcl_enabled = true;
    p.qchar_modulus_rows = p.spatial_tile_rows;
    p.qchar_modulus_cols = p.spatial_tile_cols;
    p.qchar_residue_count = p.spatial_tile_rows * p.spatial_tile_cols;
    p.qchar_anchor_stride = p.tile_out_feature_size;
    p.arhcl_halo_left = p.halo_size;
    p.arhcl_halo_right = p.halo_size;
    p.arhcl_halo_carry_rows =
        (p.spatial_tile_rows > 1 ? (p.spatial_tile_rows - 1) * p.tile_out_feature_size : 0);
    p.arhcl_halo_carry_cols =
        (p.spatial_tile_cols > 1 ? (p.spatial_tile_cols - 1) * p.tile_out_feature_size : 0);
    p.qchar_exact_recovery = 1;

    // V599 certificate fields: qchar_phase_bucket_count is the number of
    // quotient-character anchor classes selected for this layer. The saved
    // values are conservative evidence only; strict output hashes decide
    // correctness. This deliberately does not claim BSGS/sparse packing itself.
    p.qchar_phase_bucket_count = p.qchar_residue_count;
    p.qchar_orbit_rotation_saved_est =
        (p.qchar_residue_count > 1 ? (p.qchar_residue_count - 1) * p.tile_out_feature_size : 0);
    p.arhcl_terminal_rotation_saved_est =
        (p.spatial_tile_rows > 1 ? p.spatial_tile_rows - 1 : 0) +
        (p.spatial_tile_cols > 1 ? p.spatial_tile_cols - 1 : 0);
    p.arhcl_anchor_digest = DAZGOrbitV598QCharARHCLAnchorDigestU64(
        p.logical_out_feature_size,
        p.packed_in_channels,
        p.tile_out_feature_size,
        p.tile_in_feature_size,
        p.spatial_tile_rows,
        p.spatial_tile_cols);

    if (DAZGOrbitV610IsFusedSingleAnchorPlan(p)) {
        const uint64_t fused_buckets =
            DAZGOrbitV610FusedAnchorPhaseBuckets(p.logical_out_feature_size);
        p.qchar_residue_count = 1;
        p.qchar_modulus_rows = 1;
        p.qchar_modulus_cols = 1;
        p.qchar_anchor_stride = p.logical_out_feature_size;
        p.qchar_phase_bucket_count = fused_buckets;
        p.qchar_orbit_rotation_saved_est =
            (fused_buckets > 1 ? (fused_buckets - 1) * p.tile_out_feature_size : 0);
        p.arhcl_halo_carry_rows = 0;
        p.arhcl_halo_carry_cols = 0;
        p.arhcl_terminal_rotation_saved_est = 0;
        p.arhcl_anchor_digest = DAZGOrbitV599QCharARHCLAnchorDigestU64(
            p.logical_out_feature_size,
            p.packed_in_channels,
            p.tile_out_feature_size,
            p.tile_in_feature_size,
            p.spatial_tile_rows,
            p.spatial_tile_cols,
            1ULL,
            fused_buckets);
    }
}

inline bool IsR50PointwiseK1S1Candidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    if (kernel_size != 1 || stride != 1 || padding != 0) return false;

    // Never route classifier/stem through the R50 pointwise tiler.
    if (raw_in_feature_size == 1 || out_channels == 1000) {
        return false;
    }

    // Canonical Bottleneck ResNet-50 pointwise patterns under the current
    // DAZG-Orbit stem.  The route is deliberately stage-certified rather than
    // generic: this avoids accidentally retargeting toy or legacy BasicBlock
    // benchmarks that happen to contain a K=1,S=1 convolution.
    if (raw_in_feature_size == 56) {
        return (in_channels == 64  && out_channels == 64)  ||
               (in_channels == 64  && out_channels == 256) ||
               (in_channels == 256 && out_channels == 64)  ||
               (in_channels == 256 && out_channels == 128);
    }

    if (raw_in_feature_size == 28) {
        return (in_channels == 128 && out_channels == 512) ||
               (in_channels == 512 && out_channels == 128) ||
               (in_channels == 512 && out_channels == 256);
    }

    if (raw_in_feature_size == 14) {
        return (in_channels == 256  && out_channels == 1024) ||
               (in_channels == 1024 && out_channels == 256)  ||
               (in_channels == 1024 && out_channels == 512);
    }

    return false;
}


inline bool IsStageLLazyRankH7SpatialFactorCandidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    return raw_in_feature_size == 7 &&
           in_channels == 512 &&
           out_channels == 128 &&
           kernel_size == 3 &&
           stride == 1 &&
           padding == 1;
}


inline bool IsStageOMidRankH28SpatialFactorCandidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    return raw_in_feature_size == 28 &&
           in_channels == 128 &&
           out_channels == 32 &&
           kernel_size == 3 &&
           stride == 1 &&
           padding == 1;
}

inline bool IsStageOMidRankH28PointwiseFactorCandidate(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding)
{
    return raw_in_feature_size == 28 &&
           in_channels == 32 &&
           out_channels == 128 &&
           kernel_size == 1 &&
           stride == 1 &&
           padding == 0;
}

inline void PopulateCirLayoutPlanCost(
    CirLayoutPlan& p,
    uint64_t poly_degree,
    uint64_t out_channels)
{
    static const double lambda_R = 4.0;
    static const double lambda_C = 0.0;

    // Stage-M correction:
    // `pass_count` in exact tiled CirConv is a local HE tiling loop, not an
    // interactive protocol round.  The previous Stage-L score penalized it as
    // if each tile pass added online rounds, which incorrectly rejected the
    // H=7, rank=128 exact-tile layout and selected rank=256 instead.
    //
    // Keep the score protocol-aware: online rounds are handled by the
    // surrounding fixed-point/activation protocol, while this layout score
    // should choose the minimum local HE work inside one convolution.
    static const double lambda_noninteractive_pass = 0.0;

    CirHeOpEstimate est = EstimateCirHeOpCounts(
        poly_degree,
        p.packed_in_channels,
        out_channels,
        p.suggested_block_size,
        p.effective_feature_size,
        p.pass_count
    );

    p.cost_proxy_compute =
        est.mul_plain + est.add_inplace + lambda_R * est.rotate_rows;
    p.cost_proxy_round =
        lambda_noninteractive_pass *
        static_cast<double>(p.pass_count > 0 ? p.pass_count - 1 : 0);
    p.total_score =
        p.cost_proxy_compute + p.cost_proxy_round
        + lambda_C * est.ct_groups;
}

inline uint64_t SuggestCirBlockSize(
    uint64_t packed_in_channels,
    uint64_t out_channels,
    uint64_t effective_feature_size,
    uint64_t poly_degree = 8192)
{
    const uint64_t padded_feature_size = NextPow2U64(effective_feature_size);

    // Stage-N adds 512 and 256 so a padded 512->1024 classifier head can
    // use a resonant block=512 layout instead of the old block=8 path for 1000 classes.
    static const uint64_t kCandidateBlocks[] = {512, 256, 128, 64, 32, 16, 8, 4, 2, 1};
    for (uint64_t block_size : kCandidateBlocks) {
        if (packed_in_channels % block_size != 0 ||
            out_channels % block_size != 0) {
            continue;
        }

        const uint64_t ntt_size =
            block_size * padded_feature_size * padded_feature_size;

        // Stage-L single-half saturated layout.  The old rule required
        // 2*ntt_size <= N even when tile_size would be 1.  CirConv2D only
        // writes the mirrored second half for tile_size > 1, therefore
        // ntt_size <= N is the actual capacity condition for tile_size=1.
        if (ntt_size <= poly_degree) {
            return block_size;
        }
    }

    return 1;
}

inline CirLayoutPlan BuildExactTiledK3S1Plan(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding,
    uint64_t poly_degree = 8192,
    uint64_t forced_tile_out = 0)
{
    CirLayoutPlan best;
    best.total_score = 1e300;

    const uint64_t out_feature_size =
        (raw_in_feature_size + 2 * padding - kernel_size) / stride + 1;
    const uint64_t halo_size = kernel_size / 2;

    const uint64_t* candidate_tile_outs = nullptr;
    size_t candidate_count = 0;

    static const uint64_t kStage1Candidates[] = {56, 28, 14};
    static const uint64_t kStage2Candidates[] = {14};
    // H=14,C=256 K3S1 is an exact dense hot path.  Candidate 14 keeps the
    // original one-pass full spatial domain; candidate 7 is an exact 2x2
    // halo decomposition used only if the cost model prefers its block size.
    static const uint64_t kStage3Candidates[] = {14, 7};
    static const uint64_t kStage4TailCandidates[] = {4};

    // DAZGOrbit-R50-PWTile: exact pointwise spatial tiling for Bottleneck
    // 1x1 convolutions.  K=1 has zero halo, so tile_in == tile_out and
    // spatial tiles are mathematically independent.
    static const uint64_t kR50PWTileH56Candidates[] = {14};
    static const uint64_t kR50PWTileH28Candidates[] = {14};
    static const uint64_t kR50PWTileH14Candidates[] = {7};

    // H=7 rank-spatial factor: split into 2x2 tiles of output size 4.
    // Each tile uses a 6x6 halo input, padded to 8, enabling block=128.
    static const uint64_t kStageLRankH7SpatialCandidates[] = {4};

    // Stage-O mid-stage resonance:
    // H=28 is not beneficial with a naive low-rank path because the 1x1
    // pointwise factor would fall back to full-space block=8.  We therefore
    // reuse the exact tiled path for both the 3x3 spatial factor and the 1x1
    // pointwise factor with tile_out=14.  This lets both factors use block=32.
    static const uint64_t kStageOMidRankH28Candidates[] = {14};

    // Q-CHAR + ARHC-L CIFAR K3/S1 residue candidates.  These keep exact
    // recovery: every quotient tile is evaluated on an anchor-local halo patch
    // and scattered back without overlap.
    static const uint64_t kQCharARHCLH32Candidates[] = {16, 8};
    static const uint64_t kQCharARHCLH16Candidates[] = {8, 4};
    static const uint64_t kQCharARHCLH8Candidates[] = {4};
    static const uint64_t kQCharARHCLH4Candidates[] = {4};

    // V610 true fused-anchor candidates: one spatial pass over the full
    // residual domain.  This avoids the v599/v609 pass_count x residue_count
    // tile-loop blow-up.  The implementation still uses the existing exact
    // SparseBSGS substrate; the new object is the fused anchor layout decision.
    static const uint64_t kQCharARHCLH32FusedCandidates[] = {32};
    static const uint64_t kQCharARHCLH16FusedCandidates[] = {16};
    static const uint64_t kQCharARHCLH8FusedCandidates[] = {8};
    static const uint64_t kQCharARHCLH4FusedCandidates[] = {4};

    const uint64_t forced_candidates[] = {forced_tile_out};
    bool qchar_arhcl_candidate_family = false;

    if (forced_tile_out != 0) {
        candidate_tile_outs = forced_candidates;
        candidate_count = 1;
    } else if (IsQCharARHCLK3S1Candidate(raw_in_feature_size,
                                          in_channels,
                                          out_channels,
                                          kernel_size,
                                          stride,
                                          padding)) {
        qchar_arhcl_candidate_family = true;
        if (DAZGOrbitV610FusedAnchorOrbitEnabled() &&
            DAZGOrbitV610FusedAnchorScopeAllows(raw_in_feature_size)) {
            if (raw_in_feature_size == 32) {
                candidate_tile_outs = kQCharARHCLH32FusedCandidates;
                candidate_count = sizeof(kQCharARHCLH32FusedCandidates) / sizeof(kQCharARHCLH32FusedCandidates[0]);
            } else if (raw_in_feature_size == 16) {
                candidate_tile_outs = kQCharARHCLH16FusedCandidates;
                candidate_count = sizeof(kQCharARHCLH16FusedCandidates) / sizeof(kQCharARHCLH16FusedCandidates[0]);
            } else if (raw_in_feature_size == 8) {
                candidate_tile_outs = kQCharARHCLH8FusedCandidates;
                candidate_count = sizeof(kQCharARHCLH8FusedCandidates) / sizeof(kQCharARHCLH8FusedCandidates[0]);
            } else if (raw_in_feature_size == 4) {
                candidate_tile_outs = kQCharARHCLH4FusedCandidates;
                candidate_count = sizeof(kQCharARHCLH4FusedCandidates) / sizeof(kQCharARHCLH4FusedCandidates[0]);
            }
        } else if (raw_in_feature_size == 32) {
            candidate_tile_outs = kQCharARHCLH32Candidates;
            candidate_count = sizeof(kQCharARHCLH32Candidates) / sizeof(kQCharARHCLH32Candidates[0]);
        } else if (raw_in_feature_size == 16) {
            candidate_tile_outs = kQCharARHCLH16Candidates;
            candidate_count = sizeof(kQCharARHCLH16Candidates) / sizeof(kQCharARHCLH16Candidates[0]);
        } else if (raw_in_feature_size == 8) {
            candidate_tile_outs = kQCharARHCLH8Candidates;
            candidate_count = sizeof(kQCharARHCLH8Candidates) / sizeof(kQCharARHCLH8Candidates[0]);
        } else if (raw_in_feature_size == 4) {
            candidate_tile_outs = kQCharARHCLH4Candidates;
            candidate_count = sizeof(kQCharARHCLH4Candidates) / sizeof(kQCharARHCLH4Candidates[0]);
        }
    } else if (IsR50PointwiseK1S1Candidate(raw_in_feature_size,
                                           in_channels,
                                           out_channels,
                                           kernel_size,
                                           stride,
                                           padding)) {
        if (raw_in_feature_size == 56) {
            candidate_tile_outs = kR50PWTileH56Candidates;
            candidate_count = sizeof(kR50PWTileH56Candidates) / sizeof(kR50PWTileH56Candidates[0]);
        } else if (raw_in_feature_size == 28) {
            candidate_tile_outs = kR50PWTileH28Candidates;
            candidate_count = sizeof(kR50PWTileH28Candidates) / sizeof(kR50PWTileH28Candidates[0]);
        } else if (raw_in_feature_size == 14) {
            candidate_tile_outs = kR50PWTileH14Candidates;
            candidate_count = sizeof(kR50PWTileH14Candidates) / sizeof(kR50PWTileH14Candidates[0]);
        }
    } else if (IsStage1ExactTiledK3S1SearchCandidate(raw_in_feature_size,
                                                     in_channels,
                                                     out_channels,
                                                     kernel_size,
                                                     stride,
                                                     padding)) {
        candidate_tile_outs = kStage1Candidates;
        candidate_count = sizeof(kStage1Candidates) / sizeof(kStage1Candidates[0]);
    } else if (IsExactTile4K3S1Candidate(raw_in_feature_size,
                                         in_channels,
                                         out_channels,
                                         kernel_size,
                                         stride,
                                         padding)) {
        candidate_tile_outs = kStage2Candidates;
        candidate_count = sizeof(kStage2Candidates) / sizeof(kStage2Candidates[0]);
    } else if (IsStage3ExactTiledK3S1Candidate(raw_in_feature_size,
                                              in_channels,
                                              out_channels,
                                              kernel_size,
                                              stride,
                                              padding)) {
        candidate_tile_outs = kStage3Candidates;
        candidate_count = sizeof(kStage3Candidates) / sizeof(kStage3Candidates[0]);
    } else if (IsStage4TailExactTiledK3S1Candidate(raw_in_feature_size,
                                                   in_channels,
                                                   out_channels,
                                                   kernel_size,
                                                   stride,
                                                   padding)) {
        candidate_tile_outs = kStage4TailCandidates;
        candidate_count = sizeof(kStage4TailCandidates) / sizeof(kStage4TailCandidates[0]);
    } else if (IsStageLLazyRankH7SpatialFactorCandidate(raw_in_feature_size,
                                                        in_channels,
                                                        out_channels,
                                                        kernel_size,
                                                        stride,
                                                        padding)) {
        candidate_tile_outs = kStageLRankH7SpatialCandidates;
        candidate_count = sizeof(kStageLRankH7SpatialCandidates) / sizeof(kStageLRankH7SpatialCandidates[0]);
    } else if (IsStageOMidRankH28SpatialFactorCandidate(raw_in_feature_size,
                                                        in_channels,
                                                        out_channels,
                                                        kernel_size,
                                                        stride,
                                                        padding) ||
               IsStageOMidRankH28PointwiseFactorCandidate(raw_in_feature_size,
                                                          in_channels,
                                                          out_channels,
                                                          kernel_size,
                                                          stride,
                                                          padding)) {
        candidate_tile_outs = kStageOMidRankH28Candidates;
        candidate_count = sizeof(kStageOMidRankH28Candidates) / sizeof(kStageOMidRankH28Candidates[0]);
    } else {
        return best;
    }

    for (size_t idx = 0; idx < candidate_count; ++idx) {
        const uint64_t tile_out_feature_size = candidate_tile_outs[idx];
        if (tile_out_feature_size == 0) continue;

        const uint64_t spatial_tile_rows = CeilDivU64(out_feature_size, tile_out_feature_size);
        const uint64_t spatial_tile_cols = CeilDivU64(out_feature_size, tile_out_feature_size);
        const uint64_t pass_count = spatial_tile_rows * spatial_tile_cols;
        const uint64_t tile_in_feature_size =
            tile_out_feature_size + 2 * halo_size;

        const uint64_t suggested_block_size =
            SuggestCirBlockSize(in_channels,
                                out_channels,
                                tile_in_feature_size,
                                poly_degree);
        if (suggested_block_size == 0) continue;

        CirLayoutPlan cand;
        cand.mode = qchar_arhcl_candidate_family
            ? CirLayoutMode::EXACT_QCHAR_ARHCL_K3_S1
            : ((kernel_size == 1 && stride == 1 && padding == 0)
                   ? CirLayoutMode::EXACT_TILED_K1_S1
                   : CirLayoutMode::EXACT_TILED_K3_S1);
        cand.out_feature_size = out_feature_size;
        cand.route_feature_size = tile_in_feature_size;
        cand.capacity_padding = 0;
        cand.effective_feature_size = tile_in_feature_size;
        cand.packed_in_channels = in_channels;
        cand.packed_kernel_size = kernel_size;
        cand.packed_stride = 1;
        cand.packed_padding = 0;
        cand.exact_tile4_k3s1 = true;
        cand.logical_out_feature_size = out_feature_size;
        cand.packed_out_feature_size = tile_out_feature_size;
        cand.spatial_tile_rows = spatial_tile_rows;
        cand.spatial_tile_cols = spatial_tile_cols;
        cand.tile_out_feature_size = tile_out_feature_size;
        cand.tile_in_feature_size = tile_in_feature_size;
        cand.halo_size = halo_size;
        cand.phase_count = 1;
        cand.suggested_block_size = suggested_block_size;
        cand.pass_count = pass_count;
        if (qchar_arhcl_candidate_family) {
            PopulateQCharARHCLMetadata(cand);
        }
        PopulateCirLayoutPlanCost(cand, poly_degree, out_channels);

        if (cand.total_score < best.total_score) {
            best = cand;
        }
    }

    if (best.qchar_arhcl_enabled &&
        DAZGOrbitV600QCharARHCLDominanceGuardEnabled() &&
        !DAZGOrbitV600QCharARHCLForceActive()) {
        if (!DAZGOrbitV600QCharARHCLAdmitsExecution(best)) {
            DAZGOrbitV600LogQCharARHCLDominance(
                best,
                raw_in_feature_size,
                in_channels,
                out_channels,
                "shadow",
                "naive_residue_tile_loop_not_fused");
            CirLayoutPlan rejected;
            rejected.total_score = 1e300;
            return rejected;
        }
        DAZGOrbitV600LogQCharARHCLDominance(
            best,
            raw_in_feature_size,
            in_channels,
            out_channels,
            "execute",
            "dominance_guard_admitted");
    }

    return best;
}


inline CirLayoutPlan BuildExactCompactStride2Plan(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding,
    CirLayoutMode exact_mode,
    uint64_t tile_out_feature_size,
    uint64_t poly_degree = 8192)
{
    CirLayoutPlan cand;
    cand.total_score = 1e300;

    const bool exact_pointwise = IsExactCompactStride2PointwiseMode(exact_mode);
    const bool exact_k3_polyphase = IsExactCompactStride2K3PolyphaseMode(exact_mode);

    if (!exact_pointwise && !exact_k3_polyphase) {
        return cand;
    }
    if (tile_out_feature_size == 0) {
        return cand;
    }
    if (exact_pointwise && !(kernel_size == 1 && stride == 2 && padding == 0)) {
        return cand;
    }
    if (exact_k3_polyphase && !(kernel_size == 3 && stride == 2 && padding == 1)) {
        return cand;
    }

    const uint64_t out_feature_size =
        (raw_in_feature_size + 2 * padding - kernel_size) / stride + 1;

    const uint64_t packed_kernel_size = exact_pointwise ? 1 : 2;
    const uint64_t packed_in_channels = exact_pointwise ? in_channels : 4 * in_channels;
    const uint64_t tile_in_feature_size =
        tile_out_feature_size + packed_kernel_size - 1;

    const uint64_t spatial_tile_rows = CeilDivU64(out_feature_size, tile_out_feature_size);
    const uint64_t spatial_tile_cols = CeilDivU64(out_feature_size, tile_out_feature_size);
    const uint64_t pass_count = spatial_tile_rows * spatial_tile_cols;

    const uint64_t suggested_block_size =
        SuggestCirBlockSize(packed_in_channels,
                            out_channels,
                            tile_in_feature_size,
                            poly_degree);
    if (suggested_block_size == 0) {
        return cand;
    }

    cand.mode = exact_mode;
    cand.out_feature_size = out_feature_size;
    cand.route_feature_size = tile_in_feature_size;
    cand.capacity_padding = 0;
    cand.effective_feature_size = tile_in_feature_size;
    cand.packed_in_channels = packed_in_channels;
    cand.packed_kernel_size = packed_kernel_size;
    cand.packed_stride = 1;
    cand.packed_padding = 0;
    cand.exact_tile4_k3s1 = true;
    cand.logical_out_feature_size = out_feature_size;
    cand.packed_out_feature_size = tile_out_feature_size;
    cand.spatial_tile_rows = spatial_tile_rows;
    cand.spatial_tile_cols = spatial_tile_cols;
    cand.tile_out_feature_size = tile_out_feature_size;
    cand.tile_in_feature_size = tile_in_feature_size;
    cand.halo_size = packed_kernel_size - 1;
    cand.phase_count = exact_pointwise ? 1 : 4;
    cand.suggested_block_size = suggested_block_size;
    cand.pass_count = pass_count;

    PopulateCirLayoutPlanCost(cand, poly_degree, out_channels);
    return cand;
}


inline CirLayoutPlan BuildExactPCOIK3S2OrbitPlan(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding,
    uint64_t tile_out_feature_size,
    uint64_t poly_degree = 8192)
{
    CirLayoutPlan cand;
    cand.total_score = 1e300;

    if (!(kernel_size == 3 && stride == 2 && padding == 1)) {
        return cand;
    }
    if (tile_out_feature_size == 0) {
        return cand;
    }

    const uint64_t out_feature_size =
        (raw_in_feature_size + 2 * padding - kernel_size) / stride + 1;

    // PCOI keeps channel orbit untouched: no phase*Cin packed channels.
    const uint64_t packed_kernel_size = 2;
    const uint64_t packed_in_channels = in_channels;
    const uint64_t tile_in_feature_size =
        tile_out_feature_size + packed_kernel_size - 1;

    const uint64_t spatial_tile_rows = CeilDivU64(out_feature_size, tile_out_feature_size);
    const uint64_t spatial_tile_cols = CeilDivU64(out_feature_size, tile_out_feature_size);

    // DAZG_ORBIT_V475_CHANNEL_ORBIT_LOCK_BEGIN
    // PCOI changes only the spatial phase/orbit schedule.  The learned
    // channel-circulant DAZG orbit must remain identical to the canonical FULL
    // K3/S2 path; otherwise changing tile_in_feature_size changes block_size
    // and therefore changes the effective model.
    const uint64_t canonical_full_effective_feature_size =
        raw_in_feature_size + 2 * padding;
    const uint64_t suggested_block_size =
        SuggestCirBlockSize(packed_in_channels,
                            out_channels,
                            canonical_full_effective_feature_size,
                            poly_degree);
    // DAZG_ORBIT_V475_CHANNEL_ORBIT_LOCK_END
    if (suggested_block_size == 0) {
        return cand;
    }

    cand.mode = CirLayoutMode::EXACT_PCOI_K3_S2_ORBIT;
    cand.out_feature_size = out_feature_size;
    cand.route_feature_size = tile_in_feature_size;
    cand.capacity_padding = 0;
    cand.effective_feature_size = tile_in_feature_size;
    cand.packed_in_channels = packed_in_channels;
    cand.packed_kernel_size = packed_kernel_size;
    cand.packed_stride = 1;
    cand.packed_padding = 0;
    cand.exact_tile4_k3s1 = true;
    cand.logical_out_feature_size = out_feature_size;
    cand.packed_out_feature_size = tile_out_feature_size;
    cand.spatial_tile_rows = spatial_tile_rows;
    cand.spatial_tile_cols = spatial_tile_cols;
    cand.tile_out_feature_size = tile_out_feature_size;
    cand.tile_in_feature_size = tile_in_feature_size;
    cand.halo_size = packed_kernel_size - 1;
    cand.phase_count = 1;
    cand.orbit_phase_count = 4;
    cand.orbit_channel_block_size = in_channels;
    cand.suggested_block_size = suggested_block_size;
    cand.pass_count = spatial_tile_rows * spatial_tile_cols * cand.orbit_phase_count;

    PopulateCirLayoutPlanCost(cand, poly_degree, out_channels);
    return cand;
}

// DAZG_ORBIT_V720_QAHL_TO_H8_ROUTE_BEGIN
// Default-off, tuple-specific QAHL routing gate. It affects only:
//   main0: [96,16,16] -- K3/S2/P1 --> [192,8,8]
//   optional skip: [96,16,16] -- K1/S2/P0 --> [192,8,8]
// It does not change arithmetic, weights, HE parameters, model topology,
// earlier to_h16 routing, or any non-QAHL channel tuple.
inline const char* DAZGOrbitV720QahlToH8RouteMarker()
{
    return "DAZG_ORBIT_V720_FRESH_20260622_210440_79213_14940";
}

inline std::string DAZGOrbitV720QahlToH8RouteCandidate()
{
    const char* value = std::getenv("DAZG_ORBIT_V720_TO_H8_ROUTE");
    return value == nullptr ? std::string("off") : std::string(value);
}

inline bool DAZGOrbitV720QahlToH8Main0PCOIEnabled()
{
    const std::string candidate = DAZGOrbitV720QahlToH8RouteCandidate();
    return candidate == "main0_pcoi" ||
           candidate == "main0_pcoi_skip_colayout";
}

inline bool DAZGOrbitV720QahlToH8SkipColayoutEnabled()
{
    return DAZGOrbitV720QahlToH8RouteCandidate() ==
           "main0_pcoi_skip_colayout";
}
// DAZG_ORBIT_V720_QAHL_TO_H8_ROUTE_END

inline CirLayoutPlan MakeCirLayoutPlan(
    uint64_t raw_in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    uint64_t padding,
    bool enable_k3_polyphase = false,
    bool enable_exact_tile4_k3s1 = false,
    uint64_t poly_degree = 8192,
    uint64_t forced_exact_tile_out = 0)
{
    CirLayoutPlan p;
    p.out_feature_size =
        (raw_in_feature_size + 2 * padding - kernel_size) / stride + 1;
    p.logical_out_feature_size = p.out_feature_size;
    p.packed_out_feature_size = p.out_feature_size;
    p.pass_count = 1;

    // 1x1, stride=2 compact pointwise
    if (kernel_size == 1 && stride == 2 && padding == 0) {
        p.mode = CirLayoutMode::COMPACT_PW_S2;

        p.route_feature_size = p.out_feature_size;
        p.capacity_padding = 0;

        p.effective_feature_size = p.out_feature_size;
        p.packed_in_channels = in_channels;
        p.packed_kernel_size = 1;
        p.packed_stride = 1;
        p.packed_padding = 0;
        p.phase_count = 1;
        p.suggested_block_size =
            SuggestCirBlockSize(p.packed_in_channels,
                                out_channels,
                                p.effective_feature_size,
                                poly_degree);
        PopulateCirLayoutPlanCost(p, poly_degree, out_channels);

        uint64_t compact_exact_tile_out = 0;
        // DAZG_ORBIT_V502_PROJECTION_COLAYOUT_EXPORT_BINDER_20260531_BEGIN
        // CIFAR ResNet projection shortcuts.  These K1/S2 shortcut branches
        // must use a tiled exact route whose spatial tile geometry matches the
        // K3/S2 PCOI main branch in the same residual boundary.  This is the
        // co-layout precondition for the residual-boundary communication
        // supernode; it replaces unsafe runtime RCP lifting.
        if (p.out_feature_size == 16 &&
            in_channels == 64 &&
            out_channels == 128) {
            compact_exact_tile_out = 8;
        } else if (p.out_feature_size == 8 &&
                   in_channels == 128 &&
                   out_channels == 256) {
            compact_exact_tile_out = 4;
// DAZG_ORBIT_V720_QAHL_TO_H8_SKIP_BIND_BEGIN
        } else if (DAZGOrbitV720QahlToH8SkipColayoutEnabled() &&
                   p.out_feature_size == 8 &&
                   in_channels == 96 &&
                   out_channels == 192) {
            compact_exact_tile_out = 4;
// DAZG_ORBIT_V720_QAHL_TO_H8_SKIP_BIND_END
        } else if (p.out_feature_size == 4 &&
                   in_channels == 256 &&
                   out_channels == 512) {
            compact_exact_tile_out = 4;
        // DAZG_ORBIT_V502_PROJECTION_COLAYOUT_EXPORT_BINDER_20260531_END
        } else if (p.out_feature_size == 28 &&
                   in_channels == 64 &&
                   out_channels == 128) {
            compact_exact_tile_out = 14;
        } else if (p.out_feature_size == 14 &&
                   in_channels == 128 &&
                   out_channels == 256) {
            compact_exact_tile_out = 7;
        } else if (p.out_feature_size == 7 &&
                   in_channels == 256 &&
                   out_channels == 512) {
            compact_exact_tile_out = 4;
        } else if (p.out_feature_size == 28 &&
                   in_channels == 256 &&
                   out_channels == 512) {
            // Bottleneck ResNet-50 projection shortcut: 56 -> 28.
            compact_exact_tile_out = 14;
        } else if (p.out_feature_size == 14 &&
                   in_channels == 512 &&
                   out_channels == 1024) {
            // Bottleneck ResNet-50 projection shortcut: 28 -> 14.
            compact_exact_tile_out = 7;
        } else if (p.out_feature_size == 7 &&
                   in_channels == 1024 &&
                   out_channels == 2048) {
            // Bottleneck ResNet-50 projection shortcut: 14 -> 7.
            compact_exact_tile_out = 4;
        }

        if (compact_exact_tile_out != 0) {
            CirLayoutPlan exact_plan = BuildExactCompactStride2Plan(
                raw_in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                padding,
                CirLayoutMode::EXACT_COMPACT_PW_S2,
                compact_exact_tile_out,
                poly_degree
            );
// DAZG_ORBIT_V720_QAHL_TO_H8_SKIP_FORCE_BEGIN
            const bool v720_force_qahl_skip_colayout =
                DAZGOrbitV720QahlToH8SkipColayoutEnabled() &&
                p.out_feature_size == 8 &&
                in_channels == 96 &&
                out_channels == 192;
            if (IsExactTileK3S1Mode(exact_plan.mode) &&
                (v720_force_qahl_skip_colayout ||
                 exact_plan.total_score < p.total_score)) {
                return exact_plan;
            }
// DAZG_ORBIT_V720_QAHL_TO_H8_SKIP_FORCE_END
        }

        return p;
    }

    // 3x3, stride=2, padding=1 polyphase compact
    if (enable_k3_polyphase &&
        kernel_size == 3 &&
        stride == 2 &&
        padding == 1) {
        // CIFAR ResNet stage-transition main branches: use DAZG-ORBIT
        // phase-consistent orbit indexing instead of folding four spatial
        // phases into the learned channel-circulant orbit.
        uint64_t pcoi_exact_tile_out = 0;
        if (raw_in_feature_size == 32 &&
            in_channels == 64 &&
            out_channels == 128) {
            pcoi_exact_tile_out = 8;
        } else if (raw_in_feature_size == 16 &&
                   in_channels == 128 &&
                   out_channels == 256) {
            pcoi_exact_tile_out = 4;
// DAZG_ORBIT_V720_QAHL_TO_H8_MAIN0_BIND_BEGIN
        } else if (DAZGOrbitV720QahlToH8Main0PCOIEnabled() &&
                   raw_in_feature_size == 16 &&
                   in_channels == 96 &&
                   out_channels == 192) {
            pcoi_exact_tile_out = 4;
// DAZG_ORBIT_V720_QAHL_TO_H8_MAIN0_BIND_END
        } else if (raw_in_feature_size == 8 &&
                   in_channels == 256 &&
                   out_channels == 512) {
            pcoi_exact_tile_out = 4;
        }

        if (pcoi_exact_tile_out != 0) {
            CirLayoutPlan pcoi_plan = BuildExactPCOIK3S2OrbitPlan(
                raw_in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                padding,
                pcoi_exact_tile_out,
                poly_degree
            );
            if (IsExactPCOIK3S2OrbitMode(pcoi_plan.mode)) {
                return pcoi_plan;
            }
        }

        p.mode = CirLayoutMode::COMPACT_K3_S2_POLYPHASE;

        p.route_feature_size = p.out_feature_size + 1;
        p.capacity_padding = 0;

        p.effective_feature_size = p.out_feature_size + 1;
        p.packed_in_channels = 4 * in_channels;
        p.packed_kernel_size = 2;
        p.packed_stride = 1;
        p.packed_padding = 0;
        p.phase_count = 4;
        p.suggested_block_size =
            SuggestCirBlockSize(p.packed_in_channels,
                                out_channels,
                                p.effective_feature_size,
                                poly_degree);
        PopulateCirLayoutPlanCost(p, poly_degree, out_channels);

        uint64_t compact_exact_tile_out = 0;
        if (p.out_feature_size == 28 &&
            in_channels == 64 &&
            out_channels == 128) {
            compact_exact_tile_out = 14;
        } else if (p.out_feature_size == 14 &&
                   in_channels == 128 &&
                   out_channels == 256) {
            compact_exact_tile_out = 7;
        } else if (p.out_feature_size == 7 &&
                   in_channels == 256 &&
                   out_channels == 512) {
            compact_exact_tile_out = 4;
        }

        if (compact_exact_tile_out != 0) {
            CirLayoutPlan exact_plan = BuildExactCompactStride2Plan(
                raw_in_feature_size,
                in_channels,
                out_channels,
                kernel_size,
                stride,
                padding,
                CirLayoutMode::EXACT_COMPACT_K3_S2_POLYPHASE,
                compact_exact_tile_out,
                poly_degree
            );
            if (IsExactTileK3S1Mode(exact_plan.mode) &&
                exact_plan.total_score < p.total_score) {
                return exact_plan;
            }
        }

        return p;
    }

    if (enable_exact_tile4_k3s1 &&
        stride == 1 &&
        ((kernel_size == 3 && padding == 1) ||
         (kernel_size == 1 && padding == 0))) {
        // The exact tiled executor is kernel-size agnostic once the plan is
        // built: for K=1 the halo is zero, tile_in==tile_out, and depack offset
        // is zero.  Stage-O uses this to make the low-rank pointwise factor a
        // first-class tiled HE operator instead of a full-space fallback.
        CirLayoutPlan exact_plan =
            BuildExactTiledK3S1Plan(raw_in_feature_size,
                                    in_channels,
                                    out_channels,
                                    kernel_size,
                                    stride,
                                    padding,
                                    poly_degree,
                                    forced_exact_tile_out);
        if (IsExactTileK3S1Mode(exact_plan.mode)) {
            return exact_plan;
        }
    }

    // full-space
    p.mode = CirLayoutMode::FULL;

    p.route_feature_size = raw_in_feature_size;
    p.capacity_padding = padding;

    p.effective_feature_size = raw_in_feature_size + 2 * padding;
    p.packed_in_channels = in_channels;
    p.packed_kernel_size = kernel_size;
    p.packed_stride = stride;
    p.packed_padding = padding;
    p.phase_count = 1;
    p.suggested_block_size =
        SuggestCirBlockSize(p.packed_in_channels,
                            out_channels,
                            p.effective_feature_size,
                            poly_degree);
    PopulateCirLayoutPlanCost(p, poly_degree, out_channels);
    return p;
}



class Conv2D : public Module {
    public:
        uint64_t in_channels;
        uint64_t out_channels;
        uint64_t in_feature_size;
        uint64_t out_feature_size; 
        uint64_t kernel_size;
        uint64_t stride;
        uint64_t padding;
        Tensor<uint64_t> weight;
        Tensor<HE::unified::UnifiedPlaintext> weight_pt;  // We denote all plaintext(ciphertext) variables with suffix '_pt'('_ct')
        Tensor<uint64_t> bias;
        HE::HEEvaluator* HE;
        bool fused_bn;
        bool compact_stride2_k3_polyphase = false;

        Conv2D(uint64_t in_feature_size, uint64_t stride, uint64_t padding,
            const Tensor<uint64_t>& weight, const Tensor<uint64_t>& bias, HEEvaluator* HE);

        Conv2D(uint64_t in_feature_size, uint64_t in_channels, uint64_t out_channels,
            uint64_t kernel_size, uint64_t stride, HEEvaluator* HE);

        // Metadata-only constructor for composite algorithmic layers that own
        // real child Conv2D operators.  It does not allocate or randomize a
        // dense weight tensor, which avoids wasting memory when a layer is
        // implemented as multiple factorized convolutions.
        Conv2D(uint64_t in_feature_size, uint64_t in_channels, uint64_t out_channels,
            uint64_t kernel_size, uint64_t stride, uint64_t padding, HEEvaluator* HE,
            bool metadata_only);

        virtual ~Conv2D() = default;
    
        virtual Tensor<uint64_t> operator()(Tensor<uint64_t> &x) = 0;

        // DAZGOrbit DomainPulse V15: expose the exact internal conversion
        // boundary used by every Conv2D implementation.  These wrappers do
        // not change arithmetic; they let a ResNet residual block batch two
        // independent SS<->HE conversions into one network message while still
        // reusing the layer's own pack / HE compute / depack implementation.
        virtual Tensor<uint64_t> DAZGOrbitPackActivationForHE(Tensor<uint64_t> &x) {
            return PackActivation(x);
        }

        virtual Tensor<HE::unified::UnifiedCiphertext> DAZGOrbitComputeFromPackedHE(
            Tensor<HE::unified::UnifiedCiphertext> &ac_ct) {
            return HECompute(weight_pt, ac_ct);
        }

        virtual Tensor<uint64_t> DAZGOrbitDepackFromPackedSS(Tensor<uint64_t> &out_msg) {
            return DepackResult(out_msg);
        }

        virtual Tensor<HE::unified::UnifiedCiphertext> DAZGOrbitEvalToHE(Tensor<uint64_t> &x) {
            Tensor<uint64_t> ac_msg = DAZGOrbitPackActivationForHE(x);
            Tensor<HE::unified::UnifiedCiphertext> ac_ct = Operator::SSToHE(ac_msg, HE);
            return DAZGOrbitComputeFromPackedHE(ac_ct);
        }

        virtual Tensor<uint64_t> DAZGOrbitShareFromHE(
            Tensor<HE::unified::UnifiedCiphertext> &out_ct) {
            Tensor<uint64_t> out_msg = Operator::HEToSS(out_ct, HE);
            return DAZGOrbitDepackFromPackedSS(out_msg);
        }

        void fuse_bn(Tensor<uint64_t> *gamma, Tensor<uint64_t> *beta);
    private:
        virtual Tensor<HE::unified::UnifiedPlaintext> PackWeight() = 0;
        virtual Tensor<uint64_t> PackActivation(Tensor<uint64_t> &x) = 0;
        virtual Tensor<HE::unified::UnifiedCiphertext> HECompute(const Tensor<HE::unified::UnifiedPlaintext> &weight_pt, Tensor<HE::unified::UnifiedCiphertext> &ac_ct) = 0;
        virtual Tensor<uint64_t> DepackResult(Tensor<uint64_t> &out) = 0;
};


class Conv2DNest : public Conv2D {
    public:
        uint64_t tiled_in_channels;
        uint64_t tiled_out_channels;
        uint64_t tile_size;
        uint64_t padded_feature_size = 0;
        uint64_t input_rot;
        vector<uint64_t> tmp_w;

        Conv2DNest(uint64_t in_feature_size, uint64_t stride, uint64_t padding, const Tensor<uint64_t>& weight, const Tensor<uint64_t>& bias, HEEvaluator* HE);
        Conv2DNest(uint64_t in_feature_size, uint64_t in_channels, uint64_t out_channels, uint64_t kernel_size, uint64_t stride, HEEvaluator* HE);
        Tensor<uint64_t> operator()(Tensor<uint64_t> &x);

    private:
        Tensor<HE::unified::UnifiedPlaintext> PackWeight();
        Tensor<uint64_t> PackActivation(Tensor<uint64_t> &x);
        Tensor<HE::unified::UnifiedCiphertext> HECompute(const Tensor<HE::unified::UnifiedPlaintext> &weight_pt, Tensor<HE::unified::UnifiedCiphertext> &ac_ct);
        Tensor<uint64_t> DepackResult(Tensor<uint64_t> &out);
        void compute_he_params(uint64_t in_feature_size);
};


class Conv2DCheetah : public Conv2D {
    public:
        bool profile_target = false;
        std::string layer_tag;
        bool fixed_stage1_layout;
        bool prepared;
        std::vector<uint32_t> pack_index_map;
        std::vector<uint32_t> unpack_index_map;

        long long plan_us_last = 0;
        long long pack_weight_us_last = 0;
        long long transfer_us_last = 0;
        long long online_us_last = 0;

        bool plan_ready = false;
        bool packed_weight_ready = false;
        bool device_weight_ready = false;
        bool hot56_fastpath = false;
        bool hot56_tables_ready = false;

        // 预计算索引：PackActivation 用，-1 表示填 0
        std::vector<int> hot56_pack_src_index;

        // 预计算索引：DepackResult 用
        std::vector<int> hot56_depack_src_index;

        // 可复用 scratch，避免每次重新分配大块内存
        std::vector<uint64_t> hot56_padded_buf;
        std::vector<uint64_t> hot56_pack_buf;

        unsigned long N, HW, WW, CW, MW, dM, dC, dH, dW, OW, HOut, WOut, HWprime, WWprime;
        size_t polyModulusDegree = 8192;
        uint64_t plain;

        Conv2DCheetah(uint64_t in_feature_size, uint64_t stride, uint64_t padding, const Tensor<uint64_t>& weight, const Tensor<uint64_t>& bias, HEEvaluator* HE);

        Conv2DCheetah(uint64_t in_feature_size, uint64_t stride, uint64_t padding, const Tensor<uint64_t>& weight, const Tensor<uint64_t>& bias, HEEvaluator* HE,
                            Tensor<uint64_t> *gamma, Tensor<uint64_t> *beta);

        Conv2DCheetah(uint64_t in_feature_size, uint64_t in_channels, uint64_t out_channels, uint64_t kernel_size, uint64_t stride, HEEvaluator* HE);

        Tensor<uint64_t> operator()(Tensor<uint64_t> &x) override;

    private:
        int DivUpper(int a, int b);
        int CalculateCost(int H, int W, int h, int Hw, int Ww, int C, int N);
        void FindOptimalPartition(int H, int W, int h, int C, int N, int* optimal_Hw, int* optimal_Ww);
        Tensor<UnifiedCiphertext> EncryptTensor(Tensor<UnifiedPlaintext> plainTensor);
        Tensor<uint64_t> PackActivation(Tensor<uint64_t> &x) override;
        Tensor<UnifiedPlaintext> PackWeight() override;
        Tensor<UnifiedCiphertext> TensorTOHE(Tensor<uint64_t> PackActivationTensor);
        Tensor<UnifiedCiphertext> HECompute(const Tensor<UnifiedPlaintext> &weight_pt, Tensor<UnifiedCiphertext> &ac_ct) override;
        Tensor<UnifiedCiphertext> sumCP(Tensor<UnifiedCiphertext> cipherTensor, Tensor<UnifiedPlaintext> plainTensor);
        Tensor<uint64_t> DepackResult(Tensor<uint64_t> &out) override;
        Tensor<uint64_t> HETOTensor(Tensor<UnifiedCiphertext> inputCipher);
        void fuse_bn(Tensor<uint64_t> *gamma, Tensor<uint64_t> *beta);
        void PrepareHot56Tables();
        Tensor<uint64_t> PackActivationHot56(Tensor<uint64_t> &x);
        Tensor<uint64_t> DepackResultHot56(Tensor<uint64_t> &out);

        void compute_he_params(uint64_t in_feature_size);
    };


struct CirSparseBSGSEntry {
    uint64_t ti = 0;
    uint64_t tj = 0;
    uint64_t k = 0;
    uint64_t rot_idx = 0;
    uint64_t group_idx = 0;
};

struct CirSparseBSGSState {
    std::vector<CirSparseBSGSEntry> sparse_bsgs_entries;
    std::vector<uint64_t> sparse_max_rot_by_input_tile;
    std::vector<uint64_t> sparse_active_input_tile;
    std::vector<uint64_t> sparse_active_output_tile;
    Tensor<uint64_t> pack_active;
    uint64_t input_rot = 1;
    uint64_t sparse_total_packs = 0;
    uint64_t sparse_active_packs = 0;
    uint64_t sparse_zero_packs = 0;
    uint64_t sparse_rotation_slots = 0;
    uint64_t sparse_bsgs_groups = 0;
};

class CirConv2D : public Conv2D {
    public:
        uint64_t tiled_in_channels = 0;
        uint64_t tiled_out_channels = 0;
        uint64_t tile_size = 1;
        uint64_t padded_feature_size = 0;
        uint64_t input_rot = 1;
        uint64_t block_size = 1;
        uint64_t ntt_size = 0;
        uint64_t num_blocks_in = 0;
        uint64_t num_blocks_out = 0;
        Tensor<uint64_t> weight_valid;
        Tensor<uint64_t> pack_active;

        // Stage-Z2 ExactSparseBSGSLinear: PackWeight creates this exact
        // non-zero packed-weight schedule; HECompute consumes it directly so
        // padded/zero packs never enter the multiply-add hot path.
        std::vector<CirSparseBSGSEntry> sparse_bsgs_entries;
        std::vector<uint64_t> sparse_max_rot_by_input_tile;
        std::vector<uint64_t> sparse_active_input_tile;
        std::vector<uint64_t> sparse_active_output_tile;
        uint64_t sparse_total_packs = 0;
        uint64_t sparse_active_packs = 0;
        uint64_t sparse_zero_packs = 0;
        uint64_t sparse_rotation_slots = 0;
        uint64_t sparse_bsgs_groups = 0;

        // DAZG-ORBIT / PCOI-K3S2: exact stride-2 K3 main branch.  Four
        // spatial phases are packed and evaluated as external HE passes, then
        // accumulated in the HE linear domain before a single HEToSS.
        bool pcoi_k3s2_orbit = false;
        uint64_t pcoi_pack_phase = 0;
        std::vector<Tensor<HE::unified::UnifiedPlaintext>> pcoi_phase_weight_pt;
        std::vector<CirSparseBSGSState> pcoi_phase_sparse_state;

        // ===== 显式区分 compact / exact mode =====
        bool compact_stride2_pointwise = false;
        bool compact_stride2_k3_polyphase = false;
        bool enable_exact_tile4_k3s1 = false;

        // ===== 统一 layout plan（唯一真源）=====
        CirLayoutPlan plan;
        bool has_explicit_plan = false;

        // ===== mirror 字段：全部从 plan 同步 =====
        uint64_t route_feature_size = 0;
        uint64_t effective_feature_size = 0;
        uint64_t packed_in_channels = 0;
        uint64_t packed_kernel_size = 0;
        uint64_t packed_stride = 0;
        uint64_t packed_padding = 0;

        // 兼容旧字段名：现在表示“是否进入 exact tiled k3s1 mode”
        bool exact_tile4_k3s1 = false;

        uint64_t logical_out_feature_size = 0;
        uint64_t packed_out_feature_size = 0;

        uint64_t spatial_tile_rows = 1;
        uint64_t spatial_tile_cols = 1;
        uint64_t tile_out_feature_size = 0;
        uint64_t tile_in_feature_size = 0;
        uint64_t halo_size = 0;

        CirConv2D(uint64_t in_feature_size, uint64_t stride, uint64_t padding,
                uint64_t block_size, const Tensor<uint64_t>& weight,
                const Tensor<uint64_t>& bias, HEEvaluator* HE,
                bool compact_stride2_k3_polyphase = false,
                bool enable_exact_tile4_k3s1 = false,
                const CirLayoutPlan& layout_plan = CirLayoutPlan());

        CirConv2D(uint64_t in_feature_size, uint64_t in_channels,
                uint64_t out_channels, uint64_t kernel_size,
                uint64_t stride, uint64_t block_size, HEEvaluator* HE,
                bool compact_stride2_k3_polyphase = false,
                bool enable_exact_tile4_k3s1 = false,
                const CirLayoutPlan& layout_plan = CirLayoutPlan());

        Tensor<uint64_t> operator()(Tensor<uint64_t> &x);

        Tensor<uint64_t> DAZGOrbitPackActivationForHE(Tensor<uint64_t> &x) override;
        Tensor<HE::unified::UnifiedCiphertext> DAZGOrbitComputeFromPackedHE(
            Tensor<HE::unified::UnifiedCiphertext> &ac_ct) override;
        Tensor<uint64_t> DAZGOrbitDepackFromPackedSS(Tensor<uint64_t> &out_msg) override;

        bool DAZGOrbitEvalPublicLinearOnSSExact(
            const Tensor<uint64_t>& x,
            Tensor<uint64_t>& out,
            uint64_t plain_mod,
            std::string* cert = nullptr); // DAZG_ORBIT_V589_OPERATOR_BRLE_20260615

    private:
        void BuildLayoutPlan();
        void SyncPackedParamsFromPlan();
        bool IsCompactStride2Pointwise() const;
        bool IsCompactStride2K3Polyphase() const;
        bool IsExactCompactStride2Pointwise() const;
        bool IsExactCompactStride2K3Polyphase() const;
        bool IsPCOIK3S2Orbit() const;

        uint64_t GetPCOIPhaseWeightElem(uint64_t out_ch,
                                        uint64_t in_ch,
                                        uint64_t r,
                                        uint64_t s) const;
        uint64_t GetPackedWeightElem(uint64_t out_ch,
                                     uint64_t packed_in_ch,
                                     uint64_t r,
                                     uint64_t s) const;

        Tensor<HE::unified::UnifiedPlaintext> PackWeight();
        CirSparseBSGSState CaptureSparseState() const;
        void RestoreSparseState(const CirSparseBSGSState& st);
        void PreparePCOIPhaseWeights();
        Tensor<uint64_t> PackActivation(Tensor<uint64_t> &x);
        Tensor<HE::unified::UnifiedCiphertext> HECompute(const Tensor<HE::unified::UnifiedPlaintext> &weight_pt, Tensor<HE::unified::UnifiedCiphertext> &ac_ct);
        void HEComputeSparseRows(
            const Tensor<HE::unified::UnifiedPlaintext> &weight_pt,
            Tensor<HE::unified::UnifiedCiphertext> &ac_ct,
            uint64_t ac_row_offset,
            Tensor<HE::unified::UnifiedCiphertext> &out_ct,
            uint64_t out_row_offset
        );
        void FillMissingOutputWithZero(
            Tensor<HE::unified::UnifiedCiphertext> &out_ct,
            uint64_t out_row_offset,
            const std::vector<uint64_t>& has_output
        );
        Tensor<uint64_t> DepackResult(Tensor<uint64_t> &out);

        Tensor<uint64_t> PackActivationSingleMapImpl(
            Tensor<uint64_t> &x_map,
            uint64_t local_in_feature_size,
            uint64_t local_out_feature_size,
            uint64_t local_padding,
            bool local_compact_stride2_pointwise,
            bool local_compact_stride2_k3_polyphase
        );

        Tensor<uint64_t> DepackSingleMapImpl(
            Tensor<uint64_t> &out_msg,
            uint64_t row_offset_tj,
            uint64_t local_out_feature_size,
            uint64_t local_read_offset,
            uint64_t local_row_step,
            uint64_t local_col_step
        );

        // exact compact stride-2 专用：先抽出 packed worker 的局部输入，再走普通 packed worker NTT。
        Tensor<uint64_t> ExtractCompactTileWorkerMap(
            const Tensor<uint64_t>& x,
            uint64_t tile_r,
            uint64_t tile_c
        ) const;

        Tensor<uint64_t> PackActivationPackedWorkerMapImpl(
            Tensor<uint64_t> &x_worker
        );

        Tensor<uint64_t> ExtractPCOIPhaseTileWorkerMap(
            const Tensor<uint64_t>& x,
            uint64_t tile_r,
            uint64_t tile_c,
            uint64_t phase
        ) const;

        // exact tiled k3s1 专用：提取带 halo 的局部 patch，并把单 tile 输出拼回逻辑输出平面。
        Tensor<uint64_t> ExtractTileWithHalo(
            const Tensor<uint64_t>& x,
            uint64_t tile_r,
            uint64_t tile_c
        ) const;

        void ScatterTileNoOverlap(
            const Tensor<uint64_t>& tile_y,
            uint64_t tile_r,
            uint64_t tile_c,
            Tensor<uint64_t>& y
        ) const;

        void compute_he_params(uint64_t in_feature_size);
};



class Conv2DStage1Static : public Conv2D {
public:
    unsigned long N, HW, WW, CW, MW, dM, dC, dH, dW, OW, HOut, WOut, HWprime, WWprime;
    size_t polyModulusDegree = 8192;
    uint64_t plain;

    Conv2DStage1Static(uint64_t in_feature_size,
                       uint64_t stride,
                       uint64_t padding,
                       const Tensor<uint64_t>& weight,
                       const Tensor<uint64_t>& bias,
                       HE::HEEvaluator* HE);

    Conv2DStage1Static(uint64_t in_feature_size,
                       uint64_t stride,
                       uint64_t padding,
                       const Tensor<uint64_t>& weight,
                       const Tensor<uint64_t>& bias,
                       HE::HEEvaluator* HE,
                       Tensor<uint64_t>* gamma,
                       Tensor<uint64_t>* beta);

    Conv2DStage1Static(uint64_t in_feature_size,
                       uint64_t in_channels,
                       uint64_t out_channels,
                       uint64_t kernel_size,
                       uint64_t stride,
                       HE::HEEvaluator* HE);

    Tensor<uint64_t> operator()(Tensor<uint64_t> &x) override;

private:
    int DivUpper(int a, int b);
    int CalculateCost(int H, int W, int h, int Hw, int Ww, int C, int N);
    void FindOptimalPartition(int H, int W, int h, int C, int N, int* optimalHw, int* optimalWw);

    Tensor<UnifiedCiphertext> EncryptTensor(Tensor<UnifiedPlaintext> plainTensor);

    // 旧两段式路径先保留，便于对照/回退；operator() 不再调用它
    Tensor<uint64_t> PackActivation(Tensor<uint64_t> &x) override;
    Tensor<UnifiedCiphertext> TensorTOHE(Tensor<uint64_t> PackActivationTensor);

    // 新增：direct path
    void FillActivationPlaintexts(Tensor<uint64_t> &x, Tensor<UnifiedPlaintext> &T);
    Tensor<UnifiedCiphertext> PackActivationDirectToHE(Tensor<uint64_t> &x);

    Tensor<UnifiedPlaintext> PackWeight() override;
    Tensor<UnifiedCiphertext> HECompute(const Tensor<UnifiedPlaintext> &weight_pt,
                                        Tensor<UnifiedCiphertext> &ac_ct) override;
    Tensor<UnifiedCiphertext> sumCP(Tensor<UnifiedCiphertext> cipherTensor,
                                    Tensor<UnifiedPlaintext> plainTensor);
    Tensor<uint64_t> DepackResult(Tensor<uint64_t> &out) override;
    Tensor<uint64_t> HETOTensor(Tensor<UnifiedCiphertext> inputCipher);
    void fuse_bn(Tensor<uint64_t> *gamma, Tensor<uint64_t> *beta);
    void compute_he_params(uint64_t raw_in_feature_size);
    void PreparePlan();

    bool plan_ready = false;

    // 新增：把 PreparePlan() 里的局部信息变成类成员，给 direct path 用
    size_t pack_len_ = 0;
    size_t num_poly_ = 0;

    std::vector<int> pack_src_index_;
    std::vector<int> depack_src_index_;

    // 新增：复用 scratch，避免每个多项式都重新分配临时 vector
    std::vector<uint64_t> coeff_scratch_;

};

class Conv2DStage2Static : public Conv2D {
public:
    unsigned long N, HW, WW, CW, MW, dM, dC, dH, dW, OW, HOut, WOut, HWprime, WWprime;
    size_t polyModulusDegree = 8192;
    uint64_t plain;

    Conv2DStage2Static(uint64_t in_feature_size,
                       uint64_t stride,
                       uint64_t padding,
                       const Tensor<uint64_t>& weight,
                       const Tensor<uint64_t>& bias,
                       HE::HEEvaluator* HE);

    Conv2DStage2Static(uint64_t in_feature_size,
                       uint64_t stride,
                       uint64_t padding,
                       const Tensor<uint64_t>& weight,
                       const Tensor<uint64_t>& bias,
                       HE::HEEvaluator* HE,
                       Tensor<uint64_t>* gamma,
                       Tensor<uint64_t>* beta);

    Conv2DStage2Static(uint64_t in_feature_size,
                       uint64_t in_channels,
                       uint64_t out_channels,
                       uint64_t kernel_size,
                       uint64_t stride,
                       HE::HEEvaluator* HE);

    Tensor<uint64_t> operator()(Tensor<uint64_t> &x) override;

private:
    int DivUpper(int a, int b);
    int CalculateCost(int H, int W, int h, int Hw, int Ww, int C, int N);
    void FindOptimalPartition(int H, int W, int h, int C, int N, int* optimalHw, int* optimalWw);

    Tensor<UnifiedCiphertext> EncryptTensor(Tensor<UnifiedPlaintext> plainTensor);

    // 旧两段式路径先保留，便于对照/回退；operator() 不再调用它
    Tensor<uint64_t> PackActivation(Tensor<uint64_t> &x) override;
    Tensor<UnifiedCiphertext> TensorTOHE(Tensor<uint64_t> PackActivationTensor);

    // 新增：direct path
    void FillActivationPlaintexts(Tensor<uint64_t> &x, Tensor<UnifiedPlaintext> &T);
    Tensor<UnifiedCiphertext> PackActivationDirectToHE(Tensor<uint64_t> &x);

    Tensor<UnifiedPlaintext> PackWeight() override;
    Tensor<UnifiedCiphertext> HECompute(const Tensor<UnifiedPlaintext> &weight_pt,
                                        Tensor<UnifiedCiphertext> &ac_ct) override;
    Tensor<UnifiedCiphertext> sumCP(Tensor<UnifiedCiphertext> cipherTensor,
                                    Tensor<UnifiedPlaintext> plainTensor);
    Tensor<uint64_t> DepackResult(Tensor<uint64_t> &out) override;
    Tensor<uint64_t> HETOTensor(Tensor<UnifiedCiphertext> inputCipher);
    void fuse_bn(Tensor<uint64_t> *gamma, Tensor<uint64_t> *beta);
    void compute_he_params(uint64_t raw_in_feature_size);
    void PreparePlan();

    bool plan_ready = false;

    // 新增：把 PreparePlan() 里的局部信息变成类成员，给 direct path 用
    size_t pack_len_ = 0;
    size_t num_poly_ = 0;

    std::vector<int> pack_src_index_;
    std::vector<int> depack_src_index_;

    // 新增：复用 scratch，避免每个多项式都重新分配临时 vector
    std::vector<uint64_t> coeff_scratch_;

};


}
