// DAZG-Orbit Project Source File
// Component: HE/src/tfhe/BFEGeLUClear.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include "HE/tfhe/BFEGeLUClear.h"
#include "HE/tfhe/BFELutEncoder.h"
#include "HE/tfhe/PBS.h"
#include "Utils/dazg_orbit_ablation_flags.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef DAZG_ORBIT_BFE_GELU_STATS_LIMIT
#define DAZG_ORBIT_BFE_GELU_STATS_LIMIT 64
#endif

#ifndef DAZG_ORBIT_STAGE_S_INPUT_BITS
#ifdef DAZG_ORBIT_STAGE_R_INPUT_BITS
#define DAZG_ORBIT_STAGE_S_INPUT_BITS DAZG_ORBIT_STAGE_R_INPUT_BITS
#else
#define DAZG_ORBIT_STAGE_S_INPUT_BITS 8
#endif
#endif

#ifndef DAZG_ORBIT_STAGE_S_CLIP
#ifdef DAZG_ORBIT_STAGE_R_CLIP
#define DAZG_ORBIT_STAGE_S_CLIP DAZG_ORBIT_STAGE_R_CLIP
#else
#define DAZG_ORBIT_STAGE_S_CLIP 8.0
#endif
#endif

#ifndef DAZG_ORBIT_STAGE_S_COEFF_BITS
#ifdef DAZG_ORBIT_STAGE_R_COEFF_BITS
#define DAZG_ORBIT_STAGE_S_COEFF_BITS DAZG_ORBIT_STAGE_R_COEFF_BITS
#else
#define DAZG_ORBIT_STAGE_S_COEFF_BITS 24
#endif
#endif

#ifndef DAZG_ORBIT_STAGE_S_ZERO_BUCKET_RADIUS
#ifdef DAZG_ORBIT_STAGE_R_ZERO_BUCKET_RADIUS
#define DAZG_ORBIT_STAGE_S_ZERO_BUCKET_RADIUS DAZG_ORBIT_STAGE_R_ZERO_BUCKET_RADIUS
#else
#define DAZG_ORBIT_STAGE_S_ZERO_BUCKET_RADIUS 2
#endif
#endif

#ifndef DAZG_ORBIT_STAGE_T_TAIL_SPARSE
#define DAZG_ORBIT_STAGE_T_TAIL_SPARSE 1
#endif

#ifndef DAZG_ORBIT_STAGE_U_BUCKET_MEMO
#define DAZG_ORBIT_STAGE_U_BUCKET_MEMO 1
#endif

#ifndef DAZG_ORBIT_STAGE_V_RANGE_CERT
#define DAZG_ORBIT_STAGE_V_RANGE_CERT 1
#endif

#ifndef DAZG_ORBIT_STAGE_W_DOMAIN_CERT
#define DAZG_ORBIT_STAGE_W_DOMAIN_CERT 1
#endif

#ifndef DAZG_ORBIT_STAGE_X_RESIDUAL_SCATTER
#define DAZG_ORBIT_STAGE_X_RESIDUAL_SCATTER 1
#endif

#ifndef DAZG_ORBIT_STAGE_Y_BUCKET_SCHEDULE
#define DAZG_ORBIT_STAGE_Y_BUCKET_SCHEDULE 1
#endif

namespace HE {
namespace {


struct DAZGOrbitStageYAggregate {
    std::uint64_t calls = 0;
    std::uint64_t total_n = 0;
    std::uint64_t total_time_us = 0;
    std::uint64_t pbs_calls = 0;
    std::uint64_t pbs_saved = 0;
    std::uint64_t table_elided = 0;
    std::uint64_t route_elided = 0;
    std::uint64_t active_route_elements = 0;
    std::uint64_t route_elements = 0;
    std::uint64_t route_cache_hits = 0;
    std::uint64_t deterministic_scatter_hits = 0;
    std::uint64_t range_cert_hits = 0;
    std::uint64_t domain_cert_hits = 0;
    std::uint64_t residual_candidates = 0;
    std::uint64_t residual_indexed_eval = 0;
    std::uint64_t bucket_scheduled_eval = 0;
    std::uint64_t bucket_schedule_reused = 0;
};

class DAZGOrbitStageYLedger {
public:
    static DAZGOrbitStageYLedger& Instance() {
        static DAZGOrbitStageYLedger ledger;
        return ledger;
    }

    ~DAZGOrbitStageYLedger() { Emit(); }

    void Record(std::uint64_t n,
                std::uint64_t time_us,
                std::uint64_t pbs_calls,
                std::uint64_t pbs_saved,
                std::uint64_t table_elided,
                std::uint64_t route_elided,
                std::uint64_t active_route_elements,
                std::uint64_t route_elements,
                std::uint64_t route_cache_hits,
                std::uint64_t deterministic_scatter_hits,
                std::uint64_t range_cert_hits,
                std::uint64_t domain_cert_hits,
                std::uint64_t residual_candidates,
                std::uint64_t residual_indexed_eval,
                std::uint64_t bucket_scheduled_eval,
                std::uint64_t bucket_schedule_reused) {
        std::lock_guard<std::mutex> lock(mu_);
        emitted_ = false;
        agg_.calls += 1;
        agg_.total_n += n;
        agg_.total_time_us += time_us;
        agg_.pbs_calls += pbs_calls;
        agg_.pbs_saved += pbs_saved;
        agg_.table_elided += table_elided;
        agg_.route_elided += route_elided;
        agg_.active_route_elements += active_route_elements;
        agg_.route_elements += route_elements;
        agg_.route_cache_hits += route_cache_hits;
        agg_.deterministic_scatter_hits += deterministic_scatter_hits;
        agg_.range_cert_hits += range_cert_hits;
        agg_.domain_cert_hits += domain_cert_hits;
        agg_.residual_candidates += residual_candidates;
        agg_.residual_indexed_eval += residual_indexed_eval;
        agg_.bucket_scheduled_eval += bucket_scheduled_eval;
        agg_.bucket_schedule_reused += bucket_schedule_reused;
    }

    void Emit() {
        DAZGOrbitStageYAggregate a;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (emitted_ || agg_.calls == 0) return;
            emitted_ = true;
            a = agg_;
        }

        const double pbs_saved_ratio =
            a.total_n == 0 ? 0.0
                           : static_cast<double>(a.pbs_saved) /
                             static_cast<double>(a.total_n);
        const double active_ratio =
            a.total_n == 0 ? 0.0
                           : static_cast<double>(a.pbs_calls) /
                             static_cast<double>(a.total_n);
        const double table_elision_ratio =
            a.total_n == 0 ? 0.0
                           : static_cast<double>(a.table_elided) /
                             static_cast<double>(a.total_n);
        const double route_elision_ratio =
            a.total_n == 0 ? 0.0
                           : static_cast<double>(a.route_elided) /
                             static_cast<double>(a.total_n);
        const double deterministic_scatter_ratio =
            a.total_n == 0 ? 0.0
                           : static_cast<double>(a.deterministic_scatter_hits) /
                             static_cast<double>(a.total_n);
        const double route_cache_reuse_ratio =
            a.active_route_elements == 0
                ? 0.0
                : static_cast<double>(a.route_cache_hits) /
                  static_cast<double>(a.active_route_elements);
        const double residual_eval_ratio =
            a.total_n == 0 ? 0.0
                           : static_cast<double>(a.residual_indexed_eval) /
                             static_cast<double>(a.total_n);
        const double bucket_reuse_ratio =
            a.bucket_scheduled_eval == 0
                ? 0.0
                : static_cast<double>(a.bucket_schedule_reused) /
                  static_cast<double>(a.bucket_scheduled_eval);

        std::cerr << "[DAZG_ORBIT_STAGEY_CERT]"
                  << " calls=" << a.calls
                  << " total_n=" << a.total_n
                  << " total_time_us=" << a.total_time_us
                  << " pbs_calls=" << a.pbs_calls
                  << " pbs_saved=" << a.pbs_saved
                  << " pbs_saved_ratio=" << pbs_saved_ratio
                  << " active_ratio=" << active_ratio
                  << " table_elided=" << a.table_elided
                  << " table_elision_ratio=" << table_elision_ratio
                  << " route_elided=" << a.route_elided
                  << " route_elision_ratio=" << route_elision_ratio
                  << " deterministic_scatter_hits=" << a.deterministic_scatter_hits
                  << " deterministic_scatter_ratio=" << deterministic_scatter_ratio
                  << " active_route_elements=" << a.active_route_elements
                  << " route_cache_hits=" << a.route_cache_hits
                  << " route_cache_reuse_ratio=" << route_cache_reuse_ratio
                  << " residual_candidates=" << a.residual_candidates
                  << " residual_indexed_eval=" << a.residual_indexed_eval
                  << " residual_eval_ratio=" << residual_eval_ratio
                  << " bucket_scheduled_eval=" << a.bucket_scheduled_eval
                  << " bucket_schedule_reused=" << a.bucket_schedule_reused
                  << " bucket_reuse_ratio=" << bucket_reuse_ratio
                  << " range_cert_hits=" << a.range_cert_hits
                  << " domain_cert_hits=" << a.domain_cert_hits
                  << " certificate=domain_range_scatter_residual_bucket_plut"
                  << " paper_claim=certified_residual_plut_activation"
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << std::endl;


        std::cerr << "[DAZG_ORBIT_TFHE_PROFILE]"
                  << " profile=stagey_simulated_pbs_activation"
                  << " calls=" << a.calls
                  << " total_n=" << a.total_n
                  << " pbs_calls=" << a.pbs_calls
                  << " pbs_saved=" << a.pbs_saved
                  << " pbs_saved_ratio=" << pbs_saved_ratio
                  << " active_route_elements=" << a.active_route_elements
                  << " route_cache_hits=" << a.route_cache_hits
                  << " route_cache_reuse_ratio=" << route_cache_reuse_ratio
                  << " bucket_scheduled_eval=" << a.bucket_scheduled_eval
                  << " bucket_schedule_reused=" << a.bucket_schedule_reused
                  << " bucket_reuse_ratio=" << bucket_reuse_ratio
                  << " key_switch_calls=0"
                  << " external_product_calls=0"
                  << " blind_rotate_calls=0"
                  << " bootstrap_key_decompress_calls=0"
                  << " batch_scheduler=1"
                  << " lazy_materialization_ready=1"
                  << " compressed_key_hotpath=0"
                  << " parameter_policy=site_semantic_activation"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }

private:
    DAZGOrbitStageYLedger() = default;
    std::mutex mu_;
    bool emitted_ = false;
    DAZGOrbitStageYAggregate agg_;
};


using dazg_orbit::tfhe::StageSBatchStats;
using dazg_orbit::tfhe::StageSPlutConfig;
using dazg_orbit::tfhe::StageSPlutEncoder;
using dazg_orbit::tfhe::StageSPlutTables;
using dazg_orbit::tfhe::StageSSimulatedPBS;

constexpr int kStageSInputBits = DAZG_ORBIT_STAGE_S_INPUT_BITS;
constexpr double kStageSClip = DAZG_ORBIT_STAGE_S_CLIP;
constexpr int kStageSCoeffBits = DAZG_ORBIT_STAGE_S_COEFF_BITS;
constexpr int kStageSZeroBucketRadius = DAZG_ORBIT_STAGE_S_ZERO_BUCKET_RADIUS;
constexpr bool kStageTTailSparse = (DAZG_ORBIT_STAGE_T_TAIL_SPARSE != 0);
constexpr bool kStageUBucketMemo = (DAZG_ORBIT_STAGE_U_BUCKET_MEMO != 0);
constexpr bool kStageVRangeCert = (DAZG_ORBIT_STAGE_V_RANGE_CERT != 0);
constexpr bool kStageWDomainCert = (DAZG_ORBIT_STAGE_W_DOMAIN_CERT != 0);
constexpr bool kStageXResidualScatter = (DAZG_ORBIT_STAGE_X_RESIDUAL_SCATTER != 0);
constexpr bool kStageYBucketSchedule = (DAZG_ORBIT_STAGE_Y_BUCKET_SCHEDULE != 0);

static_assert(kStageSInputBits >= 6, "Stage-S expects at least 6 BFE bits");
static_assert(kStageSInputBits <= 12, "Stage-S keeps the clear PLUT compact");
static_assert(kStageSCoeffBits >= 16, "Stage-S coefficient scale is too small");
static_assert(kStageSCoeffBits <= 40, "Stage-S coefficient scale is too large");

static inline std::int64_t round_to_fp(long double x, int scale) {
    const long double y = std::ldexp(x, scale);
    if (y > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (y < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(std::llround(y));
}

static StageSPlutConfig make_stage_s_config(int scale) {
    StageSPlutConfig cfg;
    cfg.input_bits = kStageSInputBits;
    cfg.scale_bits = scale;
    cfg.coeff_scale_bits = kStageSCoeffBits;
    cfg.clip = static_cast<double>(kStageSClip);
    cfg.zero_guard_buckets = kStageSZeroBucketRadius;
    cfg.enable_tail_sparse = kStageTTailSparse;
    cfg.enable_bucket_memo = kStageUBucketMemo;
    cfg.enable_range_certificate = kStageVRangeCert;
    cfg.enable_domain_mix_certificate = kStageWDomainCert;
    cfg.enable_residual_index_scatter = kStageXResidualScatter;
    cfg.enable_bucket_schedule = kStageYBucketSchedule && dazg_orbit::ablation::EnableStageY();
    return cfg;
}

struct StageSRuntime {
    StageSPlutConfig cfg;
    StageSPlutEncoder encoder;
    StageSPlutTables tables;
    StageSSimulatedPBS pbs;

    explicit StageSRuntime(int scale)
        : cfg(make_stage_s_config(scale)),
          encoder(cfg),
          tables(encoder.build_tables()),
          pbs(encoder, tables) {}
};

static std::shared_ptr<const StageSRuntime> get_stage_s_runtime(int scale) {
    static std::mutex mu;
    static std::map<int, std::shared_ptr<const StageSRuntime>> cache;

    std::lock_guard<std::mutex> lock(mu);
    auto it = cache.find(scale);
    if (it != cache.end()) {
        return it->second;
    }

    auto runtime = std::make_shared<StageSRuntime>(scale);
    cache.emplace(scale, runtime);

    std::cerr << "[StageS PLUT build]"
              << " stage=StageY-BucketScheduled-ResidualScatter-PLUT"
              << " scale=" << scale
              << " input_bits=" << runtime->tables.input_bits
              << " lut_rows=" << runtime->tables.lut_size
              << " folded_rows=" << runtime->tables.fold_size
              << " coeff_bits=" << runtime->tables.coeff_scale_bits
              << " clip=" << runtime->tables.clip
              << " clip_fp=" << runtime->tables.clip_fp
              << " zero_guard_fp=" << runtime->tables.zero_guard_fp
              << " fold_bucket_width_fp=" << runtime->tables.fold_bucket_width_fp
              << " full_bucket_width_fp=" << runtime->tables.full_bucket_width_fp
              << " gelu_build_max_err_fp="
              << static_cast<double>(runtime->tables.gelu_build_max_err_fp)
              << " silu_build_max_err_fp="
              << static_cast<double>(runtime->tables.silu_build_max_err_fp)
              << " tail_sparse=" << (runtime->cfg.enable_tail_sparse ? 1 : 0)
              << " bucket_memo=" << (runtime->cfg.enable_bucket_memo ? 1 : 0)
              << " range_cert=" << (runtime->cfg.enable_range_certificate ? 1 : 0)
              << " domain_cert=" << (runtime->cfg.enable_domain_mix_certificate ? 1 : 0)
              << " residual_scatter=" << (runtime->cfg.enable_residual_index_scatter ? 1 : 0)
              << " enable_stage_y=" << (dazg_orbit::ablation::EnableStageY() ? 1 : 0)
              << " bucket_schedule=" << (runtime->cfg.enable_bucket_schedule ? 1 : 0)
              << " pbs_output=deterministic_scatter_or_bucket_scheduled_route_control_word"
              << std::endl;

    return runtime;
}

struct ErrorStats {
    std::int64_t max_abs_err_fp = 0;
    long double sum_abs_err_fp = 0.0L;
};

template <typename ExactFn>
static ErrorStats collect_error_stats(
    const std::vector<std::int64_t>& x_fp,
    const std::vector<std::int64_t>& y_fp,
    int scale,
    bool is_gelu,
    const StageSPlutTables& tables,
    ExactFn exact_fn) {

    ErrorStats out;
    for (std::size_t i = 0; i < x_fp.size(); ++i) {
        const std::int64_t x = x_fp[i];

        std::int64_t exact_fp = y_fp[i];

        const bool deterministic_gelu_tail =
            is_gelu && (x >= tables.clip_fp || x <= -tables.clip_fp);

        const bool deterministic_silu_tail =
            (!is_gelu) && (x >= tables.clip_fp || x <= -tables.clip_fp);

        if (!deterministic_gelu_tail && !deterministic_silu_tail) {
            const long double x_real =
                std::ldexp(static_cast<long double>(x), -scale);
            exact_fp = round_to_fp(exact_fn(x_real), scale);
        }

        const std::int64_t err =
            (y_fp[i] >= exact_fp) ? (y_fp[i] - exact_fp) : (exact_fp - y_fp[i]);

        if (err > out.max_abs_err_fp) {
            out.max_abs_err_fp = err;
        }
        out.sum_abs_err_fp += static_cast<long double>(err);
    }

    return out;
}

template <typename ExactFn>
static void eval_common(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale,
    std::vector<std::int64_t>& y_fp,
    const std::string& fn_name,
    ExactFn exact_fn) {

    static std::atomic<bool> active_once{false};
    if (!active_once.exchange(true)) {
        std::cerr << "[BFEGeLUClear active]"
                  << " fn=" << fn_name
                  << " n=" << x_fp.size()
                  << " bitwidth=" << bitwidth
                  << " scale=" << scale
                  << " enable_stage_y=" << (dazg_orbit::ablation::EnableStageY() ? 1 : 0)
                  << " encoding=StageY-BucketScheduled-ResidualScatter-TailSparse-Route-CW-PLUT"
                  << " pbs_output=deterministic_scatter_or_bucket_scheduled_route_control_word"
                  << " postprocess=integer_quadratic"
                  << " gelu_identity=GeLU(x)=x+GeLU(-x)"
                  << " route_compaction=single_pass_deterministic_scatter+bucket_scheduled_residual_queue+unique_route_words"
                  << " routes=tail_identity+tail_zero+zero_micro_poly+folded_coeff_word"
                  << std::endl;
    }

    const auto runtime = get_stage_s_runtime(scale);
    const bool is_gelu = (fn_name == "gelu");

    auto t0 = std::chrono::high_resolution_clock::now();

    y_fp.resize(x_fp.size());

    StageSBatchStats stats;
    if (is_gelu) {
        runtime->encoder.evaluate_gelu_tail_sparse_batch_fp(
            x_fp.data(),
            x_fp.size(),
            bitwidth,
            runtime->tables,
            y_fp.data(),
            &stats);
    } else {
        runtime->encoder.evaluate_silu_tail_sparse_batch_fp(
            x_fp.data(),
            x_fp.size(),
            bitwidth,
            runtime->tables,
            y_fp.data(),
            &stats);
    }

    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    static std::atomic<int> prints{0};
    const int print_slot = prints.fetch_add(1);
    if (print_slot < DAZG_ORBIT_BFE_GELU_STATS_LIMIT) {
        const ErrorStats err =
            collect_error_stats(
                x_fp,
                y_fp,
                scale,
                is_gelu,
                runtime->tables,
                exact_fn);

        const long double mean_abs_err = x_fp.empty()
            ? 0.0L
            : err.sum_abs_err_fp / static_cast<long double>(x_fp.size());

        const std::size_t coeff_hits =
            stats.folded_coeff_hits + stats.full_coeff_hits;
        const std::size_t table_elided =
            stats.tail_zero + stats.tail_identity + stats.zero_poly_hits + stats.tail_exact;
        const std::size_t in_domain =
            stats.zero_poly_hits + stats.folded_coeff_hits + stats.full_coeff_hits;
        const std::size_t route_elements =
            stats.pbs_calls + stats.route_cache_hits;
        const std::size_t pbs_saved =
            stats.route_elided + stats.route_cache_hits;

        const double range_cert_ratio = x_fp.empty()
            ? 0.0
            : static_cast<double>(stats.range_cert_hits) /
              static_cast<double>(x_fp.size());
        const double domain_cert_ratio = x_fp.empty()
            ? 0.0
            : static_cast<double>(stats.domain_cert_hits) /
              static_cast<double>(x_fp.size());
        const double deterministic_scatter_ratio = x_fp.empty()
            ? 0.0
            : static_cast<double>(stats.deterministic_scatter_hits) /
              static_cast<double>(x_fp.size());
        const double bucket_schedule_reuse_ratio =
            stats.bucket_scheduled_eval == 0
                ? 0.0
                : static_cast<double>(stats.bucket_schedule_reused) /
                  static_cast<double>(stats.bucket_scheduled_eval);

        const double active_ratio = x_fp.empty()
            ? 0.0
            : static_cast<double>(stats.pbs_calls) /
              static_cast<double>(x_fp.size());
        const double route_element_ratio = x_fp.empty()
            ? 0.0
            : static_cast<double>(route_elements) /
              static_cast<double>(x_fp.size());
        const double cache_hit_ratio = route_elements == 0
            ? 0.0
            : static_cast<double>(stats.route_cache_hits) /
              static_cast<double>(route_elements);
        const double coeff_ratio = x_fp.empty()
            ? 0.0
            : static_cast<double>(coeff_hits) /
              static_cast<double>(x_fp.size());


        const bool stagey_enabled_for_active_log = dazg_orbit::ablation::EnableStageY();
        if (stagey_enabled_for_active_log) {
            DAZGOrbitStageYLedger::Instance().Record(
                static_cast<std::uint64_t>(x_fp.size()),
                static_cast<std::uint64_t>(us),
                static_cast<std::uint64_t>(stats.pbs_calls),
                static_cast<std::uint64_t>(pbs_saved),
                static_cast<std::uint64_t>(table_elided),
                static_cast<std::uint64_t>(stats.route_elided),
                static_cast<std::uint64_t>(stats.active_route_elements),
                static_cast<std::uint64_t>(route_elements),
                static_cast<std::uint64_t>(stats.route_cache_hits),
                static_cast<std::uint64_t>(stats.deterministic_scatter_hits),
                static_cast<std::uint64_t>(stats.range_cert_hits),
                static_cast<std::uint64_t>(stats.domain_cert_hits),
                static_cast<std::uint64_t>(stats.residual_candidates),
                static_cast<std::uint64_t>(stats.residual_indexed_eval),
                static_cast<std::uint64_t>(stats.bucket_scheduled_eval),
                static_cast<std::uint64_t>(stats.bucket_schedule_reused));
        }

        std::cerr << (stagey_enabled_for_active_log ? "[DAZG_ORBIT_STAGEY_STATS]" : "[DAZG_ORBIT_STAGEY_DIAG]")
                  << " stage=StageY-BucketScheduled-ResidualScatter-PLUT"
                  << " enable_stage_y=" << (dazg_orbit::ablation::EnableStageY() ? 1 : 0)
                  << " bucket_schedule=" << (runtime->cfg.enable_bucket_schedule ? 1 : 0)
                  << " fn=" << fn_name
                  << " n=" << x_fp.size()
                  << " time_us=" << us
                  << " in_domain=" << in_domain
                  << " coeff_hits=" << coeff_hits
                  << " folded_coeff_hits=" << stats.folded_coeff_hits
                  << " full_coeff_hits=" << stats.full_coeff_hits
                  << " zero_poly_hits=" << stats.zero_poly_hits
                  << " tail_identity=" << stats.tail_identity
                  << " tail_zero=" << stats.tail_zero
                  << " tail_exact=" << stats.tail_exact
                  << " table_elided=" << table_elided
                  << " route_elided=" << stats.route_elided
                  << " active_route_elements=" << stats.active_route_elements
                  << " route_elements=" << route_elements
                  << " unique_route_keys=" << stats.unique_route_keys
                  << " route_cache_hits=" << stats.route_cache_hits
                  << " range_cert_hits=" << stats.range_cert_hits
                  << " bulk_zero_poly=" << stats.bulk_zero_poly
                  << " bulk_tail_identity=" << stats.bulk_tail_identity
                  << " bulk_tail_zero=" << stats.bulk_tail_zero
                  << " bulk_tail_exact=" << stats.bulk_tail_exact
                  << " domain_cert_hits=" << stats.domain_cert_hits
                  << " bulk_tail_pair_mix=" << stats.bulk_tail_pair_mix
                  << " bulk_tail_guard_mix=" << stats.bulk_tail_guard_mix
                  << " residual_candidates=" << stats.residual_candidates
                  << " residual_indexed_eval=" << stats.residual_indexed_eval
                  << " deterministic_scatter_hits=" << stats.deterministic_scatter_hits
                  << " identity_default_hits=" << stats.identity_default_hits
                  << " bucket_scheduled_eval=" << stats.bucket_scheduled_eval
                  << " bucket_schedule_bins=" << stats.bucket_schedule_bins
                  << " bucket_schedule_reused=" << stats.bucket_schedule_reused
                  << " bucket_schedule_reuse_ratio=" << bucket_schedule_reuse_ratio
                  << " pbs_calls=" << stats.pbs_calls
                  << " pbs_saved=" << pbs_saved
                  << " range_cert_ratio=" << range_cert_ratio
                  << " domain_cert_ratio=" << domain_cert_ratio
                  << " deterministic_scatter_ratio=" << deterministic_scatter_ratio
                  << " active_ratio=" << active_ratio
                  << " route_element_ratio=" << route_element_ratio
                  << " cache_hit_ratio=" << cache_hit_ratio
                  << " coeff_ratio=" << coeff_ratio
                  << " other_routes=" << stats.other_routes
                  << " min_fp=" << stats.min_x_fp
                  << " max_fp=" << stats.max_x_fp
                  << " clip_fp=" << runtime->tables.clip_fp
                  << " zero_guard_fp=" << runtime->tables.zero_guard_fp
                  << " fold_bucket_width_fp=" << runtime->tables.fold_bucket_width_fp
                  << " full_bucket_width_fp=" << runtime->tables.full_bucket_width_fp
                  << " max_abs_err_fp=" << err.max_abs_err_fp
                  << " mean_abs_err_fp=" << static_cast<double>(mean_abs_err)
                  << " semantic=single_pass_deterministic_scatter__bucket_scheduled_residual_queue__unique_BFE_route_words__local_integer_poly"
                  << std::endl;
    }
}

} // namespace

void EvalBFEGeLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale,
    std::vector<std::int64_t>& y_fp) {
    eval_common(
        x_fp,
        bitwidth,
        scale,
        y_fp,
        "gelu",
        [](long double x) { return StageSPlutEncoder::gelu_exact(x); });
}

std::vector<std::int64_t> EvalBFEGeLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale) {
    std::vector<std::int64_t> y_fp;
    EvalBFEGeLUClear(x_fp, bitwidth, scale, y_fp);
    return y_fp;
}

void EvalBFESiLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale,
    std::vector<std::int64_t>& y_fp) {
    eval_common(
        x_fp,
        bitwidth,
        scale,
        y_fp,
        "silu",
        [](long double x) { return StageSPlutEncoder::silu_exact(x); });
}

std::vector<std::int64_t> EvalBFESiLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale) {
    std::vector<std::int64_t> y_fp;
    EvalBFESiLUClear(x_fp, bitwidth, scale, y_fp);
    return y_fp;
}

} // namespace HE
