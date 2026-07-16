// DAZG-Orbit Project Source File
// Component: HE/src/tfhe/BFELutEncoder.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include "HE/tfhe/BFELutEncoder.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace dazg_orbit::tfhe {
namespace {

constexpr std::uint32_t kMagic = 0xB5u;
constexpr std::uint32_t kMagicShift = 24;
constexpr std::uint32_t kMagicMask = 0xFF000000u;

constexpr int kFuncReLU = 0;
constexpr int kFuncGeLU = 1;

static std::int8_t sign_extend_4(std::uint8_t x) {
    x &= 0x0Fu;
    if ((x & 0x08u) != 0) {
        return static_cast<std::int8_t>(static_cast<int>(x) - 16);
    }
    return static_cast<std::int8_t>(x);
}

static int clamp_int(int x, int lo, int hi) {
    return std::max(lo, std::min(hi, x));
}

} // namespace

BFELutEncoder::BFELutEncoder(QuantConfig qcfg, SegmentConfig scfg)
    : qcfg_(qcfg), scfg_(scfg) {
    if (qcfg_.input_bits != 8) {
        throw std::invalid_argument("BFELutEncoder currently expects input_bits == 8");
    }
    if (effective_segment_bits() < 1 || effective_segment_bits() > 4) {
        throw std::invalid_argument("BFELutEncoder expects segment bits in [1, 4]");
    }
}

int BFELutEncoder::demo_qshift() const {
    return std::max(0, qcfg_.scale_bits - 4);
}

int BFELutEncoder::effective_segment_bits() const {
    int bits = scfg_.segment_id_bits;
    if (scfg_.segment_bits != 4 || scfg_.segment_id_bits == 4) {
        bits = scfg_.segment_bits;
    }
    return clamp_int(bits, 1, 4);
}

int BFELutEncoder::effective_residual_bits() const {
    int bits = qcfg_.residual_bits;
    if (scfg_.local_u_bits != 4 || qcfg_.residual_bits == 4) {
        bits = scfg_.local_u_bits;
    }
    return clamp_int(bits, 1, 8);
}

std::uint8_t BFELutEncoder::gray_encode(std::uint8_t x) const {
    return static_cast<std::uint8_t>((x ^ (x >> 1)) & 0x0Fu);
}

std::uint8_t BFELutEncoder::gray_decode(std::uint8_t x) const {
    x &= 0x0Fu;
    x ^= static_cast<std::uint8_t>(x >> 1);
    x ^= static_cast<std::uint8_t>(x >> 2);
    x ^= static_cast<std::uint8_t>(x >> 4);
    return static_cast<std::uint8_t>(x & 0x0Fu);
}

std::uint64_t BFELutEncoder::abs_u64(std::int64_t x) const {
    if (x >= 0) return static_cast<std::uint64_t>(x);
    return static_cast<std::uint64_t>(-(x + 1)) + 1u;
}

int BFELutEncoder::choose_qshift(std::uint64_t max_abs) const {
    const int max_abs_bucket = (1 << (qcfg_.input_bits - 1)) - 1;
    int shift = 0;
    while (shift < 62 && (max_abs >> shift) > static_cast<std::uint64_t>(max_abs_bucket)) {
        ++shift;
    }
    return shift;
}

int BFELutEncoder::signed_bucket_from_fp(std::int64_t x_fp, int qshift) const {
    const std::uint64_t ax = abs_u64(x_fp);
    std::uint64_t q = 0;
    if (ax != 0) {
        if (qshift <= 0) {
            q = ax;
        } else if (qshift >= 63) {
            q = 1;
        } else {
            const std::uint64_t base = ax >> qshift;
            const std::uint64_t mask = (std::uint64_t{1} << qshift) - 1u;
            q = base + ((ax & mask) ? 1u : 0u);
        }
    }

    const int max_abs_bucket = (1 << (qcfg_.input_bits - 1)) - 1;
    if (q > static_cast<std::uint64_t>(max_abs_bucket)) {
        q = static_cast<std::uint64_t>(max_abs_bucket);
    }

    return x_fp < 0 ? -static_cast<int>(q) : static_cast<int>(q);
}

int BFELutEncoder::real_to_index(double x_real, int qshift) const {
    const double scaled = x_real * static_cast<double>(std::uint64_t{1} << qcfg_.scale_bits);
    const double bucket = scaled / static_cast<double>(std::uint64_t{1} << qshift);
    int signed_q = static_cast<int>(std::llround(bucket));
    const int max_abs_bucket = (1 << (qcfg_.input_bits - 1)) - 1;
    signed_q = std::max(-max_abs_bucket, std::min(max_abs_bucket, signed_q));
    return signed_q + (1 << (qcfg_.input_bits - 1));
}

int BFELutEncoder::real_to_index(double x_real) const {
    return real_to_index(x_real, demo_qshift());
}

std::uint32_t BFELutEncoder::encode_control_word(
    bool neg,
    bool zero_guard,
    bool sat,
    std::uint8_t seg_id,
    std::uint8_t abs_q,
    std::int8_t residual_q,
    std::uint8_t coeff_tag,
    std::uint8_t func_tag) const {

    const std::uint8_t seg_mask = static_cast<std::uint8_t>((1u << effective_segment_bits()) - 1u);
    const std::uint8_t seg_gray = scfg_.use_gray_code ? gray_encode(seg_id & seg_mask) : (seg_id & seg_mask);
    const std::uint8_t residual_raw = static_cast<std::uint8_t>(residual_q) & 0x0Fu;

    std::uint32_t cw = 0;
    cw |= static_cast<std::uint32_t>(neg ? 1u : 0u);
    cw |= static_cast<std::uint32_t>(zero_guard ? 1u : 0u) << 1;
    cw |= static_cast<std::uint32_t>(sat ? 1u : 0u) << 2;
    cw |= static_cast<std::uint32_t>(seg_gray & 0x0Fu) << 3;
    cw |= static_cast<std::uint32_t>(abs_q & 0x7Fu) << 7;
    cw |= static_cast<std::uint32_t>(residual_raw & 0x0Fu) << 14;
    cw |= static_cast<std::uint32_t>(coeff_tag & 0x07u) << 18;
    cw |= static_cast<std::uint32_t>(func_tag & 0x07u) << 21;
    cw |= (kMagic << kMagicShift);
    return cw;
}

DecodedControlWord BFELutEncoder::decode_control_word(std::uint32_t cw) const {
    if ((cw & kMagicMask) != (kMagic << kMagicShift)) {
        throw std::runtime_error("BFELutEncoder: bad control word magic");
    }

    DecodedControlWord d;
    d.neg = (cw & 1u) != 0;
    d.zero_guard = ((cw >> 1) & 1u) != 0;
    d.sat = ((cw >> 2) & 1u) != 0;
    d.seg_gray = static_cast<std::uint8_t>((cw >> 3) & 0x0Fu);
    d.seg_id = scfg_.use_gray_code ? gray_decode(d.seg_gray) : d.seg_gray;
    d.abs_q = static_cast<std::uint8_t>((cw >> 7) & 0x7Fu);
    d.residual_q = sign_extend_4(static_cast<std::uint8_t>((cw >> 14) & 0x0Fu));
    d.coeff_tag = static_cast<std::uint8_t>((cw >> 18) & 0x07u);
    d.func_tag = static_cast<std::uint8_t>((cw >> 21) & 0x07u);
    return d;
}

std::int64_t BFELutEncoder::clamp_to_bitwidth(std::int64_t x, int bitwidth) const {
    if (bitwidth <= 0) return 0;
    if (bitwidth >= 63) return x;
    const std::int64_t lo = -(std::int64_t{1} << (bitwidth - 1));
    const std::int64_t hi =  (std::int64_t{1} << (bitwidth - 1)) - 1;
    return std::min<std::int64_t>(std::max<std::int64_t>(x, lo), hi);
}

double BFELutEncoder::gelu(double x) const {
    return 0.5 * x * (1.0 + std::erf(x / std::sqrt(2.0)));
}

SegmentModel BFELutEncoder::fit_quadratic_segment(double left, double right, std::uint8_t coeff_tag) const {
    SegmentModel seg;
    seg.left = left;
    seg.right = right;
    seg.center = 0.5 * (left + right);
    seg.coeff_tag = coeff_tag;

    if (right <= -2.5) {
        seg.coeff = {0.0, 0.0, 0.0};
        return seg;
    }
    if (left >= 2.5) {
        seg.coeff = {seg.center, 1.0, 0.0};
        return seg;
    }

    const double u1 = left - seg.center;
    const double u2 = right - seg.center;
    const double c0 = gelu(seg.center);
    const double y1 = gelu(left) - c0;
    const double y2 = gelu(right) - c0;
    const double det = u1 * u2 * (u2 - u1);

    double c1 = 0.0;
    double c2 = 0.0;
    if (std::fabs(det) > 1e-12) {
        c1 = (y1 * u2 * u2 - y2 * u1 * u1) / det;
        c2 = (u1 * y2 - u2 * y1) / det;
    } else {
        c1 = 0.5;
        c2 = 0.0;
    }

    seg.coeff = {c0, c1, c2};
    return seg;
}

std::int8_t BFELutEncoder::quantize_residual(double u, double radius) const {
    if (radius <= 0.0) return 0;
    const int used_bits = std::min(4, effective_residual_bits());
    const int max_mag = (1 << (used_bits - 1)) - 1;
    const double scaled = u * static_cast<double>(max_mag) / radius;
    int q = static_cast<int>(std::llround(scaled));
    q = std::max(-max_mag, std::min(max_mag, q));
    return static_cast<std::int8_t>(q);
}

double BFELutEncoder::bucket_center_real(int signed_q, int qshift) const {
    const double fp = static_cast<double>(signed_q) * static_cast<double>(std::uint64_t{1} << qshift);
    return fp / static_cast<double>(std::uint64_t{1} << qcfg_.scale_bits);
}

void BFELutEncoder::populate_legacy_views(EncodedTables& t) const {
    const std::size_t n = t.control_lut.size();
    t.cw_lut = t.control_lut;
    t.sign_lut.resize(n);
    t.seg_lut.resize(n);
    t.u_lut.resize(n);
    t.coeff_tag_lut.resize(n);
    t.coeff0_lut.resize(n);
    t.coeff1_lut.resize(n);
    t.coeff2_lut.resize(n);

    const int legacy_u_bits = effective_residual_bits();
    const int max_mag = (1 << (std::max(1, legacy_u_bits) - 1)) - 1;

    for (std::size_t i = 0; i < n; ++i) {
        const DecodedControlWord d = decode_control_word(t.control_lut[i]);
        t.sign_lut[i] = d.neg ? 1u : 0u;
        t.seg_lut[i] = static_cast<std::uint32_t>(d.seg_id);

        int u_val = d.residual_q;
        if (legacy_u_bits > 4 && max_mag > 7) {
            u_val = static_cast<int>(std::llround(static_cast<double>(d.residual_q) * static_cast<double>(max_mag) / 7.0));
        }
        t.u_lut[i] = static_cast<std::uint32_t>(static_cast<std::int32_t>(u_val));

        t.coeff_tag_lut[i] = static_cast<std::uint32_t>(d.coeff_tag);

        if (!t.segments.empty()) {
            const SegmentModel& seg = t.segments[std::min<std::size_t>(d.seg_id, t.segments.size() - 1)];
            t.coeff0_lut[i] = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                std::llround(seg.coeff[0] * static_cast<double>(std::uint64_t{1} << qcfg_.coeff_frac_bits))));
            t.coeff1_lut[i] = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                std::llround(seg.coeff[1] * static_cast<double>(std::uint64_t{1} << qcfg_.coeff_frac_bits))));
            t.coeff2_lut[i] = static_cast<std::uint32_t>(static_cast<std::int32_t>(
                std::llround(seg.coeff[2] * static_cast<double>(std::uint64_t{1} << qcfg_.coeff_frac_bits))));
        }
    }
}

EncodedTables BFELutEncoder::build_relu() const {
    return build_relu_control_lut(demo_qshift());
}

EncodedTables BFELutEncoder::build_gelu() const {
    return build_gelu_control_lut(demo_qshift());
}

std::vector<std::int32_t> BFELutEncoder::build_sign_lut() const {
    const auto t = build_gelu();
    return std::vector<std::int32_t>(t.sign_lut.begin(), t.sign_lut.end());
}

std::vector<std::int32_t> BFELutEncoder::build_seg_lut() const {
    const auto t = build_gelu();
    return std::vector<std::int32_t>(t.seg_lut.begin(), t.seg_lut.end());
}

std::vector<std::int32_t> BFELutEncoder::build_u_lut() const {
    const auto t = build_gelu();
    return std::vector<std::int32_t>(t.u_lut.begin(), t.u_lut.end());
}

std::vector<std::int32_t> BFELutEncoder::build_coeff_lut(int which) const {
    const auto t = build_gelu();
    if (which == 0) return std::vector<std::int32_t>(t.coeff0_lut.begin(), t.coeff0_lut.end());
    if (which == 1) return std::vector<std::int32_t>(t.coeff1_lut.begin(), t.coeff1_lut.end());
    return std::vector<std::int32_t>(t.coeff2_lut.begin(), t.coeff2_lut.end());
}

EncodedTables BFELutEncoder::build_relu_control_lut(int qshift) const {
    EncodedTables t;
    t.bucket_zero = 1 << (qcfg_.input_bits - 1);
    t.max_abs_bucket = (1 << (qcfg_.input_bits - 1)) - 1;
    t.qshift = qshift;
    t.relu_mode = true;
    t.control_lut.resize(1u << qcfg_.input_bits);

    SegmentModel seg;
    seg.left = 0.0;
    seg.right = std::numeric_limits<double>::infinity();
    seg.center = 0.0;
    seg.coeff = {0.0, 1.0, 0.0};
    seg.coeff_tag = 0;
    t.segments.push_back(seg);

    const int seg_bins = 1 << effective_segment_bits();

    for (int signed_q = -t.max_abs_bucket; signed_q <= t.max_abs_bucket; ++signed_q) {
        const int idx = signed_q + t.bucket_zero;
        const bool neg = signed_q < 0;
        const int abs_q = std::abs(signed_q);
        const bool zero_guard = abs_q <= scfg_.zero_guard_radius;
        const bool sat = abs_q >= t.max_abs_bucket;
        const std::uint8_t seg_id = static_cast<std::uint8_t>(std::min(seg_bins - 1, abs_q / std::max(1, 128 / seg_bins)));
        t.control_lut[static_cast<std::size_t>(idx)] = encode_control_word(
            neg, zero_guard, sat, seg_id,
            static_cast<std::uint8_t>(abs_q),
            0, 0, kFuncReLU);
    }

    t.control_lut[0] = encode_control_word(true, false, true, static_cast<std::uint8_t>(seg_bins - 1), 127, 0, 0, kFuncReLU);
    t.control_lut.back() = encode_control_word(false, false, true, static_cast<std::uint8_t>(seg_bins - 1), 127, 0, 0, kFuncReLU);
    populate_legacy_views(t);
    return t;
}

EncodedTables BFELutEncoder::build_gelu_control_lut(int qshift) const {
    EncodedTables t;
    t.bucket_zero = 1 << (qcfg_.input_bits - 1);
    t.max_abs_bucket = (1 << (qcfg_.input_bits - 1)) - 1;
    t.qshift = qshift;
    t.relu_mode = false;
    t.control_lut.resize(1u << qcfg_.input_bits);

    std::vector<double> bp = scfg_.custom_breakpoints;
    if (bp.empty()) {
        const double clip = std::max(1.0, qcfg_.clip);
        const int seg_count = 1 << effective_segment_bits();
        bp.resize(static_cast<std::size_t>(seg_count + 1));
        for (int i = 0; i <= seg_count; ++i) {
            const double alpha = static_cast<double>(i) / static_cast<double>(seg_count);
            bp[static_cast<std::size_t>(i)] = -clip + 2.0 * clip * alpha;
        }
    }
    if (bp.size() < 2) {
        throw std::runtime_error("BFELutEncoder: need at least two breakpoints");
    }

    const std::size_t max_segments = 1u << effective_segment_bits();
    if (bp.size() - 1 > max_segments) {
        throw std::runtime_error("BFELutEncoder: too many segments for segment_bits");
    }

    t.segments.reserve(bp.size() - 1);
    for (std::size_t i = 0; i + 1 < bp.size(); ++i) {
        t.segments.push_back(fit_quadratic_segment(bp[i], bp[i + 1], static_cast<std::uint8_t>(i)));
    }

    for (int signed_q = -t.max_abs_bucket; signed_q <= t.max_abs_bucket; ++signed_q) {
        const int idx = signed_q + t.bucket_zero;
        const int abs_q = std::abs(signed_q);
        const double x_real = bucket_center_real(signed_q, qshift);

        std::size_t seg_id = 0;
        if (x_real <= t.segments.front().left) {
            seg_id = 0;
        } else if (x_real >= t.segments.back().right) {
            seg_id = t.segments.size() - 1;
        } else {
            for (std::size_t s = 0; s < t.segments.size(); ++s) {
                const bool is_last = (s + 1 == t.segments.size());
                if ((x_real >= t.segments[s].left) &&
                    (x_real < t.segments[s].right || (is_last && x_real <= t.segments[s].right))) {
                    seg_id = s;
                    break;
                }
            }
        }

        const SegmentModel& seg = t.segments[seg_id];
        const double radius = 0.5 * (seg.right - seg.left);
        const std::int8_t residual_q = quantize_residual(x_real - seg.center, radius > 0.0 ? radius : 1.0);
        const bool sat = (x_real <= t.segments.front().left) || (x_real >= t.segments.back().right);
        const bool zero_guard = std::fabs(x_real) <= bucket_center_real(scfg_.zero_guard_radius, qshift);

        t.control_lut[static_cast<std::size_t>(idx)] = encode_control_word(
            signed_q < 0,
            zero_guard,
            sat,
            static_cast<std::uint8_t>(seg_id),
            static_cast<std::uint8_t>(std::min(abs_q, 127)),
            residual_q,
            seg.coeff_tag,
            kFuncGeLU);
    }

    t.control_lut[0] = encode_control_word(true, false, true, 0, 127, -7, 0, kFuncGeLU);
    t.control_lut.back() = encode_control_word(
        false, false, true,
        static_cast<std::uint8_t>(t.segments.size() - 1),
        127, 7,
        static_cast<std::uint8_t>(t.segments.back().coeff_tag),
        kFuncGeLU);

    populate_legacy_views(t);
    return t;
}

std::int64_t BFELutEncoder::evaluate_from_control_word_fp(
    std::int64_t x_fp,
    std::uint32_t cw,
    int bitwidth,
    const EncodedTables& tables) const {

    const DecodedControlWord d = decode_control_word(cw);

    if (d.func_tag == kFuncReLU) {
        return clamp_to_bitwidth(x_fp > 0 ? x_fp : 0, bitwidth);
    }

    const double x_real = static_cast<double>(x_fp) /
                          static_cast<double>(std::uint64_t{1} << qcfg_.scale_bits);

    if (tables.segments.empty()) {
        throw std::runtime_error("BFELutEncoder: empty segment table");
    }

    const std::size_t seg_id = std::min<std::size_t>(d.seg_id, tables.segments.size() - 1);
    const SegmentModel& seg = tables.segments[seg_id];

    double y_real = 0.0;
    if (d.sat && x_real <= tables.segments.front().left) {
        y_real = 0.0;
    } else if (d.sat && x_real >= tables.segments.back().right) {
        y_real = x_real;
    } else {
        const double u = x_real - seg.center;
        y_real = seg.coeff[0] + seg.coeff[1] * u + seg.coeff[2] * u * u;
    }

    const double scaled = y_real * static_cast<double>(std::uint64_t{1} << qcfg_.scale_bits);
    return clamp_to_bitwidth(static_cast<std::int64_t>(std::llround(scaled)), bitwidth);
}

double BFELutEncoder::evaluate_from_control_word(
    std::uint32_t cw,
    const EncodedTables& tables) const {

    const DecodedControlWord d = decode_control_word(cw);
    const int signed_q = d.neg ? -static_cast<int>(d.abs_q) : static_cast<int>(d.abs_q);
    const double bucket_x = bucket_center_real(signed_q, tables.qshift);

    if (tables.relu_mode || d.func_tag == kFuncReLU) {
        return bucket_x > 0.0 ? bucket_x : 0.0;
    }

    if (tables.segments.empty()) {
        throw std::runtime_error("BFELutEncoder: empty segment table");
    }

    if (d.sat) {
        return gelu(bucket_x);
    }

    const std::size_t seg_id = std::min<std::size_t>(d.seg_id, tables.segments.size() - 1);
    const SegmentModel& seg = tables.segments[seg_id];
    const double radius = 0.5 * (seg.right - seg.left);
    const int max_mag = (1 << (std::min(4, effective_residual_bits()) - 1)) - 1;
    const double u = (max_mag > 0 && std::isfinite(radius))
        ? (radius * static_cast<double>(d.residual_q) / static_cast<double>(max_mag))
        : 0.0;

    return seg.coeff[0] + seg.coeff[1] * u + seg.coeff[2] * u * u;
}


namespace {

constexpr std::uint32_t kStageSMagic = 0xA7u;
constexpr std::uint32_t kStageSMagicShift = 24;
constexpr std::uint32_t kStageSMagicMask = 0xFF000000u;

static inline std::int64_t stage_s_saturate_i128(__int128 v) {
    const __int128 hi = static_cast<__int128>(std::numeric_limits<std::int64_t>::max());
    const __int128 lo = static_cast<__int128>(std::numeric_limits<std::int64_t>::min());
    if (v > hi) return std::numeric_limits<std::int64_t>::max();
    if (v < lo) return std::numeric_limits<std::int64_t>::min();
    return static_cast<std::int64_t>(v);
}

static inline std::int64_t stage_s_round_shift(__int128 v, int shift) {
    if (shift <= 0) {
        return stage_s_saturate_i128(v << (-shift));
    }
    const __int128 half = static_cast<__int128>(1) << (shift - 1);
    if (v >= 0) {
        return stage_s_saturate_i128((v + half) >> shift);
    }
    return -stage_s_saturate_i128(((-v) + half) >> shift);
}

static inline std::int64_t stage_s_abs_i64(std::int64_t x) {
    if (x >= 0) return x;
    if (x == std::numeric_limits<std::int64_t>::min()) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return -x;
}

static inline std::int64_t stage_s_round_to_fp(long double x, int scale) {
    const long double y = std::ldexp(x, scale);
    if (y > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (y < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(std::llround(y));
}

static inline std::int64_t stage_s_round_to_q(long double x, int qbits) {
    return stage_s_round_to_fp(x, qbits);
}

static inline std::int64_t stage_s_clamp_bitwidth(std::int64_t x, int bitwidth) {
    if (bitwidth <= 0 || bitwidth >= 63) {
        return x;
    }
    const std::int64_t lo = -(std::int64_t{1} << (bitwidth - 1));
    const std::int64_t hi =  (std::int64_t{1} << (bitwidth - 1)) - 1;
    return std::min<std::int64_t>(std::max<std::int64_t>(x, lo), hi);
}

static inline long double stage_s_normal_pdf(long double x) {
    static const long double inv_sqrt_2pi =
        0.3989422804014326779399460599343818684758586311649346577L;
    return inv_sqrt_2pi * std::exp(-0.5L * x * x);
}

static inline long double stage_s_normal_cdf(long double x) {
    static const long double inv_sqrt2 =
        0.7071067811865475244008443621048490392848359376884740L;
    return 0.5L * (1.0L + std::erf(x * inv_sqrt2));
}

static inline long double stage_s_gelu_d1(long double x) {
    return stage_s_normal_cdf(x) + x * stage_s_normal_pdf(x);
}

static inline long double stage_s_gelu_d2_half(long double x) {
    return 0.5L * stage_s_normal_pdf(x) * (2.0L - x * x);
}

static inline long double stage_s_sigmoid(long double x) {
    if (x >= 0.0L) {
        const long double e = std::exp(-x);
        return 1.0L / (1.0L + e);
    }
    const long double e = std::exp(x);
    return e / (1.0L + e);
}

static inline long double stage_s_silu_d1(long double x) {
    const long double s = stage_s_sigmoid(x);
    return s + x * s * (1.0L - s);
}

static inline long double stage_s_silu_d2_half(long double x) {
    const long double s = stage_s_sigmoid(x);
    return 0.5L * s * (1.0L - s) * (2.0L + x * (1.0L - 2.0L * s));
}

static inline long double stage_s_gelu_neg_residual(long double u) {
    return StageSPlutEncoder::gelu_exact(-u);
}

static inline long double stage_s_gelu_neg_residual_d1(long double u) {
    return -stage_s_gelu_d1(-u);
}

static inline long double stage_s_gelu_neg_residual_d2_half(long double u) {
    return stage_s_gelu_d2_half(-u);
}

template <typename F0, typename F1, typename F2>
static StageSCoeffWord stage_s_build_coeff_word(
    std::uint32_t tag,
    std::int64_t center_fp,
    int scale,
    int coeff_bits,
    F0 f0,
    F1 f1,
    F2 f2_half) {

    const long double center_real =
        std::ldexp(static_cast<long double>(center_fp), -scale);

    StageSCoeffWord w;
    w.center_fp = center_fp;
    w.a0_fp = stage_s_round_to_fp(f0(center_real), scale);
    w.a1_q = stage_s_round_to_q(f1(center_real), coeff_bits);
    w.a2_q = stage_s_round_to_q(f2_half(center_real), coeff_bits);
    w.tag = tag;
    return w;
}

static inline std::int64_t stage_s_eval_coeff_word(
    std::int64_t x_fp,
    const StageSCoeffWord& w,
    int scale,
    int coeff_bits) {

    const std::int64_t r_fp = x_fp - w.center_fp;

    const __int128 linear =
        static_cast<__int128>(w.a1_q) * static_cast<__int128>(r_fp);

    const __int128 rr =
        static_cast<__int128>(r_fp) * static_cast<__int128>(r_fp);

    const __int128 quad =
        static_cast<__int128>(w.a2_q) * rr;

    const std::int64_t y_linear = stage_s_round_shift(linear, coeff_bits);
    const std::int64_t y_quad = stage_s_round_shift(quad, coeff_bits + scale);

    return stage_s_saturate_i128(
        static_cast<__int128>(w.a0_fp) +
        static_cast<__int128>(y_linear) +
        static_cast<__int128>(y_quad));
}

static inline std::int64_t stage_s_gelu_zero_poly(
    std::int64_t x_fp,
    int scale,
    int coeff_bits) {

    static const long double phi0 =
        0.3989422804014326779399460599343818684758586311649346577L;

    const std::int64_t half_q = static_cast<std::int64_t>(1) << (coeff_bits - 1);
    const std::int64_t phi0_q = stage_s_round_to_q(phi0, coeff_bits);

    const __int128 linear =
        static_cast<__int128>(half_q) * static_cast<__int128>(x_fp);

    const __int128 xx =
        static_cast<__int128>(x_fp) * static_cast<__int128>(x_fp);

    const __int128 quad =
        static_cast<__int128>(phi0_q) * xx;

    return stage_s_saturate_i128(
        static_cast<__int128>(stage_s_round_shift(linear, coeff_bits)) +
        static_cast<__int128>(stage_s_round_shift(quad, coeff_bits + scale)));
}

static long double stage_s_estimate_gelu_error(
    const StageSPlutEncoder& enc,
    const StageSPlutTables& t) {

    long double max_err = 0.0L;
    const std::int64_t step = std::max<std::int64_t>(1, t.fold_bucket_width_fp);

    for (std::int64_t x = -t.clip_fp; x <= t.clip_fp; x += step) {
        const std::uint32_t route = enc.route_gelu_from_fp(x, t);
        const std::int64_t approx = enc.evaluate_gelu_from_route_fp(x, route, t, 0);
        const long double xr = std::ldexp(static_cast<long double>(x), -t.scale_bits);
        const std::int64_t exact =
            stage_s_round_to_fp(StageSPlutEncoder::gelu_exact(xr), t.scale_bits);
        const long double err =
            std::fabs(static_cast<long double>(approx) - static_cast<long double>(exact));
        if (err > max_err) max_err = err;
        if (x > t.clip_fp - step) break;
    }

    return max_err;
}

static long double stage_s_estimate_silu_error(
    const StageSPlutEncoder& enc,
    const StageSPlutTables& t) {

    long double max_err = 0.0L;
    const std::int64_t step = std::max<std::int64_t>(1, t.full_bucket_width_fp);

    for (std::int64_t x = -t.clip_fp; x <= t.clip_fp; x += step) {
        const std::uint32_t route = enc.route_silu_from_fp(x, t);
        const std::int64_t approx = enc.evaluate_silu_from_route_fp(x, route, t, 0);
        const long double xr = std::ldexp(static_cast<long double>(x), -t.scale_bits);
        const std::int64_t exact =
            stage_s_round_to_fp(StageSPlutEncoder::silu_exact(xr), t.scale_bits);
        const long double err =
            std::fabs(static_cast<long double>(approx) - static_cast<long double>(exact));
        if (err > max_err) max_err = err;
        if (x > t.clip_fp - step) break;
    }

    return max_err;
}

} // namespace

StageSPlutEncoder::StageSPlutEncoder(StageSPlutConfig cfg)
    : cfg_(cfg) {
    if (cfg_.input_bits < 6 || cfg_.input_bits > 12) {
        throw std::invalid_argument("StageSPlutEncoder: input_bits must be in [6, 12]");
    }
    if (cfg_.scale_bits < 0 || cfg_.scale_bits > 30) {
        throw std::invalid_argument("StageSPlutEncoder: scale_bits must be in [0, 30]");
    }
    if (cfg_.coeff_scale_bits < cfg_.scale_bits || cfg_.coeff_scale_bits > 40) {
        throw std::invalid_argument(
            "StageSPlutEncoder: coeff_scale_bits must be in [scale_bits, 40]");
    }
    if (cfg_.clip <= 0.0) {
        throw std::invalid_argument("StageSPlutEncoder: clip must be positive");
    }
    if (cfg_.zero_guard_buckets < 0) {
        throw std::invalid_argument(
            "StageSPlutEncoder: zero_guard_buckets must be non-negative");
    }
}

long double StageSPlutEncoder::gelu_exact(long double x) {
    return x * stage_s_normal_cdf(x);
}

long double StageSPlutEncoder::silu_exact(long double x) {
    return x * stage_s_sigmoid(x);
}

std::uint32_t StageSPlutEncoder::encode_route(
    StageSRouteKind kind,
    StageSFunction fn,
    bool negative,
    std::uint32_t bucket) const {

    std::uint32_t out = 0;
    out |= static_cast<std::uint32_t>(kind) & 0x07u;
    out |= (negative ? 1u : 0u) << 3;
    out |= (bucket & 0x0FFFu) << 8;
    out |= (static_cast<std::uint32_t>(fn) & 0x0Fu) << 20;
    out |= kStageSMagic << kStageSMagicShift;
    return out;
}

StageSRouteWord StageSPlutEncoder::decode_route(std::uint32_t route) const {
    if ((route & kStageSMagicMask) != (kStageSMagic << kStageSMagicShift)) {
        throw std::runtime_error("StageSPlutEncoder: bad route word magic");
    }

    StageSRouteWord r;
    r.kind = static_cast<StageSRouteKind>(route & 0x07u);
    r.negative = ((route >> 3) & 1u) != 0u;
    r.bucket = (route >> 8) & 0x0FFFu;
    r.fn = static_cast<StageSFunction>((route >> 20) & 0x0Fu);
    return r;
}

int StageSPlutEncoder::signed_bucket_from_fp(
    std::int64_t x_fp,
    const StageSPlutTables& tables) const {

    const int signed_limit = static_cast<int>((tables.lut_size >> 1) - 1u);
    const long double x_real =
        std::ldexp(static_cast<long double>(x_fp), -tables.scale_bits);
    const long double clipped =
        std::max<long double>(
            -tables.clip,
            std::min<long double>(tables.clip, x_real));

    long rounded = std::lround(
        (clipped / tables.clip) * static_cast<long double>(signed_limit));

    if (rounded < -signed_limit) rounded = -signed_limit;
    if (rounded >  signed_limit) rounded =  signed_limit;
    return static_cast<int>(rounded);
}

StageSPlutTables StageSPlutEncoder::build_tables() const {
    StageSPlutTables t;
    t.scale_bits = cfg_.scale_bits;
    t.input_bits = cfg_.input_bits;
    t.coeff_scale_bits = cfg_.coeff_scale_bits;
    t.clip = cfg_.clip;

    t.lut_size = static_cast<std::uint32_t>(1u << cfg_.input_bits);
    t.fold_size = static_cast<std::uint32_t>(1u << (cfg_.input_bits - 1));
    t.clip_fp = stage_s_round_to_fp(cfg_.clip, cfg_.scale_bits);

    const long double fold_width_real =
        static_cast<long double>(cfg_.clip) /
        static_cast<long double>(t.fold_size);

    const long double full_width_real =
        (2.0L * static_cast<long double>(cfg_.clip)) /
        static_cast<long double>(t.lut_size);

    t.fold_bucket_width_fp =
        std::max<std::int64_t>(1, stage_s_round_to_fp(fold_width_real, cfg_.scale_bits));
    t.full_bucket_width_fp =
        std::max<std::int64_t>(1, stage_s_round_to_fp(full_width_real, cfg_.scale_bits));
    t.zero_guard_fp =
        static_cast<std::int64_t>(cfg_.zero_guard_buckets) * t.fold_bucket_width_fp;

    t.gelu_route_lut.resize(t.lut_size);
    t.silu_route_lut.resize(t.lut_size);
    t.gelu_neg_residual_bank.resize(t.fold_size);
    t.silu_full_bank.resize(t.lut_size);

    for (std::uint32_t b = 0; b < t.fold_size; ++b) {
        const std::int64_t center =
            static_cast<std::int64_t>(b) * t.fold_bucket_width_fp +
            t.fold_bucket_width_fp / 2;

        t.gelu_neg_residual_bank[b] = stage_s_build_coeff_word(
            b,
            center,
            cfg_.scale_bits,
            cfg_.coeff_scale_bits,
            stage_s_gelu_neg_residual,
            stage_s_gelu_neg_residual_d1,
            stage_s_gelu_neg_residual_d2_half);
    }

    const std::int64_t full_left = -t.clip_fp;
    for (std::uint32_t b = 0; b < t.lut_size; ++b) {
        const std::int64_t center =
            full_left +
            static_cast<std::int64_t>(b) * t.full_bucket_width_fp +
            t.full_bucket_width_fp / 2;

        t.silu_full_bank[b] = stage_s_build_coeff_word(
            b,
            center,
            cfg_.scale_bits,
            cfg_.coeff_scale_bits,
            StageSPlutEncoder::silu_exact,
            stage_s_silu_d1,
            stage_s_silu_d2_half);
    }

    const int signed_limit = static_cast<int>((t.lut_size >> 1) - 1u);
    const int half = static_cast<int>(t.lut_size >> 1);

    for (std::uint32_t idx = 0; idx < t.lut_size; ++idx) {
        const int raw_signed =
            (idx >= static_cast<std::uint32_t>(half))
                ? static_cast<int>(idx) - static_cast<int>(t.lut_size)
                : static_cast<int>(idx);
        const int signed_bucket =
            std::max(-signed_limit, std::min(signed_limit, raw_signed));

        const std::int64_t bucket_fp = stage_s_round_to_fp(
            (static_cast<long double>(signed_bucket) /
             static_cast<long double>(signed_limit)) * cfg_.clip,
            cfg_.scale_bits);

        const std::int64_t u_fp = stage_s_abs_i64(bucket_fp);

        if (signed_bucket >= signed_limit) {
            t.gelu_route_lut[idx] =
                encode_route(StageSRouteKind::TailIdentity, StageSFunction::GeLU, false, 0);
        } else if (signed_bucket <= -signed_limit) {
            t.gelu_route_lut[idx] =
                encode_route(StageSRouteKind::TailZero, StageSFunction::GeLU, true, 0);
        } else if (u_fp <= t.zero_guard_fp) {
            t.gelu_route_lut[idx] =
                encode_route(StageSRouteKind::ZeroPolynomial, StageSFunction::GeLU,
                             signed_bucket < 0, 0);
        } else {
            std::uint32_t b = static_cast<std::uint32_t>(u_fp / t.fold_bucket_width_fp);
            if (b >= t.fold_size) b = t.fold_size - 1u;
            t.gelu_route_lut[idx] =
                encode_route(StageSRouteKind::FoldedCoeffWord, StageSFunction::GeLU,
                             signed_bucket < 0, b);
        }

        if (signed_bucket >= signed_limit || signed_bucket <= -signed_limit) {
            t.silu_route_lut[idx] =
                encode_route(StageSRouteKind::TailExact, StageSFunction::SiLU,
                             signed_bucket < 0, 0);
        } else {
            int linear_bucket = signed_bucket + half;
            linear_bucket = std::max(0, std::min(static_cast<int>(t.lut_size) - 1, linear_bucket));
            t.silu_route_lut[idx] =
                encode_route(StageSRouteKind::FullCoeffWord, StageSFunction::SiLU,
                             signed_bucket < 0, static_cast<std::uint32_t>(linear_bucket));
        }
    }

    t.gelu_build_max_err_fp = stage_s_estimate_gelu_error(*this, t);
    t.silu_build_max_err_fp = stage_s_estimate_silu_error(*this, t);
    return t;
}

std::uint32_t StageSPlutEncoder::route_gelu_from_fp(
    std::int64_t x_fp,
    const StageSPlutTables& tables) const {

    const int signed_bucket = signed_bucket_from_fp(x_fp, tables);
    const std::uint32_t idx =
        static_cast<std::uint32_t>(signed_bucket) & (tables.lut_size - 1u);
    return tables.gelu_route_lut[idx];
}

std::uint32_t StageSPlutEncoder::route_silu_from_fp(
    std::int64_t x_fp,
    const StageSPlutTables& tables) const {

    const int signed_bucket = signed_bucket_from_fp(x_fp, tables);
    const std::uint32_t idx =
        static_cast<std::uint32_t>(signed_bucket) & (tables.lut_size - 1u);
    return tables.silu_route_lut[idx];
}

namespace {

static inline std::int64_t stage_s_eval_gelu_decoded_route_fp(
    std::int64_t x_fp,
    const StageSRouteWord& r,
    const StageSPlutTables& tables,
    int bitwidth) {

    std::int64_t y = 0;
    switch (r.kind) {
        case StageSRouteKind::TailZero:
            y = 0;
            break;

        case StageSRouteKind::TailIdentity:
            y = x_fp;
            break;

        case StageSRouteKind::ZeroPolynomial:
            y = stage_s_gelu_zero_poly(
                x_fp,
                tables.scale_bits,
                tables.coeff_scale_bits);
            break;

        case StageSRouteKind::FoldedCoeffWord: {
            const std::uint32_t b =
                std::min<std::uint32_t>(r.bucket, tables.fold_size - 1u);
            const std::int64_t u_fp = stage_s_abs_i64(x_fp);
            const std::int64_t neg_residual = stage_s_eval_coeff_word(
                u_fp,
                tables.gelu_neg_residual_bank[b],
                tables.scale_bits,
                tables.coeff_scale_bits);

            if (r.negative) {
                y = neg_residual;
            } else {
                y = stage_s_saturate_i128(
                    static_cast<__int128>(x_fp) +
                    static_cast<__int128>(neg_residual));
            }
            break;
        }

        default:
            y = stage_s_round_to_fp(
                StageSPlutEncoder::gelu_exact(
                    std::ldexp(static_cast<long double>(x_fp), -tables.scale_bits)),
                tables.scale_bits);
            break;
    }

    return stage_s_clamp_bitwidth(y, bitwidth);
}

static inline std::int64_t stage_s_eval_silu_decoded_route_fp(
    std::int64_t x_fp,
    const StageSRouteWord& r,
    const StageSPlutTables& tables,
    int bitwidth) {

    std::int64_t y = 0;
    switch (r.kind) {
        case StageSRouteKind::TailExact:
            y = stage_s_round_to_fp(
                StageSPlutEncoder::silu_exact(
                    std::ldexp(static_cast<long double>(x_fp), -tables.scale_bits)),
                tables.scale_bits);
            break;

        case StageSRouteKind::FullCoeffWord: {
            const std::uint32_t b =
                std::min<std::uint32_t>(r.bucket, tables.lut_size - 1u);
            y = stage_s_eval_coeff_word(
                x_fp,
                tables.silu_full_bank[b],
                tables.scale_bits,
                tables.coeff_scale_bits);
            break;
        }

        default:
            y = stage_s_round_to_fp(
                StageSPlutEncoder::silu_exact(
                    std::ldexp(static_cast<long double>(x_fp), -tables.scale_bits)),
                tables.scale_bits);
            break;
    }

    return stage_s_clamp_bitwidth(y, bitwidth);
}

} // namespace

std::int64_t StageSPlutEncoder::evaluate_gelu_from_route_fp(
    std::int64_t x_fp,
    std::uint32_t route,
    const StageSPlutTables& tables,
    int bitwidth) const {

    const StageSRouteWord r = decode_route(route);
    return stage_s_eval_gelu_decoded_route_fp(x_fp, r, tables, bitwidth);
}

std::int64_t StageSPlutEncoder::evaluate_silu_from_route_fp(
    std::int64_t x_fp,
    std::uint32_t route,
    const StageSPlutTables& tables,
    int bitwidth) const {

    const StageSRouteWord r = decode_route(route);
    return stage_s_eval_silu_decoded_route_fp(x_fp, r, tables, bitwidth);
}

void StageSPlutEncoder::evaluate_gelu_tail_sparse_batch_fp(
    const std::int64_t* x_fp,
    std::size_t n,
    int bitwidth,
    const StageSPlutTables& tables,
    std::int64_t* y_fp,
    StageSBatchStats* stats) const {

    if (n > 0 && (x_fp == nullptr || y_fp == nullptr)) {
        throw std::invalid_argument("StageSPlutEncoder: null batch pointer");
    }

    StageSBatchStats local;
    local.n = n;

    if (n == 0) {
        if (stats != nullptr) {
            *stats = local;
        }
        return;
    }

    const bool schedule_buckets =
        cfg_.enable_bucket_memo &&
        cfg_.enable_bucket_schedule &&
        tables.lut_size > 0 &&
        ((tables.lut_size & (tables.lut_size - 1u)) == 0u);

    std::vector<std::size_t> residual_indices;
    std::vector<std::vector<std::size_t>> bucket_indices;

    if (!schedule_buckets) {
        residual_indices.reserve(std::min<std::size_t>(n, 4096));
    }

    std::int64_t min_x = x_fp[0];
    std::int64_t max_x = x_fp[0];

    // Stage-Y keeps Stage-X deterministic scatter, but sends coefficient-domain
    // residuals directly into per-bucket work queues.  Stage-Z keeps the public
    // route-word contract unchanged while reusing each decoded route word for
    // all elements in the same scheduled bucket.
    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t x = x_fp[i];

        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;

        if (cfg_.enable_tail_sparse) {
            if (x >= tables.clip_fp) {
                y_fp[i] = stage_s_clamp_bitwidth(x, bitwidth);
                ++local.tail_identity;
                continue;
            }

            if (x <= -tables.clip_fp) {
                y_fp[i] = 0;
                ++local.tail_zero;
                continue;
            }

            if (stage_s_abs_i64(x) <= tables.zero_guard_fp) {
                y_fp[i] = stage_s_clamp_bitwidth(
                    stage_s_gelu_zero_poly(
                        x,
                        tables.scale_bits,
                        tables.coeff_scale_bits),
                    bitwidth);
                ++local.zero_poly_hits;
                continue;
            }
        }

        if (schedule_buckets) {
            if (bucket_indices.empty()) {
                bucket_indices.resize(tables.lut_size);
            }
            const int signed_bucket = signed_bucket_from_fp(x, tables);
            const std::uint32_t idx =
                static_cast<std::uint32_t>(signed_bucket) & (tables.lut_size - 1u);
            bucket_indices[idx].push_back(i);
        } else {
            residual_indices.push_back(i);
        }
    }

    std::size_t residual_count = residual_indices.size();
    if (schedule_buckets) {
        residual_count = 0;
        for (const auto& bucket : bucket_indices) {
            residual_count += bucket.size();
        }
    }

    local.min_x_fp = min_x;
    local.max_x_fp = max_x;
    local.residual_candidates = residual_count;
    local.residual_indexed_eval = residual_count;
    local.bucket_scheduled_eval = schedule_buckets ? residual_count : 0;
    local.route_elided = n - residual_count;
    local.deterministic_scatter_hits = local.route_elided;
    local.identity_default_hits = local.tail_identity;

    if (residual_count == 0) {
        if (local.tail_identity == n && cfg_.enable_range_certificate) {
            local.range_cert_hits = n;
            local.bulk_tail_identity = n;
        } else if (local.tail_zero == n && cfg_.enable_range_certificate) {
            local.range_cert_hits = n;
            local.bulk_tail_zero = n;
        } else if (local.zero_poly_hits == n && cfg_.enable_range_certificate) {
            local.range_cert_hits = n;
            local.bulk_zero_poly = n;
        } else if (cfg_.enable_domain_mix_certificate) {
            local.domain_cert_hits = n;
            local.bulk_tail_pair_mix = local.tail_identity + local.tail_zero;
            local.bulk_tail_guard_mix = local.zero_poly_hits;
        }

        if (stats != nullptr) {
            *stats = local;
        }
        return;
    }

    if (schedule_buckets) {
        for (std::uint32_t idx = 0; idx < tables.lut_size; ++idx) {
            const auto& bucket = bucket_indices[idx];
            if (bucket.empty()) {
                continue;
            }

            const std::size_t bucket_size = bucket.size();
            const std::uint32_t route = tables.gelu_route_lut[idx];
            const StageSRouteWord r = decode_route(route);

            ++local.pbs_calls;
            ++local.unique_route_keys;
            ++local.bucket_schedule_bins;
            local.active_route_elements += bucket_size;
            if (bucket_size > 1) {
                local.route_cache_hits += bucket_size - 1;
                local.bucket_schedule_reused += bucket_size - 1;
            }

        switch (r.kind) {
            case StageSRouteKind::TailZero:
                local.tail_zero += bucket_size;
                break;
            case StageSRouteKind::TailIdentity:
                local.tail_identity += bucket_size;
                break;
            case StageSRouteKind::ZeroPolynomial:
                local.zero_poly_hits += bucket_size;
                break;
            case StageSRouteKind::FoldedCoeffWord:
                local.folded_coeff_hits += bucket_size;
                break;
            case StageSRouteKind::FullCoeffWord:
                local.full_coeff_hits += bucket_size;
                break;
            case StageSRouteKind::TailExact:
                local.tail_exact += bucket_size;
                break;
            default:
                local.other_routes += bucket_size;
                break;
        }

            for (const std::size_t i : bucket) {
                y_fp[i] = stage_s_eval_gelu_decoded_route_fp(
                    x_fp[i], r, tables, bitwidth);
            }
        }

        if (stats != nullptr) {
            *stats = local;
        }
        return;
    }

    std::vector<std::uint8_t> route_valid;
    std::vector<std::uint32_t> route_cache;
    std::vector<StageSRouteWord> decoded_cache;

    if (cfg_.enable_bucket_memo && tables.lut_size > 0) {
        route_valid.assign(tables.lut_size, 0);
        route_cache.assign(tables.lut_size, 0);
        decoded_cache.resize(tables.lut_size);
    }

    for (const std::size_t i : residual_indices) {
        const std::int64_t x = x_fp[i];

        ++local.active_route_elements;

        std::uint32_t route = 0;
        StageSRouteWord r;

        if (!route_valid.empty()) {
            const int signed_bucket = signed_bucket_from_fp(x, tables);
            const std::uint32_t idx =
                static_cast<std::uint32_t>(signed_bucket) & (tables.lut_size - 1u);

            if (route_valid[idx] != 0) {
                route = route_cache[idx];
                r = decoded_cache[idx];
                ++local.route_cache_hits;
            } else {
                route = tables.gelu_route_lut[idx];
                r = decode_route(route);
                route_valid[idx] = 1;
                route_cache[idx] = route;
                decoded_cache[idx] = r;
                ++local.pbs_calls;
                ++local.unique_route_keys;
            }
        } else {
            route = route_gelu_from_fp(x, tables);
            r = decode_route(route);
            ++local.pbs_calls;
        }

        switch (r.kind) {
            case StageSRouteKind::TailZero:
                local.tail_zero += 1;
                break;
            case StageSRouteKind::TailIdentity:
                local.tail_identity += 1;
                break;
            case StageSRouteKind::ZeroPolynomial:
                local.zero_poly_hits += 1;
                break;
            case StageSRouteKind::FoldedCoeffWord:
                local.folded_coeff_hits += 1;
                break;
            case StageSRouteKind::FullCoeffWord:
                local.full_coeff_hits += 1;
                break;
            case StageSRouteKind::TailExact:
                local.tail_exact += 1;
                break;
            default:
                local.other_routes += 1;
                break;
        }

        y_fp[i] = stage_s_eval_gelu_decoded_route_fp(x, r, tables, bitwidth);
    }

    if (stats != nullptr) {
        *stats = local;
    }
}

void StageSPlutEncoder::evaluate_silu_tail_sparse_batch_fp(
    const std::int64_t* x_fp,
    std::size_t n,
    int bitwidth,
    const StageSPlutTables& tables,
    std::int64_t* y_fp,
    StageSBatchStats* stats) const {

    if (n > 0 && (x_fp == nullptr || y_fp == nullptr)) {
        throw std::invalid_argument("StageSPlutEncoder: null batch pointer");
    }

    StageSBatchStats local;
    local.n = n;

    if (n == 0) {
        if (stats != nullptr) {
            *stats = local;
        }
        return;
    }

    const bool schedule_buckets =
        cfg_.enable_bucket_memo &&
        cfg_.enable_bucket_schedule &&
        tables.lut_size > 0 &&
        ((tables.lut_size & (tables.lut_size - 1u)) == 0u);

    std::vector<std::size_t> residual_indices;
    std::vector<std::vector<std::size_t>> bucket_indices;

    if (!schedule_buckets) {
        residual_indices.reserve(std::min<std::size_t>(n, 4096));
    }

    std::int64_t min_x = x_fp[0];
    std::int64_t max_x = x_fp[0];

    for (std::size_t i = 0; i < n; ++i) {
        const std::int64_t x = x_fp[i];

        if (x < min_x) min_x = x;
        if (x > max_x) max_x = x;

        if (cfg_.enable_tail_sparse &&
            (x >= tables.clip_fp || x <= -tables.clip_fp)) {
            y_fp[i] = stage_s_clamp_bitwidth(
                stage_s_round_to_fp(
                    silu_exact(std::ldexp(
                        static_cast<long double>(x),
                        -tables.scale_bits)),
                    tables.scale_bits),
                bitwidth);
            ++local.tail_exact;
            continue;
        }

        if (schedule_buckets) {
            if (bucket_indices.empty()) {
                bucket_indices.resize(tables.lut_size);
            }
            const int signed_bucket = signed_bucket_from_fp(x, tables);
            const std::uint32_t idx =
                static_cast<std::uint32_t>(signed_bucket) & (tables.lut_size - 1u);
            bucket_indices[idx].push_back(i);
        } else {
            residual_indices.push_back(i);
        }
    }

    std::size_t residual_count = residual_indices.size();
    if (schedule_buckets) {
        residual_count = 0;
        for (const auto& bucket : bucket_indices) {
            residual_count += bucket.size();
        }
    }

    local.min_x_fp = min_x;
    local.max_x_fp = max_x;
    local.residual_candidates = residual_count;
    local.residual_indexed_eval = residual_count;
    local.bucket_scheduled_eval = schedule_buckets ? residual_count : 0;
    local.route_elided = n - residual_count;
    local.deterministic_scatter_hits = local.route_elided;

    if (residual_count == 0) {
        if (cfg_.enable_range_certificate &&
            (min_x >= tables.clip_fp || max_x <= -tables.clip_fp)) {
            local.range_cert_hits = n;
            local.bulk_tail_exact = n;
        } else if (cfg_.enable_domain_mix_certificate) {
            local.domain_cert_hits = n;
            local.bulk_tail_pair_mix = n;
        }

        if (stats != nullptr) {
            *stats = local;
        }
        return;
    }

    if (schedule_buckets) {
        for (std::uint32_t idx = 0; idx < tables.lut_size; ++idx) {
            const auto& bucket = bucket_indices[idx];
            if (bucket.empty()) {
                continue;
            }

            const std::size_t bucket_size = bucket.size();
            const std::uint32_t route = tables.silu_route_lut[idx];
            const StageSRouteWord r = decode_route(route);

            ++local.pbs_calls;
            ++local.unique_route_keys;
            ++local.bucket_schedule_bins;
            local.active_route_elements += bucket_size;
            if (bucket_size > 1) {
                local.route_cache_hits += bucket_size - 1;
                local.bucket_schedule_reused += bucket_size - 1;
            }

        switch (r.kind) {
            case StageSRouteKind::TailZero:
                local.tail_zero += bucket_size;
                break;
            case StageSRouteKind::TailIdentity:
                local.tail_identity += bucket_size;
                break;
            case StageSRouteKind::ZeroPolynomial:
                local.zero_poly_hits += bucket_size;
                break;
            case StageSRouteKind::FoldedCoeffWord:
                local.folded_coeff_hits += bucket_size;
                break;
            case StageSRouteKind::FullCoeffWord:
                local.full_coeff_hits += bucket_size;
                break;
            case StageSRouteKind::TailExact:
                local.tail_exact += bucket_size;
                break;
            default:
                local.other_routes += bucket_size;
                break;
        }

            for (const std::size_t i : bucket) {
                y_fp[i] = stage_s_eval_silu_decoded_route_fp(
                    x_fp[i], r, tables, bitwidth);
            }
        }

        if (stats != nullptr) {
            *stats = local;
        }
        return;
    }

    std::vector<std::uint8_t> route_valid;
    std::vector<std::uint32_t> route_cache;
    std::vector<StageSRouteWord> decoded_cache;

    if (cfg_.enable_bucket_memo && tables.lut_size > 0) {
        route_valid.assign(tables.lut_size, 0);
        route_cache.assign(tables.lut_size, 0);
        decoded_cache.resize(tables.lut_size);
    }

    for (const std::size_t i : residual_indices) {
        const std::int64_t x = x_fp[i];

        ++local.active_route_elements;

        std::uint32_t route = 0;
        StageSRouteWord r;

        if (!route_valid.empty()) {
            const int signed_bucket = signed_bucket_from_fp(x, tables);
            const std::uint32_t idx =
                static_cast<std::uint32_t>(signed_bucket) & (tables.lut_size - 1u);

            if (route_valid[idx] != 0) {
                route = route_cache[idx];
                r = decoded_cache[idx];
                ++local.route_cache_hits;
            } else {
                route = tables.silu_route_lut[idx];
                r = decode_route(route);
                route_valid[idx] = 1;
                route_cache[idx] = route;
                decoded_cache[idx] = r;
                ++local.pbs_calls;
                ++local.unique_route_keys;
            }
        } else {
            route = route_silu_from_fp(x, tables);
            r = decode_route(route);
            ++local.pbs_calls;
        }

        switch (r.kind) {
            case StageSRouteKind::TailZero:
                local.tail_zero += 1;
                break;
            case StageSRouteKind::TailIdentity:
                local.tail_identity += 1;
                break;
            case StageSRouteKind::ZeroPolynomial:
                local.zero_poly_hits += 1;
                break;
            case StageSRouteKind::FoldedCoeffWord:
                local.folded_coeff_hits += 1;
                break;
            case StageSRouteKind::FullCoeffWord:
                local.full_coeff_hits += 1;
                break;
            case StageSRouteKind::TailExact:
                local.tail_exact += 1;
                break;
            default:
                local.other_routes += 1;
                break;
        }

        y_fp[i] = stage_s_eval_silu_decoded_route_fp(x, r, tables, bitwidth);
    }

    if (stats != nullptr) {
        *stats = local;
    }
}



} // namespace dazg_orbit::tfhe
