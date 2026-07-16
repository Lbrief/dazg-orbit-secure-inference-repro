// DAZG-Orbit Project Source File
// Component: HE/include/HE/tfhe/DAZGRLut.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dazg_orbit {
namespace tfhe {

// Stage-E DA-ZG-RLUT.
//
// Stage-D already made the LUT output an algorithmic control word.  Stage-E
// removes the remaining floating-point postprocess from the runtime path:
// the per-segment polynomial bank is now quantized once during table build,
// and evaluate_fp_from_tables() performs integer-only fixed-point Horner
// evaluation plus the safe residual contained in the control word.

struct DAZGQuantConfig {
    double clip = 8.0;
    int input_bits = 8;
    int scale_bits = 16;
    int poly_degree = 2;

    // Coefficients are stored with this many fractional bits.  Keep this
    // larger than scale_bits so integer polynomial evaluation does not become
    // the accuracy bottleneck.
    int coeff_scale_bits = 24;

    // Signed residual correction stored in output fixed-point LSB units.
    // residual_bits=6 means residual_q is in [-32, 31].
    int residual_bits = 6;
    int residual_fp_step = 1;
};

struct DAZGSegmentConfig {
    std::vector<double> breakpoints;
    bool use_gray_code = true;

    int zero_guard_radius_buckets = 1;
    bool use_safe_residual = true;
};

struct DAZGPoly {
    double a0 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct DAZGQPoly {
    std::int64_t a0_q = 0;
    std::int64_t a1_q = 0;
    std::int64_t a2_q = 0;
};

struct DAZGControlWord {
    bool negative = false;
    bool saturated = false;
    bool zero_guard = false;

    std::uint32_t seg_id = 0;
    std::uint32_t seg_gray = 0;

    int local_u_q = 0;
    std::uint32_t coeff_tag = 0;
    int residual_q = 0;

    std::uint32_t abs_bucket = 0;
};

struct DAZGLutTables {
    std::vector<std::uint32_t> control_lut;

    // Floating coefficients are retained only for diagnostics/offline analysis.
    // Runtime fixed-point evaluation uses qpoly.
    std::vector<DAZGPoly> poly;
    std::vector<DAZGQPoly> qpoly;

    int qshift = 0;
    int input_bits = 8;
    int scale_bits = 16;
    int coeff_scale_bits = 24;
    double clip = 8.0;
    std::vector<double> breakpoints;
    bool use_gray_code = true;

    int zero_guard_radius_buckets = 1;
    int residual_bits = 6;
    int residual_fp_step = 1;
    std::size_t residual_kept_count = 0;

    // Variant-aware ablation metadata.  Production is full/qpoly/residual/zero-guard.
    std::string dazg_variant = "full";
    bool variant_disable_zero_guard = false;
    bool variant_disable_residual = false;
    bool variant_disable_qpoly_bank = false;
    bool variant_plain_lut = false;
    bool variant_poly_approx = false;
    bool qpoly_bank_active = true;
    bool integer_postprocess = true;
    std::size_t effective_qpoly_count = 0;

    // Fixed-point exact bucket LUT for the plain_lut ablation baseline.
    std::vector<std::int64_t> plain_lut_fp;
};

class DAZGRLutEncoder {
public:
    DAZGRLutEncoder(DAZGQuantConfig qcfg, DAZGSegmentConfig scfg);

    DAZGLutTables build_gelu_tables(int qshift) const;
    DAZGLutTables build_silu_tables(int qshift) const;

    int signed_bucket_from_fp(std::int64_t x_fp, int qshift) const;
    double bucket_to_real(int signed_bucket) const;

    std::uint32_t pack_control_word(
        bool negative,
        bool saturated,
        bool zero_guard,
        std::uint32_t seg_code,
        int local_u_q,
        std::uint32_t coeff_tag,
        int residual_q,
        std::uint32_t abs_bucket) const;

    DAZGControlWord unpack_control_word(
        std::uint32_t cw,
        const DAZGLutTables& tables) const;

    // Integer-only runtime evaluator.  The returned value is in output fp scale.
    std::int64_t evaluate_fp_from_tables(
        std::int64_t x_fp,
        const DAZGLutTables& tables,
        const std::string& fn_name) const;

    // Diagnostic reference path.  Do not use this as the runtime path.
    double evaluate_double_from_control_word(
        std::uint32_t cw,
        const DAZGLutTables& tables,
        double x_real,
        const std::string& fn_name) const;

private:
    static std::uint32_t gray_encode(std::uint32_t x);
    static std::uint32_t gray_decode(std::uint32_t x);

    static int clamp_signed_bits(int v, int bits);
    static std::uint32_t encode_signed_bits(int v, int bits);
    static int decode_signed_bits(std::uint32_t v, int bits);

    static std::int64_t round_shift(__int128 value, int shift);
    static std::int64_t saturate_i128_to_i64(__int128 value);

    static double gelu_exact(double x);
    static double silu_exact(double x);

    static DAZGPoly fit_quadratic(
        double x0, double y0,
        double x1, double y1,
        double x2, double y2);

    int find_segment(double x_real) const;

    DAZGPoly fit_segment_poly(
        double left,
        double right,
        const std::string& fn_name) const;

    DAZGQPoly quantize_poly(const DAZGPoly& p) const;

    double exact_function(
        double x,
        const std::string& fn_name) const;

    int local_u_for_bucket(
        double x_lookup,
        int seg_id) const;

    std::int64_t eval_qpoly_fp(
        const DAZGQPoly& p,
        std::int64_t x_fp) const;

    int safe_residual_for_bucket(
        int signed_bucket,
        int seg_id,
        const DAZGQPoly& qpoly,
        const std::string& fn_name) const;

    DAZGLutTables build_tables(
        int qshift,
        const std::string& fn_name) const;

    DAZGQuantConfig qcfg_;
    DAZGSegmentConfig scfg_;
};

DAZGQuantConfig DefaultDAZGQuantConfig(int scale_bits = 16);
DAZGSegmentConfig DefaultDAZGSegmentConfig(double clip = 8.0, int segments = 64);

void EvalDAZGGeLUFp(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits,
    std::vector<std::int64_t>& output_fp);

std::vector<std::int64_t> EvalDAZGGeLUFp(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits);

void EvalDAZGSiLUFp(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits,
    std::vector<std::int64_t>& output_fp);

std::vector<std::int64_t> EvalDAZGSiLUFp(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits);

} // namespace tfhe
} // namespace dazg_orbit
