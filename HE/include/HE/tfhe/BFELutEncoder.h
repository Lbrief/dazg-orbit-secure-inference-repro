// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/BFELutEncoder.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dazg_orbit::tfhe {

struct QuantConfig {
    int input_bits = 8;
    int scale_bits = 16;
    int residual_bits = 4;
    int coeff_frac_bits = 24;
    double clip = 8.0;
};

struct SegmentConfig {
    int segment_bits = 4;
    int segment_id_bits = 4;
    int local_u_bits = 4;

    // Compatibility fields used by Layer/NonlinearLayer/src/TFHEGeLU.cpp
    // and older BFE tests. Stage-S currently uses quadratic local
    // reconstruction, so poly_degree is kept as an ABI/config knob here.
    int poly_degree = 2;
    bool split_signed = true;

    int zero_guard_radius = 1;
    bool use_gray_code = true;
    std::vector<double> custom_breakpoints{};

    SegmentConfig() = default;

    // Old positional form:
    //   SegmentConfig{segment_bits, poly_degree, split_signed,
    //                 zero_guard_radius, custom_breakpoints}
    SegmentConfig(
        int segment_bits_,
        int poly_degree_,
        bool split_signed_,
        int zero_guard_radius_,
        std::vector<double> custom_breakpoints_ = {})
        : segment_bits(segment_bits_),
          segment_id_bits(segment_bits_),
          local_u_bits(4),
          poly_degree(poly_degree_),
          split_signed(split_signed_),
          zero_guard_radius(zero_guard_radius_),
          use_gray_code(true),
          custom_breakpoints(custom_breakpoints_) {}

    // Compact Stage-S positional form:
    //   SegmentConfig{segment_bits, segment_id_bits, local_u_bits,
    //                 zero_guard_radius, use_gray_code, custom_breakpoints}
    SegmentConfig(
        int segment_bits_,
        int segment_id_bits_,
        int local_u_bits_,
        int zero_guard_radius_,
        bool use_gray_code_,
        std::vector<double> custom_breakpoints_)
        : segment_bits(segment_bits_),
          segment_id_bits(segment_id_bits_),
          local_u_bits(local_u_bits_),
          poly_degree(2),
          split_signed(true),
          zero_guard_radius(zero_guard_radius_),
          use_gray_code(use_gray_code_),
          custom_breakpoints(custom_breakpoints_) {}
};

struct SegmentModel {
    double left = 0.0;
    double right = 0.0;
    double center = 0.0;
    std::array<double, 3> coeff{{0.0, 0.0, 0.0}};
    std::uint8_t coeff_tag = 0;
};

struct DecodedControlWord {
    bool neg = false;
    bool zero_guard = false;
    bool sat = false;
    std::uint8_t seg_gray = 0;
    std::uint8_t seg_id = 0;
    std::uint8_t abs_q = 0;
    std::int8_t residual_q = 0;
    std::uint8_t coeff_tag = 0;
    std::uint8_t func_tag = 0;
};

struct EncodedTables {
    int bucket_zero = 0;
    int max_abs_bucket = 0;
    int qshift = 0;
    bool relu_mode = false;

    std::vector<std::uint32_t> control_lut{};
    std::vector<std::uint32_t> cw_lut{};
    std::vector<std::uint32_t> sign_lut{};
    std::vector<std::uint32_t> seg_lut{};
    std::vector<std::uint32_t> u_lut{};
    std::vector<std::uint32_t> coeff_tag_lut{};
    std::vector<std::uint32_t> coeff0_lut{};
    std::vector<std::uint32_t> coeff1_lut{};
    std::vector<std::uint32_t> coeff2_lut{};
    std::vector<SegmentModel> segments{};
};

class BFELutEncoder {
public:
    BFELutEncoder(QuantConfig qcfg = QuantConfig{}, SegmentConfig scfg = SegmentConfig{});

    int demo_qshift() const;
    int real_to_index(double x_real, int qshift) const;
    int real_to_index(double x_real) const;

    std::uint32_t encode_control_word(
        bool neg,
        bool zero_guard,
        bool sat,
        std::uint8_t seg_id,
        std::uint8_t abs_q,
        std::int8_t residual_q,
        std::uint8_t coeff_tag,
        std::uint8_t func_tag) const;

    DecodedControlWord decode_control_word(std::uint32_t cw) const;

    EncodedTables build_relu() const;
    EncodedTables build_gelu() const;
    EncodedTables build_relu_control_lut(int qshift) const;
    EncodedTables build_gelu_control_lut(int qshift) const;

    std::vector<std::int32_t> build_sign_lut() const;
    std::vector<std::int32_t> build_seg_lut() const;
    std::vector<std::int32_t> build_u_lut() const;
    std::vector<std::int32_t> build_coeff_lut(int which) const;

    std::int64_t evaluate_from_control_word_fp(
        std::int64_t x_fp,
        std::uint32_t cw,
        int bitwidth,
        const EncodedTables& tables) const;

    double evaluate_from_control_word(
        std::uint32_t cw,
        const EncodedTables& tables) const;

private:
    int effective_segment_bits() const;
    int effective_residual_bits() const;
    std::uint8_t gray_encode(std::uint8_t x) const;
    std::uint8_t gray_decode(std::uint8_t x) const;
    std::uint64_t abs_u64(std::int64_t x) const;
    int choose_qshift(std::uint64_t max_abs) const;
    int signed_bucket_from_fp(std::int64_t x_fp, int qshift) const;
    std::int64_t clamp_to_bitwidth(std::int64_t x, int bitwidth) const;
    double gelu(double x) const;
    SegmentModel fit_quadratic_segment(double left, double right, std::uint8_t coeff_tag) const;
    std::int8_t quantize_residual(double u, double radius) const;
    double bucket_center_real(int signed_q, int qshift) const;
    void populate_legacy_views(EncodedTables& t) const;

    QuantConfig qcfg_{};
    SegmentConfig scfg_{};
};

enum class StageSFunction : std::uint32_t {
    GeLU = 1,
    SiLU = 2,
};

enum class StageSRouteKind : std::uint32_t {
    TailZero = 0,
    TailIdentity = 1,
    ZeroPolynomial = 2,
    FoldedCoeffWord = 3,
    FullCoeffWord = 4,
    TailExact = 5,
};

struct StageSRouteWord {
    StageSRouteKind kind = StageSRouteKind::TailExact;
    StageSFunction fn = StageSFunction::GeLU;
    bool negative = false;
    std::uint32_t bucket = 0;
};

struct StageSCoeffWord {
    std::int64_t center_fp = 0;
    std::int64_t a0_fp = 0;
    std::int64_t a1_q = 0;
    std::int64_t a2_q = 0;
    std::uint32_t tag = 0;
};

struct StageSPlutConfig {
    int input_bits = 8;
    int scale_bits = 16;
    int coeff_scale_bits = 24;
    double clip = 8.0;
    int zero_guard_buckets = 2;

    // Stage-T: tail-sparse active-set compaction. This keeps the route-word
    // contract unchanged, but the clear/reference path no longer spends a
    // simulated PBS route on deterministic GeLU/SiLU tails or the GeLU zero
    // micro-polynomial window.
    bool enable_tail_sparse = true;

    // Stage-U: bucket-memoized residual active set. For a batch, all values
    // landing in the same low-bit BFE bucket share the same route/control word.
    // This turns dense residual route lookup into unique-bucket scheduling.
    bool enable_bucket_memo = true;

    // Stage-V: whole-batch range certificates. If a tensor is entirely in a
    // deterministic activation region, the batch exits before residual routing:
    //   GeLU: x >= clip -> identity, x <= -clip -> zero,
    //         |x| <= zero_guard -> zero micro-polynomial.
    //   SiLU: |x| >= clip tails -> exact deterministic tail.
    bool enable_range_certificate = true;

    // Stage-W: deterministic-domain certificate. After a single domain scan,
    // a tensor with no residual coefficient candidates exits without route
    // allocation even when it is a mixed positive-tail / negative-tail /
    // zero-guard tensor.
    bool enable_domain_mix_certificate = true;

    // Stage-X: deterministic scatter plus residual-index queue. A single pass
    // writes all deterministic outputs and records only coefficient-domain
    // indices for bucket-memoized route evaluation.
    bool enable_residual_index_scatter = true;

    // Stage-Y: bucket-scheduled residual queue. Residual indices are grouped
    // by low-bit BFE bucket first; each non-empty bucket materializes exactly
    // one route/control word and then evaluates all indices in that bucket.
    bool enable_bucket_schedule = true;
};

struct StageSPlutTables {
    int scale_bits = 16;
    int input_bits = 8;
    int coeff_scale_bits = 24;
    double clip = 8.0;

    std::uint32_t lut_size = 0;
    std::uint32_t fold_size = 0;
    std::int64_t clip_fp = 0;
    std::int64_t fold_bucket_width_fp = 0;
    std::int64_t full_bucket_width_fp = 0;
    std::int64_t zero_guard_fp = 0;

    std::vector<std::uint32_t> gelu_route_lut{};
    std::vector<std::uint32_t> silu_route_lut{};
    std::vector<StageSCoeffWord> gelu_neg_residual_bank{};
    std::vector<StageSCoeffWord> silu_full_bank{};

    long double gelu_build_max_err_fp = 0.0L;
    long double silu_build_max_err_fp = 0.0L;
};

struct StageSBatchStats {
    std::size_t n = 0;

    // pbs_calls means expensive route/control-word lookups actually issued.
    // With Stage-U bucket memoization enabled, this is the number of unique
    // active BFE buckets, not the number of residual elements.
    std::size_t pbs_calls = 0;

    // Elements resolved without route lookup: deterministic tails and zero
    // micro-polynomial windows.
    std::size_t route_elided = 0;

    // Residual elements that still need a route/control word semantically.
    std::size_t active_route_elements = 0;

    // Residual elements served from a per-batch bucket memo entry instead of
    // issuing a new route lookup.
    std::size_t route_cache_hits = 0;

    // Number of memo entries materialized in the current batch.
    std::size_t unique_route_keys = 0;

    // Elements discharged by a whole-batch certificate before the residual
    // route loop starts. The bulk_* counters say which certificate fired.
    std::size_t range_cert_hits = 0;
    std::size_t bulk_zero_poly = 0;
    std::size_t bulk_tail_identity = 0;
    std::size_t bulk_tail_zero = 0;
    std::size_t bulk_tail_exact = 0;

    // Stage-W mixed deterministic-domain certificates.
    std::size_t domain_cert_hits = 0;
    std::size_t bulk_tail_pair_mix = 0;
    std::size_t bulk_tail_guard_mix = 0;
    std::size_t residual_candidates = 0;

    // Stage-X one-pass deterministic scatter accounting.
    std::size_t deterministic_scatter_hits = 0;
    std::size_t residual_indexed_eval = 0;
    std::size_t identity_default_hits = 0;

    // Stage-Y bucket-scheduled residual queue accounting.
    std::size_t bucket_scheduled_eval = 0;
    std::size_t bucket_schedule_bins = 0;
    std::size_t bucket_schedule_reused = 0;

    std::size_t tail_zero = 0;
    std::size_t tail_identity = 0;
    std::size_t zero_poly_hits = 0;
    std::size_t folded_coeff_hits = 0;
    std::size_t full_coeff_hits = 0;
    std::size_t tail_exact = 0;
    std::size_t other_routes = 0;
    std::int64_t min_x_fp = 0;
    std::int64_t max_x_fp = 0;
};

class StageSPlutEncoder {
public:
    explicit StageSPlutEncoder(StageSPlutConfig cfg = StageSPlutConfig{});

    static long double gelu_exact(long double x);
    static long double silu_exact(long double x);

    std::uint32_t encode_route(
        StageSRouteKind kind,
        StageSFunction fn,
        bool negative,
        std::uint32_t bucket) const;

    StageSRouteWord decode_route(std::uint32_t route) const;

    int signed_bucket_from_fp(
        std::int64_t x_fp,
        const StageSPlutTables& tables) const;

    StageSPlutTables build_tables() const;

    std::uint32_t route_gelu_from_fp(
        std::int64_t x_fp,
        const StageSPlutTables& tables) const;

    std::uint32_t route_silu_from_fp(
        std::int64_t x_fp,
        const StageSPlutTables& tables) const;

    std::int64_t evaluate_gelu_from_route_fp(
        std::int64_t x_fp,
        std::uint32_t route,
        const StageSPlutTables& tables,
        int bitwidth) const;

    std::int64_t evaluate_silu_from_route_fp(
        std::int64_t x_fp,
        std::uint32_t route,
        const StageSPlutTables& tables,
        int bitwidth) const;

    void evaluate_gelu_tail_sparse_batch_fp(
        const std::int64_t* x_fp,
        std::size_t n,
        int bitwidth,
        const StageSPlutTables& tables,
        std::int64_t* y_fp,
        StageSBatchStats* stats = nullptr) const;

    void evaluate_silu_tail_sparse_batch_fp(
        const std::int64_t* x_fp,
        std::size_t n,
        int bitwidth,
        const StageSPlutTables& tables,
        std::int64_t* y_fp,
        StageSBatchStats* stats = nullptr) const;

private:
    StageSPlutConfig cfg_{};
};

} // namespace dazg_orbit::tfhe
