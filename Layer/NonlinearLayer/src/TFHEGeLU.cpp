// DAZG-Orbit Project Source File
// Component: Layer/NonlinearLayer/src/TFHEGeLU.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include "NonlinearLayer/TFHEGeLU.h"

#include "HE/tfhe/BFEGeLUClear.h"
#include "HE/tfhe/DAZGRLut.h"
#include "Utils/dazg_orbit_dazg_variant.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace dazg_orbit::tfhe {

constexpr int TFHEGeLU::kDAZGBitWidth;
constexpr int TFHEGeLU::kDAZGScaleBits;

namespace {

constexpr int kBitWidth = TFHEGeLU::kDAZGBitWidth;
constexpr int kScaleBits = TFHEGeLU::kDAZGScaleBits;
constexpr std::int64_t kScale = (std::int64_t{1} << kScaleBits);

std::atomic<bool> g_dazg_mainpath_logged{false};

std::int64_t min_for_signed_bits(int bits) {
    if (bits <= 0) {
        return 0;
    }
    if (bits >= 63) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return -(std::int64_t{1} << (bits - 1));
}

std::int64_t max_for_signed_bits(int bits) {
    if (bits <= 0) {
        return 0;
    }
    if (bits >= 63) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return (std::int64_t{1} << (bits - 1)) - 1;
}

std::int64_t clamp_to_dazg_word(std::int64_t v) {
    const std::int64_t lo = min_for_signed_bits(kBitWidth);
    const std::int64_t hi = max_for_signed_bits(kBitWidth);
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

long to_he_word(std::int64_t v) {
    v = clamp_to_dazg_word(v);
    const std::int64_t lo =
        static_cast<std::int64_t>(std::numeric_limits<long>::min());
    const std::int64_t hi =
        static_cast<std::int64_t>(std::numeric_limits<long>::max());
    if (v < lo) {
        return std::numeric_limits<long>::min();
    }
    if (v > hi) {
        return std::numeric_limits<long>::max();
    }
    return static_cast<long>(v);
}

std::vector<long> to_he_vector(const std::vector<std::int64_t>& xs_fp) {
    std::vector<long> out;
    out.reserve(xs_fp.size());
    for (std::int64_t x : xs_fp) {
        out.push_back(to_he_word(x));
    }
    return out;
}

std::vector<std::int64_t> from_he_vector(const std::vector<long>& ys_fp) {
    std::vector<std::int64_t> out;
    out.reserve(ys_fp.size());
    for (long y : ys_fp) {
        out.push_back(clamp_to_dazg_word(static_cast<std::int64_t>(y)));
    }
    return out;
}

void log_dazg_mainpath_once(std::size_t batch_size) {
    bool expected = false;
    if (g_dazg_mainpath_logged.compare_exchange_strong(expected, true)) {
        dazg_orbit::dazg::PrintVariantOnce("TFHEGeLU::forward_fixed");
        const dazg_orbit::dazg::VariantConfig cfg = dazg_orbit::dazg::CurrentVariantConfig();
        std::cerr << "[TFHEGeLU DAZG mainpath] fn=gelu bitwidth="
                  << kBitWidth
                  << " scale=" << kScaleBits
                  << " batch=" << batch_size
                  << " variant=" << cfg.name
                  << " dazg_variant=" << cfg.name
                  << " execution_mode=" << dazg_orbit::dazg::ExecutionMode()
                  << " route=dazg_orbit::tfhe::EvalDAZGGeLUFp"
                  << std::endl;
    }
}

BFELutEncoder make_default_gelu_encoder() {
    QuantConfig qcfg;
    qcfg.clip = 8.0;
    qcfg.input_bits = 8;
    qcfg.scale_bits = kScaleBits;
    qcfg.residual_bits = 4;
    qcfg.coeff_frac_bits = 24;

    SegmentConfig scfg;
    scfg.segment_bits = 4;
    scfg.segment_id_bits = 4;
    scfg.poly_degree = 2;
    scfg.use_gray_code = true;
    scfg.split_signed = true;
    scfg.local_u_bits = 4;
    scfg.zero_guard_radius = 1;

    return BFELutEncoder(qcfg, scfg);
}

}  // namespace

TFHEGeLU::TFHEGeLU()
    : TFHEGeLU(make_default_gelu_encoder()) {}

TFHEGeLU::TFHEGeLU(const BFELutEncoder& encoder)
    : encoder_(encoder),
      tables_(encoder_.build_gelu()),
      selector_(encoder_, tables_) {}

std::int64_t TFHEGeLU::real_to_fixed(double x) {
    if (std::isnan(x)) {
        return 0;
    }

    if (!std::isfinite(x)) {
        return std::signbit(x)
                   ? min_for_signed_bits(kBitWidth)
                   : max_for_signed_bits(kBitWidth);
    }

    const double scaled = std::nearbyint(x * static_cast<double>(kScale));

    if (scaled <= static_cast<double>(min_for_signed_bits(kBitWidth))) {
        return min_for_signed_bits(kBitWidth);
    }
    if (scaled >= static_cast<double>(max_for_signed_bits(kBitWidth))) {
        return max_for_signed_bits(kBitWidth);
    }

    return clamp_to_dazg_word(static_cast<std::int64_t>(scaled));
}

double TFHEGeLU::fixed_to_real(std::int64_t x_fp) {
    return static_cast<double>(x_fp) / static_cast<double>(kScale);
}

std::vector<std::int64_t> TFHEGeLU::forward_fixed(
    const std::vector<std::int64_t>& xs_fp) const {
    if (xs_fp.empty()) {
        return {};
    }

    log_dazg_mainpath_once(xs_fp.size());

    std::vector<std::int64_t> out;
    dazg_orbit::tfhe::EvalDAZGGeLUFp(xs_fp, kBitWidth, kScaleBits, out);

    if (out.size() != xs_fp.size()) {
        throw std::runtime_error(
            "TFHEGeLU DAZG mainpath returned an unexpected output size");
    }

    return out;
}

std::int64_t TFHEGeLU::forward_fixed(std::int64_t x_fp) const {
    const std::vector<std::int64_t> in{clamp_to_dazg_word(x_fp)};
    const std::vector<std::int64_t> out = forward_fixed(in);
    return out.empty() ? 0 : out.front();
}

double TFHEGeLU::forward(double x) const {
    return fixed_to_real(forward_fixed(real_to_fixed(x)));
}

std::vector<double> TFHEGeLU::forward(const std::vector<double>& xs) const {
    if (xs.empty()) {
        return {};
    }

    std::vector<std::int64_t> xs_fp;
    xs_fp.reserve(xs.size());
    for (double x : xs) {
        xs_fp.push_back(real_to_fixed(x));
    }

    const std::vector<std::int64_t> ys_fp = forward_fixed(xs_fp);

    std::vector<double> ys;
    ys.reserve(ys_fp.size());
    for (std::int64_t y : ys_fp) {
        ys.push_back(fixed_to_real(y));
    }
    return ys;
}

std::uint32_t TFHEGeLU::control_word(double x) const {
    return selector_.forward_control_word_from_real(x);
}

std::vector<std::uint32_t> TFHEGeLU::control_words(
    const std::vector<double>& xs) const {
    std::vector<std::uint32_t> cws;
    cws.reserve(xs.size());
    for (double x : xs) {
        cws.push_back(control_word(x));
    }
    return cws;
}

}  // namespace dazg_orbit::tfhe
