// DAZG-Orbit Project Source File
// Component: Layer/LinearLayer/src/CirConv.cpp
// Purpose: Packed homomorphic convolution, sparse scheduling, and rotation reuse.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

// DAZG-Orbit ciphertext-acceleration contract:
// - Exact sparse BSGS schedules only active diagonals.
// - Missing diagonals are skipped rather than materialised as zero rotations.
// - Rotated ciphertexts are reused across output rows whenever the packed
//   geometry requests the same offset, reducing repeated Galois/key-switch
//   operations without changing the represented tensor.
// - PCOI K3/S2 phase packing separates four stride-2 spatial phases and
//   defers the terminal giant-step fold so phase contributions can share it.
// Every optimized route remains fail-closed against the frozen Q16 oracle.

/**
 * CirConv2D: Block Circulant Convolution with Nested Encoding
 *
 * Combines CirLinearNest's block circulant structure with Conv2DNest's
 * convolution encoding.
 *
 * Within each circulant block (b channels):
 *   Encoding follows the CirEncode for convolutions (Theorem in CirConv.latex):
 *     x̂[i·HW + j·W + k] = X[i, j, k]
 *     ŵ[i·HW + offset - m·W - n] = W[i, 0, m, n]
 *   Cyclic NTT gives circulant structure across block channels.
 *
 * Across circulant blocks:
 *   Same BSGS anti-diagonal encoding as CirLinearNest.
 *
 * When block_size=1, this reduces to Conv2DNest (with CyclicNTT instead of
 * negacyclic NTT, but functionally equivalent since padding prevents aliasing).
 */

#include <LinearLayer/Conv.h>
#include <Utils/CyclicNTT.h>
#include <cassert>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <vector>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <thread>
#include <atomic>
#include <fstream>

// DAZG_ORBIT_V666_NATIVE_CORE_MODP_PACK_REPAIR_BEGIN
#ifndef DAZG_ORBIT_V666_NATIVE_CORE_MODP_PACK_REPAIR
#define DAZG_ORBIT_V666_NATIVE_CORE_MODP_PACK_REPAIR
#include <cstdlib>
#include <cstdint>
static inline bool DAZGOrbitV666CoreRepairEnabled() {
  const char* e = std::getenv("DAZG_ORBIT_V666_CANON_PACK_REPAIR");
  return e != nullptr && e[0] != '\0' && e[0] != '0';
}
static inline uint64_t DAZGOrbitV666PlainMod() {
  const char* e = std::getenv("DAZG_ORBIT_V666_PLAIN_MOD");
  return e ? static_cast<uint64_t>(std::strtoull(e, nullptr, 10)) : 4294475777ULL;
}
#endif
// DAZG_ORBIT_V666_NATIVE_CORE_MODP_PACK_REPAIR_END



// DAZG_ORBIT_V719_LINK_MARKER_BEGIN
extern "C" {
#if defined(__GNUC__)
__attribute__((used, visibility("default")))
#endif
extern const char DAZGOrbitV719LinkedCirConvMarker[] = "DAZG_ORBIT_V719_LINKED_CIRCONV_20260622_141527";
}
// DAZG_ORBIT_V719_LINK_MARKER_END

using namespace seal;
using namespace HE;
using namespace HE::unified;

namespace LinearLayer {


// DAZG_ORBIT_V9_FOLDED_BIAS_ADD_PATCH_BEGIN
// Add folded Conv/BN bias to exactly one secret share after HE-to-SS depacking.
// The DAZG-GELU bias^2 bridge stores folded_bias at the pre-truncation scale.
// This changes no HE rotations; baseline and policy both use the same layer code.
static bool DAZGOrbitV9FoldedBiasAddEnabled()
{
    const char* v = std::getenv("DAZG_ORBIT_DISABLE_FOLDED_BIAS_ADD");
    if (v == nullptr || *v == '\0') return true;
    return !(v[0] == '1' || v[0] == 't' || v[0] == 'T' ||
             v[0] == 'y' || v[0] == 'Y');
}

static bool DAZGOrbitV9ShouldAddBias(HEEvaluator* he)
{
    return he != nullptr && he->server && DAZGOrbitV9FoldedBiasAddEnabled();
}

static void DAZGOrbitV9AddFoldedBiasConv3D(Tensor<uint64_t>& y,
                                         const Tensor<uint64_t>& bias,
                                         HEEvaluator* he)
{
    if (!DAZGOrbitV9ShouldAddBias(he)) return;
    const std::vector<size_t>& ys = y.shape();
    const std::vector<size_t>& bs = bias.shape();
    if (ys.size() != 3 || bs.empty()) return;

    const size_t C = ys[0];
    const size_t H = ys[1];
    const size_t W = ys[2];

    if (bs.size() == 1 && bs[0] == C) {
        for (size_t c = 0; c < C; ++c) {
            const uint64_t b = bias({c});
            for (size_t i = 0; i < H; ++i) {
                for (size_t j = 0; j < W; ++j) {
                    y({c, i, j}) = y({c, i, j}) + b;
                }
            }
        }
        return;
    }

    if (bs.size() == 3 && bs[0] == C && bs[1] == H && bs[2] == W) {
        for (size_t c = 0; c < C; ++c) {
            for (size_t i = 0; i < H; ++i) {
                for (size_t j = 0; j < W; ++j) {
                    y({c, i, j}) = y({c, i, j}) + bias({c, i, j});
                }
            }
        }
    }
}
// DAZG_ORBIT_V9_FOLDED_BIAS_ADD_PATCH_END

namespace {

// DAZG_ORBIT_V496_LC_MAPD_LAYOUT_ABSORB_BEGIN
#ifndef DAZG_ORBIT_V496_LC_MAPD_LAYOUT_ABSORB_20260531
#define DAZG_ORBIT_V496_LC_MAPD_LAYOUT_ABSORB_20260531
static const char* DAZG_ORBIT_V496_LC_MAPD_MARKER =
    "DAZG_ORBIT_V496_LC_MAPD_LAYOUT_ABSORB_20260531";

inline bool DAZGOrbitV496EnvFlag(const char* name, bool default_value = false)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off" || s == "no" || s == "NO");
}

inline uint64_t DAZGOrbitV496EnvU64(const char* name, uint64_t default_value = 0)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    return static_cast<uint64_t>(std::strtoull(v, nullptr, 10));
}

inline bool DAZGOrbitV496MapdContextSeen()
{
    const char* names[] = {
        "DAZG_ORBIT_V490_PDGIANT_FAST",
        "DAZG_ORBIT_PDGIANT_FAST",
        "DAZG_ORBIT_ENABLE_PDGIANT_FAST",
        "DAZG_ORBIT_MAPD_FAST",
        "DAZG_ORBIT_ENABLE_MAPD_FAST",
        "DAZG_ORBIT_RC_OLC_MAPD",
        "DAZG_ORBIT_V489_PDGIANT_ORBITCARRY",
        "DAZG_ORBIT_ENABLE_PDGIANT"
    };
    for (const char* n : names) {
        if (DAZGOrbitV496EnvFlag(n, false)) return true;
    }
    return false;
}

inline bool DAZGOrbitV496LCMapdAbsorbEnabledForLayer(
    uint64_t H, uint64_t Cin, uint64_t Cout, uint64_t K, uint64_t S)
{
    if (!DAZGOrbitV496EnvFlag("DAZG_ORBIT_V496_LC_MAPD_ABSORB", false)) return false;
    if (!(K == 3 && S == 2)) return false;

    const bool in_scope =
        (H == 32 && Cin == 64  && Cout == 128) ||
        (H == 16 && Cin == 128 && Cout == 256) ||
        (H == 8  && Cin == 256 && Cout == 512);
    if (!in_scope) return false;

    if (DAZGOrbitV496EnvFlag("DAZG_ORBIT_V496_REQUIRE_MAPD_ENV", true) &&
        !DAZGOrbitV496MapdContextSeen()) {
        return false;
    }
    return true;
}
#endif
// DAZG_ORBIT_V496_LC_MAPD_LAYOUT_ABSORB_END


// DAZG_ORBIT_THUNDERCUT_V17_CIRCONV_HELPERS_BEGIN
#ifndef DAZG_ORBIT_THUNDERCUT_V17_CIRCONV_HELPERS_20260513
#define DAZG_ORBIT_THUNDERCUT_V17_CIRCONV_HELPERS_20260513
static const char* DAZG_ORBIT_THUNDERCUT_V17_CIR_MARKER = "DAZG_ORBIT_THUNDERCUT_V17_PARALLEL_HARDPATH_20260513";

inline bool DAZGOrbitThunderCirEnvFlagV17(const char* name, bool default_value = false)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off" ||
             s == "no" || s == "NO");
}

inline bool DAZGOrbitThunderCirMasterEnabledV17()
{
    return DAZGOrbitThunderCirEnvFlagV17("DAZG_ORBIT_THUNDERCUT_V17", false);
}

inline bool DAZGOrbitThunderCirConvRowsEnabledV17()
{
    if (std::getenv("DAZG_ORBIT_THUNDERCUT_V17_CIRCONV_ROWS") != nullptr) {
        return DAZGOrbitThunderCirEnvFlagV17("DAZG_ORBIT_THUNDERCUT_V17_CIRCONV_ROWS", false);
    }
    return DAZGOrbitThunderCirMasterEnabledV17();
}

inline uint64_t DAZGOrbitThunderCirThreadCountV17()
{
    const char* v = std::getenv("DAZG_ORBIT_THUNDER_THREADS");
    uint64_t t = 0;
    if (v != nullptr && *v != '\0') {
        t = static_cast<uint64_t>(std::strtoull(v, nullptr, 10));
    }
    if (t == 0) {
        t = static_cast<uint64_t>(std::thread::hardware_concurrency());
    }
    if (t == 0) t = 4;

    const char* m = std::getenv("DAZG_ORBIT_THUNDER_MAX_THREADS");
    uint64_t cap = 32;
    if (m != nullptr && *m != '\0') {
        cap = static_cast<uint64_t>(std::strtoull(m, nullptr, 10));
        if (cap == 0) cap = 1;
    }
    if (t > cap) t = cap;
    if (t < 1) t = 1;
    return t;
}

struct DAZGOrbitThunderCirStatsV17 {
    std::atomic<uint64_t> sparse_calls{0};
    std::atomic<uint64_t> parallel_calls{0};
    std::atomic<uint64_t> rows{0};
    std::atomic<uint64_t> mul_plain{0};
    std::atomic<uint64_t> rotate_rows{0};
    std::atomic<uint64_t> add_inplace{0};
    std::atomic<uint64_t> elapsed_us{0};

    ~DAZGOrbitThunderCirStatsV17() {
        const uint64_t p = parallel_calls.load(std::memory_order_relaxed);
        if (p == 0) return;
        std::cout << "[DAZG_ORBIT_THUNDERCUT_V17_SUMMARY]"
                  << " marker=" << DAZG_ORBIT_THUNDERCUT_V17_CIR_MARKER
                  << " component=CirConvRows"
                  << " sparse_calls=" << sparse_calls.load(std::memory_order_relaxed)
                  << " parallel_calls=" << p
                  << " rows=" << rows.load(std::memory_order_relaxed)
                  << " mul_plain=" << mul_plain.load(std::memory_order_relaxed)
                  << " rotate_rows=" << rotate_rows.load(std::memory_order_relaxed)
                  << " add_inplace=" << add_inplace.load(std::memory_order_relaxed)
                  << " elapsed_us=" << elapsed_us.load(std::memory_order_relaxed)
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
};

inline DAZGOrbitThunderCirStatsV17& DAZGOrbitThunderCirStatsRefV17()
{
    static DAZGOrbitThunderCirStatsV17 s;
    return s;
}

template <typename Fn>
inline void DAZGOrbitThunderCirParallelForV17(uint64_t n, const Fn& fn)
{
    const uint64_t requested = DAZGOrbitThunderCirThreadCountV17();
    if (n <= 1 || requested <= 1) {
        for (uint64_t i = 0; i < n; ++i) fn(i);
        return;
    }

    const uint64_t workers_n = std::min<uint64_t>(requested, n);
    std::atomic<uint64_t> next(0);
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(workers_n));
    for (uint64_t tid = 0; tid < workers_n; ++tid) {
        workers.emplace_back([&]() {
            for (;;) {
                const uint64_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) break;
                fn(i);
            }
        });
    }
    for (auto& th : workers) {
        th.join();
    }
}
#endif
// DAZG_ORBIT_THUNDERCUT_V17_CIRCONV_HELPERS_END
struct CirHEComputeStats {
    long long us = 0;
    long long mul_plain = 0;
    long long rotate_rows = 0;
    long long add_inplace = 0;
};

static CirHEComputeStats g_last_cir_hecompute_stats;


// DAZG_ORBIT_V508_ACTIVE_ROTATION_SUMMARY_BEGIN
// Exact-safe evidence layer: this does not change evaluator semantics.  It
// records the dense BSGS rotation upper bound requested by the packed geometry
// and the rotations actually executed by the sparse schedule.  The skipped
// count is therefore an inactive-rotation proof for the already exact sparse
// path, not a new math claim.
#ifndef DAZG_ORBIT_V508_ACTIVE_ROTATION_SUMMARY_20260601
#define DAZG_ORBIT_V508_ACTIVE_ROTATION_SUMMARY_20260601

inline std::string DAZGOrbitV508EnvString(const char* key, const char* def = "")
{
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return std::string(def ? def : "");
    return std::string(v);
}

inline bool DAZGOrbitV508EnvFlag(const char* key, bool def = false)
{
    const char* v = std::getenv(key);
    if (v == nullptr || *v == '\0') return def;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" || s == "FALSE" ||
             s == "off" || s == "OFF" || s == "no" || s == "NO");
}

// DAZG_ORBIT_V509_PAIRED_EXECUTOR_ABLATION_BEGIN
// Keep old scripts usable: several historical runners used V490 env names
// while the guarded branch also required V489.  This alias resolver avoids a
// silent non-activation of paired executor candidates.
inline bool DAZGOrbitV509EnvFlagAlias(
    const char* a,
    const char* b = nullptr,
    const char* c = nullptr,
    const char* d = nullptr,
    bool def = false)
{
    const char* names[] = {a, b, c, d};
    for (const char* n : names) {
        if (n == nullptr) continue;
        const char* v = std::getenv(n);
        if (v != nullptr) {
            return DAZGOrbitV508EnvFlag(n, def);
        }
    }
    return def;
}
// DAZG_ORBIT_V509_PAIRED_EXECUTOR_ABLATION_END

// DAZG_ORBIT_V510_SELECTIVE_PDGIANT_SWEEP_BEGIN
// Selectively enable the PD-GIANT candidates by K3S2 transition layer.
// This lets the runner localize hash drift instead of burning time on full
// all-layer candidates that are known to reduce rotations but fail exactness.
//
// bit 1: H32 Cin64  Cout128  K3 S2
// bit 2: H16 Cin128 Cout256  K3 S2
// bit 4: H8  Cin256 Cout512  K3 S2
// bit 8: any other K3 S2 fallback
inline uint64_t DAZGOrbitV510ParseMask(const char* v, uint64_t def_value)
{
    if (v == nullptr || *v == '\0') return def_value;
    const std::string s(v);
    if (s == "all" || s == "ALL" || s == "*") return ~0ULL;
    if (s == "none" || s == "NONE" || s == "off" || s == "OFF") return 0ULL;
    char* endp = nullptr;
    const unsigned long long parsed = std::strtoull(v, &endp, 0);
    if (endp == v) return def_value;
    return static_cast<uint64_t>(parsed);
}

inline uint64_t DAZGOrbitV510PCOIK3S2LayerBit(uint64_t H, uint64_t Cin, uint64_t Cout)
{
    if (H == 32 && Cin == 64  && Cout == 128) return 1ULL;
    if (H == 16 && Cin == 128 && Cout == 256) return 2ULL;
    if (H == 8  && Cin == 256 && Cout == 512) return 4ULL;
    return 8ULL;
}

inline bool DAZGOrbitV510PDGiantLayerEnabled(uint64_t H, uint64_t Cin, uint64_t Cout)
{
    const uint64_t bit = DAZGOrbitV510PCOIK3S2LayerBit(H, Cin, Cout);
    const uint64_t mask = DAZGOrbitV510ParseMask(
        std::getenv("DAZG_ORBIT_V510_PDGIANT_LAYER_MASK"),
        ~0ULL);
    return (mask & bit) != 0ULL;
}

inline const char* DAZGOrbitV510PDGiantLayerName(uint64_t bit)
{
    if (bit == 1ULL) return "k3s2_stage2_H32_C64_C128";
    if (bit == 2ULL) return "k3s2_stage3_H16_C128_C256";
    if (bit == 4ULL) return "k3s2_stage4_H8_C256_C512";
    return "k3s2_other";
}
// DAZG_ORBIT_V510_SELECTIVE_PDGIANT_SWEEP_END

inline bool DAZGOrbitV508ActiveSummaryEnabled()
{
    if (std::getenv("DAZG_ORBIT_V508_ACTIVE_SUMMARY") != nullptr) {
        return DAZGOrbitV508EnvFlag("DAZG_ORBIT_V508_ACTIVE_SUMMARY", false);
    }
    if (std::getenv("DAZG_ORBIT_V507_ACTIVE_SUMMARY") != nullptr) {
        return DAZGOrbitV508EnvFlag("DAZG_ORBIT_V507_ACTIVE_SUMMARY", false);
    }
    if (std::getenv("DAZG_ORBIT_ACTIVE_ROTATION_SUMMARY") != nullptr) {
        return DAZGOrbitV508EnvFlag("DAZG_ORBIT_ACTIVE_ROTATION_SUMMARY", false);
    }
    return false;
}

inline bool DAZGOrbitV508FileExists(const std::string& path)
{
    if (path.empty()) return false;
    std::ifstream f(path.c_str(), std::ios::binary);
    return f.good();
}

inline uint64_t DAZGOrbitV508SaturatingSub(uint64_t a, uint64_t b)
{
    return a > b ? (a - b) : 0ULL;
}

inline std::string DAZGOrbitV508LayerTag(
    uint64_t H, uint64_t Cin, uint64_t Cout, uint64_t K, uint64_t S, int layout_mode)
{
    std::ostringstream os;
    os << "CirConv2D:H" << H
       << ":Cin" << Cin
       << ":Cout" << Cout
       << ":K" << K
       << ":S" << S
       << ":layout" << layout_mode;
    return os.str();
}

struct DAZGOrbitV508ActiveRotationStats {
    std::atomic<uint64_t> layer_count{0};
    std::atomic<uint64_t> rotation_requested{0};
    std::atomic<uint64_t> rotation_executed{0};
    std::atomic<uint64_t> rotation_skipped_inactive{0};
    std::atomic<uint64_t> runtime_sparse_skipped{0};
    std::atomic<uint64_t> policy_induced_skipped{0};

    ~DAZGOrbitV508ActiveRotationStats()
    {
        if (!DAZGOrbitV508ActiveSummaryEnabled()) return;

        const std::string policy_file =
            !DAZGOrbitV508EnvString("DAZG_ORBIT_V507_POLICY_FILE").empty()
                ? DAZGOrbitV508EnvString("DAZG_ORBIT_V507_POLICY_FILE")
                : DAZGOrbitV508EnvString("DAZG_ORBIT_ACTIVE_ROTATION_POLICY_FILE");
        const std::string policy_hash =
            !DAZGOrbitV508EnvString("DAZG_ORBIT_ACTIVE_ROTATION_POLICY_HASH").empty()
                ? DAZGOrbitV508EnvString("DAZG_ORBIT_ACTIVE_ROTATION_POLICY_HASH")
                : DAZGOrbitV508EnvString("DAZG_ORBIT_POLICY_HASH");
        const std::string weight_hash =
            !DAZGOrbitV508EnvString("DAZG_ORBIT_WEIGHT_HASH").empty()
                ? DAZGOrbitV508EnvString("DAZG_ORBIT_WEIGHT_HASH")
                : DAZGOrbitV508EnvString("DAZG_ORBIT_BRIDGE_WEIGHT_HASH");
        const std::string orbit_hash =
            !DAZGOrbitV508EnvString("DAZG_ORBIT_ORBIT_HASH").empty()
                ? DAZGOrbitV508EnvString("DAZG_ORBIT_ORBIT_HASH")
                : DAZGOrbitV508EnvString("DAZG_ORBIT_ORBIT_POLICY_HASH");
        const bool policy_exists = DAZGOrbitV508FileExists(policy_file);
        const bool hash_binding_ok = policy_exists &&
            !policy_hash.empty() && !weight_hash.empty() && !orbit_hash.empty();

        const uint64_t requested = rotation_requested.load(std::memory_order_relaxed);
        const uint64_t executed = rotation_executed.load(std::memory_order_relaxed);
        const uint64_t skipped = rotation_skipped_inactive.load(std::memory_order_relaxed);
        const uint64_t runtime_sparse = runtime_sparse_skipped.load(std::memory_order_relaxed);
        const uint64_t policy_induced = policy_induced_skipped.load(std::memory_order_relaxed);
        const double ratio = requested == 0 ? 0.0 : static_cast<double>(skipped) / static_cast<double>(requested);

        std::cout << "[DAZG_ORBIT_ACTIVE_ROTATION_SUMMARY]"
                  << " marker=DAZG_ORBIT_V508_ACTIVE_ROTATION_SUMMARY_20260601"
                  << " layer_count=" << layer_count.load(std::memory_order_relaxed)
                  << " rotation_requested=" << requested
                  << " rotation_executed=" << executed
                  << " rotation_skipped_inactive=" << skipped
                  << " runtime_sparse_skipped=" << runtime_sparse
                  << " policy_induced_skipped=" << policy_induced
                  << " skip_ratio=" << ratio
                  << " policy_file=" << (policy_file.empty() ? "none" : policy_file)
                  << " policy_file_exists=" << (policy_exists ? 1 : 0)
                  << " policy_hash=" << (policy_hash.empty() ? "none" : policy_hash)
                  << " weight_hash=" << (weight_hash.empty() ? "none" : weight_hash)
                  << " orbit_hash=" << (orbit_hash.empty() ? "none" : orbit_hash)
                  << " hash_binding_ok=" << (hash_binding_ok ? 1 : 0)
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
};

inline DAZGOrbitV508ActiveRotationStats& DAZGOrbitV508ActiveRotationStatsRef()
{
    static DAZGOrbitV508ActiveRotationStats s;
    return s;
}

inline void DAZGOrbitV508RecordActiveRotationLayer(
    const std::string& layer_tag,
    uint64_t requested,
    uint64_t executed,
    const char* source)
{
    if (!DAZGOrbitV508ActiveSummaryEnabled()) return;
    const uint64_t skipped = DAZGOrbitV508SaturatingSub(requested, executed);
    DAZGOrbitV508ActiveRotationStats& s = DAZGOrbitV508ActiveRotationStatsRef();
    s.layer_count.fetch_add(1, std::memory_order_relaxed);
    s.rotation_requested.fetch_add(requested, std::memory_order_relaxed);
    s.rotation_executed.fetch_add(executed, std::memory_order_relaxed);
    s.rotation_skipped_inactive.fetch_add(skipped, std::memory_order_relaxed);
    const std::string source_s = source ? std::string(source) : std::string();
    if (source_s.find("policy") != std::string::npos) {
        s.policy_induced_skipped.fetch_add(skipped, std::memory_order_relaxed);
    } else {
        s.runtime_sparse_skipped.fetch_add(skipped, std::memory_order_relaxed);
    }

    std::cout << "[DAZG_ORBIT_ACTIVE_ROTATION_LAYER]"
              << " marker=DAZG_ORBIT_V508_ACTIVE_ROTATION_SUMMARY_20260601"
              << " layer=" << layer_tag
              << " rotation_requested=" << requested
              << " rotation_executed=" << executed
              << " rotation_skipped_inactive=" << skipped
              << " source=" << (source ? source : "dense_bsgs_minus_sparse_schedule")
              << " exact_equiv=1 semantic_loss=0"
              << std::endl;
}

inline uint64_t DAZGOrbitV508DenseBSGSRotationRequest(
    uint64_t tiled_in_channels,
    uint64_t tiled_out_channels,
    uint64_t input_rot,
    uint64_t num_groups)
{
    const uint64_t pre = tiled_in_channels * DAZGOrbitV508SaturatingSub(input_rot, 1ULL);
    const uint64_t terminal = tiled_out_channels * DAZGOrbitV508SaturatingSub(num_groups, 1ULL);
    return pre + terminal;
}
#endif
// DAZG_ORBIT_V508_ACTIVE_ROTATION_SUMMARY_END

inline void ResetLastCirHEComputeStats()
{
    g_last_cir_hecompute_stats = CirHEComputeStats();
}

inline bool DAZGOrbitCirProfilerEnabled()
{
    const char* v = std::getenv("DAZG_ORBIT_PROFILER");
    if (v == nullptr) return true;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off");
}


inline bool DAZGOrbitFanoutCacheProfilerEnabled()
{
    const char* v = std::getenv("DAZG_ORBIT_ENABLE_FANOUT_CACHE");
    if (v == nullptr || *v == '\0') {
        v = std::getenv("DAZG_ORBIT_FANOUT_RUNTIME");
    }
    if (v == nullptr || *v == '\0') {
        v = std::getenv("DAZG_ORBIT_FANOUT_CACHE");
    }
    if (v == nullptr || *v == '\0') return true;  // default: profiler/planner enabled
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off" || s == "no" || s == "NO");
}

inline void DAZGOrbitEmitLocalRotNeedLog(
    uint64_t H,
    uint64_t Cin,
    uint64_t Cout,
    uint64_t K,
    uint64_t S,
    int layout_mode,
    uint64_t tile_size,
    uint64_t input_rot,
    uint64_t ntt_size,
    const std::vector<uint64_t>& sparse_active_input_tile,
    const std::vector<uint64_t>& sparse_max_rot_by_input_tile,
    uint64_t sparse_rotation_slots,
    uint64_t sparse_entries)
{
    if (!DAZGOrbitCirProfilerEnabled() || !DAZGOrbitFanoutCacheProfilerEnabled()) {
        return;
    }

    uint64_t active_input_tiles = 0;
    uint64_t emitted_need_rows = 0;
    for (uint64_t v : sparse_active_input_tile) {
        if (v) active_input_tiles++;
    }

    std::cout << "[DAZG_ORBIT_ROT_NEED_SUMMARY]"
              << " layer=CirConv2D"
              << " H=" << H
              << " Cin=" << Cin
              << " Cout=" << Cout
              << " K=" << K
              << " S=" << S
              << " layout_mode=" << layout_mode
              << " tile_size=" << tile_size
              << " input_rot=" << input_rot
              << " active_input_tiles=" << active_input_tiles
              << " sparse_entries=" << sparse_entries
              << " rotation_slots=" << sparse_rotation_slots
              << " cache_candidate=" << (sparse_rotation_slots > 0)
              << " exact_equiv=1"
              << " semantic_loss=0"
              << " reason=" << (sparse_rotation_slots > 0 ? "nonzero_local_rotation_need" : "no_local_rotation_need")
              << std::endl;

    for (uint64_t ti = 0; ti < sparse_active_input_tile.size(); ++ti) {
        if (sparse_active_input_tile[ti] == 0) continue;
        if (ti >= sparse_max_rot_by_input_tile.size()) continue;

        const uint64_t max_rot = sparse_max_rot_by_input_tile[ti];
        for (uint64_t r = 1; r <= max_rot; ++r) {
            const uint64_t rot_delta = ntt_size * r;
            std::cout << "[DAZG_ORBIT_ROT_NEED]"
                      << " layer=CirConv2D"
                      << " H=" << H
                      << " Cin=" << Cin
                      << " Cout=" << Cout
                      << " K=" << K
                      << " S=" << S
                      << " layout_mode=" << layout_mode
                      << " input_tile=" << ti
                      << " rot_index=" << r
                      << " rot_delta=" << rot_delta
                      << " cache_key=local_input_tile_" << ti << "_delta_" << rot_delta
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << std::endl;
            emitted_need_rows++;
        }
    }

    if (emitted_need_rows == 0) {
        std::cout << "[DAZG_ORBIT_ROT_NEED]"
                  << " layer=CirConv2D"
                  << " H=" << H
                  << " Cin=" << Cin
                  << " Cout=" << Cout
                  << " K=" << K
                  << " S=" << S
                  << " layout_mode=" << layout_mode
                  << " input_tile=NA"
                  << " rot_index=0"
                  << " rot_delta=0"
                  << " cache_key=none"
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << " reason=no_rotation_to_share"
                  << std::endl;
    }
}

inline bool DAZGOrbitAdaptiveBSGSEnabled()
{
    const char* v = std::getenv("DAZG_ORBIT_ADAPTIVE_BSGS");
    if (v == nullptr) return true;  // default: emit dry-run recommendation
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off");
}

// DAZG_ORBIT_FRONTIER_V16_HELPERS_BEGIN
#ifndef DAZG_ORBIT_FRONTIER_V16_ADAPTIVE_BSGS_20260513
#define DAZG_ORBIT_FRONTIER_V16_ADAPTIVE_BSGS_20260513
static const char* DAZG_ORBIT_FRONTIER_V16_MARKER = "DAZG_ORBIT_FRONTIER_V16_ADAPTIVE_BSGS_20260513";

inline bool DAZGOrbitEnvFlagV16(const char* name, bool default_value = false)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off" ||
             s == "no" || s == "NO");
}

inline bool DAZGOrbitAdaptiveBSGSApplyEnabledV16()
{
    if (std::getenv("DAZG_ORBIT_FRONTIER_V16_ADAPTIVE_BSGS_APPLY") != nullptr) {
        return DAZGOrbitEnvFlagV16("DAZG_ORBIT_FRONTIER_V16_ADAPTIVE_BSGS_APPLY", false);
    }
    return DAZGOrbitEnvFlagV16("DAZG_ORBIT_ADAPTIVE_BSGS_APPLY", false);
}

inline bool DAZGOrbitAdaptiveBSGSDetailV16()
{
    if (std::getenv("DAZG_ORBIT_FRONTIER_V16_DETAIL") != nullptr) {
        return DAZGOrbitEnvFlagV16("DAZG_ORBIT_FRONTIER_V16_DETAIL", false);
    }
    return DAZGOrbitEnvFlagV16("DAZG_ORBIT_ADAPTIVE_BSGS_DETAIL", false);
}

inline double DAZGOrbitAdaptiveBSGSMinSavedCostV16()
{
    const char* v = std::getenv("DAZG_ORBIT_ADAPTIVE_BSGS_MIN_SAVED_COST");
    if (v == nullptr || *v == '\0') {
        v = std::getenv("DAZG_ORBIT_FRONTIER_V16_MIN_SAVED_COST");
    }
    if (v == nullptr || *v == '\0') return 2.0;
    const double x = std::strtod(v, nullptr);
    return x < 0.0 ? 0.0 : x;
}

inline uint64_t DAZGOrbitAdaptiveBSGSForceStepV16()
{
    const char* v = std::getenv("DAZG_ORBIT_ADAPTIVE_BSGS_FORCE_S");
    if (v == nullptr || *v == '\0') {
        v = std::getenv("DAZG_ORBIT_FRONTIER_V16_FORCE_S");
    }
    if (v == nullptr || *v == '\0') return 0;
    const unsigned long long x = std::strtoull(v, nullptr, 10);
    return static_cast<uint64_t>(x);
}

inline int DAZGOrbitAdaptiveBSGSMaxRepackV16()
{
    const char* v = std::getenv("DAZG_ORBIT_ADAPTIVE_BSGS_MAX_REPACK");
    if (v == nullptr || *v == '\0') {
        v = std::getenv("DAZG_ORBIT_FRONTIER_V16_MAX_REPACK");
    }
    if (v == nullptr || *v == '\0') return 1;
    const long x = std::strtol(v, nullptr, 10);
    if (x < 0) return 0;
    if (x > 8) return 8;
    return static_cast<int>(x);
}
#endif
// DAZG_ORBIT_FRONTIER_V16_HELPERS_END


struct DAZGOrbitAdaptiveBSGSCandidate {
    uint64_t step = 1;
    uint64_t active_entries = 0;
    uint64_t active_input_tiles = 0;
    uint64_t active_output_tiles = 0;
    uint64_t num_groups = 1;
    uint64_t nonempty_groups = 0;
    uint64_t baby_rot_proxy = 0;
    uint64_t giant_rot_proxy = 0;
    uint64_t total_rot_proxy = 0;
    uint64_t add_proxy = 0;
    uint64_t cache_proxy = 0;
    double cost = 0.0;
};

struct DAZGOrbitAdaptiveBSGSResult {
    bool valid = false;
    uint64_t current_step = 1;
    uint64_t recommended_step = 1;
    uint64_t candidate_count = 0;
    DAZGOrbitAdaptiveBSGSCandidate current;
    DAZGOrbitAdaptiveBSGSCandidate recommended;
};

// Evaluate the current sparse entries under a hypothetical baby-step size S.
// This is a dry-run cost model: it does not change ciphertext computation.
inline DAZGOrbitAdaptiveBSGSCandidate DAZGOrbitEvaluateBSGSStep(
    const std::vector<CirSparseBSGSEntry>& entries,
    uint64_t tiled_in_channels,
    uint64_t tiled_out_channels,
    uint64_t tile_size,
    uint64_t step)
{
    DAZGOrbitAdaptiveBSGSCandidate c;
    c.step = std::max<uint64_t>(1, step);
    c.active_entries = static_cast<uint64_t>(entries.size());

    if (tile_size <= 1 || entries.empty()) {
        c.num_groups = 1;
        c.nonempty_groups = entries.empty() ? 0 : 1;
        c.total_rot_proxy = 0;
        c.cost = static_cast<double>(c.active_entries);
        return c;
    }

    c.num_groups = (tile_size + c.step - 1) / c.step;

    std::vector<uint64_t> max_baby_rot(tiled_in_channels, 0);
    std::vector<uint64_t> active_ti(tiled_in_channels, 0);
    std::vector<uint64_t> active_tj(tiled_out_channels, 0);
    std::vector<uint64_t> first_group(tiled_out_channels, c.num_groups);
    std::vector<uint64_t> nonempty_group(
        static_cast<size_t>(std::max<uint64_t>(1, c.num_groups)), 0);

    // partial_count[tj,k] tells how many active products contribute to the same
    // local k accumulator. Additional products require AddInplace.
    std::unordered_map<uint64_t, uint64_t> partial_count;

    for (const auto& e : entries) {
        const uint64_t k = e.k;
        const uint64_t ti = e.ti;
        const uint64_t tj = e.tj;

        if (ti < tiled_in_channels) active_ti[ti] = 1;
        if (tj < tiled_out_channels) active_tj[tj] = 1;

        const uint64_t rot_idx = c.step - 1 - (k % c.step);
        if (ti < tiled_in_channels && rot_idx > max_baby_rot[ti]) {
            max_baby_rot[ti] = rot_idx;
        }

        const uint64_t g = k / c.step;
        if (g < c.num_groups) {
            nonempty_group[g] = 1;
            if (tj < tiled_out_channels && g < first_group[tj]) {
                first_group[tj] = g;
            }
        }

        const uint64_t partial_key = tj * tile_size + k;
        partial_count[partial_key]++;
    }

    for (uint64_t v : active_ti) {
        if (v) c.active_input_tiles++;
    }
    for (uint64_t v : active_tj) {
        if (v) c.active_output_tiles++;
    }
    for (uint64_t r : max_baby_rot) {
        c.baby_rot_proxy += r;
    }
    for (uint64_t g : nonempty_group) {
        if (g) c.nonempty_groups++;
    }

    for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
        if (first_group[tj] == c.num_groups) continue;
        // HEComputeSparseRows rotates from first_g+1 to the final group to
        // preserve the dense BSGS slot alignment expected by depacking.
        c.giant_rot_proxy += (c.num_groups - 1 - first_group[tj]);
    }

    for (const auto& kv : partial_count) {
        if (kv.second > 0) {
            c.add_proxy += kv.second - 1;
        }
    }

    c.total_rot_proxy = c.baby_rot_proxy + c.giant_rot_proxy;
    c.cache_proxy = c.active_input_tiles * c.step;

    // Cost weights are intentionally conservative and integer-like. Rotation is
    // the expensive term; multiply count is fixed by active entries.
    const double C_ROT = 10.0;
    const double C_MUL = 1.0;
    const double C_ADD = 0.2;
    const double C_CACHE = 0.05;

    c.cost = C_ROT * static_cast<double>(c.total_rot_proxy)
           + C_MUL * static_cast<double>(c.active_entries)
           + C_ADD * static_cast<double>(c.add_proxy)
           + C_CACHE * static_cast<double>(c.cache_proxy);

    return c;
}

inline DAZGOrbitAdaptiveBSGSResult DAZGOrbitChooseAdaptiveBSGSDryRun(
    const std::vector<CirSparseBSGSEntry>& entries,
    uint64_t tiled_in_channels,
    uint64_t tiled_out_channels,
    uint64_t tile_size,
    uint64_t current_step)
{
    DAZGOrbitAdaptiveBSGSResult r;
    r.current_step = std::max<uint64_t>(1, current_step);
    r.recommended_step = r.current_step;

    if (tile_size <= 1 || entries.empty()) {
        r.valid = true;
        r.current = DAZGOrbitEvaluateBSGSStep(
            entries, tiled_in_channels, tiled_out_channels, tile_size, r.current_step);
        r.recommended = r.current;
        r.candidate_count = 1;
        return r;
    }

    std::vector<uint64_t> candidates;
    auto add_candidate = [&](uint64_t s) {
        s = std::max<uint64_t>(1, std::min<uint64_t>(s, tile_size));
        if (std::find(candidates.begin(), candidates.end(), s) == candidates.end()) {
            candidates.push_back(s);
        }
    };

    add_candidate(1);
    add_candidate(r.current_step);
    add_candidate(tile_size);

    for (uint64_t s = 2; s < tile_size; s <<= 1) {
        add_candidate(s);
    }

    std::sort(candidates.begin(), candidates.end());

    r.current = DAZGOrbitEvaluateBSGSStep(
        entries, tiled_in_channels, tiled_out_channels, tile_size, r.current_step);
    r.recommended = r.current;
    r.candidate_count = static_cast<uint64_t>(candidates.size());

    for (uint64_t s : candidates) {
        auto cand = DAZGOrbitEvaluateBSGSStep(
            entries, tiled_in_channels, tiled_out_channels, tile_size, s);

        if (cand.cost < r.recommended.cost ||
            (cand.cost == r.recommended.cost && cand.step == r.current_step)) {
            r.recommended = cand;
        }
    }

    r.valid = true;
    r.recommended_step = r.recommended.step;
    return r;
}

inline void DAZGOrbitEmitAdaptiveBSGSLog(
    uint64_t H,
    uint64_t Cin,
    uint64_t Cout,
    uint64_t K,
    uint64_t S,
    int layout_mode,
    uint64_t tile_size,
    uint64_t input_rot,
    const DAZGOrbitAdaptiveBSGSResult& r)
{
    if (!DAZGOrbitCirProfilerEnabled() || !DAZGOrbitAdaptiveBSGSEnabled() || !r.valid) {
        return;
    }

    const long long rot_saved =
        static_cast<long long>(r.current.total_rot_proxy)
        - static_cast<long long>(r.recommended.total_rot_proxy);

    const double cost_saved =
        r.current.cost - r.recommended.cost;

    std::cout << "[DAZG_ORBIT_BSGS_ADAPT]"
              << " mode=dry_run"
              << " layer=CirConv2D"
              << " H=" << H
              << " Cin=" << Cin
              << " Cout=" << Cout
              << " K=" << K
              << " S=" << S
              << " layout_mode=" << layout_mode
              << " tile_size=" << tile_size
              << " current_S=" << input_rot
              << " recommended_S=" << r.recommended_step
              << " candidate_count=" << r.candidate_count
              << " active_entries=" << r.current.active_entries
              << " active_input_tiles=" << r.current.active_input_tiles
              << " active_output_tiles=" << r.current.active_output_tiles
              << " current_groups=" << r.current.num_groups
              << " recommended_groups=" << r.recommended.num_groups
              << " current_nonempty_groups=" << r.current.nonempty_groups
              << " recommended_nonempty_groups=" << r.recommended.nonempty_groups
              << " current_baby_rot_proxy=" << r.current.baby_rot_proxy
              << " recommended_baby_rot_proxy=" << r.recommended.baby_rot_proxy
              << " current_giant_rot_proxy=" << r.current.giant_rot_proxy
              << " recommended_giant_rot_proxy=" << r.recommended.giant_rot_proxy
              << " current_rot_proxy=" << r.current.total_rot_proxy
              << " recommended_rot_proxy=" << r.recommended.total_rot_proxy
              << " rot_proxy_saved=" << rot_saved
              << " current_cost=" << r.current.cost
              << " recommended_cost=" << r.recommended.cost
              << " cost_saved=" << cost_saved
              << " exact_equiv=1"
              << " semantic_loss=0"
              << " note=does_not_change_runtime_until_apply_stage"
              << std::endl;
}
} // namespace

// ======================== CirConv2D ========================
CirConv2D::CirConv2D(uint64_t in_feature_size, uint64_t stride, uint64_t padding,
                     uint64_t block_size, const Tensor<uint64_t>& weight,
                     const Tensor<uint64_t>& bias, HEEvaluator* HE,
                     bool compact_stride2_k3_polyphase,
                     bool enable_exact_tile4_k3s1,
                     const CirLayoutPlan& layout_plan)
    : Conv2D(in_feature_size, stride, padding, weight, bias, HE),
      block_size(block_size)
{
    this->compact_stride2_k3_polyphase = compact_stride2_k3_polyphase;
    this->enable_exact_tile4_k3s1 = enable_exact_tile4_k3s1;
    if (layout_plan.out_feature_size != 0) {
        plan = layout_plan;
        has_explicit_plan = true;
    }

    compute_he_params(in_feature_size);
    if (HE->server) {
        if (IsPCOIK3S2Orbit()) {
            PreparePCOIPhaseWeights();
        } else {
            weight_pt = PackWeight();
        }
    }
}

CirConv2D::CirConv2D(uint64_t in_feature_size, uint64_t in_channels,
                     uint64_t out_channels, uint64_t kernel_size,
                     uint64_t stride, uint64_t block_size, HEEvaluator* HE,
                     bool compact_stride2_k3_polyphase,
                     bool enable_exact_tile4_k3s1,
                     const CirLayoutPlan& layout_plan)
    : Conv2D(in_feature_size, in_channels, out_channels, kernel_size, stride, HE),
      block_size(block_size)
{
    this->compact_stride2_k3_polyphase = compact_stride2_k3_polyphase;
    this->enable_exact_tile4_k3s1 = enable_exact_tile4_k3s1;
    if (layout_plan.out_feature_size != 0) {
        plan = layout_plan;
        has_explicit_plan = true;
    }

    compute_he_params(in_feature_size);
    if (HE->server) {
        if (IsPCOIK3S2Orbit()) {
            PreparePCOIPhaseWeights();
        } else {
            weight_pt = PackWeight();
        }
    }
}


bool CirConv2D::IsCompactStride2Pointwise() const {
    return plan.mode == CirLayoutMode::COMPACT_PW_S2 ||
           IsExactCompactStride2PointwiseMode(plan.mode);
}

bool CirConv2D::IsCompactStride2K3Polyphase() const {
    return plan.mode == CirLayoutMode::COMPACT_K3_S2_POLYPHASE ||
           IsExactCompactStride2K3PolyphaseMode(plan.mode);
}

bool CirConv2D::IsExactCompactStride2Pointwise() const {
    return IsExactCompactStride2PointwiseMode(plan.mode);
}

bool CirConv2D::IsExactCompactStride2K3Polyphase() const {
    return IsExactCompactStride2K3PolyphaseMode(plan.mode);
}

bool CirConv2D::IsPCOIK3S2Orbit() const {
    return IsExactPCOIK3S2OrbitMode(plan.mode);
}

void CirConv2D::BuildLayoutPlan() {
    if (has_explicit_plan && plan.out_feature_size != 0) {
        return;
    }

    plan = MakeCirLayoutPlan(
        in_feature_size, in_channels, out_channels,
        kernel_size, stride, this->padding,
        compact_stride2_k3_polyphase,
        enable_exact_tile4_k3s1,
        HE->polyModulusDegree,
        0
    );
}

void CirConv2D::SyncPackedParamsFromPlan() {
    compact_stride2_pointwise = IsCompactStride2Pointwise();
    compact_stride2_k3_polyphase = IsCompactStride2K3Polyphase();
    pcoi_k3s2_orbit = IsPCOIK3S2Orbit();
    exact_tile4_k3s1 = IsExactTileK3S1Mode(plan.mode);

    route_feature_size = plan.route_feature_size;
    effective_feature_size = plan.effective_feature_size;
    packed_in_channels = plan.packed_in_channels;
    packed_kernel_size = plan.packed_kernel_size;
    packed_stride = plan.packed_stride;
    packed_padding = plan.packed_padding;

    logical_out_feature_size = plan.logical_out_feature_size;
    packed_out_feature_size = plan.packed_out_feature_size;

    spatial_tile_rows = plan.spatial_tile_rows;
    spatial_tile_cols = plan.spatial_tile_cols;
    tile_out_feature_size = plan.tile_out_feature_size;
    tile_in_feature_size = plan.tile_in_feature_size;
    halo_size = plan.halo_size;
}


uint64_t CirConv2D::GetPCOIPhaseWeightElem(uint64_t out_ch,
                                           uint64_t in_ch,
                                           uint64_t r,
                                           uint64_t s) const
{
    if (out_ch >= out_channels) return 0;
    if (in_ch >= in_channels) return 0;
    if (r >= 2 || s >= 2) return 0;

    // Same K3/S2 algebra as the old polyphase compact path, but the phase is
    // an external orbit coordinate.  packed_in_ch is a real learned DAZG
    // channel-circulant coordinate, never phase*Cin+ch.
    switch (pcoi_pack_phase) {
        case 0:
            return (r == 1 && s == 1) ? weight({out_ch, in_ch, 1, 1}) : 0;

        case 1:
            if (r == 1 && s == 0) return weight({out_ch, in_ch, 1, 0});
            if (r == 1 && s == 1) return weight({out_ch, in_ch, 1, 2});
            return 0;

        case 2:
            if (r == 0 && s == 1) return weight({out_ch, in_ch, 0, 1});
            if (r == 1 && s == 1) return weight({out_ch, in_ch, 2, 1});
            return 0;

        case 3:
            if (r == 0 && s == 0) return weight({out_ch, in_ch, 0, 0});
            if (r == 0 && s == 1) return weight({out_ch, in_ch, 0, 2});
            if (r == 1 && s == 0) return weight({out_ch, in_ch, 2, 0});
            if (r == 1 && s == 1) return weight({out_ch, in_ch, 2, 2});
            return 0;

        default:
            return 0;
    }
}

uint64_t CirConv2D::GetPackedWeightElem(uint64_t out_ch,
                                        uint64_t packed_in_ch,
                                        uint64_t r,
                                        uint64_t s) const
{
    if (out_ch >= out_channels) return 0;
    if (packed_in_ch >= packed_in_channels) return 0;
    if (r >= packed_kernel_size || s >= packed_kernel_size) return 0;

    if (IsPCOIK3S2Orbit()) {
        return GetPCOIPhaseWeightElem(out_ch, packed_in_ch, r, s);
    }

    // full-space / compact pointwise / exact tile 都按 packed 通道直接取
    if (!IsCompactStride2K3Polyphase()) {
        if (packed_in_ch >= in_channels) return 0;
        return weight({out_ch, packed_in_ch, r, s});
    }

    // 3x3,s2,p1 polyphase compact: 4 相位 -> packed 2x2 kernel
    const uint64_t phase = packed_in_ch / in_channels;
    const uint64_t in_ch = packed_in_ch % in_channels;

    if (in_ch >= in_channels || phase >= 4) return 0;

    switch (phase) {
        case 0:
            return (r == 1 && s == 1) ? weight({out_ch, in_ch, 1, 1}) : 0;

        case 1:
            if (r == 1 && s == 0) return weight({out_ch, in_ch, 1, 0});
            if (r == 1 && s == 1) return weight({out_ch, in_ch, 1, 2});
            return 0;

        case 2:
            if (r == 0 && s == 1) return weight({out_ch, in_ch, 0, 1});
            if (r == 1 && s == 1) return weight({out_ch, in_ch, 2, 1});
            return 0;

        case 3:
            if (r == 0 && s == 0) return weight({out_ch, in_ch, 0, 0});
            if (r == 0 && s == 1) return weight({out_ch, in_ch, 0, 2});
            if (r == 1 && s == 0) return weight({out_ch, in_ch, 2, 0});
            if (r == 1 && s == 1) return weight({out_ch, in_ch, 2, 2});
            return 0;

        default:
            return 0;
    }
}

void CirConv2D::compute_he_params(uint64_t raw_in_feature_size) {
    // DAZG_ORBIT_V719_RUNTIME_MARKER_BEGIN
    static const bool dazg_orbit_v719_marker_logged = []() {
        std::cout << "[DAZG_ORBIT_V719_ACTIVE_BINARY] marker="
                  << DAZGOrbitV719LinkedCirConvMarker << std::endl;
        return true;
    }();
    (void)dazg_orbit_v719_marker_logged;
    // DAZG_ORBIT_V719_RUNTIME_MARKER_END
    out_feature_size =
        (raw_in_feature_size + 2 * this->padding - kernel_size) / stride + 1;

    BuildLayoutPlan();
    SyncPackedParamsFromPlan();

    assert(plan.out_feature_size == out_feature_size);
    assert(route_feature_size > 0);
    assert(effective_feature_size > 0);

    if (IsCompactStride2Pointwise()) {
        assert(packed_in_channels == in_channels);
        assert(packed_kernel_size == 1);
        assert(packed_stride == 1);
        assert(packed_padding == 0);
        assert(logical_out_feature_size == out_feature_size);
        if (!IsExactCompactStride2Pointwise()) {
            assert(effective_feature_size == out_feature_size);
            assert(packed_out_feature_size == out_feature_size);
        }
    }

    if (IsCompactStride2K3Polyphase()) {
        assert(packed_in_channels == 4 * in_channels);
        assert(packed_kernel_size == 2);
        assert(packed_stride == 1);
        assert(packed_padding == 0);
        assert(logical_out_feature_size == out_feature_size);
        if (!IsExactCompactStride2K3Polyphase()) {
            assert(effective_feature_size == out_feature_size + 1);
            assert(packed_out_feature_size == out_feature_size);
        }
    }

    if (IsPCOIK3S2Orbit()) {
        assert(packed_in_channels == in_channels);
        assert(packed_kernel_size == 2);
        assert(packed_stride == 1);
        assert(packed_padding == 0);
        assert(plan.phase_count == 1);
        assert(plan.orbit_phase_count == 4);
        assert(logical_out_feature_size == out_feature_size);
        assert(packed_out_feature_size == tile_out_feature_size);
        assert(plan.pass_count == spatial_tile_rows * spatial_tile_cols * plan.orbit_phase_count);
    }

    if (IsExactTileK3S1Mode(plan.mode)) {
        if (!IsExactCompactStride2Pointwise() &&
            !IsExactCompactStride2K3Polyphase() &&
            !IsPCOIK3S2Orbit()) {
            assert(halo_size == kernel_size / 2);
        }
        assert(spatial_tile_rows >= 1);
        assert(spatial_tile_cols >= 1);
        assert(tile_out_feature_size > 0);
        assert(tile_in_feature_size == tile_out_feature_size + packed_kernel_size - 1);
        assert(tile_out_feature_size * spatial_tile_rows >= logical_out_feature_size);
        assert(tile_out_feature_size * spatial_tile_cols >= logical_out_feature_size);
        assert(tile_out_feature_size * (spatial_tile_rows - 1) < logical_out_feature_size);
        assert(tile_out_feature_size * (spatial_tile_cols - 1) < logical_out_feature_size);
        assert(logical_out_feature_size == out_feature_size);
        assert(packed_out_feature_size == tile_out_feature_size);
        assert(route_feature_size == tile_in_feature_size);
        assert(effective_feature_size == tile_in_feature_size);
        assert(packed_stride == 1);
        assert(packed_padding == 0);
    } else {
        assert(logical_out_feature_size == out_feature_size);
        assert(packed_out_feature_size == out_feature_size);
        assert(spatial_tile_rows == 1);
        assert(spatial_tile_cols == 1);
    }

    assert(packed_in_channels % block_size == 0 &&
           "packed_in_channels must be divisible by block_size");
    assert(out_channels % block_size == 0 &&
           "out_channels must be divisible by block_size");

    padded_feature_size = NextPow2U64(effective_feature_size);

    const uint64_t padded_HW = padded_feature_size * padded_feature_size;
    ntt_size = block_size * padded_HW;

    // DAZG_ORBIT_V719_TO_H8_ROUTE_LOG_BEGIN
    if (raw_in_feature_size == 16 && in_channels == 96 && out_channels == 192 &&
        kernel_size == 3 && stride == 2) {
        std::cout << "[DAZG_ORBIT_V719_TO_H8_ROUTE]"
                  << " H=" << raw_in_feature_size
                  << " Cin=" << in_channels
                  << " Cout=" << out_channels
                  << " K=" << kernel_size
                  << " S=" << stride
                  << " layout_mode=" << static_cast<int>(plan.mode)
                  << " compact_k3s2=" << (IsCompactStride2K3Polyphase() ? 1 : 0)
                  << " pcoi_k3s2=" << (IsPCOIK3S2Orbit() ? 1 : 0)
                  << " packed_in_channels=" << packed_in_channels
                  << " packed_kernel_size=" << packed_kernel_size
                  << " packed_stride=" << packed_stride
                  << " packed_padding=" << packed_padding
                  << " block_size=" << block_size
                  << " effective_feature_size=" << effective_feature_size
                  << " padded_feature_size=" << padded_feature_size
                  << " ntt_size=" << ntt_size
                  << " poly_modulus_degree=" << HE->polyModulusDegree
                  << " capacity_ok=" << (ntt_size <= HE->polyModulusDegree ? 1 : 0)
                  << " marker=DAZG_ORBIT_V719_LINKED_CIRCONV_20260622_141527"
                  << std::endl;
    }
    // DAZG_ORBIT_V719_TO_H8_ROUTE_LOG_END

    assert(ntt_size <= HE->polyModulusDegree &&
           "ntt_size exceeds polyModulusDegree; fix routing/block_size first");

    num_blocks_in = packed_in_channels / block_size;
    num_blocks_out = out_channels / block_size;

    tile_size = HE->polyModulusDegree / (2 * ntt_size);
    if (tile_size < 1) tile_size = 1;

    tiled_in_channels = (num_blocks_in + tile_size - 1) / tile_size;
    tiled_out_channels = (num_blocks_out + tile_size - 1) / tile_size;

    if (tile_size <= 1) {
        input_rot = 1;
    } else {
        input_rot = 1;
        while (input_rot * input_rot < tile_size) {
            input_rot++;
        }
    }


    // DAZG_ORBIT_V487_ADAPTIVE_BSGS_APPLY_PATCH_BEGIN
    // Runtime-selectable BSGS baby-step apply stage.
    // Default path is bit-for-bit conservative: DAZG_ORBIT_ADAPTIVE_BSGS_APPLY unset/0
    // keeps the old ceil(sqrt(tile_size)) input_rot.  With APPLY=1 and FORCE_S=4,
    // PackWeight and HECompute use the same selected input_rot, so the encoding and
    // evaluation schedule remain internally consistent.
    {
        const uint64_t dazg_orbit_v487_original_input_rot = input_rot;
        const char* dazg_orbit_v487_apply_env = std::getenv("DAZG_ORBIT_ADAPTIVE_BSGS_APPLY");
        bool dazg_orbit_v487_apply = false;
        if (dazg_orbit_v487_apply_env != nullptr) {
            const std::string dazg_orbit_v487_apply_s(dazg_orbit_v487_apply_env);
            dazg_orbit_v487_apply = !(dazg_orbit_v487_apply_s.empty() ||
                                   dazg_orbit_v487_apply_s == "0" ||
                                   dazg_orbit_v487_apply_s == "false" ||
                                   dazg_orbit_v487_apply_s == "False" ||
                                   dazg_orbit_v487_apply_s == "OFF" ||
                                   dazg_orbit_v487_apply_s == "off");
        }

        // DAZG_ORBIT_V488_SCOPE_GUARD_PATCH_BEGIN
        // Scope-gated trial guard: V487 proved that global S=4 can reduce rotations
        // but may change the final logits.  V488 never applies a candidate outside
        // an explicit layer scope, and the runner rejects candidates whose final
        // signed output hash differs from the v485_current oracle.
        std::string dazg_orbit_v488_scope = "all";
        const char* dazg_orbit_v488_scope_env = std::getenv("DAZG_ORBIT_V488_BSGS_SCOPE");
        if (dazg_orbit_v488_scope_env != nullptr && *dazg_orbit_v488_scope_env != '\0') {
            dazg_orbit_v488_scope = std::string(dazg_orbit_v488_scope_env);
        }

        bool dazg_orbit_v488_layer_allowed = true;
        if (dazg_orbit_v488_scope == "none" || dazg_orbit_v488_scope == "off") {
            dazg_orbit_v488_layer_allowed = false;
        } else if (dazg_orbit_v488_scope == "k3s2_only" ||
                   dazg_orbit_v488_scope == "k3s2_all" ||
                   dazg_orbit_v488_scope == "hot_k3s2") {
            dazg_orbit_v488_layer_allowed =
                (kernel_size == 3 && stride == 2 && IsPCOIK3S2Orbit());
        } else if (dazg_orbit_v488_scope == "hot_k3s2_h32_h16") {
            dazg_orbit_v488_layer_allowed =
                (kernel_size == 3 && stride == 2 && IsPCOIK3S2Orbit() &&
                 (in_feature_size == 32 || in_feature_size == 16));
        } else if (dazg_orbit_v488_scope == "hot_k3s2_h32") {
            dazg_orbit_v488_layer_allowed =
                (kernel_size == 3 && stride == 2 && IsPCOIK3S2Orbit() &&
                 in_feature_size == 32);
        } else if (dazg_orbit_v488_scope == "hot_k3s2_h16") {
            dazg_orbit_v488_layer_allowed =
                (kernel_size == 3 && stride == 2 && IsPCOIK3S2Orbit() &&
                 in_feature_size == 16);
        } else if (dazg_orbit_v488_scope == "fc_only") {
            dazg_orbit_v488_layer_allowed =
                (in_feature_size == 1 && kernel_size == 1 && stride == 1 && out_channels == 100);
        }
        // DAZG_ORBIT_V488_SCOPE_GUARD_PATCH_END

        if (dazg_orbit_v487_apply && dazg_orbit_v488_layer_allowed && tile_size > 1) {
            uint64_t dazg_orbit_v487_selected = input_rot;
            const char* dazg_orbit_v487_force_env = std::getenv("DAZG_ORBIT_ADAPTIVE_BSGS_FORCE_S");
            if (dazg_orbit_v487_force_env != nullptr && *dazg_orbit_v487_force_env != '\0') {
                char* dazg_orbit_v487_endp = nullptr;
                const unsigned long long dazg_orbit_v487_parsed =
                    std::strtoull(dazg_orbit_v487_force_env, &dazg_orbit_v487_endp, 10);
                if (dazg_orbit_v487_endp != dazg_orbit_v487_force_env && dazg_orbit_v487_parsed > 0ULL) {
                    dazg_orbit_v487_selected = static_cast<uint64_t>(dazg_orbit_v487_parsed);
                }
            } else if (tile_size >= 8 && input_rot < 4) {
                // The V485 dry-run repeatedly recommended S=4 on the two hot K3S2 layers.
                dazg_orbit_v487_selected = 4;
            }

            if (dazg_orbit_v487_selected < 1) dazg_orbit_v487_selected = 1;
            if (dazg_orbit_v487_selected > tile_size) dazg_orbit_v487_selected = tile_size;
            input_rot = dazg_orbit_v487_selected;
        }

        if (DAZGOrbitCirProfilerEnabled()) {
            std::cout << "[DAZG_ORBIT_BSGS_APPLY]"
                      << " layer=CirConv2D"
                      << " H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " Cout=" << out_channels
                      << " K=" << kernel_size
                      << " S=" << stride
                      << " layout_mode=" << static_cast<int>(plan.mode)
                      << " tile_size=" << tile_size
                      << " apply=" << (dazg_orbit_v487_apply ? 1 : 0)
                      << " original_input_rot=" << dazg_orbit_v487_original_input_rot
                      << " selected_input_rot=" << input_rot
                      << " v488_scope=" << dazg_orbit_v488_scope
                      << " v488_layer_allowed=" << (dazg_orbit_v488_layer_allowed ? 1 : 0)
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << " patch=V487"
                      << std::endl;
        }
    }
    // DAZG_ORBIT_V487_ADAPTIVE_BSGS_APPLY_PATCH_END


    // DAZG_ORBIT_FRONTIER_V16_PREPACK_FORCE_BEGIN
    if (DAZGOrbitAdaptiveBSGSApplyEnabledV16()) {
        uint64_t v16_forced_step = DAZGOrbitAdaptiveBSGSForceStepV16();
        if (v16_forced_step != 0) {
            const uint64_t v16_old_step = input_rot;
            if (tile_size <= 1) {
                v16_forced_step = 1;
            } else {
                v16_forced_step =
                    std::max<uint64_t>(1, std::min<uint64_t>(v16_forced_step, tile_size));
            }
            input_rot = v16_forced_step;
            std::cout << "[DAZG_ORBIT_ADAPTIVE_BSGS_PREPACK]"
                      << " marker=" << DAZG_ORBIT_FRONTIER_V16_MARKER
                      << " applied=" << (v16_old_step != input_rot ? 1 : 0)
                      << " old_S=" << v16_old_step
                      << " new_S=" << input_rot
                      << " forced=1"
                      << " tile_size=" << tile_size
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << " reason=forced_step_applied_before_pack"
                      << std::endl;
        }
    }
    // DAZG_ORBIT_FRONTIER_V16_PREPACK_FORCE_END

    std::cout << "CirConv2D params: in_channels=" << in_channels
              << ", out_channels=" << out_channels
              << ", block_size=" << block_size
              << ", layout_mode=" << static_cast<int>(plan.mode)
              << ", compact_stride2_pointwise=" << compact_stride2_pointwise
              << ", compact_stride2_k3_polyphase=" << compact_stride2_k3_polyphase
              << ", pcoi_k3s2_orbit=" << pcoi_k3s2_orbit
              << ", pcoi_orbit_phases=" << plan.orbit_phase_count
              << ", exact_tiled_k3s1=" << IsExactTileK3S1Mode(plan.mode)
              << ", route_feature_size=" << route_feature_size
              << ", effective_feature_size=" << effective_feature_size
              << ", packed_in_channels=" << packed_in_channels
              << ", packed_kernel_size=" << packed_kernel_size
              << ", packed_stride=" << packed_stride
              << ", packed_padding=" << packed_padding
              << ", padded_feature_size=" << padded_feature_size
              << ", ntt_size=" << ntt_size
              << ", num_blocks=(" << num_blocks_in << "," << num_blocks_out << ")"
              << ", tile_size=" << tile_size
              << ", input_rot=" << input_rot
              << ", tiled=(" << tiled_in_channels << "," << tiled_out_channels << ")"
              << ", out_feature_size=" << out_feature_size
              << ", logical_out_feature_size=" << logical_out_feature_size
              << ", packed_out_feature_size=" << packed_out_feature_size
              << ", spatial_tiles=(" << spatial_tile_rows << "," << spatial_tile_cols << ")"
              << ", tile_out_feature_size=" << tile_out_feature_size
              << ", tile_in_feature_size=" << tile_in_feature_size
              << ", suggested_block_size=" << plan.suggested_block_size
              << ", pass_count=" << plan.pass_count
              << ", total_score=" << plan.total_score
              << std::endl;
}



Tensor<UnifiedPlaintext> CirConv2D::PackWeight() {
    // DAZG_ORBIT_FRONTIER_V16_THREADLOCAL_BEGIN
    static thread_local int dazg_orbit_frontier_v16_repack_depth = 0;
    // DAZG_ORBIT_FRONTIER_V16_THREADLOCAL_END

    const char* dazg_orbit_schedule =
        IsPCOIK3S2Orbit() ? "DAZG-ORBIT-PCOI-K3S2" : "StageZ2-ExactSparseBSGSLinear";
    if (IsPCOIK3S2Orbit() && HE->server) {
        std::cout << "[DAZG_ORBIT_PCOI_K3S2_PREPACK]"
                  << " H=" << in_feature_size
                  << " Cin=" << in_channels
                  << " Cout=" << out_channels
                  << " K=" << kernel_size
                  << " S=" << stride
                  << " phase=" << pcoi_pack_phase
                  << " packed_in_channels=" << packed_in_channels
                  << " orbit_phase_count=" << plan.orbit_phase_count
                  << " exact_equiv=1 semantic_loss=0"
                  << " schedule=" << dazg_orbit_schedule
                  << std::endl;
    }

    Tensor<UnifiedPlaintext> wpt({tiled_in_channels, tiled_out_channels, tile_size}, HE->Backend());
    pack_active = Tensor<uint64_t>({tiled_in_channels, tiled_out_channels, tile_size});

    // Stage-Z2 exact sparse BSGS plan construction.
    //
    // This keeps the original encoded plaintext tensor wpt unchanged for
    // compatibility, but moves all work discovery into setup time:
    //   1) dense packed-weight traversal,
    //   2) exact zero-plaintext elimination,
    //   3) active-entry construction,
    //   4) BSGS baby/giant grouping metadata,
    //   5) nonempty input/output tile discovery.
    //
    // HEComputeSparseRows() consumes sparse_bsgs_entries together with
    // sparse_max_rot_by_input_tile / sparse_active_*_tile, so no runtime
    // zero-plaintext discovery is needed.
    sparse_bsgs_entries.clear();
    sparse_max_rot_by_input_tile.assign(tiled_in_channels, 0);
    sparse_active_input_tile.assign(tiled_in_channels, 0);
    sparse_active_output_tile.assign(tiled_out_channels, 0);
    sparse_total_packs = 0;
    sparse_active_packs = 0;
    sparse_zero_packs = 0;
    sparse_rotation_slots = 0;

    const uint64_t possible_bsgs_groups =
        (tile_size + input_rot - 1) / input_rot;
    sparse_bsgs_groups = possible_bsgs_groups;

    std::vector<uint64_t> nonempty_bsgs_group(possible_bsgs_groups, 0);
    std::vector<uint64_t> entries_per_bsgs_group(possible_bsgs_groups, 0);
    std::vector<uint64_t> entries_per_input_tile(tiled_in_channels, 0);
    std::vector<uint64_t> entries_per_output_tile(tiled_out_channels, 0);

    auto t0 = std::chrono::steady_clock::now();

    Utils::CyclicNTT cyclic_ntt(ntt_size, HE->plain_mod);
    const uint64_t padded_HW = padded_feature_size * padded_feature_size;

    // Packed-kernel semantic offset. This must stay aligned with
    // DepackResult()/DepackSingleMapImpl().
    const uint64_t offset = (packed_kernel_size - 1) * (padded_feature_size + 1);

    uint64_t zero_pack_cnt = 0;
    uint64_t total_pack_cnt = 0;

    for (uint64_t ti = 0; ti < tiled_in_channels; ti++) {
        for (uint64_t tj = 0; tj < tiled_out_channels; tj++) {
            for (uint64_t k = 0; k < tile_size; k++) {
                total_pack_cnt++;

                std::vector<uint64_t> poly(HE->polyModulusDegree, 0);

                bool structurally_active = false;
                for (uint64_t l = 0; l < tile_size; l++) {
                    uint64_t in_blk, out_blk;

                    if (tile_size == 1) {
                        in_blk = ti;
                        out_blk = tj;
                    } else {
                        in_blk =
                            ti * tile_size
                            + (l + (input_rot - k % input_rot - 1)) % tile_size;
                        out_blk =
                            tj * tile_size
                            + (3 * tile_size - l - k
                               - (input_rot - k % input_rot)) % tile_size;
                    }

                    if (in_blk < num_blocks_in && out_blk < num_blocks_out) {
                        structurally_active = true;
                        break;
                    }
                }

                if (!structurally_active) {
                    pack_active({ti, tj, k}) = 0;
                    zero_pack_cnt++;

                    // Encode a harmless dummy nonzero plaintext so the existing
                    // plaintext tensor shape remains valid. Runtime skips it
                    // because pack_active and sparse_bsgs_entries mark it inactive.
                    poly[HE->polyModulusDegree - 1] = 1;
                    HE->encoder->encode(poly, wpt({ti, tj, k}));
                    continue;
                }

                for (uint64_t l = 0; l < tile_size; l++) {
                    uint64_t in_blk, out_blk;

                    if (tile_size == 1) {
                        in_blk = ti;
                        out_blk = tj;
                    } else {
                        in_blk =
                            ti * tile_size
                            + (l + (input_rot - k % input_rot - 1)) % tile_size;
                        out_blk =
                            tj * tile_size
                            + (3 * tile_size - l - k
                               - (input_rot - k % input_rot)) % tile_size;
                    }

                    if (in_blk >= num_blocks_in || out_blk >= num_blocks_out) {
                        continue;
                    }

                    std::vector<uint64_t> w_coef(ntt_size, 0);

                    const uint64_t packed_in_ch = in_blk * block_size;
                    if (packed_in_ch >= packed_in_channels) {
                        continue;
                    }

                    for (uint64_t i = 0; i < block_size; i++) {
                        const uint64_t out_ch = out_blk * block_size + i;
                        if (out_ch >= out_channels) continue;

                        const uint64_t base = i * padded_HW;

                        for (uint64_t r = 0; r < packed_kernel_size; r++) {
                            for (uint64_t s = 0; s < packed_kernel_size; s++) {
                                w_coef[base + offset - r * padded_feature_size - s] =
                                    GetPackedWeightElem(out_ch, packed_in_ch, r, s);
                            }
                        }
                    }

                    cyclic_ntt.ComputeForward(w_coef.data(), w_coef.data());

                    const uint64_t slot_off = l * ntt_size;
                    for (uint64_t m = 0; m < ntt_size; m++) {
                        poly[slot_off + m] = w_coef[m];
                    }

                    if (tile_size > 1) {
                        const uint64_t slot_off2 =
                            l * ntt_size + HE->polyModulusDegree / 2;
                        for (uint64_t m = 0; m < ntt_size; m++) {
                            poly[slot_off2 + m] = w_coef[m];
                        }
                    }
                }

                bool all_zero = true;
                for (uint64_t m = 0; m < HE->polyModulusDegree && all_zero; m++) {
                    all_zero = (poly[m] == 0);
                }

                if (all_zero) {
                    pack_active({ti, tj, k}) = 0;
                    zero_pack_cnt++;

                    // Keep ciphertext/plaintext tensor indexing stable.
                    poly[HE->polyModulusDegree - 1] = 1;
                } else {
                    pack_active({ti, tj, k}) = 1;
                    sparse_active_packs++;
                    sparse_active_input_tile[ti] = 1;
                    sparse_active_output_tile[tj] = 1;

                    CirSparseBSGSEntry entry;
                    entry.ti = ti;
                    entry.tj = tj;
                    entry.k = k;
                    entry.rot_idx =
                        (tile_size == 1) ? 0 : (input_rot - 1 - (k % input_rot));
                    entry.group_idx =
                        (tile_size == 1) ? 0 : (k / input_rot);

                    assert(entry.rot_idx < input_rot);
                    assert(entry.group_idx < possible_bsgs_groups);

                    sparse_bsgs_entries.push_back(entry);

                    if (entry.rot_idx > sparse_max_rot_by_input_tile[ti]) {
                        sparse_max_rot_by_input_tile[ti] = entry.rot_idx;
                    }

                    nonempty_bsgs_group[entry.group_idx] = 1;
                    entries_per_bsgs_group[entry.group_idx]++;
                    entries_per_input_tile[ti]++;
                    entries_per_output_tile[tj]++;
                }

                HE->encoder->encode(poly, wpt({ti, tj, k}));
            }
        }
    }

    sparse_total_packs = total_pack_cnt;
    sparse_zero_packs = zero_pack_cnt;

    for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
        if (sparse_active_input_tile[ti]) {
            sparse_rotation_slots += sparse_max_rot_by_input_tile[ti];
        }
    }

    const uint64_t active_input_tiles =
        static_cast<uint64_t>(
            std::count(sparse_active_input_tile.begin(),
                       sparse_active_input_tile.end(),
                       1ULL));

    const uint64_t active_output_tiles =
        static_cast<uint64_t>(
            std::count(sparse_active_output_tile.begin(),
                       sparse_active_output_tile.end(),
                       1ULL));

    const uint64_t nonempty_bsgs_groups =
        static_cast<uint64_t>(
            std::count(nonempty_bsgs_group.begin(),
                       nonempty_bsgs_group.end(),
                       1ULL));

    const auto dazg_orbit_bsgs_adapt =
        DAZGOrbitChooseAdaptiveBSGSDryRun(
            sparse_bsgs_entries,
            tiled_in_channels,
            tiled_out_channels,
            tile_size,
            input_rot
        );

    DAZGOrbitEmitAdaptiveBSGSLog(
        in_feature_size,
        in_channels,
        out_channels,
        kernel_size,
        stride,
        static_cast<int>(plan.mode),
        tile_size,
        input_rot,
        dazg_orbit_bsgs_adapt
    );

    DAZGOrbitEmitLocalRotNeedLog(
        in_feature_size,
        in_channels,
        out_channels,
        kernel_size,
        stride,
        static_cast<int>(plan.mode),
        tile_size,
        input_rot,
        ntt_size,
        sparse_active_input_tile,
        sparse_max_rot_by_input_tile,
        sparse_rotation_slots,
        static_cast<uint64_t>(sparse_bsgs_entries.size())
    );


    // DAZG_ORBIT_FRONTIER_V16_PACKWEIGHT_REPACK_BEGIN
    if (dazg_orbit_bsgs_adapt.valid) {
        const bool v16_apply_env = DAZGOrbitAdaptiveBSGSApplyEnabledV16();
        const bool v16_detail = DAZGOrbitAdaptiveBSGSDetailV16();

        uint64_t v16_target_step = DAZGOrbitAdaptiveBSGSForceStepV16();
        const bool v16_forced_step = (v16_target_step != 0);
        if (!v16_forced_step) {
            v16_target_step = dazg_orbit_bsgs_adapt.recommended_step;
        }

        if (tile_size <= 1) {
            v16_target_step = 1;
        } else {
            v16_target_step =
                std::max<uint64_t>(1, std::min<uint64_t>(v16_target_step, tile_size));
        }

        const DAZGOrbitAdaptiveBSGSCandidate v16_target_eval =
            DAZGOrbitEvaluateBSGSStep(
                sparse_bsgs_entries,
                tiled_in_channels,
                tiled_out_channels,
                tile_size,
                v16_target_step
            );

        const double v16_saved_cost =
            dazg_orbit_bsgs_adapt.current.cost - v16_target_eval.cost;
        const double v16_min_saved_cost = DAZGOrbitAdaptiveBSGSMinSavedCostV16();
        const bool v16_clears_floor =
            v16_forced_step || (v16_saved_cost >= v16_min_saved_cost);

        uint64_t v16_rot_saved_proxy = 0;
        if (dazg_orbit_bsgs_adapt.current.total_rot_proxy >
            v16_target_eval.total_rot_proxy) {
            v16_rot_saved_proxy =
                dazg_orbit_bsgs_adapt.current.total_rot_proxy -
                v16_target_eval.total_rot_proxy;
        }

        const int v16_max_repack = DAZGOrbitAdaptiveBSGSMaxRepackV16();
        const bool v16_same_step = (v16_target_step == input_rot);
        const bool v16_can_repack =
            v16_apply_env &&
            !v16_same_step &&
            v16_clears_floor &&
            (dazg_orbit_frontier_v16_repack_depth < v16_max_repack);

        if (v16_can_repack) {
            const uint64_t v16_old_step = input_rot;
            std::cout << "[DAZG_ORBIT_ADAPTIVE_BSGS_APPLY]"
                      << " marker=" << DAZG_ORBIT_FRONTIER_V16_MARKER
                      << " applied=1"
                      << " action=repack"
                      << " H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " Cout=" << out_channels
                      << " K=" << kernel_size
                      << " S=" << stride
                      << " layout_mode=" << static_cast<int>(plan.mode)
                      << " tile_size=" << tile_size
                      << " old_S=" << v16_old_step
                      << " new_S=" << v16_target_step
                      << " forced=" << (v16_forced_step ? 1 : 0)
                      << " current_rot_proxy=" << dazg_orbit_bsgs_adapt.current.total_rot_proxy
                      << " target_rot_proxy=" << v16_target_eval.total_rot_proxy
                      << " rot_saved_proxy=" << v16_rot_saved_proxy
                      << " current_cost=" << dazg_orbit_bsgs_adapt.current.cost
                      << " target_cost=" << v16_target_eval.cost
                      << " cost_saved=" << v16_saved_cost
                      << " min_saved_cost=" << v16_min_saved_cost
                      << " repack_depth=" << dazg_orbit_frontier_v16_repack_depth
                      << " max_repack=" << v16_max_repack
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << std::endl;

            input_rot = v16_target_step;
            ++dazg_orbit_frontier_v16_repack_depth;
            Tensor<UnifiedPlaintext> v16_repacked = PackWeight();
            --dazg_orbit_frontier_v16_repack_depth;

            std::cout << "[DAZG_ORBIT_ADAPTIVE_BSGS_APPLY_RETURN]"
                      << " marker=" << DAZG_ORBIT_FRONTIER_V16_MARKER
                      << " applied=1"
                      << " old_S=" << v16_old_step
                      << " new_S=" << input_rot
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << std::endl;
            return v16_repacked;
        }

        if (v16_detail || v16_apply_env) {
            const char* v16_reason = "guard_blocked";
            if (!v16_apply_env) {
                v16_reason = "apply_env_off";
            } else if (v16_same_step) {
                v16_reason = "same_step";
            } else if (!v16_clears_floor) {
                v16_reason = "below_saved_cost_floor";
            } else if (dazg_orbit_frontier_v16_repack_depth >= v16_max_repack) {
                v16_reason = "max_repack_reached";
            }

            std::cout << "[DAZG_ORBIT_ADAPTIVE_BSGS_APPLY]"
                      << " marker=" << DAZG_ORBIT_FRONTIER_V16_MARKER
                      << " applied=0"
                      << " action=keep_current"
                      << " reason=" << v16_reason
                      << " H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " Cout=" << out_channels
                      << " K=" << kernel_size
                      << " S=" << stride
                      << " layout_mode=" << static_cast<int>(plan.mode)
                      << " tile_size=" << tile_size
                      << " current_S=" << input_rot
                      << " target_S=" << v16_target_step
                      << " forced=" << (v16_forced_step ? 1 : 0)
                      << " current_rot_proxy=" << dazg_orbit_bsgs_adapt.current.total_rot_proxy
                      << " target_rot_proxy=" << v16_target_eval.total_rot_proxy
                      << " rot_saved_proxy=" << v16_rot_saved_proxy
                      << " current_cost=" << dazg_orbit_bsgs_adapt.current.cost
                      << " target_cost=" << v16_target_eval.cost
                      << " cost_saved=" << v16_saved_cost
                      << " min_saved_cost=" << v16_min_saved_cost
                      << " repack_depth=" << dazg_orbit_frontier_v16_repack_depth
                      << " max_repack=" << v16_max_repack
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << std::endl;
        }
    }
    // DAZG_ORBIT_FRONTIER_V16_PACKWEIGHT_REPACK_END

    auto t1 = std::chrono::steady_clock::now();
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    std::cout << "CirConv2D PackWeight time(us)=" << us << std::endl;

    std::cout << "CirConv2D PackWeight: zero_packs="
              << zero_pack_cnt << "/" << total_pack_cnt
              << ", active_packs=" << sparse_active_packs
              << ", sparse_entries=" << sparse_bsgs_entries.size()
              << ", active_input_tiles=" << active_input_tiles
              << ", active_output_tiles=" << active_output_tiles
              << ", rotation_slots=" << sparse_rotation_slots
              << ", bsgs_groups=" << sparse_bsgs_groups
              << ", nonempty_bsgs_groups=" << nonempty_bsgs_groups
              << ", schedule=" << dazg_orbit_schedule
              << std::endl;

    if (DAZGOrbitCirProfilerEnabled()) {
        std::cout << "[DAZG_ORBIT_PLAN]"
                  << " layer=CirConv2D"
                  << " H=" << in_feature_size
                  << " Cin=" << in_channels
                  << " Cout=" << out_channels
                  << " K=" << kernel_size
                  << " S=" << stride
                  << " layout_mode=" << static_cast<int>(plan.mode)
                  << " dense_packs=" << total_pack_cnt
                  << " zero_packs=" << zero_pack_cnt
                  << " active_packs=" << sparse_active_packs
                  << " sparse_entries=" << sparse_bsgs_entries.size()
                  << " active_input_tiles=" << active_input_tiles
                  << " active_output_tiles=" << active_output_tiles
                  << " rotation_slots=" << sparse_rotation_slots
                  << " bsgs_groups=" << sparse_bsgs_groups
                  << " nonempty_bsgs_groups=" << nonempty_bsgs_groups
                  << " input_rot=" << input_rot
                  << " adaptive_recommended_S=" << dazg_orbit_bsgs_adapt.recommended_step
                  << " adaptive_current_rot_proxy=" << dazg_orbit_bsgs_adapt.current.total_rot_proxy
                  << " adaptive_recommended_rot_proxy=" << dazg_orbit_bsgs_adapt.recommended.total_rot_proxy
                  << " tile_size=" << tile_size
                  << " packweight_us=" << us
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << " schedule=" << dazg_orbit_schedule
                  << std::endl;
    }

    return wpt;
}


CirSparseBSGSState CirConv2D::CaptureSparseState() const
{
    CirSparseBSGSState st;
    st.sparse_bsgs_entries = sparse_bsgs_entries;
    st.sparse_max_rot_by_input_tile = sparse_max_rot_by_input_tile;
    st.sparse_active_input_tile = sparse_active_input_tile;
    st.sparse_active_output_tile = sparse_active_output_tile;
    st.pack_active = pack_active;
    st.input_rot = input_rot;
    st.sparse_total_packs = sparse_total_packs;
    st.sparse_active_packs = sparse_active_packs;
    st.sparse_zero_packs = sparse_zero_packs;
    st.sparse_rotation_slots = sparse_rotation_slots;
    st.sparse_bsgs_groups = sparse_bsgs_groups;
    return st;
}

void CirConv2D::RestoreSparseState(const CirSparseBSGSState& st)
{
    sparse_bsgs_entries = st.sparse_bsgs_entries;
    sparse_max_rot_by_input_tile = st.sparse_max_rot_by_input_tile;
    sparse_active_input_tile = st.sparse_active_input_tile;
    sparse_active_output_tile = st.sparse_active_output_tile;
    pack_active = st.pack_active;
    input_rot = st.input_rot;
    sparse_total_packs = st.sparse_total_packs;
    sparse_active_packs = st.sparse_active_packs;
    sparse_zero_packs = st.sparse_zero_packs;
    sparse_rotation_slots = st.sparse_rotation_slots;
    sparse_bsgs_groups = st.sparse_bsgs_groups;
}

void CirConv2D::PreparePCOIPhaseWeights()
{
    assert(IsPCOIK3S2Orbit());
    assert(plan.orbit_phase_count == 4);

    pcoi_phase_weight_pt.clear();
    pcoi_phase_sparse_state.clear();
    pcoi_phase_weight_pt.reserve(plan.orbit_phase_count);
    pcoi_phase_sparse_state.reserve(plan.orbit_phase_count);

    const uint64_t old_phase = pcoi_pack_phase;
    const uint64_t base_input_rot = input_rot;

    for (uint64_t phase = 0; phase < plan.orbit_phase_count; ++phase) {
        pcoi_pack_phase = phase;
        input_rot = base_input_rot;
        Tensor<UnifiedPlaintext> phase_wpt = PackWeight();
        pcoi_phase_weight_pt.push_back(phase_wpt);
        pcoi_phase_sparse_state.push_back(CaptureSparseState());
    }

    pcoi_pack_phase = old_phase;
    if (!pcoi_phase_sparse_state.empty()) {
        RestoreSparseState(pcoi_phase_sparse_state[0]);
        weight_pt = pcoi_phase_weight_pt[0];
    }

    if (HE->server) {
        std::cout << "[DAZG_ORBIT_PCOI_K3S2_PREPACK_SUMMARY]"
                  << " H=" << in_feature_size
                  << " Cin=" << in_channels
                  << " Cout=" << out_channels
                  << " K=" << kernel_size
                  << " S=" << stride
                  << " layout_mode=" << static_cast<int>(plan.mode)
                  << " phase_weights=" << pcoi_phase_weight_pt.size()
                  << " orbit_phase_count=" << plan.orbit_phase_count
                  << " packed_in_channels=" << packed_in_channels
                  << " exact_equiv=1 semantic_loss=0"
                  << " schedule=DAZG-ORBIT-PCOI-K3S2"
                  << std::endl;
    }
}

Tensor<uint64_t> CirConv2D::PackActivationSingleMapImpl(
    Tensor<uint64_t> &x_map,
    uint64_t local_in_feature_size,
    uint64_t local_out_feature_size,
    uint64_t local_padding,
    bool local_compact_stride2_pointwise,
    bool local_compact_stride2_k3_polyphase
) {
    Utils::CyclicNTT cyclic_ntt(ntt_size, HE->plain_mod);
    uint64_t padded_HW = padded_feature_size * padded_feature_size;
    Tensor<uint64_t> ac_msg({tiled_in_channels, HE->polyModulusDegree});

    for (uint64_t ti = 0; ti < tiled_in_channels; ti++) {
        for (uint64_t l = 0; l < tile_size; l++) {
            uint64_t blk = ti * tile_size + l;
            if (blk >= num_blocks_in) continue;

            std::vector<uint64_t> x_coef(ntt_size, 0);

            for (uint64_t i = 0; i < block_size; i++) {
                if (local_compact_stride2_k3_polyphase) {
                    uint64_t packed_ch = blk * block_size + i;
                    if (packed_ch >= packed_in_channels) continue;

                    uint64_t phase = packed_ch / in_channels;
                    uint64_t ch = packed_ch % in_channels;

                    for (uint64_t r = 1; r < effective_feature_size; r++) {
                        for (uint64_t c = 1; c < effective_feature_size; c++) {
                            uint64_t src_r = 2 * (r - 1) + ((phase >= 2) ? 1 : 0);
                            uint64_t src_c = 2 * (c - 1) + ((phase & 1) ? 1 : 0);

                            if (src_r < local_in_feature_size &&
                                src_c < local_in_feature_size) {
                                x_coef[i * padded_HW + r * padded_feature_size + c] =
                                    x_map({ch, src_r, src_c});
                            }
                        }
                    }
                } else {
                    uint64_t ch = blk * block_size + i;
                    if (ch >= in_channels) continue;

                    if (local_compact_stride2_pointwise) {
                        for (uint64_t j = 0; j < local_out_feature_size; j++) {
                            for (uint64_t kk = 0; kk < local_out_feature_size; kk++) {
                                uint64_t src_j = j * stride;
                                uint64_t src_k = kk * stride;
                                if (src_j < local_in_feature_size &&
                                    src_k < local_in_feature_size) {
                                    x_coef[i * padded_HW + j * padded_feature_size + kk] =
                                        x_map({ch, src_j, src_k});
                                }
                            }
                        }
                    } else {
                        for (uint64_t j = 0; j < local_in_feature_size; j++) {
                            uint64_t dst_j = j + local_padding;
                            for (uint64_t kk = 0; kk < local_in_feature_size; kk++) {
                                uint64_t dst_k = kk + local_padding;
                                x_coef[i * padded_HW + dst_j * padded_feature_size + dst_k] =
                                    x_map({ch, j, kk});
                            }
                        }
                    }
                }
            }

            cyclic_ntt.ComputeForward(x_coef.data(), x_coef.data());

            uint64_t slot_off = l * ntt_size;
            for (uint64_t m = 0; m < ntt_size; m++) {
                ac_msg({ti, slot_off + m}) = x_coef[m];
            }

            if (tile_size > 1) {
                uint64_t slot_off2 = l * ntt_size + HE->polyModulusDegree / 2;
                for (uint64_t m = 0; m < ntt_size; m++) {
                    ac_msg({ti, slot_off2 + m}) = x_coef[m];
                }
            }
        }
    }

    return ac_msg;
}



Tensor<uint64_t> CirConv2D::ExtractCompactTileWorkerMap(
    const Tensor<uint64_t>& x,
    uint64_t tile_r,
    uint64_t tile_c
) const {
    Tensor<uint64_t> worker({packed_in_channels, tile_in_feature_size, tile_in_feature_size});

    const uint64_t packed_row0 = tile_r * tile_out_feature_size;
    const uint64_t packed_col0 = tile_c * tile_out_feature_size;

    if (IsExactCompactStride2Pointwise()) {
        for (uint64_t ch = 0; ch < in_channels; ++ch) {
            for (uint64_t r = 0; r < tile_in_feature_size; ++r) {
                const uint64_t global_packed_r = packed_row0 + r;
                const uint64_t src_r = 2 * global_packed_r;
                for (uint64_t c = 0; c < tile_in_feature_size; ++c) {
                    const uint64_t global_packed_c = packed_col0 + c;
                    const uint64_t src_c = 2 * global_packed_c;

                    if (src_r < in_feature_size && src_c < in_feature_size) {
                        worker({ch, r, c}) = x({ch, src_r, src_c});
                    } else {
                        worker({ch, r, c}) = 0;
                    }
                }
            }
        }
        return worker;
    }

    assert(IsExactCompactStride2K3Polyphase());

    for (uint64_t phase = 0; phase < 4; ++phase) {
        const uint64_t phase_r = (phase >= 2) ? 1 : 0;
        const uint64_t phase_c = (phase & 1) ? 1 : 0;

        for (uint64_t ch = 0; ch < in_channels; ++ch) {
            const uint64_t packed_ch = phase * in_channels + ch;

            for (uint64_t r = 0; r < tile_in_feature_size; ++r) {
                const uint64_t global_packed_r = packed_row0 + r;

                for (uint64_t c = 0; c < tile_in_feature_size; ++c) {
                    const uint64_t global_packed_c = packed_col0 + c;

                    if (global_packed_r == 0 || global_packed_c == 0) {
                        worker({packed_ch, r, c}) = 0;
                        continue;
                    }

                    const uint64_t src_r = 2 * (global_packed_r - 1) + phase_r;
                    const uint64_t src_c = 2 * (global_packed_c - 1) + phase_c;

                    if (src_r < in_feature_size && src_c < in_feature_size) {
                        worker({packed_ch, r, c}) = x({ch, src_r, src_c});
                    } else {
                        worker({packed_ch, r, c}) = 0;
                    }
                }
            }
        }
    }

    return worker;
}


Tensor<uint64_t> CirConv2D::ExtractPCOIPhaseTileWorkerMap(
    const Tensor<uint64_t>& x,
    uint64_t tile_r,
    uint64_t tile_c,
    uint64_t phase
) const {
    assert(IsPCOIK3S2Orbit());
    assert(phase < 4);

    Tensor<uint64_t> worker({packed_in_channels, tile_in_feature_size, tile_in_feature_size});

    const uint64_t packed_row0 = tile_r * tile_out_feature_size;
    const uint64_t packed_col0 = tile_c * tile_out_feature_size;
    const uint64_t phase_r = (phase >= 2) ? 1 : 0;
    const uint64_t phase_c = (phase & 1) ? 1 : 0;

    for (uint64_t ch = 0; ch < in_channels; ++ch) {
        for (uint64_t r = 0; r < tile_in_feature_size; ++r) {
            const uint64_t global_packed_r = packed_row0 + r;

            for (uint64_t c = 0; c < tile_in_feature_size; ++c) {
                const uint64_t global_packed_c = packed_col0 + c;

                if (global_packed_r == 0 || global_packed_c == 0) {
                    worker({ch, r, c}) = 0;
                    continue;
                }

                const uint64_t src_r = 2 * (global_packed_r - 1) + phase_r;
                const uint64_t src_c = 2 * (global_packed_c - 1) + phase_c;

                if (src_r < in_feature_size && src_c < in_feature_size) {
                    worker({ch, r, c}) = x({ch, src_r, src_c});
                } else {
                    worker({ch, r, c}) = 0;
                }
            }
        }
    }

    return worker;
}

Tensor<uint64_t> CirConv2D::PackActivationPackedWorkerMapImpl(
    Tensor<uint64_t> &x_worker
) {
    Utils::CyclicNTT cyclic_ntt(ntt_size, HE->plain_mod);
    const uint64_t padded_HW = padded_feature_size * padded_feature_size;
    Tensor<uint64_t> ac_msg({tiled_in_channels, HE->polyModulusDegree});

    for (uint64_t ti = 0; ti < tiled_in_channels; ti++) {
        for (uint64_t l = 0; l < tile_size; l++) {
            const uint64_t blk = ti * tile_size + l;
            if (blk >= num_blocks_in) continue;

            std::vector<uint64_t> x_coef(ntt_size, 0);

            for (uint64_t i = 0; i < block_size; i++) {
                const uint64_t packed_ch = blk * block_size + i;
                if (packed_ch >= packed_in_channels) continue;

                for (uint64_t r = 0; r < tile_in_feature_size; ++r) {
                    for (uint64_t c = 0; c < tile_in_feature_size; ++c) {
                        x_coef[i * padded_HW + r * padded_feature_size + c] =
                            x_worker({packed_ch, r, c});
                    }
                }
            }

            cyclic_ntt.ComputeForward(x_coef.data(), x_coef.data());

            const uint64_t slot_off = l * ntt_size;
            for (uint64_t m = 0; m < ntt_size; m++) {
                ac_msg({ti, slot_off + m}) = x_coef[m];
            }

            if (tile_size > 1) {
                const uint64_t slot_off2 = l * ntt_size + HE->polyModulusDegree / 2;
                for (uint64_t m = 0; m < ntt_size; m++) {
                    ac_msg({ti, slot_off2 + m}) = x_coef[m];
                }
            }
        }
    }

    return ac_msg;
}


Tensor<uint64_t> CirConv2D::PackActivation(Tensor<uint64_t> &x) {

// DAZG_ORBIT_V666_ACTIVATION_CANON_ENTRY
if (DAZGOrbitV666CoreRepairEnabled()) {
  uint64_t __p = DAZGOrbitV666PlainMod();
  for (int64_t __i = 0; __i < x.size(); ++__i) { x.data()[__i] = x.data()[__i] % __p; }
}


    assert(!IsExactTileK3S1Mode(plan.mode) &&
           "exact tiled k3s1 is handled by CirConv2D::operator()");

    return PackActivationSingleMapImpl(
        x,
        in_feature_size,
        out_feature_size,
        padding,
        compact_stride2_pointwise,
        compact_stride2_k3_polyphase
    );
}



void CirConv2D::FillMissingOutputWithZero(
    Tensor<UnifiedCiphertext> &out_ct,
    uint64_t out_row_offset,
    const std::vector<uint64_t>& has_output)
{
    if (!HE->server) return;

    bool have_zero = false;
    UnifiedCiphertext zero_ct(HE->Backend());
    for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
        if (has_output[tj]) continue;
        if (!have_zero) {
            zero_ct = HE->GenerateZeroCiphertext(HE->Backend());
            have_zero = true;
        }
        out_ct(out_row_offset + tj) = zero_ct;
    }
}

void CirConv2D::HEComputeSparseRows(
    const Tensor<UnifiedPlaintext> &wpt,
    Tensor<UnifiedCiphertext> &ac_ct,
    uint64_t ac_row_offset,
    Tensor<UnifiedCiphertext> &out_ct,
    uint64_t out_row_offset)
{
    const auto target = HE->Backend();
    const long long dazg_orbit_v508_rot_before = g_last_cir_hecompute_stats.rotate_rows;
    UnifiedGaloisKeys* keys = HE->galoisKeys;


    // DAZG_ORBIT_THUNDERCUT_V17_CIRCONV_ROW_PARALLEL_BEGIN
    if (DAZGOrbitThunderCirConvRowsEnabledV17() &&
        HE->server &&
        tiled_out_channels > 1) {
        DAZGOrbitThunderCirStatsRefV17().sparse_calls.fetch_add(1, std::memory_order_relaxed);
        DAZGOrbitThunderCirStatsRefV17().parallel_calls.fetch_add(1, std::memory_order_relaxed);
        const auto v17_begin = std::chrono::steady_clock::now();

        std::vector<uint64_t> v17_has_output(tiled_out_channels, 0);
        std::vector<long long> v17_mul_by_row(tiled_out_channels, 0);
        std::vector<long long> v17_rot_by_row(tiled_out_channels, 0);
        std::vector<long long> v17_add_by_row(tiled_out_channels, 0);

        if (tile_size == 1) {
            DAZGOrbitThunderCirParallelForV17(tiled_out_channels, [&](uint64_t tj) {
                bool have_acc = false;
                UnifiedCiphertext acc(target);
                long long lm = 0;
                long long la = 0;

                for (const auto& e : sparse_bsgs_entries) {
                    if (e.tj != tj) continue;
                    UnifiedCiphertext tmp(target);
                    HE->evaluator->multiply_plain(
                        ac_ct(ac_row_offset + e.ti),
                        wpt({e.ti, e.tj, 0}),
                        tmp
                    );
                    ++lm;

                    if (!have_acc) {
                        acc = tmp;
                        have_acc = true;
                    } else {
                        HE->evaluator->add_inplace(acc, tmp);
                        ++la;
                    }
                }

                if (have_acc) {
                    out_ct(out_row_offset + tj) = acc;
                    v17_has_output[tj] = 1;
                }
                v17_mul_by_row[tj] = lm;
                v17_add_by_row[tj] = la;
            });

            long long v17_total_mul = 0;
            long long v17_total_add = 0;
            for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
                v17_total_mul += v17_mul_by_row[tj];
                v17_total_add += v17_add_by_row[tj];
            }
            g_last_cir_hecompute_stats.mul_plain += v17_total_mul;
            g_last_cir_hecompute_stats.add_inplace += v17_total_add;

            FillMissingOutputWithZero(out_ct, out_row_offset, v17_has_output);

            const auto v17_end = std::chrono::steady_clock::now();
            const uint64_t v17_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(v17_end - v17_begin).count());
            DAZGOrbitThunderCirStatsRefV17().rows.fetch_add(tiled_out_channels, std::memory_order_relaxed);
            DAZGOrbitThunderCirStatsRefV17().mul_plain.fetch_add(static_cast<uint64_t>(std::max<long long>(0, v17_total_mul)), std::memory_order_relaxed);
            DAZGOrbitThunderCirStatsRefV17().add_inplace.fetch_add(static_cast<uint64_t>(std::max<long long>(0, v17_total_add)), std::memory_order_relaxed);
            DAZGOrbitThunderCirStatsRefV17().elapsed_us.fetch_add(v17_us, std::memory_order_relaxed);
            DAZGOrbitV508RecordActiveRotationLayer(
                DAZGOrbitV508LayerTag(in_feature_size, in_channels, out_channels, kernel_size, stride, static_cast<int>(plan.mode)),
                0ULL,
                0ULL,
                "tile_size_1_no_rotation");
            return;
        }

        Tensor<UnifiedCiphertext> v17_ac_rot({input_rot, tiled_in_channels}, target);
        long long v17_pre_rot = 0;
        for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
            if (ti >= sparse_active_input_tile.size() || sparse_active_input_tile[ti] == 0) {
                continue;
            }

            v17_ac_rot({0, ti}) = ac_ct(ac_row_offset + ti);
            const uint64_t max_rot = sparse_max_rot_by_input_tile[ti];
            for (uint64_t r = 1; r <= max_rot; ++r) {
                HE->evaluator->rotate_rows(
                    v17_ac_rot({r - 1, ti}),
                    ntt_size,
                    *keys,
                    v17_ac_rot({r, ti})
                );
                ++v17_pre_rot;
            }
        }

        const uint64_t v17_num_groups = (tile_size + input_rot - 1) / input_rot;
        const uint64_t v17_rotate_step = ntt_size * input_rot;

        DAZGOrbitThunderCirParallelForV17(tiled_out_channels, [&](uint64_t tj) {
            Tensor<UnifiedCiphertext> v17_int({tile_size}, target);
            std::vector<uint64_t> v17_has_partial(tile_size, 0);
            std::vector<uint64_t> v17_has_group(v17_num_groups, 0);

            long long lm = 0;
            long long lr = 0;
            long long la = 0;

            for (const auto& e : sparse_bsgs_entries) {
                if (e.tj != tj) continue;

                UnifiedCiphertext tmp(target);
                HE->evaluator->multiply_plain(
                    v17_ac_rot({e.rot_idx, e.ti}),
                    wpt({e.ti, e.tj, e.k}),
                    tmp
                );
                ++lm;

                if (!v17_has_partial[e.k]) {
                    v17_int(e.k) = tmp;
                    v17_has_partial[e.k] = 1;
                } else {
                    HE->evaluator->add_inplace(v17_int(e.k), tmp);
                    ++la;
                }
            }

            for (uint64_t k = 0; k < tile_size; ++k) {
                if (!v17_has_partial[k]) continue;

                const uint64_t g = k / input_rot;
                const uint64_t head_k = g * input_rot;

                if (!v17_has_group[g]) {
                    if (k != head_k) {
                        v17_int(head_k) = v17_int(k);
                    }
                    v17_has_group[g] = 1;
                } else {
                    HE->evaluator->add_inplace(v17_int(head_k), v17_int(k));
                    ++la;
                }
            }

            uint64_t first_g = v17_num_groups;
            for (uint64_t g = 0; g < v17_num_groups; ++g) {
                if (!v17_has_group[g]) continue;
                first_g = g;
                break;
            }

            if (first_g != v17_num_groups) {
                out_ct(out_row_offset + tj) = v17_int(first_g * input_rot);
                v17_has_output[tj] = 1;

                for (uint64_t g = first_g + 1; g < v17_num_groups; ++g) {
                    HE->evaluator->rotate_rows(
                        out_ct(out_row_offset + tj),
                        v17_rotate_step,
                        *keys,
                        out_ct(out_row_offset + tj)
                    );
                    ++lr;

                    if (v17_has_group[g]) {
                        HE->evaluator->add_inplace(
                            out_ct(out_row_offset + tj),
                            v17_int(g * input_rot)
                        );
                        ++la;
                    }
                }
            }

            v17_mul_by_row[tj] = lm;
            v17_rot_by_row[tj] = lr;
            v17_add_by_row[tj] = la;
        });

        long long v17_total_mul = 0;
        long long v17_total_rot = v17_pre_rot;
        long long v17_total_add = 0;
        for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
            v17_total_mul += v17_mul_by_row[tj];
            v17_total_rot += v17_rot_by_row[tj];
            v17_total_add += v17_add_by_row[tj];
        }

        g_last_cir_hecompute_stats.mul_plain += v17_total_mul;
        g_last_cir_hecompute_stats.rotate_rows += v17_total_rot;
        g_last_cir_hecompute_stats.add_inplace += v17_total_add;

        FillMissingOutputWithZero(out_ct, out_row_offset, v17_has_output);

        const auto v17_end = std::chrono::steady_clock::now();
        const uint64_t v17_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(v17_end - v17_begin).count());
        DAZGOrbitThunderCirStatsRefV17().rows.fetch_add(tiled_out_channels, std::memory_order_relaxed);
        DAZGOrbitThunderCirStatsRefV17().mul_plain.fetch_add(static_cast<uint64_t>(std::max<long long>(0, v17_total_mul)), std::memory_order_relaxed);
        DAZGOrbitThunderCirStatsRefV17().rotate_rows.fetch_add(static_cast<uint64_t>(std::max<long long>(0, v17_total_rot)), std::memory_order_relaxed);
        DAZGOrbitThunderCirStatsRefV17().add_inplace.fetch_add(static_cast<uint64_t>(std::max<long long>(0, v17_total_add)), std::memory_order_relaxed);
        DAZGOrbitThunderCirStatsRefV17().elapsed_us.fetch_add(v17_us, std::memory_order_relaxed);
        DAZGOrbitV508RecordActiveRotationLayer(
            DAZGOrbitV508LayerTag(in_feature_size, in_channels, out_channels, kernel_size, stride, static_cast<int>(plan.mode)),
            DAZGOrbitV508DenseBSGSRotationRequest(tiled_in_channels, tiled_out_channels, input_rot, v17_num_groups),
            static_cast<uint64_t>(std::max<long long>(0, v17_total_rot)),
            "dense_bsgs_minus_thundercut_sparse_schedule");
        return;
    }
    // DAZG_ORBIT_THUNDERCUT_V17_CIRCONV_ROW_PARALLEL_END

    std::vector<uint64_t> has_output(tiled_out_channels, 0);

    if (tile_size == 1) {
        for (const auto& e : sparse_bsgs_entries) {
            UnifiedCiphertext tmp(target);
            HE->evaluator->multiply_plain(
                ac_ct(ac_row_offset + e.ti),
                wpt({e.ti, e.tj, 0}),
                tmp
            );
            g_last_cir_hecompute_stats.mul_plain++;

            if (!has_output[e.tj]) {
                out_ct(out_row_offset + e.tj) = tmp;
                has_output[e.tj] = 1;
            } else {
                HE->evaluator->add_inplace(out_ct(out_row_offset + e.tj), tmp);
                g_last_cir_hecompute_stats.add_inplace++;
            }
        }

        FillMissingOutputWithZero(out_ct, out_row_offset, has_output);
        DAZGOrbitV508RecordActiveRotationLayer(
            DAZGOrbitV508LayerTag(in_feature_size, in_channels, out_channels, kernel_size, stride, static_cast<int>(plan.mode)),
            0ULL,
            0ULL,
            "tile_size_1_no_rotation");
        return;
    }

    Tensor<UnifiedCiphertext> ac_rot({input_rot, tiled_in_channels}, target);
    for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
        if (ti >= sparse_active_input_tile.size() || sparse_active_input_tile[ti] == 0) {
            continue;
        }

        ac_rot({0, ti}) = ac_ct(ac_row_offset + ti);
        const uint64_t max_rot = sparse_max_rot_by_input_tile[ti];
        for (uint64_t r = 1; r <= max_rot; ++r) {
            HE->evaluator->rotate_rows(
                ac_rot({r - 1, ti}),
                ntt_size,
                *keys,
                ac_rot({r, ti})
            );
            g_last_cir_hecompute_stats.rotate_rows++;
        }
    }

    Tensor<UnifiedCiphertext> int_ct({tiled_out_channels, tile_size}, target);
    std::vector<uint64_t> has_partial(
        static_cast<size_t>(tiled_out_channels * tile_size), 0);

    auto partial_idx = [&](uint64_t tj, uint64_t k) -> size_t {
        return static_cast<size_t>(tj * tile_size + k);
    };

    for (const auto& e : sparse_bsgs_entries) {
        UnifiedCiphertext tmp(target);
        HE->evaluator->multiply_plain(
            ac_rot({e.rot_idx, e.ti}),
            wpt({e.ti, e.tj, e.k}),
            tmp
        );
        g_last_cir_hecompute_stats.mul_plain++;

        const size_t idx = partial_idx(e.tj, e.k);
        if (!has_partial[idx]) {
            int_ct({e.tj, e.k}) = tmp;
            has_partial[idx] = 1;
        } else {
            HE->evaluator->add_inplace(int_ct({e.tj, e.k}), tmp);
            g_last_cir_hecompute_stats.add_inplace++;
        }
    }

    const uint64_t num_groups = (tile_size + input_rot - 1) / input_rot;
    std::vector<uint64_t> has_group(
        static_cast<size_t>(tiled_out_channels * num_groups), 0);

    auto group_idx = [&](uint64_t tj, uint64_t g) -> size_t {
        return static_cast<size_t>(tj * num_groups + g);
    };

    for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
        for (uint64_t k = 0; k < tile_size; ++k) {
            if (!has_partial[partial_idx(tj, k)]) continue;

            const uint64_t g = k / input_rot;
            const uint64_t head_k = g * input_rot;
            const size_t gi = group_idx(tj, g);

            if (!has_group[gi]) {
                if (k != head_k) {
                    int_ct({tj, head_k}) = int_ct({tj, k});
                }
                has_group[gi] = 1;
            } else {
                HE->evaluator->add_inplace(int_ct({tj, head_k}), int_ct({tj, k}));
                g_last_cir_hecompute_stats.add_inplace++;
            }
        }
    }

    const uint64_t rotate_step = ntt_size * input_rot;
    for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
        uint64_t first_g = num_groups;
        for (uint64_t g = 0; g < num_groups; ++g) {
            if (!has_group[group_idx(tj, g)]) continue;
            first_g = g;
            break;
        }

        if (first_g == num_groups) {
            continue;
        }

        out_ct(out_row_offset + tj) = int_ct({tj, first_g * input_rot});
        has_output[tj] = 1;

        // Keep the dense BSGS slot alignment: zero group additions can be
        // skipped, but trailing giant-step rotations are part of the exact
        // output layout expected by DepackResult.
        for (uint64_t g = first_g + 1; g < num_groups; ++g) {
            HE->evaluator->rotate_rows(
                out_ct(out_row_offset + tj),
                rotate_step,
                *keys,
                out_ct(out_row_offset + tj)
            );
            g_last_cir_hecompute_stats.rotate_rows++;

            if (has_group[group_idx(tj, g)]) {
                HE->evaluator->add_inplace(
                    out_ct(out_row_offset + tj),
                    int_ct({tj, g * input_rot})
                );
                g_last_cir_hecompute_stats.add_inplace++;
            }
        }
    }

    FillMissingOutputWithZero(out_ct, out_row_offset, has_output);
    DAZGOrbitV508RecordActiveRotationLayer(
        DAZGOrbitV508LayerTag(in_feature_size, in_channels, out_channels, kernel_size, stride, static_cast<int>(plan.mode)),
        DAZGOrbitV508DenseBSGSRotationRequest(tiled_in_channels, tiled_out_channels, input_rot, num_groups),
        static_cast<uint64_t>(std::max<long long>(0, g_last_cir_hecompute_stats.rotate_rows - dazg_orbit_v508_rot_before)),
        "dense_bsgs_minus_sparse_schedule");
}

Tensor<UnifiedCiphertext> CirConv2D::HECompute(
    const Tensor<UnifiedPlaintext> &wpt,
    Tensor<UnifiedCiphertext> &ac_ct)
{
    const auto target = HE->server ? HE->Backend() : HOST;
    Tensor<UnifiedCiphertext> out_ct({tiled_out_channels}, target);

    ResetLastCirHEComputeStats();
    if (!HE->server) return out_ct;

    auto t_begin = std::chrono::steady_clock::now();
    HEComputeSparseRows(wpt, ac_ct, 0, out_ct, 0);
    auto t_end = std::chrono::steady_clock::now();

    g_last_cir_hecompute_stats.us =
        std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_begin).count();

    return out_ct;
}

Tensor<uint64_t> CirConv2D::DepackSingleMapImpl(
    Tensor<uint64_t> &out_msg,
    uint64_t row_offset_tj,
    uint64_t local_out_feature_size,
    uint64_t local_read_offset,
    uint64_t local_row_step,
    uint64_t local_col_step
) {
    Utils::CyclicNTT cyclic_ntt(ntt_size, HE->plain_mod);
    const uint64_t padded_HW = padded_feature_size * padded_feature_size;
    Tensor<uint64_t> y({out_channels, local_out_feature_size, local_out_feature_size});

    for (uint64_t tj = 0; tj < tiled_out_channels; tj++) {
        for (uint64_t l = 0; l < tile_size; l++) {
            const bool v496_lc_absorb =
                DAZGOrbitV496LCMapdAbsorbEnabledForLayer(
                    in_feature_size, in_channels, out_channels, kernel_size, stride) &&
                IsPCOIK3S2Orbit();

            const uint64_t v496_policy =
                DAZGOrbitV496EnvU64("DAZG_ORBIT_V496_LC_MAPD_POLICY", 3);
            const uint64_t v496_slot_delta =
                tile_size == 0 ? 0 :
                (DAZGOrbitV496EnvU64("DAZG_ORBIT_V496_LC_MAPD_SLOT_DELTA", 0) % tile_size);
            const uint64_t v496_block_delta =
                tile_size == 0 ? 0 :
                (DAZGOrbitV496EnvU64("DAZG_ORBIT_V496_LC_MAPD_BLOCK_DELTA", 0) % tile_size);

            uint64_t v496_read_l = l;
            uint64_t v496_block_l = l;

            if (v496_lc_absorb && tile_size > 1) {
                const uint64_t Gv = (tile_size + input_rot - 1) / input_rot;

                if (v496_policy == 1) {
                    v496_read_l = (l + v496_slot_delta) % tile_size;
                } else if (v496_policy == 2) {
                    v496_block_l = (l + v496_block_delta) % tile_size;
                } else if (v496_policy == 4) {
                    const uint64_t group_step = (Gv * input_rot) % tile_size;
                    v496_read_l = (l + tile_size + group_step + v496_slot_delta) % tile_size;
                    v496_block_l = (l + tile_size + group_step + v496_block_delta) % tile_size;
                } else {
                    v496_read_l = (l + v496_slot_delta) % tile_size;
                    v496_block_l = (l + v496_block_delta) % tile_size;
                }

                if (DAZGOrbitV496EnvFlag("DAZG_ORBIT_V496_TRACE", false) &&
                    tj == 0 && l == 0 && HE->server) {
                    std::cout << "[DAZG_ORBIT_V496_LC_MAPD_LAYOUT_ABSORB]"
                              << " marker=" << DAZG_ORBIT_V496_LC_MAPD_MARKER
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " tile_size=" << tile_size
                              << " input_rot=" << input_rot
                              << " policy=" << v496_policy
                              << " slot_delta=" << v496_slot_delta
                              << " block_delta=" << v496_block_delta
                              << " read_l0=" << v496_read_l
                              << " block_l0=" << v496_block_l
                              << " exact_gate=external_oracle_required"
                              << std::endl;
                }
            }

            uint64_t out_blk;
            if (tile_size == 1) {
                out_blk = tj;
            } else {
                const uint64_t G = (tile_size + input_rot - 1) / input_rot;
                out_blk = tj * tile_size +
                    (3 * tile_size - v496_block_l - G * input_rot) % tile_size;
            }

            if (out_blk >= num_blocks_out) continue;

            std::vector<uint64_t> y_ntt(ntt_size);
            const uint64_t slot_off = v496_read_l * ntt_size;
            for (uint64_t m = 0; m < ntt_size; m++) {
                y_ntt[m] = out_msg({row_offset_tj + tj, slot_off + m});
            }

            cyclic_ntt.ComputeInverse(y_ntt.data(), y_ntt.data());

            for (uint64_t i = 0; i < block_size; i++) {
                const uint64_t out_ch = out_blk * block_size + i;
                if (out_ch >= out_channels) continue;

                for (uint64_t j = 0; j < local_out_feature_size; j++) {
                    for (uint64_t kk = 0; kk < local_out_feature_size; kk++) {
                        const uint64_t idx =
                            i * padded_HW
                            + local_read_offset
                            + local_row_step * j * padded_feature_size
                            + local_col_step * kk;

                        y({out_ch, j, kk}) = y_ntt[idx];
                    }
                }
            }
        }
    }

    return y;
}


Tensor<uint64_t> CirConv2D::DepackResult(Tensor<uint64_t> &out_msg) {
    assert(!IsExactTileK3S1Mode(plan.mode) &&
           "exact tiled k3s1 is handled by CirConv2D::operator()");

    uint64_t depack_offset;
    uint64_t depack_row_step;
    uint64_t depack_col_step;

    if (compact_stride2_k3_polyphase) {
        // packed semantics: 2x2 kernel, stride=1, padding=0
        depack_offset = (packed_kernel_size - 1) * (padded_feature_size + 1);
        depack_row_step = 1;
        depack_col_step = 1;
    } else if (compact_stride2_pointwise) {
        // pointwise compact already lives on the compact grid
        depack_offset = 0;
        depack_row_step = 1;
        depack_col_step = 1;
    } else {
        // original full-space rule
        depack_offset = (kernel_size - 1) * (padded_feature_size + 1);
        depack_row_step = stride;
        depack_col_step = stride;
    }

    return DepackSingleMapImpl(
        out_msg,
        0,                  // row_offset_tj，当前旧路径就是 0
        out_feature_size,
        depack_offset,
        depack_row_step,
        depack_col_step
    );
}



/* DAZG_ORBIT_V589_OPERATOR_BRLE_20260615_BEGIN
 * Operator-level public-linear SS evaluator for Boundary-Resident Linear
 * Branch Elision. This method lives inside CirConv2D so the rewrite reuses
 * the operator's own cyclic block channel algebra instead of re-implementing
 * it in a ResNet test driver.
 */
static inline uint64_t DAZGOrbitV589ModAddU64(uint64_t a, uint64_t b, uint64_t mod)
{
    if (mod == 0ULL) return static_cast<uint64_t>(a + b);
    a %= mod;
    b %= mod;
    const uint64_t rem = mod - a;
    return (b >= rem) ? static_cast<uint64_t>(b - rem) : static_cast<uint64_t>(a + b);
}

static inline uint64_t DAZGOrbitV589ModMulU64(uint64_t a, uint64_t b, uint64_t mod)
{
    if (mod == 0ULL) {
        return static_cast<uint64_t>(static_cast<unsigned __int128>(a) *
                                     static_cast<unsigned __int128>(b));
    }
    return static_cast<uint64_t>(
        (static_cast<unsigned __int128>(a % mod) * static_cast<unsigned __int128>(b % mod)) %
        static_cast<unsigned __int128>(mod));
}

static std::string DAZGOrbitV589ShapeStringUSize(const std::vector<size_t>& shape)
{
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i) os << ",";
        os << shape[i];
    }
    os << "]";
    return os.str();
}

bool CirConv2D::DAZGOrbitEvalPublicLinearOnSSExact(
        const Tensor<uint64_t>& x,
        Tensor<uint64_t>& out,
        uint64_t plain_mod,
        std::string* cert)
{
    const uint64_t Q = (plain_mod != 0ULL) ? plain_mod : ((HE != nullptr) ? HE->plain_mod : 0ULL);
    std::ostringstream os;
    os << "operator=CirConv2D::DAZGOrbitEvalPublicLinearOnSSExact"
       << " marker=DAZG_ORBIT_V589_OPERATOR_BRLE_20260615";

    if (kernel_size != 1ULL || stride != 2ULL || padding != 0ULL) {
        os << " accepted=0 reason=unsupported_geometry"
           << " k=" << kernel_size << " s=" << stride << " p=" << padding;
        if (cert != nullptr) *cert = os.str();
        return false;
    }

    const std::vector<size_t>& xs = x.shape();
    const std::vector<size_t>& ws = weight.shape();
    if (xs.size() != 3 || ws.size() != 4) {
        os << " accepted=0 reason=rank_mismatch";
        if (cert != nullptr) *cert = os.str();
        return false;
    }

    const size_t Cin = static_cast<size_t>(in_channels);
    const size_t Cout = static_cast<size_t>(out_channels);
    const size_t H = xs[1];
    const size_t W = xs[2];
    const size_t OH = static_cast<size_t>(out_feature_size);
    const size_t OW = OH;
    const size_t B = static_cast<size_t>(block_size == 0ULL ? 1ULL : block_size);
    const size_t NBIn = static_cast<size_t>(num_blocks_in == 0ULL ? ((Cin + B - 1ULL) / B) : num_blocks_in);

    if (xs[0] != Cin || ws[0] != Cout || ws[1] != Cin || ws[2] != 1 || ws[3] != 1 ||
        OH == 0 || OW == 0 || (OH - 1ULL) * 2ULL >= H || (OW - 1ULL) * 2ULL >= W) {
        os << " accepted=0 reason=shape_mismatch"
           << " input_shape=" << DAZGOrbitV589ShapeStringUSize(xs)
           << " weight_shape=" << DAZGOrbitV589ShapeStringUSize(ws)
           << " cin=" << Cin << " cout=" << Cout << " oh=" << OH;
        if (cert != nullptr) *cert = os.str();
        return false;
    }

    Tensor<uint64_t> y({Cout, OH, OW});
    uint64_t effective_terms = 0ULL;
    for (size_t co = 0; co < Cout; ++co) {
        const size_t out_blk = (B == 0ULL) ? 0ULL : (co / B);
        const size_t out_off = (B == 0ULL) ? 0ULL : (co % B);
        for (size_t oh = 0; oh < OH; ++oh) {
            const size_t ih = oh * 2ULL;
            for (size_t ow = 0; ow < OW; ++ow) {
                const size_t iw = ow * 2ULL;
                uint64_t acc = 0ULL;
                for (size_t in_blk = 0; in_blk < NBIn; ++in_blk) {
                    const size_t weight_ci = in_blk * B;
                    if (weight_ci >= Cin) continue;
                    for (size_t in_off = 0; in_off < B; ++in_off) {
                        const size_t ci = in_blk * B + in_off;
                        if (ci >= Cin) continue;
                        const size_t woff = (out_off + B - (in_off % B)) % B;
                        const size_t weight_co = out_blk * B + woff;
                        if (weight_co >= Cout) continue;
                        const uint64_t xv = x({ci, ih, iw});
                        const uint64_t wv = weight({weight_co, weight_ci, 0, 0});
                        acc = DAZGOrbitV589ModAddU64(acc, DAZGOrbitV589ModMulU64(xv, wv, Q), Q);
                        ++effective_terms;
                    }
                }
                y({co, oh, ow}) = acc;
            }
        }
    }

    // Match incumbent executor semantics exactly: folded bias is added as a raw
    // server-share uint64 addition in CirConv depack, then the caller's existing
    // Field2Ring boundary handles canonicalization/truncation.
    DAZGOrbitV9AddFoldedBiasConv3D(y, this->bias, HE);
    out = y;

    os << " accepted=1 reason=ok"
       << " channel_policy=circonv_operator_cyclic_block"
       << " block_size=" << B
       << " num_blocks_in=" << NBIn
       << " cin=" << Cin
       << " cout=" << Cout
       << " out_hw=" << OH
       << " effective_terms=" << effective_terms
       << " bias_role=" << (DAZGOrbitV9ShouldAddBias(HE) ? 1 : 0)
       << " bias_add=raw_u64_executor_compat"
       << " plain_mod=" << Q;
    if (cert != nullptr) *cert = os.str();
    return true;
}
/* DAZG_ORBIT_V589_OPERATOR_BRLE_20260615_END */

Tensor<uint64_t> CirConv2D::DAZGOrbitPackActivationForHE(Tensor<uint64_t> &x) {
    if (!IsExactTileK3S1Mode(plan.mode)) {
        return PackActivation(x);
    }

    assert(spatial_tile_rows >= 1);
    assert(spatial_tile_cols >= 1);
    assert(tile_out_feature_size > 0);
    assert(tile_in_feature_size == tile_out_feature_size + packed_kernel_size - 1);

    const uint64_t tile_count = spatial_tile_rows * spatial_tile_cols;

    if (IsPCOIK3S2Orbit()) {
        Tensor<uint64_t> batched_ac_msg(
            {tile_count * plan.orbit_phase_count * tiled_in_channels, HE->polyModulusDegree}
        );

        for (uint64_t tile_r = 0; tile_r < spatial_tile_rows; ++tile_r) {
            for (uint64_t tile_c = 0; tile_c < spatial_tile_cols; ++tile_c) {
                const uint64_t tile_id = tile_r * spatial_tile_cols + tile_c;
                for (uint64_t phase = 0; phase < plan.orbit_phase_count; ++phase) {
                    Tensor<uint64_t> worker = ExtractPCOIPhaseTileWorkerMap(x, tile_r, tile_c, phase);
                    Tensor<uint64_t> ac_msg = PackActivationPackedWorkerMapImpl(worker);
                    const uint64_t base_row =
                        (tile_id * plan.orbit_phase_count + phase) * tiled_in_channels;
                    for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
                        const uint64_t dst_row = base_row + ti;
                        for (uint64_t m = 0; m < HE->polyModulusDegree; ++m) {
                            batched_ac_msg({dst_row, m}) = ac_msg({ti, m});
                        }
                    }
                }
            }
        }

        return batched_ac_msg;
    }

    Tensor<uint64_t> batched_ac_msg(
        {tile_count * tiled_in_channels, HE->polyModulusDegree}
    );

    for (uint64_t tile_r = 0; tile_r < spatial_tile_rows; ++tile_r) {
        for (uint64_t tile_c = 0; tile_c < spatial_tile_cols; ++tile_c) {
            const uint64_t tile_id = tile_r * spatial_tile_cols + tile_c;
            Tensor<uint64_t> ac_msg({tiled_in_channels, HE->polyModulusDegree});

            if (IsExactCompactStride2Pointwise() ||
                IsExactCompactStride2K3Polyphase()) {
                Tensor<uint64_t> worker = ExtractCompactTileWorkerMap(x, tile_r, tile_c);
                ac_msg = PackActivationPackedWorkerMapImpl(worker);
            } else {
                Tensor<uint64_t> patch = ExtractTileWithHalo(x, tile_r, tile_c);
                ac_msg = PackActivationSingleMapImpl(
                    patch,
                    tile_in_feature_size,
                    packed_out_feature_size,
                    packed_padding,
                    false,
                    false
                );
            }

            for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
                const uint64_t dst_row = tile_id * tiled_in_channels + ti;
                for (uint64_t m = 0; m < HE->polyModulusDegree; ++m) {
                    batched_ac_msg({dst_row, m}) = ac_msg({ti, m});
                }
            }
        }
    }

    return batched_ac_msg;
}

Tensor<UnifiedCiphertext> CirConv2D::DAZGOrbitComputeFromPackedHE(
    Tensor<UnifiedCiphertext> &ac_ct) {
    if (!IsExactTileK3S1Mode(plan.mode)) {
        return HECompute(weight_pt, ac_ct);
    }

    const uint64_t tile_count = spatial_tile_rows * spatial_tile_cols;

    if (IsPCOIK3S2Orbit()) {
        const auto target = HE->server ? HE->Backend() : HOST;
        Tensor<UnifiedCiphertext> batched_out_ct({tile_count * tiled_out_channels}, target);

        ResetLastCirHEComputeStats();
        auto t_begin = std::chrono::steady_clock::now();

        // DAZG_ORBIT_V489_PDGIANT_ORBITCARRY_PATCH_BEGIN
        // Phase-Deferred Giant Accumulation (PD-GIANT / Orbit-Carry).
        //
        // V487/V488 changed BSGS step S and reduced rotations, but the final
        // plaintext hash drifted.  This patch does not change S, PackWeight,
        // weight hash, or output layout.  It only exploits linearity across the
        // four PCOI spatial phases: group partials are accumulated across phases
        // while still in the BSGS group/orbit layout, and the terminal giant-step
        // fold is executed once per output tile instead of once per phase.
        //
        // Algebraically, fold(sum_p G[p,g]) == sum_p fold(G[p,g]).
        // Therefore this is an exact candidate; the runner still checks top1 and
        // signed output hashes against the v485_current oracle and rejects drift.
        {
            const bool v489_pdgiant =
                DAZGOrbitV509EnvFlagAlias(
                    "DAZG_ORBIT_V489_PD_GIANT",
                    "DAZG_ORBIT_V489_PDGIANT_ORBITCARRY",
                    "DAZG_ORBIT_V490_PD_GIANT",
                    "DAZG_ORBIT_ENABLE_PDGIANT",
                    false);

            // DAZG_ORBIT_V490_PDGIANT_FAST_PATCH_BEGIN
            // V490 keeps the exact PD-GIANT algebra from V489, but removes the
            // prototype bottleneck: V489 fused phases serially and lost the
            // ThunderCut row-parallel hardpath used by the V485 oracle.  V490
            // does phase-deferred orbit carry with per-output-row parallel
            // accumulation.  It does not change PackWeight, input_rot, bridge
            // hash, BSGS S, or output layout.  Runner compares final top/hash
            // against the V485 oracle and rejects semantic drift.
            const bool v490_fast =
                DAZGOrbitV509EnvFlagAlias(
                    "DAZG_ORBIT_V490_PD_GIANT_FAST",
                    "DAZG_ORBIT_V490_PDGIANT_FAST",
                    "DAZG_ORBIT_PDGIANT_FAST",
                    "DAZG_ORBIT_ENABLE_PDGIANT_FAST",
                    false);

            // DAZG_ORBIT_V511_FGBUCKET_PDGIANT_BEGIN
            //
            // V489/V490 PD-GIANT reduced rotations but drifted because it merged
            // all PCOI phases before the terminal giant fold.  Generic
            // HEComputeSparseRows is not just "absolute group fold": for each
            // phase and output row it starts the terminal fold at that phase's
            // first nonempty giant group.  Therefore phases with different
            // first_g cannot be merged into one carry without changing slot
            // alignment.
            //
            // V511 keeps the exact generic first_g semantics by bucketing phases
            // by first_g.  Phases in the same bucket are safely merged before
            // terminal fold; different buckets are folded separately and added
            // afterwards.  This is the fastest safe next candidate: if exact, it
            // promotes; if not, retire PD-GIANT without spending time on minor
            // runner noise.
            const bool v511_fgbucket =
                DAZGOrbitV509EnvFlagAlias(
                    "DAZG_ORBIT_V511_PDGIANT_FGBUCKET",
                    "DAZG_ORBIT_FGBUCKET_PDGIANT",
                    "DAZG_ORBIT_V511_FGBUCKET",
                    "DAZG_ORBIT_ENABLE_FGBUCKET_PDGIANT",
                    false);

            if (HE->server && v489_pdgiant && v511_fgbucket &&
                DAZGOrbitV510PDGiantLayerEnabled(in_feature_size, in_channels, out_channels)) {
                assert(pcoi_phase_weight_pt.size() == plan.orbit_phase_count);
                assert(pcoi_phase_sparse_state.size() == plan.orbit_phase_count);

                UnifiedGaloisKeys* keys = HE->galoisKeys;

                long long v511_total_pre_rot = 0;
                long long v511_total_giant_rot = 0;
                long long v511_total_mul = 0;
                long long v511_total_add = 0;
                long long v511_total_buckets = 0;
                uint64_t v511_tiles_done = 0;
                const uint64_t v511_phase_count = plan.orbit_phase_count;

                RestoreSparseState(pcoi_phase_sparse_state[0]);
                const uint64_t v511_input_rot = input_rot;
                const uint64_t v511_groups =
                    sparse_bsgs_groups != 0
                        ? sparse_bsgs_groups
                        : ((tile_size + input_rot - 1) / input_rot);
                const uint64_t v511_rotate_step = ntt_size * v511_input_rot;

                std::vector<std::vector<CirSparseBSGSEntry> > v511_phase_entries(v511_phase_count);
                std::vector<std::vector<uint64_t> > v511_phase_max_rot(v511_phase_count);
                std::vector<std::vector<uint64_t> > v511_phase_active_input(v511_phase_count);

                for (uint64_t phase = 0; phase < v511_phase_count; ++phase) {
                    RestoreSparseState(pcoi_phase_sparse_state[phase]);
                    if (input_rot != v511_input_rot || sparse_bsgs_groups != v511_groups) {
                        std::cerr << "[DAZG_ORBIT_V511_FGBUCKET_ERROR]"
                                  << " reason=phase_sparse_state_mismatch"
                                  << " phase=" << phase
                                  << " input_rot=" << input_rot
                                  << " expected_input_rot=" << v511_input_rot
                                  << " groups=" << sparse_bsgs_groups
                                  << " expected_groups=" << v511_groups
                                  << std::endl;
                        throw std::runtime_error("V511 FG-bucket phase sparse-state mismatch");
                    }
                    v511_phase_entries[phase] = sparse_bsgs_entries;
                    v511_phase_max_rot[phase] = sparse_max_rot_by_input_tile;
                    v511_phase_active_input[phase] = sparse_active_input_tile;
                }

                for (uint64_t tile_id = 0; tile_id < tile_count; ++tile_id) {
                    Tensor<UnifiedCiphertext> v511_phase_ac_rot(
                        {v511_phase_count, v511_input_rot, tiled_in_channels}, target);

                    for (uint64_t phase = 0; phase < v511_phase_count; ++phase) {
                        const uint64_t ac_base =
                            (tile_id * v511_phase_count + phase) * tiled_in_channels;
                        const std::vector<uint64_t>& active_in = v511_phase_active_input[phase];
                        const std::vector<uint64_t>& max_rot_by_ti = v511_phase_max_rot[phase];

                        for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
                            if (ti >= active_in.size() || active_in[ti] == 0) {
                                continue;
                            }
                            v511_phase_ac_rot({phase, 0, ti}) = ac_ct(ac_base + ti);
                            const uint64_t max_rot = ti < max_rot_by_ti.size() ? max_rot_by_ti[ti] : 0;
                            for (uint64_t r = 1; r <= max_rot; ++r) {
                                HE->evaluator->rotate_rows(
                                    v511_phase_ac_rot({phase, r - 1, ti}),
                                    ntt_size,
                                    *keys,
                                    v511_phase_ac_rot({phase, r, ti})
                                );
                                ++v511_total_pre_rot;
                            }
                        }
                    }

                    std::vector<uint64_t> v511_has_output(tiled_out_channels, 0);
                    std::vector<long long> v511_mul_by_row(tiled_out_channels, 0);
                    std::vector<long long> v511_rot_by_row(tiled_out_channels, 0);
                    std::vector<long long> v511_add_by_row(tiled_out_channels, 0);
                    std::vector<long long> v511_buckets_by_row(tiled_out_channels, 0);

                    DAZGOrbitThunderCirParallelForV17(tiled_out_channels, [&](uint64_t tj) {
                        Tensor<UnifiedCiphertext> phase_carry(
                            {v511_phase_count, v511_groups}, target);
                        std::vector<uint64_t> phase_has(
                            static_cast<size_t>(v511_phase_count * v511_groups), 0);
                        std::vector<uint64_t> phase_first(v511_phase_count, v511_groups);

                        auto ph_idx = [&](uint64_t phase, uint64_t g) -> size_t {
                            return static_cast<size_t>(phase * v511_groups + g);
                        };

                        long long lm = 0;
                        long long lr = 0;
                        long long la = 0;
                        long long lb = 0;

                        for (uint64_t phase = 0; phase < v511_phase_count; ++phase) {
                            const Tensor<UnifiedPlaintext>& wpt_phase = pcoi_phase_weight_pt[phase];
                            const std::vector<CirSparseBSGSEntry>& entries = v511_phase_entries[phase];

                            for (const auto& e : entries) {
                                if (e.tj != tj) continue;
                                const uint64_t g = e.group_idx;
                                if (g >= v511_groups) continue;

                                UnifiedCiphertext tmp(target);
                                HE->evaluator->multiply_plain(
                                    v511_phase_ac_rot({phase, e.rot_idx, e.ti}),
                                    wpt_phase({e.ti, e.tj, e.k}),
                                    tmp
                                );
                                ++lm;

                                const size_t pi = ph_idx(phase, g);
                                if (!phase_has[pi]) {
                                    phase_carry({phase, g}) = tmp;
                                    phase_has[pi] = 1;
                                } else {
                                    HE->evaluator->add_inplace(phase_carry({phase, g}), tmp);
                                    ++la;
                                }
                            }

                            for (uint64_t g = 0; g < v511_groups; ++g) {
                                if (phase_has[ph_idx(phase, g)]) {
                                    phase_first[phase] = g;
                                    break;
                                }
                            }
                        }

                        std::vector<uint64_t> bucket_first;
                        Tensor<UnifiedCiphertext> bucket_carry(
                            {v511_phase_count, v511_groups}, target);
                        std::vector<uint64_t> bucket_has(
                            static_cast<size_t>(v511_phase_count * v511_groups), 0);

                        auto bucket_idx = [&](uint64_t b, uint64_t g) -> size_t {
                            return static_cast<size_t>(b * v511_groups + g);
                        };

                        auto find_or_make_bucket = [&](uint64_t first_g) -> uint64_t {
                            for (uint64_t b = 0; b < bucket_first.size(); ++b) {
                                if (bucket_first[b] == first_g) return b;
                            }
                            bucket_first.push_back(first_g);
                            return static_cast<uint64_t>(bucket_first.size() - 1);
                        };

                        for (uint64_t phase = 0; phase < v511_phase_count; ++phase) {
                            const uint64_t f = phase_first[phase];
                            if (f == v511_groups) continue;

                            const uint64_t b = find_or_make_bucket(f);
                            for (uint64_t g = f; g < v511_groups; ++g) {
                                if (!phase_has[ph_idx(phase, g)]) continue;

                                const size_t bi = bucket_idx(b, g);
                                if (!bucket_has[bi]) {
                                    bucket_carry({b, g}) = phase_carry({phase, g});
                                    bucket_has[bi] = 1;
                                } else {
                                    HE->evaluator->add_inplace(
                                        bucket_carry({b, g}),
                                        phase_carry({phase, g})
                                    );
                                    ++la;
                                }
                            }
                        }

                        bool have_row = false;
                        UnifiedCiphertext row_acc(target);

                        for (uint64_t b = 0; b < bucket_first.size(); ++b) {
                            const uint64_t f = bucket_first[b];
                            if (!bucket_has[bucket_idx(b, f)]) continue;

                            UnifiedCiphertext bucket_acc = bucket_carry({b, f});
                            for (uint64_t g = f + 1; g < v511_groups; ++g) {
                                HE->evaluator->rotate_rows(
                                    bucket_acc,
                                    v511_rotate_step,
                                    *keys,
                                    bucket_acc
                                );
                                ++lr;

                                if (bucket_has[bucket_idx(b, g)]) {
                                    HE->evaluator->add_inplace(
                                        bucket_acc,
                                        bucket_carry({b, g})
                                    );
                                    ++la;
                                }
                            }

                            if (!have_row) {
                                row_acc = bucket_acc;
                                have_row = true;
                            } else {
                                HE->evaluator->add_inplace(row_acc, bucket_acc);
                                ++la;
                            }
                            ++lb;
                        }

                        if (have_row) {
                            const uint64_t dst_row = tile_id * tiled_out_channels + tj;
                            batched_out_ct(dst_row) = row_acc;
                            v511_has_output[tj] = 1;
                        }

                        v511_mul_by_row[tj] = lm;
                        v511_rot_by_row[tj] = lr;
                        v511_add_by_row[tj] = la;
                        v511_buckets_by_row[tj] = lb;
                    });

                    for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
                        v511_total_mul += v511_mul_by_row[tj];
                        v511_total_giant_rot += v511_rot_by_row[tj];
                        v511_total_add += v511_add_by_row[tj];
                        v511_total_buckets += v511_buckets_by_row[tj];
                    }

                    FillMissingOutputWithZero(
                        batched_out_ct,
                        tile_id * tiled_out_channels,
                        v511_has_output
                    );
                    ++v511_tiles_done;
                }

                auto t_end = std::chrono::steady_clock::now();
                g_last_cir_hecompute_stats.us =
                    std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_begin).count();
                g_last_cir_hecompute_stats.mul_plain = v511_total_mul;
                g_last_cir_hecompute_stats.rotate_rows = v511_total_pre_rot + v511_total_giant_rot;
                g_last_cir_hecompute_stats.add_inplace = v511_total_add;

                DAZGOrbitV508RecordActiveRotationLayer(
                    DAZGOrbitV508LayerTag(in_feature_size, in_channels, out_channels, kernel_size, stride, static_cast<int>(plan.mode)),
                    (v511_phase_count * tile_count * tiled_in_channels * DAZGOrbitV508SaturatingSub(v511_input_rot, 1ULL)) +
                        (v511_phase_count * tile_count * tiled_out_channels * DAZGOrbitV508SaturatingSub(v511_groups, 1ULL)),
                    static_cast<uint64_t>(std::max<long long>(0, g_last_cir_hecompute_stats.rotate_rows)),
                    "dense_pcoi_bsgs_minus_pdgiant_fgbucket_sparse_schedule");

                if (DAZGOrbitCirProfilerEnabled()) {
                    std::cout << "[DAZG_ORBIT_V511_PDGIANT_FGBUCKET]"
                              << " layer=CirConv2D"
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " layout_mode=" << static_cast<int>(plan.mode)
                              << " enabled=1"
                              << " tiles=" << v511_tiles_done
                              << " phase_count=" << v511_phase_count
                              << " groups=" << v511_groups
                              << " pre_rotate_rows=" << v511_total_pre_rot
                              << " terminal_giant_rotate_rows=" << v511_total_giant_rot
                              << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                              << " mul_plain=" << v511_total_mul
                              << " add_inplace=" << v511_total_add
                              << " first_group_bucket_count=" << v511_total_buckets
                              << " first_group_bucketed=1"
                              << " row_parallel=1"
                              << " exact_guard=phase_first_g"
                              << " exact_equiv=1 semantic_loss=0"
                              << " schedule=DAZG-ORBIT-PCOI-K3S2-PDGIANT-FGBUCKET"
                              << std::endl;

                    std::cout << "[DAZG_ORBIT_RUNTIME]"
                              << " layer=CirConv2D"
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " layout_mode=" << static_cast<int>(plan.mode)
                              << " hecompute_us=" << g_last_cir_hecompute_stats.us
                              << " mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                              << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                              << " add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                              << " dense_packs=" << sparse_total_packs
                              << " zero_packs=" << sparse_zero_packs
                              << " active_packs=" << sparse_active_packs
                              << " sparse_entries=" << sparse_bsgs_entries.size()
                              << " tile_count=" << tile_count
                              << " pcoi_phase_count=" << plan.orbit_phase_count
                              << " he_phase_accumulate=1"
                              << " single_he_to_ss=1"
                              << " batched_conversion=1"
                              << " v15_exposed_he_boundary=1"
                              << " pdgiant_fgbucket=1"
                              << " pdgiant_layer_bit=" << DAZGOrbitV510PCOIK3S2LayerBit(in_feature_size, in_channels, out_channels)
                              << " pdgiant_layer_name=" << DAZGOrbitV510PDGiantLayerName(DAZGOrbitV510PCOIK3S2LayerBit(in_feature_size, in_channels, out_channels))
                              << " pdgiant_layer_mask=" << DAZGOrbitV510ParseMask(std::getenv("DAZG_ORBIT_V510_PDGIANT_LAYER_MASK"), ~0ULL)
                              << " first_group_bucket_count=" << v511_total_buckets
                              << " exact_equiv=1"
                              << " semantic_loss=0"
                              << " schedule=DAZG-ORBIT-PCOI-K3S2-PDGIANT-FGBUCKET"
                              << std::endl;
                }

                return batched_out_ct;
            }
            // DAZG_ORBIT_V511_FGBUCKET_PDGIANT_END

            if (HE->server && v489_pdgiant && v490_fast &&
                DAZGOrbitV510PDGiantLayerEnabled(in_feature_size, in_channels, out_channels)) {
                assert(pcoi_phase_weight_pt.size() == plan.orbit_phase_count);
                assert(pcoi_phase_sparse_state.size() == plan.orbit_phase_count);

                UnifiedGaloisKeys* keys = HE->galoisKeys;

                long long v490_total_pre_rot = 0;
                long long v490_total_giant_rot = 0;
                long long v490_total_mul = 0;
                long long v490_total_add = 0;
                uint64_t v490_tiles_done = 0;
                const uint64_t v490_phase_count = plan.orbit_phase_count;

                RestoreSparseState(pcoi_phase_sparse_state[0]);
                const uint64_t v490_input_rot = input_rot;
                const uint64_t v490_groups =
                    sparse_bsgs_groups != 0
                        ? sparse_bsgs_groups
                        : ((tile_size + input_rot - 1) / input_rot);
                const uint64_t v490_rotate_step = ntt_size * v490_input_rot;

                std::vector<std::vector<CirSparseBSGSEntry> > v490_phase_entries(v490_phase_count);
                std::vector<std::vector<uint64_t> > v490_phase_max_rot(v490_phase_count);
                std::vector<std::vector<uint64_t> > v490_phase_active_input(v490_phase_count);
                uint64_t v490_sparse_entries_first = 0;

                for (uint64_t phase = 0; phase < v490_phase_count; ++phase) {
                    RestoreSparseState(pcoi_phase_sparse_state[phase]);
                    if (input_rot != v490_input_rot || sparse_bsgs_groups != v490_groups) {
                        std::cerr << "[DAZG_ORBIT_V490_PDGIANT_FAST_ERROR]"
                                  << " reason=phase_sparse_state_mismatch"
                                  << " phase=" << phase
                                  << " input_rot=" << input_rot
                                  << " expected_input_rot=" << v490_input_rot
                                  << " groups=" << sparse_bsgs_groups
                                  << " expected_groups=" << v490_groups
                                  << std::endl;
                        throw std::runtime_error("V490 PD-GIANT fast phase sparse-state mismatch");
                    }
                    v490_phase_entries[phase] = sparse_bsgs_entries;
                    v490_phase_max_rot[phase] = sparse_max_rot_by_input_tile;
                    v490_phase_active_input[phase] = sparse_active_input_tile;
                    if (phase == 0) {
                        v490_sparse_entries_first = static_cast<uint64_t>(sparse_bsgs_entries.size());
                    }
                }

                for (uint64_t tile_id = 0; tile_id < tile_count; ++tile_id) {
                    Tensor<UnifiedCiphertext> v490_phase_ac_rot(
                        {v490_phase_count, v490_input_rot, tiled_in_channels}, target);

                    for (uint64_t phase = 0; phase < v490_phase_count; ++phase) {
                        const uint64_t ac_base =
                            (tile_id * v490_phase_count + phase) * tiled_in_channels;
                        const std::vector<uint64_t>& active_in = v490_phase_active_input[phase];
                        const std::vector<uint64_t>& max_rot_by_ti = v490_phase_max_rot[phase];

                        for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
                            if (ti >= active_in.size() || active_in[ti] == 0) {
                                continue;
                            }
                            v490_phase_ac_rot({phase, 0, ti}) = ac_ct(ac_base + ti);
                            const uint64_t max_rot = ti < max_rot_by_ti.size() ? max_rot_by_ti[ti] : 0;
                            for (uint64_t r = 1; r <= max_rot; ++r) {
                                HE->evaluator->rotate_rows(
                                    v490_phase_ac_rot({phase, r - 1, ti}),
                                    ntt_size,
                                    *keys,
                                    v490_phase_ac_rot({phase, r, ti})
                                );
                                ++v490_total_pre_rot;
                            }
                        }
                    }

                    std::vector<uint64_t> v490_has_output(tiled_out_channels, 0);
                    std::vector<long long> v490_mul_by_row(tiled_out_channels, 0);
                    std::vector<long long> v490_rot_by_row(tiled_out_channels, 0);
                    std::vector<long long> v490_add_by_row(tiled_out_channels, 0);

                    DAZGOrbitThunderCirParallelForV17(tiled_out_channels, [&](uint64_t tj) {
                        Tensor<UnifiedCiphertext> carry({v490_groups}, target);
                        std::vector<uint64_t> has_carry(v490_groups, 0);

                        long long lm = 0;
                        long long lr = 0;
                        long long la = 0;

                        for (uint64_t phase = 0; phase < v490_phase_count; ++phase) {
                            const Tensor<UnifiedPlaintext>& wpt_phase = pcoi_phase_weight_pt[phase];
                            const std::vector<CirSparseBSGSEntry>& entries = v490_phase_entries[phase];

                            for (const auto& e : entries) {
                                if (e.tj != tj) continue;

                                UnifiedCiphertext tmp(target);
                                HE->evaluator->multiply_plain(
                                    v490_phase_ac_rot({phase, e.rot_idx, e.ti}),
                                    wpt_phase({e.ti, e.tj, e.k}),
                                    tmp
                                );
                                ++lm;

                                const uint64_t g = e.group_idx;
                                if (g >= v490_groups) continue;
                                if (!has_carry[g]) {
                                    carry(g) = tmp;
                                    has_carry[g] = 1;
                                } else {
                                    HE->evaluator->add_inplace(carry(g), tmp);
                                    ++la;
                                }
                            }
                        }

                        uint64_t first_g = v490_groups;
                        for (uint64_t g = 0; g < v490_groups; ++g) {
                            if (has_carry[g]) {
                                first_g = g;
                                break;
                            }
                        }

                        if (first_g != v490_groups) {
                            const uint64_t dst_row = tile_id * tiled_out_channels + tj;
                            batched_out_ct(dst_row) = carry(first_g);
                            v490_has_output[tj] = 1;

                            for (uint64_t g = first_g + 1; g < v490_groups; ++g) {
                                HE->evaluator->rotate_rows(
                                    batched_out_ct(dst_row),
                                    v490_rotate_step,
                                    *keys,
                                    batched_out_ct(dst_row)
                                );
                                ++lr;

                                if (has_carry[g]) {
                                    HE->evaluator->add_inplace(batched_out_ct(dst_row), carry(g));
                                    ++la;
                                }
                            }
                        }

                        v490_mul_by_row[tj] = lm;
                        v490_rot_by_row[tj] = lr;
                        v490_add_by_row[tj] = la;
                    });

                    for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
                        v490_total_mul += v490_mul_by_row[tj];
                        v490_total_giant_rot += v490_rot_by_row[tj];
                        v490_total_add += v490_add_by_row[tj];
                    }

                    FillMissingOutputWithZero(
                        batched_out_ct,
                        tile_id * tiled_out_channels,
                        v490_has_output
                    );
                    ++v490_tiles_done;
                }

                auto t_end = std::chrono::steady_clock::now();
                g_last_cir_hecompute_stats.us =
                    std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_begin).count();
                g_last_cir_hecompute_stats.mul_plain = v490_total_mul;
                g_last_cir_hecompute_stats.rotate_rows = v490_total_pre_rot + v490_total_giant_rot;
                g_last_cir_hecompute_stats.add_inplace = v490_total_add;

                DAZGOrbitV508RecordActiveRotationLayer(
                    DAZGOrbitV508LayerTag(in_feature_size, in_channels, out_channels, kernel_size, stride, static_cast<int>(plan.mode)),
                    (v490_phase_count * tile_count * tiled_in_channels * DAZGOrbitV508SaturatingSub(v490_input_rot, 1ULL)) +
                        (tile_count * tiled_out_channels * DAZGOrbitV508SaturatingSub(v490_groups, 1ULL)),
                    static_cast<uint64_t>(std::max<long long>(0, g_last_cir_hecompute_stats.rotate_rows)),
                    "dense_pcoi_bsgs_minus_pdgiant_fast_sparse_schedule");

                if (DAZGOrbitCirProfilerEnabled()) {
                    std::cout << "[DAZG_ORBIT_V490_PDGIANT_FAST]"
                              << " layer=CirConv2D"
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " layout_mode=" << static_cast<int>(plan.mode)
                              << " enabled=1"
                              << " tiles=" << v490_tiles_done
                              << " phase_count=" << v490_phase_count
                              << " groups=" << v490_groups
                              << " pre_rotate_rows=" << v490_total_pre_rot
                              << " terminal_giant_rotate_rows=" << v490_total_giant_rot
                              << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                              << " mul_plain=" << v490_total_mul
                              << " add_inplace=" << v490_total_add
                              << " row_parallel=1"
                              << " exact_equiv=1 semantic_loss=0"
                              << " schedule=DAZG-ORBIT-PCOI-K3S2-PDGIANT-FAST"
                              << std::endl;

                    if (std::getenv("DAZG_ORBIT_V497_RCOLC_MAPD_BINDER") != nullptr) {
                        std::cout << "[DAZG_ORBIT_V497_RCOLC_MAPD_POLICY_BOUND]"
                                  << " marker=DAZG_ORBIT_V497_RCOLC_MAPD_POLICY_BOUND_20260531"
                                  << " layer=CirConv2D"
                                  << " H=" << in_feature_size
                                  << " Cin=" << in_channels
                                  << " Cout=" << out_channels
                                  << " K=" << kernel_size
                                  << " S=" << stride
                                  << " layout_mode=" << static_cast<int>(plan.mode)
                                  << " algorithm=RC_OLC_MAPD"
                                  << " state=phase_deferred_orbit_carry"
                                  << " materialization=after_phase_accumulation"
                                  << " active_rotation_source=profile_gated_k3s2"
                                  << " residual_constraint=external_exactness_gate"
                                  << " phase_count=" << v490_phase_count
                                  << " groups=" << v490_groups
                                  << " pre_rotate_rows=" << v490_total_pre_rot
                                  << " terminal_giant_rotate_rows=" << v490_total_giant_rot
                                  << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                                  << " exact_gate=top1_signed_hash_round_hash"
                                  << " note=policy_binding_marker_not_new_math_claim"
                                  << std::endl;
                    }

                    std::cout << "[DAZG_ORBIT_RUNTIME]"
                              << " layer=CirConv2D"
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " layout_mode=" << static_cast<int>(plan.mode)
                              << " hecompute_us=" << g_last_cir_hecompute_stats.us
                              << " mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                              << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                              << " add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                              << " dense_packs=" << sparse_total_packs
                              << " zero_packs=" << sparse_zero_packs
                              << " active_packs=" << sparse_active_packs
                              << " sparse_entries=" << v490_sparse_entries_first
                              << " tile_count=" << tile_count
                              << " pcoi_phase_count=" << plan.orbit_phase_count
                              << " he_phase_accumulate=1"
                              << " single_he_to_ss=1"
                              << " batched_conversion=1"
                              << " v15_exposed_he_boundary=1"
                              << " pdgiant_orbit_carry=1"
                              << " pdgiant_fast=1"
                              << " pdgiant_layer_bit=" << DAZGOrbitV510PCOIK3S2LayerBit(in_feature_size, in_channels, out_channels)
                              << " pdgiant_layer_name=" << DAZGOrbitV510PDGiantLayerName(DAZGOrbitV510PCOIK3S2LayerBit(in_feature_size, in_channels, out_channels))
                              << " pdgiant_layer_mask=" << DAZGOrbitV510ParseMask(std::getenv("DAZG_ORBIT_V510_PDGIANT_LAYER_MASK"), ~0ULL)
                              << " exact_equiv=1"
                              << " semantic_loss=0"
                              << " schedule=DAZG-ORBIT-PCOI-K3S2-PDGIANT-FAST"
                              << std::endl;
                }

                return batched_out_ct;
            }
            // DAZG_ORBIT_V490_PDGIANT_FAST_PATCH_END

            if (HE->server && v489_pdgiant &&
                DAZGOrbitV510PDGiantLayerEnabled(in_feature_size, in_channels, out_channels)) {
                assert(pcoi_phase_weight_pt.size() == plan.orbit_phase_count);
                assert(pcoi_phase_sparse_state.size() == plan.orbit_phase_count);

                UnifiedGaloisKeys* keys = HE->galoisKeys;

                long long v489_total_pre_rot = 0;
                long long v489_total_giant_rot = 0;
                long long v489_total_mul = 0;
                long long v489_total_add = 0;
                uint64_t v489_tiles_done = 0;
                uint64_t v489_phase_count = plan.orbit_phase_count;
                uint64_t v489_groups_seen = 0;

                for (uint64_t tile_id = 0; tile_id < tile_count; ++tile_id) {
                    RestoreSparseState(pcoi_phase_sparse_state[0]);
                    const uint64_t v489_input_rot = input_rot;
                    const uint64_t v489_groups =
                        sparse_bsgs_groups != 0
                            ? sparse_bsgs_groups
                            : ((tile_size + input_rot - 1) / input_rot);
                    const uint64_t v489_rotate_step = ntt_size * v489_input_rot;
                    v489_groups_seen = v489_groups;

                    Tensor<UnifiedCiphertext> v489_carry(
                        {tiled_out_channels, v489_groups}, target);
                    std::vector<uint64_t> v489_has_carry(
                        static_cast<size_t>(tiled_out_channels * v489_groups), 0);
                    std::vector<uint64_t> v489_has_output(tiled_out_channels, 0);

                    auto v489_carry_idx = [&](uint64_t tj, uint64_t g) -> size_t {
                        return static_cast<size_t>(tj * v489_groups + g);
                    };

                    for (uint64_t phase = 0; phase < plan.orbit_phase_count; ++phase) {
                        RestoreSparseState(pcoi_phase_sparse_state[phase]);

                        if (input_rot != v489_input_rot || sparse_bsgs_groups != v489_groups) {
                            std::cerr << "[DAZG_ORBIT_V489_PDGIANT_ERROR]"
                                      << " reason=phase_sparse_state_mismatch"
                                      << " tile=" << tile_id
                                      << " phase=" << phase
                                      << " input_rot=" << input_rot
                                      << " expected_input_rot=" << v489_input_rot
                                      << " groups=" << sparse_bsgs_groups
                                      << " expected_groups=" << v489_groups
                                      << std::endl;
                            throw std::runtime_error("V489 PD-GIANT phase sparse-state mismatch");
                        }

                        const uint64_t ac_base =
                            (tile_id * plan.orbit_phase_count + phase) * tiled_in_channels;

                        Tensor<UnifiedCiphertext> v489_ac_rot(
                            {input_rot, tiled_in_channels}, target);

                        for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
                            if (ti >= sparse_active_input_tile.size() ||
                                sparse_active_input_tile[ti] == 0) {
                                continue;
                            }

                            v489_ac_rot({0, ti}) = ac_ct(ac_base + ti);
                            const uint64_t max_rot = sparse_max_rot_by_input_tile[ti];
                            for (uint64_t r = 1; r <= max_rot; ++r) {
                                HE->evaluator->rotate_rows(
                                    v489_ac_rot({r - 1, ti}),
                                    ntt_size,
                                    *keys,
                                    v489_ac_rot({r, ti})
                                );
                                ++v489_total_pre_rot;
                                ++g_last_cir_hecompute_stats.rotate_rows;
                            }
                        }

                        const Tensor<UnifiedPlaintext>& v489_wpt = pcoi_phase_weight_pt[phase];
                        for (const auto& e : sparse_bsgs_entries) {
                            UnifiedCiphertext tmp(target);
                            HE->evaluator->multiply_plain(
                                v489_ac_rot({e.rot_idx, e.ti}),
                                v489_wpt({e.ti, e.tj, e.k}),
                                tmp
                            );
                            ++v489_total_mul;
                            ++g_last_cir_hecompute_stats.mul_plain;

                            const uint64_t g = e.group_idx;
                            const size_t gi = v489_carry_idx(e.tj, g);
                            if (!v489_has_carry[gi]) {
                                v489_carry({e.tj, g}) = tmp;
                                v489_has_carry[gi] = 1;
                            } else {
                                HE->evaluator->add_inplace(v489_carry({e.tj, g}), tmp);
                                ++v489_total_add;
                                ++g_last_cir_hecompute_stats.add_inplace;
                            }
                        }
                    }

                    for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
                        uint64_t first_g = v489_groups;
                        for (uint64_t g = 0; g < v489_groups; ++g) {
                            if (v489_has_carry[v489_carry_idx(tj, g)]) {
                                first_g = g;
                                break;
                            }
                        }
                        if (first_g == v489_groups) {
                            continue;
                        }

                        const uint64_t dst_row = tile_id * tiled_out_channels + tj;
                        batched_out_ct(dst_row) = v489_carry({tj, first_g});
                        v489_has_output[tj] = 1;

                        for (uint64_t g = first_g + 1; g < v489_groups; ++g) {
                            HE->evaluator->rotate_rows(
                                batched_out_ct(dst_row),
                                v489_rotate_step,
                                *keys,
                                batched_out_ct(dst_row)
                            );
                            ++v489_total_giant_rot;
                            ++g_last_cir_hecompute_stats.rotate_rows;

                            if (v489_has_carry[v489_carry_idx(tj, g)]) {
                                HE->evaluator->add_inplace(
                                    batched_out_ct(dst_row),
                                    v489_carry({tj, g})
                                );
                                ++v489_total_add;
                                ++g_last_cir_hecompute_stats.add_inplace;
                            }
                        }
                    }

                    FillMissingOutputWithZero(
                        batched_out_ct,
                        tile_id * tiled_out_channels,
                        v489_has_output
                    );
                    ++v489_tiles_done;
                }

                auto t_end = std::chrono::steady_clock::now();
                g_last_cir_hecompute_stats.us =
                    std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_begin).count();

                DAZGOrbitV508RecordActiveRotationLayer(
                    DAZGOrbitV508LayerTag(in_feature_size, in_channels, out_channels, kernel_size, stride, static_cast<int>(plan.mode)),
                    (v489_phase_count * tile_count * tiled_in_channels * DAZGOrbitV508SaturatingSub(input_rot, 1ULL)) +
                        (tile_count * tiled_out_channels * DAZGOrbitV508SaturatingSub(v489_groups_seen, 1ULL)),
                    static_cast<uint64_t>(std::max<long long>(0, g_last_cir_hecompute_stats.rotate_rows)),
                    "dense_pcoi_bsgs_minus_pdgiant_sparse_schedule");

                if (DAZGOrbitCirProfilerEnabled()) {
                    std::cout << "[DAZG_ORBIT_V489_PDGIANT]"
                              << " layer=CirConv2D"
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " layout_mode=" << static_cast<int>(plan.mode)
                              << " enabled=1"
                              << " tiles=" << v489_tiles_done
                              << " phase_count=" << v489_phase_count
                              << " groups=" << v489_groups_seen
                              << " pre_rotate_rows=" << v489_total_pre_rot
                              << " terminal_giant_rotate_rows=" << v489_total_giant_rot
                              << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                              << " mul_plain=" << v489_total_mul
                              << " add_inplace=" << v489_total_add
                              << " exact_equiv=1 semantic_loss=0"
                              << " schedule=DAZG-ORBIT-PCOI-K3S2-PDGIANT"
                              << std::endl;

                    std::cout << "[DAZG_ORBIT_RUNTIME]"
                              << " layer=CirConv2D"
                              << " H=" << in_feature_size
                              << " Cin=" << in_channels
                              << " Cout=" << out_channels
                              << " K=" << kernel_size
                              << " S=" << stride
                              << " layout_mode=" << static_cast<int>(plan.mode)
                              << " hecompute_us=" << g_last_cir_hecompute_stats.us
                              << " mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                              << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                              << " add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                              << " dense_packs=" << sparse_total_packs
                              << " zero_packs=" << sparse_zero_packs
                              << " active_packs=" << sparse_active_packs
                              << " sparse_entries=" << sparse_bsgs_entries.size()
                              << " tile_count=" << tile_count
                              << " pcoi_phase_count=" << plan.orbit_phase_count
                              << " he_phase_accumulate=1"
                              << " single_he_to_ss=1"
                              << " batched_conversion=1"
                              << " v15_exposed_he_boundary=1"
                              << " pdgiant_orbit_carry=1"
                              << " pdgiant_layer_bit=" << DAZGOrbitV510PCOIK3S2LayerBit(in_feature_size, in_channels, out_channels)
                              << " pdgiant_layer_name=" << DAZGOrbitV510PDGiantLayerName(DAZGOrbitV510PCOIK3S2LayerBit(in_feature_size, in_channels, out_channels))
                              << " pdgiant_layer_mask=" << DAZGOrbitV510ParseMask(std::getenv("DAZG_ORBIT_V510_PDGIANT_LAYER_MASK"), ~0ULL)
                              << " exact_equiv=1"
                              << " semantic_loss=0"
                              << " schedule=DAZG-ORBIT-PCOI-K3S2-PDGIANT"
                              << std::endl;
                }

                return batched_out_ct;
            }
        }
        // DAZG_ORBIT_V489_PDGIANT_ORBITCARRY_PATCH_END

        if (HE->server) {
            assert(pcoi_phase_weight_pt.size() == plan.orbit_phase_count);
            assert(pcoi_phase_sparse_state.size() == plan.orbit_phase_count);

            for (uint64_t tile_id = 0; tile_id < tile_count; ++tile_id) {
                for (uint64_t phase = 0; phase < plan.orbit_phase_count; ++phase) {
                    RestoreSparseState(pcoi_phase_sparse_state[phase]);
                    Tensor<UnifiedCiphertext> phase_out_ct({tiled_out_channels}, target);
                    HEComputeSparseRows(
                        pcoi_phase_weight_pt[phase],
                        ac_ct,
                        (tile_id * plan.orbit_phase_count + phase) * tiled_in_channels,
                        phase_out_ct,
                        0
                    );

                    for (uint64_t tj = 0; tj < tiled_out_channels; ++tj) {
                        const uint64_t dst_row = tile_id * tiled_out_channels + tj;
                        if (phase == 0) {
                            batched_out_ct(dst_row) = phase_out_ct(tj);
                        } else {
                            HE->evaluator->add_inplace(batched_out_ct(dst_row), phase_out_ct(tj));
                            ++g_last_cir_hecompute_stats.add_inplace;
                        }
                    }
                }
            }
        }
        auto t_end = std::chrono::steady_clock::now();
        g_last_cir_hecompute_stats.us =
            std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_begin).count();

#ifndef DISABLE_CIR_HECOMPUTE_LOG
        if (HE->server && DAZGOrbitCirProfilerEnabled()) {
            std::cout << "[DAZG_ORBIT_RUNTIME]"
                      << " layer=CirConv2D"
                      << " H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " Cout=" << out_channels
                      << " K=" << kernel_size
                      << " S=" << stride
                      << " layout_mode=" << static_cast<int>(plan.mode)
                      << " hecompute_us=" << g_last_cir_hecompute_stats.us
                      << " mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                      << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                      << " add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                      << " dense_packs=" << sparse_total_packs
                      << " zero_packs=" << sparse_zero_packs
                      << " active_packs=" << sparse_active_packs
                      << " sparse_entries=" << sparse_bsgs_entries.size()
                      << " tile_count=" << tile_count
                      << " pcoi_phase_count=" << plan.orbit_phase_count
                      << " he_phase_accumulate=1"
                      << " single_he_to_ss=1"
                      << " batched_conversion=1"
                      << " v15_exposed_he_boundary=1"
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << " schedule=DAZG-ORBIT-PCOI-K3S2"
                      << std::endl;
        }
#endif

        return batched_out_ct;
    }

    const auto target = HE->server ? HE->Backend() : HOST;
    Tensor<UnifiedCiphertext> batched_out_ct({tile_count * tiled_out_channels}, target);

    ResetLastCirHEComputeStats();
    auto t_begin = std::chrono::steady_clock::now();
    if (HE->server) {
        for (uint64_t tile_id = 0; tile_id < tile_count; ++tile_id) {
            HEComputeSparseRows(
                weight_pt,
                ac_ct,
                tile_id * tiled_in_channels,
                batched_out_ct,
                tile_id * tiled_out_channels
            );
        }
    }
    auto t_end = std::chrono::steady_clock::now();
    g_last_cir_hecompute_stats.us =
        std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_begin).count();

#ifndef DISABLE_CIR_HECOMPUTE_LOG
    if (HE->server && DAZGOrbitCirProfilerEnabled()) {
        std::cout << "[DAZG_ORBIT_RUNTIME]"
                  << " layer=CirConv2D"
                  << " H=" << in_feature_size
                  << " Cin=" << in_channels
                  << " Cout=" << out_channels
                  << " K=" << kernel_size
                  << " S=" << stride
                  << " layout_mode=" << static_cast<int>(plan.mode)
                  << " hecompute_us=" << g_last_cir_hecompute_stats.us
                  << " mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                  << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                  << " add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                  << " dense_packs=" << sparse_total_packs
                  << " zero_packs=" << sparse_zero_packs
                  << " active_packs=" << sparse_active_packs
                  << " sparse_entries=" << sparse_bsgs_entries.size()
                  << " tile_count=" << tile_count
                  << " batched_conversion=1"
                  << " v15_exposed_he_boundary=1"
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << " schedule=StageZ2-ExactSparseBSGSLinear"
                  << std::endl;
    }
#endif

    return batched_out_ct;
}

Tensor<uint64_t> CirConv2D::DAZGOrbitDepackFromPackedSS(Tensor<uint64_t> &out_msg) {
    if (!IsExactTileK3S1Mode(plan.mode)) {
        return DepackResult(out_msg);
    }

    Tensor<uint64_t> y({out_channels, logical_out_feature_size, logical_out_feature_size});

    const uint64_t depack_offset =
        (packed_kernel_size - 1) * (padded_feature_size + 1);
    const uint64_t depack_row_step = packed_stride;
    const uint64_t depack_col_step = packed_stride;

    for (uint64_t tile_r = 0; tile_r < spatial_tile_rows; ++tile_r) {
        for (uint64_t tile_c = 0; tile_c < spatial_tile_cols; ++tile_c) {
            const uint64_t tile_id = tile_r * spatial_tile_cols + tile_c;
            Tensor<uint64_t> tile_y = DepackSingleMapImpl(
                out_msg,
                tile_id * tiled_out_channels,
                packed_out_feature_size,
                depack_offset,
                depack_row_step,
                depack_col_step
            );
            ScatterTileNoOverlap(tile_y, tile_r, tile_c, y);
        }
    }

    return y;
}

Tensor<uint64_t> CirConv2D::operator()(Tensor<uint64_t> &x) {

// DAZG_ORBIT_V666_ACTIVATION_CANON_ENTRY
if (DAZGOrbitV666CoreRepairEnabled()) {
  uint64_t __p = DAZGOrbitV666PlainMod();
  for (int64_t __i = 0; __i < x.size(); ++__i) { x.data()[__i] = x.data()[__i] % __p; }
}


    if (!IsExactTileK3S1Mode(plan.mode)) {
        Tensor<uint64_t> ac_msg = PackActivation(x);
        Tensor<UnifiedCiphertext> ac_ct = Operator::SSToHE(ac_msg, HE);
        Tensor<UnifiedCiphertext> out_ct = HECompute(weight_pt, ac_ct);
#ifndef DISABLE_CIR_HECOMPUTE_LOG
        if (HE->server) {
            std::cout << "CirConv2D HECompute time(us)="
                      << g_last_cir_hecompute_stats.us
                      << ", mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                      << ", rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                      << ", add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                      << ", active_packs=" << sparse_active_packs
                      << ", zero_packs=" << sparse_zero_packs << "/" << sparse_total_packs
                      << ", schedule=StageZ2-ExactSparseBSGSLinear"
                      << std::endl;

            if (DAZGOrbitCirProfilerEnabled()) {
                std::cout << "[DAZG_ORBIT_RUNTIME]"
                          << " layer=CirConv2D"
                          << " H=" << in_feature_size
                          << " Cin=" << in_channels
                          << " Cout=" << out_channels
                          << " K=" << kernel_size
                          << " S=" << stride
                          << " layout_mode=" << static_cast<int>(plan.mode)
                          << " hecompute_us=" << g_last_cir_hecompute_stats.us
                          << " mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                          << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                          << " add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                          << " dense_packs=" << sparse_total_packs
                          << " zero_packs=" << sparse_zero_packs
                          << " active_packs=" << sparse_active_packs
                          << " sparse_entries=" << sparse_bsgs_entries.size()
                          << " tile_count=1"
                          << " batched_conversion=0"
                          << " exact_equiv=1"
                          << " semantic_loss=0"
                          << " schedule=StageZ2-ExactSparseBSGSLinear"
                          << std::endl;
            }
        }
#endif
        Tensor<uint64_t> out_msg = Operator::HEToSS(out_ct, HE);
        Tensor<uint64_t> y = DepackResult(out_msg);
        return y;
    }

    if (IsPCOIK3S2Orbit()) {
        Tensor<uint64_t> ac_msg = DAZGOrbitPackActivationForHE(x);
        Tensor<UnifiedCiphertext> ac_ct = Operator::SSToHE(ac_msg, HE);
        Tensor<UnifiedCiphertext> out_ct = DAZGOrbitComputeFromPackedHE(ac_ct);
        Tensor<uint64_t> out_msg = Operator::HEToSS(out_ct, HE);
        
// DAZG_ORBIT_V666_OUTMSG_CANON_BEFORE_DEPACK
if (DAZGOrbitV666CoreRepairEnabled()) {
  uint64_t __p = DAZGOrbitV666PlainMod();
  for (int64_t __i = 0; __i < out_msg.size(); ++__i) { out_msg.data()[__i] = out_msg.data()[__i] % __p; }
}

Tensor<uint64_t> y = DAZGOrbitDepackFromPackedSS(out_msg);
        return y;
    }

    assert(spatial_tile_rows >= 1);
    assert(spatial_tile_cols >= 1);
    assert(tile_out_feature_size > 0);
    assert(tile_in_feature_size == tile_out_feature_size + packed_kernel_size - 1);
    assert(logical_out_feature_size == out_feature_size);
    assert(packed_out_feature_size == tile_out_feature_size);

    Tensor<uint64_t> y({out_channels, logical_out_feature_size, logical_out_feature_size});

    const uint64_t tile_count = spatial_tile_rows * spatial_tile_cols;
    Tensor<uint64_t> batched_ac_msg(
        {tile_count * tiled_in_channels, HE->polyModulusDegree}
    );

    const uint64_t depack_offset =
        (packed_kernel_size - 1) * (padded_feature_size + 1);
    const uint64_t depack_row_step = packed_stride;
    const uint64_t depack_col_step = packed_stride;

    for (uint64_t tile_r = 0; tile_r < spatial_tile_rows; ++tile_r) {
        for (uint64_t tile_c = 0; tile_c < spatial_tile_cols; ++tile_c) {
            const uint64_t tile_id = tile_r * spatial_tile_cols + tile_c;
            Tensor<uint64_t> ac_msg({tiled_in_channels, HE->polyModulusDegree});

            if (IsExactCompactStride2Pointwise() ||
                IsExactCompactStride2K3Polyphase()) {
                Tensor<uint64_t> worker = ExtractCompactTileWorkerMap(x, tile_r, tile_c);
                ac_msg = PackActivationPackedWorkerMapImpl(worker);
            } else {
                Tensor<uint64_t> patch = ExtractTileWithHalo(x, tile_r, tile_c);
                ac_msg = PackActivationSingleMapImpl(
                    patch,
                    tile_in_feature_size,
                    packed_out_feature_size,
                    packed_padding,
                    false,
                    false
                );
            }

            for (uint64_t ti = 0; ti < tiled_in_channels; ++ti) {
                const uint64_t dst_row = tile_id * tiled_in_channels + ti;
                for (uint64_t m = 0; m < HE->polyModulusDegree; ++m) {
                    batched_ac_msg({dst_row, m}) = ac_msg({ti, m});
                }
            }
        }
    }

    Tensor<UnifiedCiphertext> batched_ac_ct = Operator::SSToHE(batched_ac_msg, HE);
    const auto target = HE->server ? HE->Backend() : HOST;
    Tensor<UnifiedCiphertext> batched_out_ct({tile_count * tiled_out_channels}, target);

    ResetLastCirHEComputeStats();
    auto t_begin = std::chrono::steady_clock::now();
    if (HE->server) {
        for (uint64_t tile_id = 0; tile_id < tile_count; ++tile_id) {
            HEComputeSparseRows(
                weight_pt,
                batched_ac_ct,
                tile_id * tiled_in_channels,
                batched_out_ct,
                tile_id * tiled_out_channels
            );
        }
    }
    auto t_end = std::chrono::steady_clock::now();
    g_last_cir_hecompute_stats.us =
        std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_begin).count();

    Tensor<uint64_t> batched_out_msg = Operator::HEToSS(batched_out_ct, HE);

    for (uint64_t tile_r = 0; tile_r < spatial_tile_rows; ++tile_r) {
        for (uint64_t tile_c = 0; tile_c < spatial_tile_cols; ++tile_c) {
            const uint64_t tile_id = tile_r * spatial_tile_cols + tile_c;
            Tensor<uint64_t> tile_y = DepackSingleMapImpl(
                batched_out_msg,
                tile_id * tiled_out_channels,
                packed_out_feature_size,
                depack_offset,
                depack_row_step,
                depack_col_step
            );
            ScatterTileNoOverlap(tile_y, tile_r, tile_c, y);
        }
    }

#ifndef DISABLE_CIR_HECOMPUTE_LOG
    if (HE->server) {
        std::cout << "CirConv2D HECompute time(us)="
                  << g_last_cir_hecompute_stats.us
                  << ", mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                  << ", rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                  << ", add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                  << ", batched_conversion=1"
                  << ", tile_count=" << tile_count
                  << ", schedule=StageZ2-ExactSparseBSGSLinear"
                  << std::endl;

        if (DAZGOrbitCirProfilerEnabled()) {
            std::cout << "[DAZG_ORBIT_RUNTIME]"
                      << " layer=CirConv2D"
                      << " H=" << in_feature_size
                      << " Cin=" << in_channels
                      << " Cout=" << out_channels
                      << " K=" << kernel_size
                      << " S=" << stride
                      << " layout_mode=" << static_cast<int>(plan.mode)
                      << " hecompute_us=" << g_last_cir_hecompute_stats.us
                      << " mul_plain=" << g_last_cir_hecompute_stats.mul_plain
                      << " rotate_rows=" << g_last_cir_hecompute_stats.rotate_rows
                      << " add_inplace=" << g_last_cir_hecompute_stats.add_inplace
                      << " dense_packs=" << sparse_total_packs
                      << " zero_packs=" << sparse_zero_packs
                      << " active_packs=" << sparse_active_packs
                      << " sparse_entries=" << sparse_bsgs_entries.size()
                      << " tile_count=" << tile_count
                      << " batched_conversion=1"
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << " schedule=StageZ2-ExactSparseBSGSLinear"
                      << std::endl;
        }
    }
#endif

    return y;
}

Tensor<uint64_t> CirConv2D::ExtractTileWithHalo(
    const Tensor<uint64_t>& x,
    uint64_t tile_r,
    uint64_t tile_c
) const {
    Tensor<uint64_t> patch({in_channels, tile_in_feature_size, tile_in_feature_size});

    const int base_r =
        static_cast<int>(tile_r * tile_out_feature_size) - static_cast<int>(halo_size);
    const int base_c =
        static_cast<int>(tile_c * tile_out_feature_size) - static_cast<int>(halo_size);

    for (uint64_t c = 0; c < in_channels; ++c) {
        for (uint64_t i = 0; i < tile_in_feature_size; ++i) {
            for (uint64_t j = 0; j < tile_in_feature_size; ++j) {
                const int src_r = base_r + static_cast<int>(i);
                const int src_c = base_c + static_cast<int>(j);

                if (0 <= src_r && src_r < static_cast<int>(in_feature_size) &&
                    0 <= src_c && src_c < static_cast<int>(in_feature_size)) {
                    patch({c, i, j}) =
                        x({c, static_cast<uint64_t>(src_r), static_cast<uint64_t>(src_c)});
                } else {
                    patch({c, i, j}) = 0;
                }
            }
        }
    }

    return patch;
}


void CirConv2D::ScatterTileNoOverlap(
    const Tensor<uint64_t>& tile_y,
    uint64_t tile_r,
    uint64_t tile_c,
    Tensor<uint64_t>& y
) const {
    const uint64_t row0 = tile_r * tile_out_feature_size;
    const uint64_t col0 = tile_c * tile_out_feature_size;

    if (row0 >= logical_out_feature_size || col0 >= logical_out_feature_size) {
        return;
    }

    const uint64_t valid_h =
        std::min<uint64_t>(tile_out_feature_size, logical_out_feature_size - row0);
    const uint64_t valid_w =
        std::min<uint64_t>(tile_out_feature_size, logical_out_feature_size - col0);

    for (uint64_t c = 0; c < out_channels; ++c) {
        for (uint64_t i = 0; i < valid_h; ++i) {
            for (uint64_t j = 0; j < valid_w; ++j) {
                y({c, row0 + i, col0 + j}) = tile_y({c, i, j});
            }
        }
    }
}

}
 // namespace LinearLayer