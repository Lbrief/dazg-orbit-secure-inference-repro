// DAZG-Orbit Project Source File
// Component: HE/src/tfhe/DAZGRLut.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include "HE/tfhe/DAZGRLut.h"
#include "Utils/dazg_orbit_dazg_variant.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace dazg_orbit {
namespace tfhe {
namespace {

#if defined(DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX)
#  if defined(__GNUC__)
static const char DazgCheckpoint013TailFixMarker[] __attribute__((used)) =
    "DAZG_CHECKPOINT013_DAZG_GELU_TAIL_CONTRACT_20260715";
#  else
static const char DazgCheckpoint013TailFixMarker[] =
    "DAZG_CHECKPOINT013_DAZG_GELU_TAIL_CONTRACT_20260715";
#  endif
#endif

static inline double clamp_double(double x, double lo, double hi) {
    return std::min(std::max(x, lo), hi);
}

static inline std::int64_t round_to_fp(double x, int scale_bits) {
    return static_cast<std::int64_t>(std::llround(std::ldexp(x, scale_bits)));
}

static inline double eval_poly_double(const DAZGPoly& p, double x) {
    return (p.a2 * x + p.a1) * x + p.a0;
}

static inline double global_poly_approx_for_variant(double x, const std::string& fn_name) {
    const double xc = clamp_double(x, -8.0, 8.0);
    if (fn_name == "gelu") {
        const double y = 0.5 * xc + 0.1994711402 * xc * xc - 0.0030 * xc * xc * xc;
        return clamp_double(y, -0.5, 8.0);
    }
    if (fn_name == "silu") {
        const double sig = clamp_double(0.5 + 0.25 * xc - 0.0104166667 * xc * xc * xc, 0.0, 1.0);
        return xc * sig;
    }
    throw std::invalid_argument("DAZGRLutEncoder: unsupported function name");
}

static inline std::uint32_t lut_index_from_signed_bucket(
    int signed_bucket,
    int input_bits) {
    const int lut_size = 1 << input_bits;
    const int wrapped = signed_bucket & (lut_size - 1);
    return static_cast<std::uint32_t>(wrapped);
}

} // namespace

DAZGRLutEncoder::DAZGRLutEncoder(DAZGQuantConfig qcfg, DAZGSegmentConfig scfg)
    : qcfg_(std::move(qcfg)), scfg_(std::move(scfg)) {
    if (qcfg_.input_bits < 2 || qcfg_.input_bits > 15) {
        throw std::invalid_argument("DAZGRLutEncoder: input_bits must be in [2, 15]");
    }
    if (qcfg_.scale_bits < 0 || qcfg_.scale_bits > 30) {
        throw std::invalid_argument("DAZGRLutEncoder: scale_bits must be in [0, 30]");
    }
    if (qcfg_.coeff_scale_bits < qcfg_.scale_bits || qcfg_.coeff_scale_bits > 40) {
        throw std::invalid_argument(
            "DAZGRLutEncoder: coeff_scale_bits must be in [scale_bits, 40]");
    }
    if (qcfg_.clip <= 0.0) {
        throw std::invalid_argument("DAZGRLutEncoder: clip must be positive");
    }
    if (qcfg_.residual_bits < 0 || qcfg_.residual_bits > 6) {
        throw std::invalid_argument("DAZGRLutEncoder: residual_bits must be in [0, 6]");
    }
    if (qcfg_.residual_fp_step <= 0) {
        throw std::invalid_argument("DAZGRLutEncoder: residual_fp_step must be positive");
    }
    if (scfg_.breakpoints.size() < 2) {
        throw std::invalid_argument("DAZGRLutEncoder: need at least 2 breakpoints");
    }
    if (scfg_.breakpoints.size() > 65) {
        throw std::invalid_argument(
            "DAZGRLutEncoder: this 32-bit control-word format supports at most 64 segments");
    }
    if (std::abs(scfg_.breakpoints.front() + qcfg_.clip) > 1e-9 ||
        std::abs(scfg_.breakpoints.back() - qcfg_.clip) > 1e-9) {
        throw std::invalid_argument(
            "DAZGRLutEncoder: breakpoints must start at -clip and end at +clip");
    }
    for (std::size_t i = 1; i < scfg_.breakpoints.size(); ++i) {
        if (!(scfg_.breakpoints[i] > scfg_.breakpoints[i - 1])) {
            throw std::invalid_argument("DAZGRLutEncoder: breakpoints must be strictly increasing");
        }
    }
    if (scfg_.zero_guard_radius_buckets < 0) {
        throw std::invalid_argument("DAZGRLutEncoder: zero_guard_radius_buckets must be non-negative");
    }
}

std::uint32_t DAZGRLutEncoder::gray_encode(std::uint32_t x) {
    return x ^ (x >> 1U);
}

std::uint32_t DAZGRLutEncoder::gray_decode(std::uint32_t x) {
    std::uint32_t y = x;
    while (x >>= 1U) {
        y ^= x;
    }
    return y;
}

int DAZGRLutEncoder::clamp_signed_bits(int v, int bits) {
    if (bits <= 0) {
        return 0;
    }
    const int lo = -(1 << (bits - 1));
    const int hi =  (1 << (bits - 1)) - 1;
    return std::min(std::max(v, lo), hi);
}

std::uint32_t DAZGRLutEncoder::encode_signed_bits(int v, int bits) {
    if (bits <= 0) {
        return 0;
    }
    const int clipped = clamp_signed_bits(v, bits);
    const std::uint32_t mask = (1u << bits) - 1u;
    return static_cast<std::uint32_t>(clipped) & mask;
}

int DAZGRLutEncoder::decode_signed_bits(std::uint32_t v, int bits) {
    if (bits <= 0) {
        return 0;
    }
    const std::uint32_t mask = (1u << bits) - 1u;
    v &= mask;
    const std::uint32_t sign = 1u << (bits - 1);
    if ((v & sign) == 0u) {
        return static_cast<int>(v);
    }
    return static_cast<int>(v) - static_cast<int>(1u << bits);
}

std::int64_t DAZGRLutEncoder::round_shift(__int128 value, int shift) {
    if (shift <= 0) {
        return saturate_i128_to_i64(value << (-shift));
    }

    const __int128 half = static_cast<__int128>(1) << (shift - 1);
    if (value >= 0) {
        return saturate_i128_to_i64((value + half) >> shift);
    }
    return -saturate_i128_to_i64(((-value) + half) >> shift);
}

std::int64_t DAZGRLutEncoder::saturate_i128_to_i64(__int128 value) {
    const __int128 hi = static_cast<__int128>(std::numeric_limits<std::int64_t>::max());
    const __int128 lo = static_cast<__int128>(std::numeric_limits<std::int64_t>::min());
    if (value > hi) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value < lo) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

double DAZGRLutEncoder::gelu_exact(double x) {
    return 0.5 * x * (1.0 + std::erf(x / std::sqrt(2.0)));
}

double DAZGRLutEncoder::silu_exact(double x) {
    return x / (1.0 + std::exp(-x));
}

double DAZGRLutEncoder::exact_function(
    double x,
    const std::string& fn_name) const {
    if (fn_name == "gelu") {
        return gelu_exact(x);
    }
    if (fn_name == "silu") {
        return silu_exact(x);
    }
    throw std::invalid_argument("DAZGRLutEncoder: unsupported function name");
}

DAZGPoly DAZGRLutEncoder::fit_quadratic(
    double x0, double y0,
    double x1, double y1,
    double x2, double y2) {

    const double d0 = (x0 - x1) * (x0 - x2);
    const double d1 = (x1 - x0) * (x1 - x2);
    const double d2 = (x2 - x0) * (x2 - x1);

    if (std::abs(d0) < 1e-12 || std::abs(d1) < 1e-12 || std::abs(d2) < 1e-12) {
        throw std::runtime_error("DAZGRLutEncoder: degenerate quadratic fit");
    }

    DAZGPoly p;
    p.a2 = y0 / d0 + y1 / d1 + y2 / d2;
    p.a1 = -(x1 + x2) * y0 / d0
           -(x0 + x2) * y1 / d1
           -(x0 + x1) * y2 / d2;
    p.a0 = (x1 * x2) * y0 / d0
           +(x0 * x2) * y1 / d1
           +(x0 * x1) * y2 / d2;
    return p;
}

int DAZGRLutEncoder::find_segment(double x_real) const {
    if (x_real <= scfg_.breakpoints.front()) {
        return 0;
    }
    if (x_real >= scfg_.breakpoints.back()) {
        return static_cast<int>(scfg_.breakpoints.size()) - 2;
    }

    const auto it = std::upper_bound(
        scfg_.breakpoints.begin(), scfg_.breakpoints.end(), x_real);
    const std::ptrdiff_t idx = std::distance(scfg_.breakpoints.begin(), it) - 1;
    return static_cast<int>(std::max<std::ptrdiff_t>(0, idx));
}

DAZGPoly DAZGRLutEncoder::fit_segment_poly(
    double left,
    double right,
    const std::string& fn_name) const {

    const double mid = 0.5 * (left + right);
    return fit_quadratic(
        left, exact_function(left, fn_name),
        mid,  exact_function(mid, fn_name),
        right, exact_function(right, fn_name));
}

DAZGQPoly DAZGRLutEncoder::quantize_poly(const DAZGPoly& p) const {
    DAZGQPoly qp;
    qp.a0_q = round_to_fp(p.a0, qcfg_.coeff_scale_bits);
    qp.a1_q = round_to_fp(p.a1, qcfg_.coeff_scale_bits);
    qp.a2_q = round_to_fp(p.a2, qcfg_.coeff_scale_bits);
    return qp;
}

double DAZGRLutEncoder::bucket_to_real(int signed_bucket) const {
    const int signed_limit = (1 << (qcfg_.input_bits - 1)) - 1;
    const int clipped = std::min(std::max(signed_bucket, -signed_limit), signed_limit);
    return (static_cast<double>(clipped) / static_cast<double>(signed_limit)) * qcfg_.clip;
}

int DAZGRLutEncoder::signed_bucket_from_fp(std::int64_t x_fp, int qshift) const {
    if (qshift != 0) {
        throw std::invalid_argument(
            "DAZGRLutEncoder: Stage-E DA-ZG-RLUT uses qshift=0; rescale input before calling if needed");
    }

    const int signed_limit = (1 << (qcfg_.input_bits - 1)) - 1;
    const double x_real = std::ldexp(static_cast<double>(x_fp), -qcfg_.scale_bits);
    const double clipped = clamp_double(x_real, -qcfg_.clip, qcfg_.clip);
    const double scaled = (clipped / qcfg_.clip) * static_cast<double>(signed_limit);

    long rounded = std::lround(scaled);
    if (rounded < -signed_limit) rounded = -signed_limit;
    if (rounded >  signed_limit) rounded =  signed_limit;
    return static_cast<int>(rounded);
}

int DAZGRLutEncoder::local_u_for_bucket(
    double x_lookup,
    int seg_id) const {

    const double left = scfg_.breakpoints[static_cast<std::size_t>(seg_id)];
    const double right = scfg_.breakpoints[static_cast<std::size_t>(seg_id + 1)];
    const double center = 0.5 * (left + right);
    const double half_width = 0.5 * (right - left);

    if (half_width <= 0.0) {
        return 0;
    }

    const double u = (x_lookup - center) / half_width;
    return clamp_signed_bits(static_cast<int>(std::lround(u * 31.0)), 6);
}

std::int64_t DAZGRLutEncoder::eval_qpoly_fp(
    const DAZGQPoly& p,
    std::int64_t x_fp) const {

    const std::int64_t x2_fp = round_shift(
        static_cast<__int128>(x_fp) * static_cast<__int128>(x_fp),
        qcfg_.scale_bits);

    const std::int64_t t1_coeff_scale = round_shift(
        static_cast<__int128>(p.a1_q) * static_cast<__int128>(x_fp),
        qcfg_.scale_bits);

    const std::int64_t t2_coeff_scale = round_shift(
        static_cast<__int128>(p.a2_q) * static_cast<__int128>(x2_fp),
        qcfg_.scale_bits);

    const __int128 y_coeff_scale =
        static_cast<__int128>(p.a0_q) +
        static_cast<__int128>(t1_coeff_scale) +
        static_cast<__int128>(t2_coeff_scale);

    return round_shift(y_coeff_scale, qcfg_.coeff_scale_bits - qcfg_.scale_bits);
}

int DAZGRLutEncoder::safe_residual_for_bucket(
    int signed_bucket,
    int seg_id,
    const DAZGQPoly& qpoly,
    const std::string& fn_name) const {

    if (qcfg_.residual_bits <= 0) {
        return 0;
    }

    const double x0 = bucket_to_real(signed_bucket);
    const std::int64_t x0_fp = round_to_fp(x0, qcfg_.scale_bits);

    const std::int64_t exact_fp = round_to_fp(exact_function(x0, fn_name), qcfg_.scale_bits);
    const std::int64_t poly_fp = eval_qpoly_fp(qpoly, x0_fp);

    const std::int64_t raw_residual_fp = exact_fp - poly_fp;
    int residual_q = static_cast<int>(std::llround(
        static_cast<double>(raw_residual_fp) / static_cast<double>(qcfg_.residual_fp_step)));
    residual_q = clamp_signed_bits(residual_q, qcfg_.residual_bits);

    if (!scfg_.use_safe_residual || residual_q == 0) {
        return residual_q;
    }

    const int signed_limit = (1 << (qcfg_.input_bits - 1)) - 1;
    const double bucket_half_width =
        0.5 * qcfg_.clip / static_cast<double>(signed_limit);

    const double samples[3] = {
        clamp_double(x0 - bucket_half_width, -qcfg_.clip, qcfg_.clip),
        x0,
        clamp_double(x0 + bucket_half_width, -qcfg_.clip, qcfg_.clip)
    };

    std::int64_t base_max = 0;
    std::int64_t corr_max = 0;
    std::int64_t base_sum = 0;
    std::int64_t corr_sum = 0;

    const std::int64_t residual_fp =
        static_cast<std::int64_t>(residual_q) *
        static_cast<std::int64_t>(qcfg_.residual_fp_step);

    for (double x : samples) {
        const std::int64_t x_fp = round_to_fp(x, qcfg_.scale_bits);
        const std::int64_t target = round_to_fp(exact_function(x, fn_name), qcfg_.scale_bits);
        const std::int64_t base = eval_qpoly_fp(qpoly, x_fp);
        const std::int64_t corr = base + residual_fp;

        const std::int64_t base_err = std::llabs(base - target);
        const std::int64_t corr_err = std::llabs(corr - target);

        base_max = std::max(base_max, base_err);
        corr_max = std::max(corr_max, corr_err);
        base_sum += base_err;
        corr_sum += corr_err;
    }

    if (corr_max <= base_max && corr_sum <= base_sum) {
        return residual_q;
    }
    return 0;
}

std::uint32_t DAZGRLutEncoder::pack_control_word(
    bool negative,
    bool saturated,
    bool zero_guard,
    std::uint32_t seg_code,
    int local_u_q,
    std::uint32_t coeff_tag,
    int residual_q,
    std::uint32_t abs_bucket) const {

    // 31      : sign
    // 30      : saturation
    // 29      : zero_guard
    // 23..28  : 6-bit Gray-coded segment id
    // 17..22  : 6-bit signed local coordinate u
    // 13..16  : 4-bit coefficient-bank tag
    //  7..12  : 6-bit signed safe residual
    //  0.. 6  : absolute bucket id, saturated to [0, 127]
    std::uint32_t cw = 0;
    cw |= negative ? (1u << 31) : 0u;
    cw |= saturated ? (1u << 30) : 0u;
    cw |= zero_guard ? (1u << 29) : 0u;
    cw |= (seg_code & 0x3Fu) << 23;
    cw |= encode_signed_bits(local_u_q, 6) << 17;
    cw |= (coeff_tag & 0x0Fu) << 13;
    cw |= encode_signed_bits(residual_q, 6) << 7;
    cw |= (std::min<std::uint32_t>(abs_bucket, 127u) & 0x7Fu);
    return cw;
}

DAZGControlWord DAZGRLutEncoder::unpack_control_word(
    std::uint32_t cw,
    const DAZGLutTables& tables) const {

    DAZGControlWord out;
    out.negative = ((cw >> 31) & 1u) != 0u;
    out.saturated = ((cw >> 30) & 1u) != 0u;
    out.zero_guard = ((cw >> 29) & 1u) != 0u;

    out.seg_gray = (cw >> 23) & 0x3Fu;
    out.seg_id = tables.use_gray_code ? gray_decode(out.seg_gray) : out.seg_gray;

    out.local_u_q = decode_signed_bits((cw >> 17) & 0x3Fu, 6);
    out.coeff_tag = (cw >> 13) & 0x0Fu;
    out.residual_q = decode_signed_bits((cw >> 7) & 0x3Fu, 6);
    out.abs_bucket = cw & 0x7Fu;
    return out;
}

DAZGLutTables DAZGRLutEncoder::build_tables(
    int qshift,
    const std::string& fn_name) const {

    if (qshift != 0) {
        throw std::invalid_argument(
            "DAZGRLutEncoder: Stage-E DA-ZG-RLUT uses qshift=0; rescale input before calling if needed");
    }

    const int lut_size = 1 << qcfg_.input_bits;
    const int signed_limit = (1 << (qcfg_.input_bits - 1)) - 1;

    DAZGLutTables tables;
    tables.control_lut.resize(static_cast<std::size_t>(lut_size));
    tables.poly.resize(scfg_.breakpoints.size() - 1);
    tables.qpoly.resize(scfg_.breakpoints.size() - 1);
    tables.qshift = qshift;
    tables.input_bits = qcfg_.input_bits;
    tables.scale_bits = qcfg_.scale_bits;
    tables.coeff_scale_bits = qcfg_.coeff_scale_bits;
    tables.clip = qcfg_.clip;
    tables.breakpoints = scfg_.breakpoints;
    tables.use_gray_code = scfg_.use_gray_code;
    tables.zero_guard_radius_buckets = scfg_.zero_guard_radius_buckets;
    tables.residual_bits = qcfg_.residual_bits;
    tables.residual_fp_step = qcfg_.residual_fp_step;
    tables.residual_kept_count = 0;

    const dazg_orbit::dazg::VariantConfig variant = dazg_orbit::dazg::CurrentVariantConfig();
    dazg_orbit::dazg::EmitVariantLineOnce("DAZGRLutEncoder::build_tables");

    tables.zero_guard_radius_buckets = variant.zero_guard_enabled ? scfg_.zero_guard_radius_buckets : 0;
    tables.effective_qpoly_count = 0;
    tables.dazg_variant = variant.name;
    tables.variant_disable_zero_guard = !variant.zero_guard_enabled;
    tables.variant_disable_residual = !variant.residual_enabled;
    tables.variant_disable_qpoly_bank = !variant.qpoly_bank_enabled;
    tables.variant_plain_lut = variant.plain_lut;
    tables.variant_poly_approx = variant.poly_approx;
    tables.qpoly_bank_active = variant.qpoly_bank_enabled;
    tables.integer_postprocess = variant.qpoly_bank_enabled && !variant.plain_lut && !variant.poly_approx;
    if (variant.plain_lut) {
        tables.plain_lut_fp.assign(static_cast<std::size_t>(lut_size), 0);
    } else {
        tables.plain_lut_fp.clear();
    }

    for (std::size_t i = 0; i + 1 < scfg_.breakpoints.size(); ++i) {
        tables.poly[i] = fit_segment_poly(
            scfg_.breakpoints[i],
            scfg_.breakpoints[i + 1],
            fn_name);
        if (variant.qpoly_bank_enabled) {
            tables.qpoly[i] = quantize_poly(tables.poly[i]);
            if (tables.qpoly[i].a0_q != 0 || tables.qpoly[i].a1_q != 0 || tables.qpoly[i].a2_q != 0) {
                ++tables.effective_qpoly_count;
            }
        } else {
            tables.qpoly[i] = DAZGQPoly{};
        }
    }

    for (int idx = 0; idx < lut_size; ++idx) {
        const int signed_bucket_raw =
            (idx >= (lut_size >> 1)) ? (idx - lut_size) : idx;
        const int signed_bucket =
            std::min(std::max(signed_bucket_raw, -signed_limit), signed_limit);

        const double x_lookup = clamp_double(
            bucket_to_real(signed_bucket),
            -qcfg_.clip,
            qcfg_.clip);

        const bool negative = x_lookup < 0.0;
        const bool saturated = (std::abs(signed_bucket_raw) >= signed_limit);
        const bool zero_guard =
            variant.zero_guard_enabled &&
            std::abs(signed_bucket) <= scfg_.zero_guard_radius_buckets;

        if (variant.plain_lut) {
            tables.plain_lut_fp[static_cast<std::size_t>(idx)] =
                round_to_fp(exact_function(x_lookup, fn_name), qcfg_.scale_bits);
        }

        const int seg = find_segment(x_lookup);
        const std::uint32_t seg_code =
            scfg_.use_gray_code
                ? gray_encode(static_cast<std::uint32_t>(seg))
                : static_cast<std::uint32_t>(seg);

        const int local_u_q = local_u_for_bucket(x_lookup, seg);
        const std::uint32_t coeff_tag = static_cast<std::uint32_t>(seg) & 0x0Fu;

        const int residual_q = variant.residual_enabled && variant.qpoly_bank_enabled
            ? safe_residual_for_bucket(
                signed_bucket,
                seg,
                tables.qpoly[static_cast<std::size_t>(seg)],
                fn_name)
            : 0;

        if (residual_q != 0) {
            ++tables.residual_kept_count;
        }

        tables.control_lut[static_cast<std::size_t>(idx)] =
            pack_control_word(
                negative,
                saturated,
                zero_guard,
                seg_code,
                local_u_q,
                coeff_tag,
                residual_q,
                static_cast<std::uint32_t>(std::min(std::abs(signed_bucket), signed_limit)));
    }


    std::cerr << "[DAZG_ORBIT_DAZG_BUILD]"
              << " function=" << fn_name
              << " variant=" << tables.dazg_variant
              << " dazg_variant=" << tables.dazg_variant
              << " zero_guard_radius_buckets=" << tables.zero_guard_radius_buckets
              << " zero_guard_enabled=" << (variant.zero_guard_enabled ? 1 : 0)
              << " residual_enabled=" << (variant.residual_enabled ? 1 : 0)
              << " qpoly_bank_active=" << (tables.qpoly_bank_active ? 1 : 0)
              << " residual_kept_count=" << tables.residual_kept_count
              << " qpoly_count=" << tables.qpoly.size()
              << " effective_qpoly_count=" << tables.effective_qpoly_count
              << " plain_lut=" << (tables.variant_plain_lut ? 1 : 0)
              << " poly_approx=" << (tables.variant_poly_approx ? 1 : 0)
              << " plain_lut_entries=" << tables.plain_lut_fp.size()
              << " integer_postprocess=" << (tables.integer_postprocess ? 1 : 0)
              << " execution_mode=" << dazg_orbit::dazg::ExecutionMode()
              << std::endl;

    return tables;
}

DAZGLutTables DAZGRLutEncoder::build_gelu_tables(int qshift) const {
    return build_tables(qshift, "gelu");
}

DAZGLutTables DAZGRLutEncoder::build_silu_tables(int qshift) const {
    return build_tables(qshift, "silu");
}

double DAZGRLutEncoder::evaluate_double_from_control_word(
    std::uint32_t cw,
    const DAZGLutTables& tables,
    double x_real,
    const std::string& fn_name) const {

    (void)fn_name;

    const DAZGControlWord decoded = unpack_control_word(cw, tables);
    const std::size_t seg_idx =
        std::min<std::size_t>(
            static_cast<std::size_t>(decoded.seg_id),
            tables.poly.size() - 1);

    const double x_eval = clamp_double(
        x_real,
        tables.breakpoints.front(),
        tables.breakpoints.back());

    double y = eval_poly_double(tables.poly[seg_idx], x_eval);

    const double residual_real =
        static_cast<double>(decoded.residual_q * tables.residual_fp_step) /
        static_cast<double>(std::int64_t{1} << tables.scale_bits);

    y += residual_real;
    return y;
}

std::int64_t DAZGRLutEncoder::evaluate_fp_from_tables(
    std::int64_t x_fp,
    const DAZGLutTables& tables,
    const std::string& fn_name) const {

    (void)fn_name;

    const int signed_bucket = signed_bucket_from_fp(x_fp, tables.qshift);
    const std::uint32_t idx =
        lut_index_from_signed_bucket(signed_bucket, tables.input_bits);

    const std::uint32_t packed = tables.control_lut[idx];
    const DAZGControlWord cw = unpack_control_word(packed, tables);

    const std::size_t seg_idx =
        std::min<std::size_t>(
            static_cast<std::size_t>(cw.seg_id),
            tables.qpoly.size() - 1);

    const std::int64_t clipped_x_fp = std::min<std::int64_t>(
        std::max<std::int64_t>(
            x_fp,
            -round_to_fp(tables.clip, tables.scale_bits)),
        round_to_fp(tables.clip, tables.scale_bits));

    if (tables.variant_plain_lut && idx < tables.plain_lut_fp.size()) {
        return tables.plain_lut_fp[idx];
    }

    const double x_eval_real = clamp_double(
        std::ldexp(static_cast<double>(clipped_x_fp), -tables.scale_bits),
        tables.breakpoints.front(),
        tables.breakpoints.back());

    if (tables.variant_poly_approx) {
        return round_to_fp(global_poly_approx_for_variant(x_eval_real, fn_name), tables.scale_bits);
    }

    if (!tables.qpoly_bank_active) {
        return round_to_fp(eval_poly_double(tables.poly[seg_idx], x_eval_real), tables.scale_bits);
    }

    const DAZGQPoly& qp = tables.qpoly[seg_idx];

    const std::int64_t x2_fp = round_shift(
        static_cast<__int128>(clipped_x_fp) * static_cast<__int128>(clipped_x_fp),
        tables.scale_bits);

    const std::int64_t t1_coeff_scale = round_shift(
        static_cast<__int128>(qp.a1_q) * static_cast<__int128>(clipped_x_fp),
        tables.scale_bits);

    const std::int64_t t2_coeff_scale = round_shift(
        static_cast<__int128>(qp.a2_q) * static_cast<__int128>(x2_fp),
        tables.scale_bits);

    const __int128 y_coeff_scale =
        static_cast<__int128>(qp.a0_q) +
        static_cast<__int128>(t1_coeff_scale) +
        static_cast<__int128>(t2_coeff_scale);

    std::int64_t y_fp =
        round_shift(y_coeff_scale, tables.coeff_scale_bits - tables.scale_bits);

    if (!tables.variant_disable_residual) {
        y_fp += static_cast<std::int64_t>(cw.residual_q) *
                static_cast<std::int64_t>(tables.residual_fp_step);
    }

    return y_fp;
}

DAZGQuantConfig DefaultDAZGQuantConfig(int scale_bits) {
    DAZGQuantConfig qcfg;
    qcfg.clip = 8.0;
    qcfg.input_bits = 8;
    qcfg.scale_bits = scale_bits;
    qcfg.poly_degree = 2;
    qcfg.coeff_scale_bits = 24;
    qcfg.residual_bits = 6;
    qcfg.residual_fp_step = 1;
    return qcfg;
}

DAZGSegmentConfig DefaultDAZGSegmentConfig(double clip, int segments) {
    if (segments < 1) {
        throw std::invalid_argument("DefaultDAZGSegmentConfig: segments must be positive");
    }
    constexpr double kConcentration = 2.0;
    DAZGSegmentConfig scfg;
    scfg.breakpoints.reserve(static_cast<std::size_t>(segments + 1));
    const double denom = std::sinh(kConcentration);
    for (int i = 0; i <= segments; ++i) {
        const double t = 2.0 * static_cast<double>(i) / static_cast<double>(segments) - 1.0;
        scfg.breakpoints.push_back(clip * std::sinh(kConcentration * t) / denom);
    }
    scfg.breakpoints.front() = -clip;
    scfg.breakpoints.back() = clip;
    scfg.use_gray_code = true;
    scfg.zero_guard_radius_buckets = 1;
    scfg.use_safe_residual = true;
    return scfg;
}

namespace {

void EvalDAZGActivationFpImpl(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits,
    const std::string& fn_name,
    std::vector<std::int64_t>& output_fp) {

    (void)bitwidth;
    DAZGQuantConfig qcfg = DefaultDAZGQuantConfig(scale_bits);
    DAZGSegmentConfig scfg = DefaultDAZGSegmentConfig(qcfg.clip, 64);
    DAZGRLutEncoder enc(qcfg, scfg);
    DAZGLutTables tables = (fn_name == "gelu")
        ? enc.build_gelu_tables(0)
        : enc.build_silu_tables(0);

    output_fp.clear();
    output_fp.reserve(input_fp.size());
#if defined(DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX)
    const std::int64_t clip_fp = round_to_fp(qcfg.clip, scale_bits);
#endif
    for (std::int64_t x : input_fp) {
#if defined(DAZG_CHECKPOINT013_STAGE_S_TAIL_FIX)
        // Checkpoint-013 Stage-S contract. Preserve the frozen central
        // evaluator and override only the deterministic tails.
        if (fn_name == "gelu" && x <= -clip_fp) {
            output_fp.push_back(0);
        } else if (fn_name == "gelu" && x >= clip_fp) {
            output_fp.push_back(x);
        } else {
            output_fp.push_back(enc.evaluate_fp_from_tables(x, tables, fn_name));
        }
#else
        // Frozen P60 semantics. This is intentionally compiled in a separate
        // build tree from checkpoint-013 so the two validated lanes cannot
        // silently alter one another.
        output_fp.push_back(enc.evaluate_fp_from_tables(x, tables, fn_name));
#endif
    }

    const auto cfg = dazg_orbit::dazg::CurrentVariantConfig();
    std::cerr << "[DAZG_ORBIT_DAZG_EVAL]"
              << " fn=" << fn_name
              << " variant=" << cfg.name
              << " dazg_variant=" << tables.dazg_variant
              << " samples=" << input_fp.size()
              << " qpoly_count=" << tables.qpoly.size()
              << " effective_qpoly_count=" << tables.effective_qpoly_count
              << " zero_guard_enabled=" << (cfg.zero_guard_enabled ? 1 : 0)
              << " residual_enabled=" << (cfg.residual_enabled ? 1 : 0)
              << " qpoly_bank_active=" << (tables.qpoly_bank_active ? 1 : 0)
              << " plain_lut=" << (tables.variant_plain_lut ? 1 : 0)
              << " poly_approx=" << (tables.variant_poly_approx ? 1 : 0)
              << " integer_postprocess=" << (tables.integer_postprocess ? 1 : 0)
              << std::endl;
}

} // namespace

void EvalDAZGGeLUFp(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits,
    std::vector<std::int64_t>& output_fp) {
    EvalDAZGActivationFpImpl(input_fp, bitwidth, scale_bits, "gelu", output_fp);
}

std::vector<std::int64_t> EvalDAZGGeLUFp(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits) {
    std::vector<std::int64_t> out;
    EvalDAZGGeLUFp(input_fp, bitwidth, scale_bits, out);
    return out;
}

void EvalDAZGSiLUFp(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits,
    std::vector<std::int64_t>& output_fp) {
    EvalDAZGActivationFpImpl(input_fp, bitwidth, scale_bits, "silu", output_fp);
}

std::vector<std::int64_t> EvalDAZGSiLUFp(
    const std::vector<std::int64_t>& input_fp,
    int bitwidth,
    int scale_bits) {
    std::vector<std::int64_t> out;
    EvalDAZGSiLUFp(input_fp, bitwidth, scale_bits, out);
    return out;
}

} // namespace tfhe
} // namespace dazg_orbit
