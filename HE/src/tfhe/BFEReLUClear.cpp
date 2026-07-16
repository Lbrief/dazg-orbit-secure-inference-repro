// DAZG-Orbit Project Source File
// Component: HE/src/tfhe/BFEReLUClear.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include "HE/tfhe/BFEReLUClear.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <vector>

namespace HE {
namespace {

#ifndef DAZG_ORBIT_BFE_RELU_INPUT_BITS
#define DAZG_ORBIT_BFE_RELU_INPUT_BITS 8
#endif

#ifndef DAZG_ORBIT_BFE_RELU_SELF_CHECK
#define DAZG_ORBIT_BFE_RELU_SELF_CHECK 1
#endif

static_assert(DAZG_ORBIT_BFE_RELU_INPUT_BITS == 8,
              "Stage-Y BFE-ReLU uses an 8-bit signed route LUT.");

constexpr int kInputBits = DAZG_ORBIT_BFE_RELU_INPUT_BITS;
constexpr int kSignedBuckets = 1 << kInputBits;
constexpr int kBucketZero = 1 << (kInputBits - 1);
constexpr int kMaxAbsBucket = (1 << (kInputBits - 1)) - 1;

constexpr std::uint32_t kReluMagic = 0xA71u;
constexpr std::uint32_t kReluMagicShift = 20;
constexpr std::uint32_t kReluMagicMask = 0xFFF00000u;

struct DecodedReluCW {
    bool neg = false;
    bool zero_guard = false;
    bool sat = false;
    std::uint8_t seg_gray = 0;
    std::uint8_t abs_q = 0;
};

static inline std::uint8_t gray4(std::uint8_t x) {
    return static_cast<std::uint8_t>((x ^ (x >> 1)) & 0x0Fu);
}

static inline std::uint32_t pack_relu_cw(
    bool neg,
    bool zero_guard,
    bool sat,
    std::uint8_t seg_gray,
    std::uint8_t abs_q) {

    return (kReluMagic << kReluMagicShift) |
           (static_cast<std::uint32_t>(neg ? 1u : 0u) << 0) |
           (static_cast<std::uint32_t>(zero_guard ? 1u : 0u) << 1) |
           (static_cast<std::uint32_t>(sat ? 1u : 0u) << 2) |
           (static_cast<std::uint32_t>(seg_gray & 0x0Fu) << 3) |
           (static_cast<std::uint32_t>(abs_q) << 7);
}

static inline DecodedReluCW unpack_relu_cw(std::uint32_t cw) {
    if ((cw & kReluMagicMask) != (kReluMagic << kReluMagicShift)) {
        std::cerr << "[StageY BFEReLU invalid control word]"
                  << " cw=0x" << std::hex << cw << std::dec
                  << std::endl;
        std::abort();
    }

    DecodedReluCW d;
    d.neg = (cw & 1u) != 0;
    d.zero_guard = (cw & 2u) != 0;
    d.sat = (cw & 4u) != 0;
    d.seg_gray = static_cast<std::uint8_t>((cw >> 3) & 0x0Fu);
    d.abs_q = static_cast<std::uint8_t>((cw >> 7) & 0xFFu);
    return d;
}

static inline std::uint64_t abs_u64(std::int64_t x) {
    if (x >= 0) {
        return static_cast<std::uint64_t>(x);
    }
    return static_cast<std::uint64_t>(-(x + 1)) + 1u;
}

static inline int choose_qshift(std::uint64_t max_abs) {
    int shift = 0;
    while (shift < 62 &&
           (max_abs >> shift) > static_cast<std::uint64_t>(kMaxAbsBucket)) {
        ++shift;
    }
    return shift;
}

static inline std::uint32_t ceil_shift_to_u32(std::uint64_t x, int shift) {
    if (x == 0) {
        return 0;
    }
    if (shift <= 0) {
        return static_cast<std::uint32_t>(x);
    }
    if (shift >= 63) {
        return 1;
    }

    const std::uint64_t base = x >> shift;
    const std::uint64_t mask = (std::uint64_t{1} << shift) - 1u;
    return static_cast<std::uint32_t>(base + ((x & mask) ? 1u : 0u));
}

static inline int signed_bucket_from_fp(std::int64_t x_fp, int qshift) {
    std::uint32_t q = ceil_shift_to_u32(abs_u64(x_fp), qshift);
    if (q > static_cast<std::uint32_t>(kMaxAbsBucket)) {
        q = static_cast<std::uint32_t>(kMaxAbsBucket);
    }

    if (x_fp < 0) {
        return -static_cast<int>(q);
    }
    return static_cast<int>(q);
}

static inline std::array<std::uint32_t, kSignedBuckets> build_relu_lut() {
    std::array<std::uint32_t, kSignedBuckets> lut{};

    for (int signed_q = -kMaxAbsBucket; signed_q <= kMaxAbsBucket; ++signed_q) {
        const int idx = signed_q + kBucketZero;
        const bool neg = signed_q < 0;
        const int abs_q_i = std::abs(signed_q);
        const bool zero_guard = abs_q_i <= 1;
        const bool sat = abs_q_i >= kMaxAbsBucket;
        const std::uint8_t abs_q = static_cast<std::uint8_t>(abs_q_i);
        const std::uint8_t seg = static_cast<std::uint8_t>(std::min(15, abs_q_i / 8));

        lut[static_cast<std::size_t>(idx)] =
            pack_relu_cw(neg, zero_guard, sat, gray4(seg), abs_q);
    }

    return lut;
}

static inline const std::array<std::uint32_t, kSignedBuckets>& relu_lut() {
    static const std::array<std::uint32_t, kSignedBuckets> lut = build_relu_lut();
    return lut;
}

static inline std::int64_t clamp_to_bitwidth(std::int64_t x, int bitwidth) {
    if (bitwidth <= 0) {
        return 0;
    }
    if (bitwidth >= 63) {
        return x;
    }

    const std::int64_t lo = -(std::int64_t{1} << (bitwidth - 1));
    const std::int64_t hi =  (std::int64_t{1} << (bitwidth - 1)) - 1;
    return std::min<std::int64_t>(std::max<std::int64_t>(x, lo), hi);
}

static inline std::int64_t scaled_bucket_base(std::uint8_t abs_q, int qshift) {
    if (abs_q == 0) {
        return 0;
    }
    if (qshift >= 63) {
        return std::numeric_limits<std::int64_t>::max();
    }

    const std::uint64_t limit =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) >> qshift;

    if (static_cast<std::uint64_t>(abs_q) > limit) {
        return std::numeric_limits<std::int64_t>::max();
    }

    return static_cast<std::int64_t>(
        static_cast<std::uint64_t>(abs_q) << qshift);
}

static inline std::int64_t eval_relu_from_cw(
    std::int64_t x_fp,
    std::uint32_t cw,
    int qshift,
    int bitwidth) {

    const DecodedReluCW d = unpack_relu_cw(cw);

    if (d.neg) {
        return 0;
    }

    if (d.zero_guard) {
        return clamp_to_bitwidth(x_fp > 0 ? x_fp : 0, bitwidth);
    }

    const std::int64_t base = scaled_bucket_base(d.abs_q, qshift);
    const std::int64_t residual = x_fp - base;
    return clamp_to_bitwidth(base + residual, bitwidth);
}

static std::mutex& log_mutex() {
    static std::mutex m;
    return m;
}

} // namespace

void EvalBFEReLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale,
    std::vector<std::int64_t>& y_fp) {

    std::uint64_t max_abs = 0;
    for (const std::int64_t x : x_fp) {
        max_abs = std::max(max_abs, abs_u64(x));
    }

    const int qshift = choose_qshift(max_abs);
    const auto& lut = relu_lut();

    static std::atomic<bool> once{false};
    if (!once.exchange(true)) {
        std::lock_guard<std::mutex> lock(log_mutex());
        std::cerr << "[StageY BFEReLU active]"
                  << " encoding=StageY-BucketScheduled-ReLU-CW-LUT"
                  << " input_bits=" << kInputBits
                  << " lut_size=" << lut.size()
                  << " zero_guard=on"
                  << " gray_segment=on"
                  << " exact_residual=on"
                  << std::endl;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();

    y_fp.resize(x_fp.size());

    std::size_t neg_count = 0;
    std::size_t zero_guard_count = 0;
    std::size_t sat_count = 0;
    std::size_t pos_count = 0;
    std::uint64_t cw_checksum = 1469598103934665603ull;

    for (std::size_t i = 0; i < x_fp.size(); ++i) {
        const int signed_q = signed_bucket_from_fp(x_fp[i], qshift);
        const int lut_idx = signed_q + kBucketZero;

        const std::uint32_t cw = lut[static_cast<std::size_t>(lut_idx)];
        const DecodedReluCW d = unpack_relu_cw(cw);

        neg_count += d.neg ? 1u : 0u;
        zero_guard_count += d.zero_guard ? 1u : 0u;
        sat_count += d.sat ? 1u : 0u;
        pos_count += (!d.neg && !d.zero_guard) ? 1u : 0u;

        cw_checksum ^= static_cast<std::uint64_t>(cw);
        cw_checksum *= 1099511628211ull;

        const std::int64_t y =
            eval_relu_from_cw(x_fp[i], cw, qshift, bitwidth);

#if DAZG_ORBIT_BFE_RELU_SELF_CHECK
        const std::int64_t ref =
            clamp_to_bitwidth(x_fp[i] > 0 ? x_fp[i] : 0, bitwidth);

        if (y != ref) {
            std::lock_guard<std::mutex> lock(log_mutex());
            std::cerr << "[StageY BFEReLU selfcheck failed]"
                      << " i=" << i
                      << " x=" << x_fp[i]
                      << " y=" << y
                      << " ref=" << ref
                      << " signed_q=" << signed_q
                      << " cw=0x" << std::hex << cw << std::dec
                      << " qshift=" << qshift
                      << std::endl;
            std::abort();
        }
#endif

        y_fp[i] = y;
    }

    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::high_resolution_clock::now() - t0).count();

    static std::atomic<int> prints{0};
    if (prints.fetch_add(1) < 16) {
        std::lock_guard<std::mutex> lock(log_mutex());
        std::cerr << "[StageY BFEReLU stats]"
                  << " n=" << x_fp.size()
                  << " bitwidth=" << bitwidth
                  << " scale=" << scale
                  << " max_abs=" << max_abs
                  << " qshift=" << qshift
                  << " neg=" << neg_count
                  << " pos=" << pos_count
                  << " zero_guard=" << zero_guard_count
                  << " sat=" << sat_count
                  << " cw_checksum=" << cw_checksum
                  << " time_us=" << us
                  << std::endl;
    }
}

std::vector<std::int64_t> EvalBFEReLUClear(
    const std::vector<std::int64_t>& x_fp,
    int bitwidth,
    int scale) {

    std::vector<std::int64_t> y_fp;
    EvalBFEReLUClear(x_fp, bitwidth, scale, y_fp);
    return y_fp;
}

} // namespace HE
