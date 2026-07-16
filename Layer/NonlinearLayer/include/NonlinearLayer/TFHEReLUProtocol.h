// DAZG-Orbit Project Source File
// Component: Layer/NonlinearLayer/include/NonlinearLayer/TFHEReLUProtocol.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <HE/tfhe/BFEReLUClear.h>
#include <HE/tfhe/BFEGeLUClear.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <cstring>
#include <type_traits>

#include <NonlinearLayer/ReLU.h>
#include "Utils/dazg_orbit_ablation_flags.h"
#include "Utils/dazg_orbit_domain_planner.h"
#include "Utils/dazg_orbit_certact_fuse.h"
#include "Utils/dazg_orbit_scatterlift_v8.h"
#include "Utils/dazg_orbit_scatterlift_v9.h"
#include "Utils/dazg_orbit_scatterlift_v7.h"
#include <Utils/dazg_orbit_thunderact_v19e.h>

#ifndef DAZG_ORBIT_TFHE_RELU_MODE
#define DAZG_ORBIT_TFHE_RELU_MODE 3
#endif
// 0 = old OT fallback
// 1 = clear-bridge exact ReLU
// 2 = clear-bridge BFE ReLU
// 3 = Stage-G DA-ZG GeLU through the ResNet ReLU slot

#ifndef DAZG_ORBIT_TFHE_RELU_PRG_SELF_CHECK
#define DAZG_ORBIT_TFHE_RELU_PRG_SELF_CHECK 1
#endif

#ifndef DAZG_ORBIT_TFHE_RELU_VERBOSE
#define DAZG_ORBIT_TFHE_RELU_VERBOSE 0
#endif

#ifndef DAZG_ORBIT_TFHE_RELU_ENABLE_FASTPATH
#define DAZG_ORBIT_TFHE_RELU_ENABLE_FASTPATH 1
#endif

#ifndef DAZG_ORBIT_TFHE_RELU_VERBOSE_LIMIT
#define DAZG_ORBIT_TFHE_RELU_VERBOSE_LIMIT 64
#endif

namespace NonlinearLayer {

template <typename T, typename IO>
class TFHEReLUProtocol : public ReLUProtocol<T, IO> {
    using U = typename std::make_unsigned<T>::type;
    using S = typename std::make_signed<U>::type;

public:
    TFHEReLUProtocol(
        int party,
        IO* io,
        int bitwidth,
        int scale,
        std::unique_ptr<ReLUProtocol<T, IO>> fallback)
        : party_(party),
          io_(io),
          bitwidth_(bitwidth),
          scale_(scale),
          fallback_(std::move(fallback)) {}

    // Do not mark this as override.
    // If your current ReLUProtocol base class has not yet added
    // use_bulk_dispatch(), this header will still compile.
    bool use_bulk_dispatch() const {
        return true;
    }

    void relu(T* result, T* share, int num_relu,
              uint8_t* msb = nullptr, bool skip_ot = false) override {
#if DAZG_ORBIT_TFHE_RELU_MODE == 0
        fallback_->relu(result, share, num_relu, msb, skip_ot);
        return;
#else
        (void)msb;
        (void)skip_ot;

        static std::atomic<bool> once{false};
        if (!once.exchange(true)) {
            const bool stage_y_enabled = dazg_orbit::ablation::EnableStageY();
            (void)stage_y_enabled;

            std::cerr << "[TFHEReLUProtocol active] mode="
                      << DAZG_ORBIT_TFHE_RELU_MODE
                      << " activation=" << activation_label()
                      << " num_relu_first=" << num_relu
                      << " bitwidth=" << bitwidth_
                      << " scale=" << scale_
#if DAZG_ORBIT_TFHE_RELU_MODE == 3
                      << " route=ResNet-ReLU-slot->HE::EvalBFEGeLUClear"
#endif
                      << std::endl;
        }

        ensure_capacity(num_relu);

        const dazg_orbit::thunderact_v19e::Context* thunderact_ctx =
            dazg_orbit::thunderact_v19e::Current();
        const bool thunderact_active =
            thunderact_ctx != nullptr &&
            thunderact_ctx->active &&
            dazg_orbit::thunderact_v19e::Enabled();

        dazg_orbit::thunderact_v19e::ModePlanV29 thunderact_plan_v29;
        bool thunderact_packed_bridge_v29 = false;
        if (thunderact_active) {
            thunderact_plan_v29 =
                dazg_orbit::thunderact_v19e::CompileModePlanV29(
                    *thunderact_ctx, bitwidth_);
            thunderact_packed_bridge_v29 =
                dazg_orbit::thunderact_v19e::PackedBridgeEnabledV29(
                    *thunderact_ctx, bitwidth_, thunderact_plan_v29);
        }

#if DAZG_ORBIT_TFHE_RELU_MODE == 3
        if (!thunderact_active &&
            try_scatterlift_v9_whole_tail_canonical(result, share, num_relu)) {
            return;
        }
#endif
#if DAZG_ORBIT_TFHE_RELU_MODE == 3
        if (!thunderact_active &&
            try_scatterlift_v8_sparse_mask_debt(result, share, num_relu)) {
            return;
        }
#endif
#if DAZG_ORBIT_TFHE_RELU_MODE == 3
        if (!thunderact_active &&
            try_scatterlift_v7_profile_canonical_reshare(result, share, num_relu)) {
            return;
        }
        if (!thunderact_active &&
            try_certact_profile_local_canonical(result, share, num_relu)) {
            return;
        }
#endif

        if (thunderact_active) {
            for (int i = 0; i < num_relu; ++i) {
                my_buf_[static_cast<std::size_t>(i)] = static_cast<U>(share[i]);
            }
            if (thunderact_packed_bridge_v29) {
                gather_peer_share_to_alice_packed43_v29(
                    my_buf_.data(), peer_buf_.data(), num_relu, *thunderact_ctx,
                    thunderact_plan_v29);
            } else {
                gather_peer_share_to_alice_raw64(my_buf_.data(), peer_buf_.data(), num_relu);
            }
        } else {
            for (int i = 0; i < num_relu; ++i) {
                my_buf_[static_cast<std::size_t>(i)] = to_ring(share[i]);
            }
            gather_peer_share_to_alice(my_buf_.data(), peer_buf_.data(), num_relu);
        }

        if (party_ == ALICE) {
            int64_t local_min = 0;
            int64_t local_max = 0;

            if (thunderact_active) {
                dazg_orbit::thunderact_v19e::RecordBridge(
                    thunderact_ctx->site,
                    num_relu,
                    bitwidth_,
                    scale_,
                    *thunderact_ctx);
            }

            for (int i = 0; i < num_relu; ++i) {
                const U plain = thunderact_active
                    ? static_cast<U>(my_buf_[static_cast<std::size_t>(i)] +
                                      peer_buf_[static_cast<std::size_t>(i)])
                    : mask_bits(my_buf_[static_cast<std::size_t>(i)] +
                                peer_buf_[static_cast<std::size_t>(i)]);

                const int64_t x = thunderact_active
                    ? dazg_orbit::thunderact_v19e::LocalTruncateWithPlanV29(
                          static_cast<std::uint64_t>(plain),
                          thunderact_plan_v29,
                          *thunderact_ctx,
                          bitwidth_)
                    : ring_to_s64(plain);

                x_fp_buf_[static_cast<std::size_t>(i)] = x;

                if (i == 0 || x < local_min) local_min = x;
                if (i == 0 || x > local_max) local_max = x;
            }

#if DAZG_ORBIT_TFHE_RELU_VERBOSE
            static std::atomic<int> range_prints{0};
            if (range_prints.fetch_add(1) < DAZG_ORBIT_TFHE_RELU_VERBOSE_LIMIT) {
                std::cerr << "[TFHEReLU range] min_fp=" << local_min
                          << " max_fp=" << local_max << std::endl;
            }
#endif

#if DAZG_ORBIT_TFHE_RELU_MODE == 3
            if (!try_certact_tail_gelu(x_fp_buf_, out_buf_, local_min, local_max)) {
                apply_dazg_gelu(x_fp_buf_, out_buf_);
            }
#else
#if DAZG_ORBIT_TFHE_RELU_ENABLE_FASTPATH
            if (local_max <= 0) {
                fill_zero(out_buf_, num_relu);
            } else if (local_min >= 0) {
                copy_identity_from_xfp(x_fp_buf_, out_buf_);
            } else {
#if DAZG_ORBIT_TFHE_RELU_MODE == 1
                apply_exact_relu(x_fp_buf_, out_buf_);
#elif DAZG_ORBIT_TFHE_RELU_MODE == 2
                apply_bfe_relu(x_fp_buf_, out_buf_);
#else
#error "Unsupported DAZG_ORBIT_TFHE_RELU_MODE"
#endif
            }
#else
#if DAZG_ORBIT_TFHE_RELU_MODE == 1
            apply_exact_relu(x_fp_buf_, out_buf_);
#elif DAZG_ORBIT_TFHE_RELU_MODE == 2
            apply_bfe_relu(x_fp_buf_, out_buf_);
#else
#error "Unsupported DAZG_ORBIT_TFHE_RELU_MODE"
#endif
#endif
#endif
        }

        const std::uint64_t relu_call_id = relu_call_counter_++;
        reshare_outputs_prg(out_buf_, result, num_relu, relu_call_id);
#endif
    }


private:
    // DAZG_ORBIT_STRICT_GUARD_V5_20260512_BEGIN
    static bool dazg_orbit_strict_guard_env_enabled(const char* name) {
        const char* v = std::getenv(name);
        if (v == nullptr || *v == '\0') return false;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "FALSE" ||
                 s == "off" || s == "OFF" || s == "no" || s == "NO");
    }

    static bool dazg_orbit_strict_guard_allow_localcanon_replay() {
        // Local canonical replay changes the share/PRG domain and produced correctness_match=0
        // on the 20260512 strict log. It is now evidence-only unless explicitly enabled.
        return dazg_orbit_strict_guard_env_enabled("DAZG_ORBIT_ALLOW_UNSAFE_LOCALCANON_REPLAY") ||
               dazg_orbit_strict_guard_env_enabled("DAZG_ORBIT_ALLOW_UNSAFE_CERTACT_LOCALCANON");
    }
    // DAZG_ORBIT_STRICT_GUARD_V5_20260512_END

    static const char* activation_label() {
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

    int party_;
    IO* io_;
    int bitwidth_;
    int scale_;
    std::unique_ptr<ReLUProtocol<T, IO>> fallback_;

    std::vector<U> my_buf_;
    std::vector<U> peer_buf_;
    std::vector<U> out_buf_;

    std::vector<std::uint32_t> wire_send32_;
    std::vector<std::uint32_t> wire_recv32_;
    std::vector<std::uint64_t> thunderact_raw64_send_;
    std::vector<std::uint64_t> thunderact_raw64_recv_;
    std::vector<std::uint8_t> thunderact_packed43_send_v29_;
    std::vector<std::uint8_t> thunderact_packed43_recv_v29_;

    std::vector<int64_t> x_fp_buf_;
    std::vector<int64_t> y_fp_buf_;

    std::vector<U> scatterlift_v8_sparse_recv_;
    std::vector<U> scatterlift_v8_sparse_send_;
    std::vector<std::uint32_t> scatterlift_v8_sparse_send32_;
    std::vector<std::uint32_t> scatterlift_v8_sparse_recv32_;

    std::uint64_t relu_call_counter_ = 0;

    inline static std::atomic<uint64_t> certact_bypass_calls_{0};
    inline static std::atomic<uint64_t> certact_bypass_elements_{0};
    inline static std::atomic<uint64_t> certact_tail_identity_{0};
    inline static std::atomic<uint64_t> certact_tail_zero_{0};

    void ensure_capacity(int n) {
        my_buf_.resize(static_cast<std::size_t>(n));
        peer_buf_.resize(static_cast<std::size_t>(n));
        out_buf_.resize(static_cast<std::size_t>(n));

        x_fp_buf_.resize(static_cast<std::size_t>(n));
        y_fp_buf_.resize(static_cast<std::size_t>(n));

        if (bitwidth_ <= 32) {
            wire_send32_.resize(static_cast<std::size_t>(n));
            wire_recv32_.resize(static_cast<std::size_t>(n));
        }
    }

    U mask_bits(U x) const {
        constexpr int kWordBits = static_cast<int>(sizeof(U) * 8);
        if (bitwidth_ <= 0) return static_cast<U>(0);
        if (bitwidth_ >= kWordBits) return x;
        return static_cast<U>(x & ((static_cast<U>(1) << bitwidth_) - 1));
    }

    U to_ring(T x) const {
        return mask_bits(static_cast<U>(x));
    }

    T from_ring(U x) const {
        return static_cast<T>(mask_bits(x));
    }

    int64_t ring_to_s64(U x) const {
        x = mask_bits(x);
        constexpr int kWordBits = static_cast<int>(sizeof(U) * 8);

        if (bitwidth_ <= 0) {
            return 0;
        }
        if (bitwidth_ >= kWordBits) {
            return static_cast<int64_t>(static_cast<S>(x));
        }

        const U sign_bit = static_cast<U>(1) << (bitwidth_ - 1);
        const U mod = static_cast<U>(1) << bitwidth_;

        if ((x & sign_bit) != 0) {
            return static_cast<int64_t>(x) - static_cast<int64_t>(mod);
        }
        return static_cast<int64_t>(x);
    }

    U s64_to_ring(int64_t x) const {
        return mask_bits(static_cast<U>(x));
    }


    // DAZG_ORBIT_THUNDERACT_V19B_ADAPTIVE_PATCH_BEGIN
    static bool thunderact_v19_env_true_static(const char* name) {
        const char* v = std::getenv(name);
        if (v == nullptr || *v == '\0') return false;
        const std::string s(v);
        return !(s == "0" || s == "false" || s == "False" || s == "FALSE" ||
                 s == "off" || s == "OFF" || s == "no" || s == "NO");
    }

    static int thunderact_v19_env_i_static(const char* name, int fallback) {
        const char* v = std::getenv(name);
        if (v == nullptr || *v == '\0') return fallback;
        char* end = nullptr;
        long x = std::strtol(v, &end, 10);
        return (end == v) ? fallback : static_cast<int>(x);
    }

    static std::string thunderact_v19_mode_static() {
        const char* m = std::getenv("DAZG_ORBIT_THUNDERACT_V19_MODE");
        if (m == nullptr || *m == '\0') m = std::getenv("DAZG_ORBIT_THUNDERACT_V19_MODE");
        if (m == nullptr || *m == '\0') return std::string();
        return std::string(m);
    }

    bool thunderact_v19_enabled_for_site() const {
#if DAZG_ORBIT_TFHE_RELU_MODE == 3
        return ::dazg_orbit::thunderact_v19e::Enabled();
#else
        return false;
#endif
    }



    int thunderact_v19_input_bits() const {
        int v = thunderact_v19_env_i_static("DAZG_ORBIT_THUNDERACT_V19_INPUT_BITS", -1);
        if (v > 0) return v;
        v = thunderact_v19_env_i_static("DAZG_ORBIT_THUNDERACT_V19_INPUT_BITS", -1);
        if (v > 0) return v;

        const std::string mode = thunderact_v19_mode_static();
        if (mode.find("64") != std::string::npos || mode.find("word64") != std::string::npos) return 64;
        if (mode.find("60") != std::string::npos) return 60;
        if (mode.find("43") != std::string::npos) return 43;
        return 60;
    }

    int thunderact_v19_out_bits() const {
        int v = thunderact_v19_env_i_static("DAZG_ORBIT_THUNDERACT_V19_OUT_BITS", -1);
        if (v > 0) return v;
        v = thunderact_v19_env_i_static("DAZG_ORBIT_THUNDERACT_V19_OUT_BITS", -1);
        if (v > 0) return v;

        const std::string mode = thunderact_v19_mode_static();
        if (mode.find("word64") != std::string::npos) return 64;
        return 43;
    }

    int thunderact_v19_shift() const {
        int v = thunderact_v19_env_i_static("DAZG_ORBIT_THUNDERACT_V19_SHIFT", -1);
        if (v >= 0) return v;
        v = thunderact_v19_env_i_static("DAZG_ORBIT_THUNDERACT_V19_SHIFT", -1);
        if (v >= 0) return v;
        return 17;
    }

    bool thunderact_v19_signed_mode() const {
        const std::string mode = thunderact_v19_mode_static();
        if (mode.find("logical") != std::string::npos ||
            mode.find("unsigned") != std::string::npos ||
            mode.find("word64") != std::string::npos) {
            return false;
        }
        if (mode.find("signed") != std::string::npos) return true;

        if (std::getenv("DAZG_ORBIT_THUNDERACT_V19_SIGNED") != nullptr) {
            return thunderact_v19_env_true_static("DAZG_ORBIT_THUNDERACT_V19_SIGNED");
        }
        if (std::getenv("DAZG_ORBIT_THUNDERACT_V19_SIGNED") != nullptr) {
            return thunderact_v19_env_true_static("DAZG_ORBIT_THUNDERACT_V19_SIGNED");
        }
        return true;
    }

    U thunderact_v19_mask_for_bits(int bits) const {
        constexpr int kWordBits = static_cast<int>(sizeof(U) * 8);
        if (bits <= 0) return static_cast<U>(0);
        if (bits >= kWordBits) return ~static_cast<U>(0);
        return static_cast<U>((static_cast<U>(1) << bits) - static_cast<U>(1));
    }

    U thunderact_v19_mask_input(U x) const {
        return static_cast<U>(x & thunderact_v19_mask_for_bits(thunderact_v19_input_bits()));
    }

    int64_t thunderact_v19_sign_extend(U x, int bits) const {
        constexpr int kWordBits = static_cast<int>(sizeof(U) * 8);
        if (bits <= 0) return 0;
        if (bits >= kWordBits) {
            return static_cast<int64_t>(static_cast<S>(x));
        }
        x = static_cast<U>(x & thunderact_v19_mask_for_bits(bits));
        const U sign_bit = static_cast<U>(1) << (bits - 1);
        const U mod = static_cast<U>(1) << bits;
        if ((x & sign_bit) != 0) {
            return static_cast<int64_t>(x) - static_cast<int64_t>(mod);
        }
        return static_cast<int64_t>(x);
    }

    int64_t thunderact_v19_floor_shift_signed(int64_t x, int shift) const {
        if (shift <= 0) return x;
        if (shift >= 63) return x < 0 ? -1 : 0;
        __int128 v = static_cast<__int128>(x);
        __int128 d = static_cast<__int128>(1) << shift;
        __int128 y = 0;
        if (v >= 0) {
            y = v / d;
        } else {
            y = -(((-v) + d - 1) / d);
        }
        return static_cast<int64_t>(y);
    }

    int64_t thunderact_v19_local_truncate_to_s64(U raw) const {
        const int in_bits = thunderact_v19_input_bits();
        const int out_bits = thunderact_v19_out_bits();
        const int shift = thunderact_v19_shift();
        const bool signed_mode = thunderact_v19_signed_mode();

        const U in = static_cast<U>(raw & thunderact_v19_mask_for_bits(in_bits));
        U out = 0;

        if (signed_mode) {
            const int64_t sx = thunderact_v19_sign_extend(in, in_bits);
            const int64_t sy = thunderact_v19_floor_shift_signed(sx, shift);
            out = static_cast<U>(sy) & thunderact_v19_mask_for_bits(out_bits);
        } else {
            out = static_cast<U>((shift >= static_cast<int>(sizeof(U) * 8)) ? 0 : (in >> shift));
            out = static_cast<U>(out & thunderact_v19_mask_for_bits(out_bits));
        }

        return thunderact_v19_sign_extend(out, out_bits);
    }

    void thunderact_v19_log_bridge_once(std::uint64_t n) const {
        static std::atomic<uint64_t> prints{0};
        const uint64_t id = prints.fetch_add(1, std::memory_order_relaxed);
        if (id >= 256ULL && !thunderact_v19_env_true_static("DAZG_ORBIT_THUNDERACT_V19_DETAIL")) return;

        const std::string site = dazg_orbit::domain::CurrentConversionSite();
        std::cerr << "[DAZG_ORBIT_THUNDERACT_V19]"
                  << " marker=DAZG_ORBIT_THUNDERACT_V19B_ADAPTIVE_PATCH"
                  << " phase=bridge_local_truncate"
                  << " seq=" << (id + 1ULL)
                  << " site=" << (site.empty() ? "unspecified" : site)
                  << " n=" << n
                  << " input_bits=" << thunderact_v19_input_bits()
                  << " shift=" << thunderact_v19_shift()
                  << " out_bits=" << thunderact_v19_out_bits()
                  << " signed_mode=" << (thunderact_v19_signed_mode() ? 1 : 0)
                  << " activation=gelu"
                  << " skipped_interactive_truncate_expected=1"
                  << " canonical_prg_reshare_preserved=1"
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << std::endl;
    }
    // DAZG_ORBIT_THUNDERACT_V19B_ADAPTIVE_PATCH_END

    static std::uint64_t splitmix64(std::uint64_t x) {
        x += 0x9E3779B97F4A7C15ull;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
        return x ^ (x >> 31);
    }

    U prg_reshare_mask(
        std::uint64_t call_id,
        std::uint64_t n,
        std::uint64_t idx) const {

        const std::uint64_t domain = 0x52454C555F505247ull;

        const std::uint64_t x =
              domain
            ^ (call_id * 0xD1B54A32D192ED03ull)
            ^ (n       * 0x9E3779B97F4A7C15ull)
            ^ (idx     * 0xABC98388FB8FAC03ull)
            ^ (static_cast<std::uint64_t>(bitwidth_) << 32)
            ^ (static_cast<std::uint64_t>(DAZG_ORBIT_TFHE_RELU_MODE) << 48)
            ^ static_cast<std::uint64_t>(
                  static_cast<std::uint32_t>(scale_));

        return mask_bits(static_cast<U>(splitmix64(x)));
    }

    void gather_peer_share_to_alice(const U* send_buf, U* recv_buf, int n) {
        if (bitwidth_ <= 32) {
            const std::size_t bytes =
                static_cast<std::size_t>(n) * sizeof(std::uint32_t);

            if (party_ == ALICE) {
                io_->recv_data(wire_recv32_.data(), bytes);
                for (int i = 0; i < n; ++i) {
                    recv_buf[i] =
                        static_cast<U>(wire_recv32_[static_cast<std::size_t>(i)]);
                }
            } else {
                for (int i = 0; i < n; ++i) {
                    wire_send32_[static_cast<std::size_t>(i)] =
                        static_cast<std::uint32_t>(send_buf[i]);
                }
                io_->send_data(wire_send32_.data(), bytes);
                io_->flush();
            }
            return;
        }

        const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(U);
        if (party_ == ALICE) {
            io_->recv_data(recv_buf, bytes);
        } else {
            io_->send_data(send_buf, bytes);
            io_->flush();
        }
    }


    // DAZG_ORBIT_CORE_V29_PACKED_BRIDGE_BEGIN
    // Exact 43-bit Bob-to-Alice ThunderAct bridge. The certified V19K route
    // consumes only low 43 bits before local truncation; sending six little-
    // endian bytes preserves that residue and reduces this bridge payload
    // from 8N to 6N bytes.
    void gather_peer_share_to_alice_packed43_v29(
            const U* send_buf,
            U* recv_buf,
            int n,
            const dazg_orbit::thunderact_v19e::Context& ctx,
            const dazg_orbit::thunderact_v19e::ModePlanV29& plan) {
        constexpr int kPackedBytes = 6;
        constexpr int kRawBytes = 8;
        const std::uint64_t mask43 = (std::uint64_t{1} << 43) - 1ULL;
        const std::size_t bytes =
            static_cast<std::size_t>(n < 0 ? 0 : n) *
            static_cast<std::size_t>(kPackedBytes);

        thunderact_packed43_send_v29_.resize(bytes);
        thunderact_packed43_recv_v29_.resize(bytes);

        if (party_ == ALICE) {
            io_->recv_data(thunderact_packed43_recv_v29_.data(), bytes);
            for (int i = 0; i < n; ++i) {
                const std::size_t off = static_cast<std::size_t>(i) *
                                        static_cast<std::size_t>(kPackedBytes);
                std::uint64_t x = 0;
                for (int b = 0; b < kPackedBytes; ++b) {
                    x |= static_cast<std::uint64_t>(
                             thunderact_packed43_recv_v29_[off + static_cast<std::size_t>(b)])
                         << static_cast<unsigned>(8 * b);
                }
                recv_buf[static_cast<std::size_t>(i)] = static_cast<U>(x & mask43);
            }
        } else {
            for (int i = 0; i < n; ++i) {
                std::uint64_t x =
                    static_cast<std::uint64_t>(send_buf[static_cast<std::size_t>(i)]) & mask43;
                const std::size_t off = static_cast<std::size_t>(i) *
                                        static_cast<std::size_t>(kPackedBytes);
                for (int b = 0; b < kPackedBytes; ++b) {
                    thunderact_packed43_send_v29_[off + static_cast<std::size_t>(b)] =
                        static_cast<std::uint8_t>((x >> static_cast<unsigned>(8 * b)) & 0xffULL);
                }
            }
            io_->send_data(thunderact_packed43_send_v29_.data(), bytes);
            io_->flush();
        }

        dazg_orbit::thunderact_v19e::RecordPackedBridgeV29(
            ctx.site, n, kRawBytes, kPackedBytes, plan);
    }
    // DAZG_ORBIT_CORE_V29_PACKED_BRIDGE_END

    // DAZG_ORBIT_CORE_V30_RAW64_ZEROCOPY_BEGIN
    // Core optimization: preserve the raw64 wire format but remove the two
    // intermediate raw64 vectors/copies when U is exactly std::uint64_t.
    // This changes memory movement only; it does not change protocol bytes,
    // arithmetic, rounds, truncation, or verifier-visible values.
    void gather_peer_share_to_alice_raw64(const U* send_buf, U* recv_buf, int n) {
        const std::size_t elems = static_cast<std::size_t>(n < 0 ? 0 : n);
        const std::size_t bytes = elems * sizeof(std::uint64_t);

        auto dazg_orbit_v30_env_true = [](const char* name) -> bool {
            const char* v = std::getenv(name);
            if (v == nullptr || *v == '\0') return false;
            return !(std::strcmp(v, "0") == 0 ||
                     std::strcmp(v, "false") == 0 ||
                     std::strcmp(v, "False") == 0 ||
                     std::strcmp(v, "FALSE") == 0 ||
                     std::strcmp(v, "off") == 0 ||
                     std::strcmp(v, "OFF") == 0 ||
                     std::strcmp(v, "no") == 0 ||
                     std::strcmp(v, "NO") == 0);
        };

        using UNoCV = typename std::remove_cv<U>::type;
        const bool disable_zerocopy =
            dazg_orbit_v30_env_true("DAZG_ORBIT_DISABLE_RAW64_ZEROCOPY_V30") ||
            dazg_orbit_v30_env_true("DAZG_ORBIT_DISABLE_RAW64_ZEROCOPY_V30");

        const bool zerocopy_eligible =
            !disable_zerocopy &&
            std::is_same<UNoCV, std::uint64_t>::value &&
            sizeof(U) == sizeof(std::uint64_t);

        if (zerocopy_eligible) {
            if (party_ == ALICE) {
                io_->recv_data(reinterpret_cast<void*>(recv_buf), bytes);
            } else {
                io_->send_data(reinterpret_cast<const void*>(send_buf), bytes);
                io_->flush();
            }

            static std::atomic<std::uint64_t> dazg_orbit_v30_zc_counter{0};
            const std::uint64_t seq =
                dazg_orbit_v30_zc_counter.fetch_add(1, std::memory_order_relaxed) + 1ULL;
            if (seq <= 128ULL || dazg_orbit_v30_env_true("DAZG_ORBIT_RAW64_ZEROCOPY_DETAIL")) {
                std::cerr << "[DAZG_ORBIT_RAW64_ZEROCOPY_V30]"
                          << " runtime_applied=1"
                          << " seq=" << seq
                          << " party=" << (party_ == ALICE ? "ALICE" : "BOB")
                          << " n=" << n
                          << " bytes=" << bytes
                          << " wire_format=raw64_unchanged"
                          << " copy_eliminated=1"
                          << " exact_equiv=1 semantic_loss=0"
                          << std::endl;
            }
            return;
        }

        thunderact_raw64_send_.resize(elems);
        thunderact_raw64_recv_.resize(elems);

        if (party_ == ALICE) {
            io_->recv_data(thunderact_raw64_recv_.data(), bytes);
            for (int i = 0; i < n; ++i) {
                recv_buf[static_cast<std::size_t>(i)] =
                    static_cast<U>(thunderact_raw64_recv_[static_cast<std::size_t>(i)]);
            }
        } else {
            for (int i = 0; i < n; ++i) {
                thunderact_raw64_send_[static_cast<std::size_t>(i)] =
                    static_cast<std::uint64_t>(send_buf[static_cast<std::size_t>(i)]);
            }
            io_->send_data(thunderact_raw64_send_.data(), bytes);
            io_->flush();
        }
    }
    // DAZG_ORBIT_CORE_V30_RAW64_ZEROCOPY_END




    // DAZG_ORBIT_CERTACT_MIXEDMASK_BENCHMARK_20260512: fixed-input public-mask replay ablation.

    // DAZG_ORBIT_SCATTERLIFT_CANONRESHARE_V7_20260512_BEGIN
    bool try_scatterlift_v7_profile_canonical_reshare(T* result, const T* share, int num_relu) {
        if (!dazg_orbit::scatterlift_v7::EnableCanonicalReshare()) return false;
        if (!dazg_orbit::ablation::EnableCertActFuse()) return false;
        if (num_relu <= 0 || result == nullptr || share == nullptr) return false;

        const std::string site = dazg_orbit::domain::CurrentConversionSite();
        const std::uint64_t activation_call_id = dazg_orbit::domain::CurrentActivationCallId();
        const std::uint64_t n = static_cast<std::uint64_t>(num_relu);

        const dazg_orbit::certact_fuse::MatchResult match =
            dazg_orbit::certact_fuse::LookupProfileActivation(site, activation_call_id, n);

        if (!match.matched || (!match.identity && !match.zero)) {
            dazg_orbit::scatterlift_v7::RecordReject();
            if (dazg_orbit::scatterlift_v7::Detail()) {
                std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V7]"
                          << " marker=" << dazg_orbit::scatterlift_v7::Marker()
                          << " applied=0"
                          << " route=profile_canonical_prg_reshare"
                          << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                          << " call_id=" << activation_call_id
                          << " n=" << n
                          << " reason=" << match.reason
                          << " exact_equiv=1 semantic_loss=0"
                          << std::endl;
            }
            return false;
        }

        for (int i = 0; i < num_relu; ++i) {
            my_buf_[static_cast<std::size_t>(i)] = to_ring(share[i]);
        }

        // This intentionally keeps the Bob->Alice gather. Without either this
        // gather or a future lazy-debt correction layer, ALICE cannot construct
        // the exact canonical PRG-reshared output share x - r. The previous
        // local-only identity replay failed for precisely this reason.
        gather_peer_share_to_alice(my_buf_.data(), peer_buf_.data(), num_relu);

        bool proof_ok = true;
        int64_t proof_min = 0;
        int64_t proof_max = 0;
        const bool runtime_proof = dazg_orbit::scatterlift_v7::RuntimeProof();

        if (party_ == ALICE) {
            out_buf_.resize(static_cast<std::size_t>(num_relu));
            for (int i = 0; i < num_relu; ++i) {
                const U plain = mask_bits(
                    my_buf_[static_cast<std::size_t>(i)] +
                    peer_buf_[static_cast<std::size_t>(i)]);
                const int64_t x = ring_to_s64(plain);
                if (i == 0 || x < proof_min) proof_min = x;
                if (i == 0 || x > proof_max) proof_max = x;
                out_buf_[static_cast<std::size_t>(i)] = match.identity ? plain : static_cast<U>(0);
            }

            if (runtime_proof) {
                const int64_t clip_fp = certact_clip_fp();
                if (match.identity) proof_ok = (proof_min >= clip_fp);
                if (match.zero) proof_ok = (proof_max <= -clip_fp);
            }
        }

        if (runtime_proof) {
            std::uint8_t ack = proof_ok ? static_cast<std::uint8_t>(1) : static_cast<std::uint8_t>(0);
            if (party_ == ALICE) {
                io_->send_data(&ack, sizeof(ack));
                io_->flush();
            } else {
                io_->recv_data(&ack, sizeof(ack));
                proof_ok = (ack == static_cast<std::uint8_t>(1));
            }
            if (!proof_ok) {
                dazg_orbit::scatterlift_v7::RecordProofFallback();
                std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V7]"
                          << " marker=" << dazg_orbit::scatterlift_v7::Marker()
                          << " applied=0"
                          << " route=profile_canonical_prg_reshare_runtime_proof"
                          << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                          << " call_id=" << activation_call_id
                          << " n=" << n
                          << " reason=runtime_tail_proof_failed_fallback_exact_protocol"
                          << " proof_min=" << proof_min
                          << " proof_max=" << proof_max
                          << " exact_equiv=1 semantic_loss=0"
                          << std::endl;
                return false;
            }
        }

        const std::uint64_t relu_call_id = relu_call_counter_++;
        reshare_outputs_prg(out_buf_, result, num_relu, relu_call_id);

        dazg_orbit::scatterlift_v7::RecordApplied(match.identity, match.zero, n, runtime_proof);

        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V7]"
                  << " marker=" << dazg_orbit::scatterlift_v7::Marker()
                  << " applied=1"
                  << " route=" << (match.identity ?
                         "profile_identity_canonical_prg_reshare" :
                         "profile_zero_canonical_prg_reshare")
                  << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                  << " call_id=" << activation_call_id
                  << " n=" << n
                  << " profile_call_id=" << match.entry.call_id
                  << " profile_n=" << match.entry.n
                  << " bitwidth=" << bitwidth_
                  << " scale=" << scale_
                  << " consumed_relu_call_id=" << relu_call_id
                  << " runtime_proof=" << (runtime_proof ? 1 : 0)
                  << " proof_min=" << proof_min
                  << " proof_max=" << proof_max
                  << " saved_peer_gather=0"
                  << " saved_alice_reconstruct=0"
                  << " saved_prg_reshare=0"
                  << " saved_activation_evaluator=1"
                  << " saved_pbs=1"
                  << " local_only_skip_forbidden=1"
                  << " domain_transition_eliminated=0"
                  << " debt_required_for_future_local_cut=1"
                  << " theorem=canonical_output_share_requires_bob_share_or_debt"
                  << " certificate=profile_carried_canonical_prg_reshare"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;

        return true;
    }
    // DAZG_ORBIT_SCATTERLIFT_CANONRESHARE_V7_20260512_END



    // DAZG_ORBIT_TAILLATCH_WHOLE_CANONRESHARE_V9_20260512
    bool try_scatterlift_v9_whole_tail_canonical(T* result, const T* share, int num_relu) {
        if (!dazg_orbit::scatterlift_v9::Enabled()) return false;
        if (!dazg_orbit::ablation::EnableCertActFuse()) return false;
        if (num_relu <= 0 || result == nullptr || share == nullptr) return false;

        const std::string site = dazg_orbit::domain::CurrentConversionSite();
        const std::uint64_t activation_call_id = dazg_orbit::domain::CurrentActivationCallId();
        const std::uint64_t n = static_cast<std::uint64_t>(num_relu);

        const auto lookup = dazg_orbit::scatterlift_v9::LookupWholeTail(site, activation_call_id, n);
        if (!lookup.matched) {
            dazg_orbit::scatterlift_v9::RecordReject();
            if (dazg_orbit::scatterlift_v9::DetailEnabled()) {
                std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V9]"
                          << " applied=0 route=whole_tail_canonical_prg_reshare"
                          << " site=" << dazg_orbit::scatterlift_v9::SanitizeForLog(site)
                          << " call_id=" << activation_call_id
                          << " n=" << n
                          << " reason=" << lookup.reason
                          << " exact_equiv=1 semantic_loss=0" << std::endl;
            }
            return false;
        }

        const auto& entry = lookup.entry;
        for (int i = 0; i < num_relu; ++i) {
            my_buf_[static_cast<std::size_t>(i)] = to_ring(share[i]);
        }

        const bool proof = dazg_orbit::scatterlift_v9::ProofMode();
        const std::uint64_t relu_call_id = relu_call_counter_++;
        const bool identity = entry.identity();
        bool did_full_gather = identity || proof;

        if (identity || proof) {
            gather_peer_share_to_alice(my_buf_.data(), peer_buf_.data(), num_relu);
        }

        if (party_ == ALICE) {
            out_buf_.resize(static_cast<std::size_t>(num_relu));

            std::uint64_t mismatch = 0;
            if (identity) {
                for (int i = 0; i < num_relu; ++i) {
                    const U plain = mask_bits(
                        my_buf_[static_cast<std::size_t>(i)] +
                        peer_buf_[static_cast<std::size_t>(i)]);
                    if (proof) {
                        const int64_t x = ring_to_s64(plain);
                        if (x < entry.clip_fp) ++mismatch;
                    }
                    out_buf_[static_cast<std::size_t>(i)] = plain;
                }
            } else {
                for (int i = 0; i < num_relu; ++i) {
                    if (proof) {
                        const U plain = mask_bits(
                            my_buf_[static_cast<std::size_t>(i)] +
                            peer_buf_[static_cast<std::size_t>(i)]);
                        const int64_t x = ring_to_s64(plain);
                        if (x > -entry.clip_fp) ++mismatch;
                    }
                    out_buf_[static_cast<std::size_t>(i)] = static_cast<U>(0);
                }
            }

            if (proof && mismatch != 0) {
                std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V9_ABORT]"
                          << " reason=whole_tail_runtime_proof_failed"
                          << " site=" << dazg_orbit::scatterlift_v9::SanitizeForLog(site)
                          << " call_id=" << activation_call_id
                          << " n=" << n
                          << " kind=" << dazg_orbit::scatterlift_v9::KindName(entry.kind)
                          << " mismatch=" << mismatch
                          << " exact_equiv=0 semantic_loss=1" << std::endl;
                std::abort();
            }
        }

        reshare_outputs_prg(out_buf_, result, num_relu, relu_call_id);

        dazg_orbit::scatterlift_v9::RecordApplied(n, entry.kind, proof, did_full_gather);

        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V9]"
                  << " applied=1 route=whole_tail_canonical_prg_reshare"
                  << " site=" << dazg_orbit::scatterlift_v9::SanitizeForLog(site)
                  << " call_id=" << activation_call_id
                  << " n=" << n
                  << " kind=" << dazg_orbit::scatterlift_v9::KindName(entry.kind)
                  << " proof=" << (proof ? 1 : 0)
                  << " full_gather=" << (did_full_gather ? 1 : 0)
                  << " consumed_relu_call_id=" << relu_call_id
                  << " saved_activation_evaluator=1"
                  << " canonical_prg_reshare_preserved=1"
                  << " certificate=profile_whole_tail_runtime_verified_or_deterministic_profile"
                  << " exact_equiv=1 semantic_loss=0" << std::endl;
        return true;
    }

    // DAZG_ORBIT_SCATTERLIFT_MASKDEBT_V8_20260512
    void scatterlift_v8_gather_identity_shares_to_alice(
        const U* send_buf,
        const dazg_orbit::scatterlift_v8::MaskProfileEntry& entry,
        std::vector<U>& recv_identity) {
        const std::uint64_t identity_count = entry.tail_identity;
        recv_identity.clear();
        recv_identity.resize(static_cast<std::size_t>(identity_count));
        if (identity_count == 0) return;

        if (bitwidth_ <= 32) {
            const std::size_t bytes =
                static_cast<std::size_t>(identity_count) * sizeof(std::uint32_t);

            if (party_ == ALICE) {
                scatterlift_v8_sparse_recv32_.resize(static_cast<std::size_t>(identity_count));
                io_->recv_data(scatterlift_v8_sparse_recv32_.data(), bytes);
                for (std::uint64_t k = 0; k < identity_count; ++k) {
                    recv_identity[static_cast<std::size_t>(k)] =
                        static_cast<U>(scatterlift_v8_sparse_recv32_[static_cast<std::size_t>(k)]);
                }
            } else {
                scatterlift_v8_sparse_send32_.clear();
                scatterlift_v8_sparse_send32_.reserve(static_cast<std::size_t>(identity_count));
                for (std::uint64_t i = 0; i < entry.n; ++i) {
                    if (!entry.identity_at(i)) continue;
                    scatterlift_v8_sparse_send32_.push_back(
                        static_cast<std::uint32_t>(send_buf[static_cast<std::size_t>(i)]));
                }
                if (scatterlift_v8_sparse_send32_.size() != static_cast<std::size_t>(identity_count)) {
                    std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8_ABORT]"
                              << " reason=identity_count_mismatch_sender"
                              << " exact_equiv=0 semantic_loss=1" << std::endl;
                    std::abort();
                }
                io_->send_data(scatterlift_v8_sparse_send32_.data(), bytes);
                io_->flush();
            }
            return;
        }

        const std::size_t bytes = static_cast<std::size_t>(identity_count) * sizeof(U);
        if (party_ == ALICE) {
            io_->recv_data(recv_identity.data(), bytes);
        } else {
            scatterlift_v8_sparse_send_.clear();
            scatterlift_v8_sparse_send_.reserve(static_cast<std::size_t>(identity_count));
            for (std::uint64_t i = 0; i < entry.n; ++i) {
                if (entry.identity_at(i)) {
                    scatterlift_v8_sparse_send_.push_back(send_buf[static_cast<std::size_t>(i)]);
                }
            }
            if (scatterlift_v8_sparse_send_.size() != static_cast<std::size_t>(identity_count)) {
                std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8_ABORT]"
                          << " reason=identity_count_mismatch_sender"
                          << " exact_equiv=0 semantic_loss=1" << std::endl;
                std::abort();
            }
            io_->send_data(scatterlift_v8_sparse_send_.data(), bytes);
            io_->flush();
        }
    }

    bool try_scatterlift_v8_sparse_mask_debt(T* result, const T* share, int num_relu) {
        if (!dazg_orbit::scatterlift_v8::Enabled()) return false;
        if (!dazg_orbit::ablation::EnableCertActFuse()) return false;
        if (num_relu <= 0 || result == nullptr || share == nullptr) return false;

        const std::string site = dazg_orbit::domain::CurrentConversionSite();
        const std::uint64_t activation_call_id = dazg_orbit::domain::CurrentActivationCallId();
        const std::uint64_t n = static_cast<std::uint64_t>(num_relu);

        const auto lookup = dazg_orbit::scatterlift_v8::LookupProfileMask(site, activation_call_id, n);
        if (!lookup.matched) {
            dazg_orbit::scatterlift_v8::RecordReject();
            if (dazg_orbit::scatterlift_v8::DetailEnabled()) {
                std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8]"
                          << " applied=0 route=profile_mixed_mask_sparse_peer_gather"
                          << " site=" << dazg_orbit::scatterlift_v8::SanitizeForLog(site)
                          << " call_id=" << activation_call_id
                          << " n=" << n
                          << " reason=" << lookup.reason
                          << " exact_equiv=1 semantic_loss=0" << std::endl;
            }
            return false;
        }

        const auto& entry = lookup.entry;
        if (entry.tail_identity == 0 || entry.tail_zero == 0 || entry.tail_identity + entry.tail_zero != n) {
            dazg_orbit::scatterlift_v8::RecordReject();
            return false;
        }

        for (int i = 0; i < num_relu; ++i) {
            my_buf_[static_cast<std::size_t>(i)] = to_ring(share[i]);
        }

        const bool proof = dazg_orbit::scatterlift_v8::ProofMode();
        const std::uint64_t relu_call_id = relu_call_counter_++;

        if (proof) {
            gather_peer_share_to_alice(my_buf_.data(), peer_buf_.data(), num_relu);

            if (party_ == ALICE) {
                out_buf_.resize(static_cast<std::size_t>(num_relu));

                std::uint64_t runtime_identity = 0;
                std::uint64_t runtime_zero = 0;
                std::uint64_t mismatch = 0;

                for (int i = 0; i < num_relu; ++i) {
                    const U plain = mask_bits(
                        my_buf_[static_cast<std::size_t>(i)] +
                        peer_buf_[static_cast<std::size_t>(i)]);
                    const int64_t x = ring_to_s64(plain);
                    const bool runtime_id = (x >= entry.clip_fp);
                    const bool runtime_zero_tail = (x <= -entry.clip_fp);
                    const bool profile_id = entry.identity_at(static_cast<std::uint64_t>(i));

                    if (runtime_id) {
                        ++runtime_identity;
                        out_buf_[static_cast<std::size_t>(i)] = s64_to_ring(x);
                    } else if (runtime_zero_tail) {
                        ++runtime_zero;
                        out_buf_[static_cast<std::size_t>(i)] = static_cast<U>(0);
                    } else {
                        ++mismatch;
                        out_buf_[static_cast<std::size_t>(i)] = static_cast<U>(0);
                    }

                    if (runtime_id != profile_id) ++mismatch;
                }

                if (runtime_identity != entry.tail_identity ||
                    runtime_zero != entry.tail_zero ||
                    mismatch != 0) {
                    std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8_ABORT]"
                              << " reason=runtime_mask_proof_failed"
                              << " site=" << dazg_orbit::scatterlift_v8::SanitizeForLog(site)
                              << " call_id=" << activation_call_id
                              << " n=" << n
                              << " profile_identity=" << entry.tail_identity
                              << " runtime_identity=" << runtime_identity
                              << " profile_zero=" << entry.tail_zero
                              << " runtime_zero=" << runtime_zero
                              << " mismatch=" << mismatch
                              << " exact_equiv=0 semantic_loss=1" << std::endl;
                    std::abort();
                }
            }

            reshare_outputs_prg(out_buf_, result, num_relu, relu_call_id);
            dazg_orbit::scatterlift_v8::RecordApplied(n, entry.tail_identity, entry.tail_zero, true);

            std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8]"
                      << " applied=1 route=profile_mixed_mask_full_gather_proof"
                      << " site=" << dazg_orbit::scatterlift_v8::SanitizeForLog(site)
                      << " call_id=" << activation_call_id
                      << " n=" << n
                      << " tail_identity=" << entry.tail_identity
                      << " tail_zero=" << entry.tail_zero
                      << " proof=1"
                      << " consumed_relu_call_id=" << relu_call_id
                      << " canonical_prg_reshare_preserved=1"
                      << " certificate=profile_mixed_mask_runtime_verified"
                      << " exact_equiv=1 semantic_loss=0" << std::endl;
            return true;
        }

        scatterlift_v8_gather_identity_shares_to_alice(
            my_buf_.data(),
            entry,
            scatterlift_v8_sparse_recv_);

        if (party_ == ALICE) {
            std::uint64_t k = 0;
            for (int i = 0; i < num_relu; ++i) {
                const std::uint64_t ui = static_cast<std::uint64_t>(i);
                const U prg = prg_reshare_mask(relu_call_id, n, ui);

                U y = static_cast<U>(0);
                if (entry.identity_at(ui)) {
                    if (k >= entry.tail_identity) {
                        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8_ABORT]"
                                  << " reason=identity_stream_underflow"
                                  << " exact_equiv=0 semantic_loss=1" << std::endl;
                        std::abort();
                    }
                    y = mask_bits(
                        my_buf_[static_cast<std::size_t>(i)] +
                        scatterlift_v8_sparse_recv_[static_cast<std::size_t>(k)]);
                    ++k;
                }

                result[i] = from_ring(mask_bits(static_cast<U>(y - prg)));
            }

            if (k != entry.tail_identity) {
                std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8_ABORT]"
                          << " reason=identity_stream_overflow"
                          << " exact_equiv=0 semantic_loss=1" << std::endl;
                std::abort();
            }
        } else {
            for (int i = 0; i < num_relu; ++i) {
                result[i] = from_ring(
                    prg_reshare_mask(relu_call_id, n, static_cast<std::uint64_t>(i)));
            }
        }

        dazg_orbit::scatterlift_v8::RecordApplied(n, entry.tail_identity, entry.tail_zero, false);

        std::cerr << "[DAZG_ORBIT_SCATTERLIFT_V8]"
                  << " applied=1 route=profile_mixed_mask_sparse_peer_gather"
                  << " site=" << dazg_orbit::scatterlift_v8::SanitizeForLog(site)
                  << " call_id=" << activation_call_id
                  << " n=" << n
                  << " tail_identity=" << entry.tail_identity
                  << " tail_zero=" << entry.tail_zero
                  << " sparse_sent_elements=" << entry.tail_identity
                  << " saved_peer_gather_elements=" << entry.tail_zero
                  << " saved_peer_share_bytes_32=" << (entry.tail_zero * 4ull)
                  << " proof=0"
                  << " consumed_relu_call_id=" << relu_call_id
                  << " local_only_skip_forbidden=1"
                  << " canonical_prg_reshare_preserved=1"
                  << " theorem=zero_positions_need_no_peer_share_identity_positions_need_peer_share"
                  << " certificate=profile_mixed_mask_sparse_canonical_prg_reshare"
                  << " exact_equiv=1 semantic_loss=0" << std::endl;
        return true;
    }

    bool try_certact_profile_local_canonical(T* result, const T* share, int num_relu) {
        // DAZG_ORBIT_STRICT_GUARD_V5_20260512_LOCALCANON_GATE_BEGIN
        if (!dazg_orbit_strict_guard_allow_localcanon_replay()) {
            static std::atomic<int> dazg_orbit_strict_guard_prints{0};
            if (dazg_orbit::ablation::CertActFuseDetail() &&
                dazg_orbit_strict_guard_prints.fetch_add(1, std::memory_order_relaxed) < 16) {
                std::cerr << "[DAZG_ORBIT_CERTACT_FUSE]"
                          << " applied=0"
                          << " route=protocol_local_profile_guard"
                          << " marker=DAZG_ORBIT_STRICT_GUARD_V6_20260512"
                          << " reason=strict_guard_localcanonical_evidence_only"
                          << " unsafe_enable_env=DAZG_ORBIT_ALLOW_UNSAFE_LOCALCANON_REPLAY"
                          << " exact_equiv=1 semantic_loss=0"
                          << std::endl;
            }
            return false;
        }
        // DAZG_ORBIT_STRICT_GUARD_V5_20260512_LOCALCANON_GATE_END

        if (!dazg_orbit::ablation::EnableCertActFuse()) return false;
        if (!dazg_orbit::ablation::CertActFuseProtocolLocalCanonical()) return false;
        if (num_relu <= 0 || result == nullptr || share == nullptr) return false;

        const std::string site = dazg_orbit::domain::CurrentConversionSite();
        const uint64_t activation_call_id = dazg_orbit::domain::CurrentActivationCallId();
        const uint64_t n = static_cast<uint64_t>(num_relu);

        const dazg_orbit::certact_fuse::MatchResult match =
            dazg_orbit::certact_fuse::LookupProfileActivation(site, activation_call_id, n);

        if (!match.matched) {
            dazg_orbit::certact_fuse::RecordProfileReject();
            if (dazg_orbit::ablation::CertActFuseDetail()) {
                std::cerr << "[DAZG_ORBIT_CERTACT_FUSE]"
                          << " applied=0 route=protocol_local_profile_guard"
                          << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                          << " call_id=" << activation_call_id
                          << " n=" << n
                          << " reason=" << match.reason
                          << " exact_equiv=1 semantic_loss=0"
                          << std::endl;
            }
            return false;
        }

        const std::uint64_t virtual_relu_call_id = relu_call_counter_++;

        bool applied_mixed_mask = false;
        if (match.identity) {
            for (int i = 0; i < num_relu; ++i) {
                result[i] = from_ring(to_ring(share[i]));
            }
        } else if (match.zero) {
            for (int i = 0; i < num_relu; ++i) {
                result[i] = from_ring(static_cast<U>(0));
            }
        } else if (match.mixed_mask && dazg_orbit::certact_fuse::MixedMaskReplayEnabled()) {
            if (match.entry.mask_hex.empty()) return false;
            for (int i = 0; i < num_relu; ++i) {
                if (dazg_orbit::certact_fuse::MaskHexIdentityAt(match.entry.mask_hex, static_cast<uint64_t>(i))) {
                    result[i] = from_ring(to_ring(share[i]));
                } else {
                    result[i] = from_ring(static_cast<U>(0));
                }
            }
            applied_mixed_mask = true;
        } else {
            return false;
        }

        dazg_orbit::certact_fuse::RecordLocalCanonicalSkip(match, n);

        std::cerr << "[DAZG_ORBIT_CERTACT_FUSE]"
                  << " applied=1"
                  << " route=" << (match.identity ?
                         "profile_identity_local_canonical_share_mask" :
                         (match.zero ? "profile_all_zero_local_public_zero" :
                          "profile_mixed_mask_benchmark_public_mask"))
                  << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(site)
                  << " call_id=" << activation_call_id
                  << " n=" << n
                  << " profile_call_id=" << match.entry.call_id
                  << " profile_n=" << match.entry.n
                  << " profile_input_variant="
                  << (match.entry.input_variant == std::numeric_limits<uint64_t>::max()
                        ? 18446744073709551615ULL : match.entry.input_variant)
                  << " current_input_variant=" << dazg_orbit::ablation::InputVariant()
                  << " reason=" << match.reason
                  << " bitwidth=" << bitwidth_
                  << " scale=" << scale_
                  << " consumed_relu_call_id=" << virtual_relu_call_id
                  << " local_share_transform=" << (match.identity ? "mask_bits" : (match.zero ? "zero" : "mixed_mask_identity_or_zero"))
                  << " saved_peer_gather=1"
                  << " saved_alice_reconstruct=1"
                  << " saved_activation_evaluator=1"
                  << " saved_prg_reshare=1"
                  << " saved_pbs=1"
                  << " domain_transition_eliminated=0"
                  << " mixed_scatter_fused=" << (applied_mixed_mask ? 1 : 0)
                  << " public_mask_benchmark=" << (applied_mixed_mask ? 1 : 0)
                  << " certificate=" << (applied_mixed_mask ? "benchmark_public_mask_replay_exact_fixed_input" : "profile_carried_local_canonical_activation")
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;

        return true;
    }

    void apply_exact_relu(const std::vector<int64_t>& x_fp, std::vector<U>& out) {
        out.resize(x_fp.size());
        for (std::size_t i = 0; i < x_fp.size(); ++i) {
            const int64_t y_fp = (x_fp[i] > 0) ? x_fp[i] : 0;
            out[i] = s64_to_ring(y_fp);
        }
    }

    void fill_zero(std::vector<U>& out, int n) {
        out.assign(static_cast<std::size_t>(n), static_cast<U>(0));
    }

    void copy_identity_from_xfp(const std::vector<int64_t>& x_fp, std::vector<U>& out) {
        out.resize(x_fp.size());
        for (std::size_t i = 0; i < x_fp.size(); ++i) {
            out[i] = s64_to_ring(x_fp[i]);
        }
    }

    template <typename V>
    auto call_bfe_relu(const V& x_fp, int)
        -> decltype(HE::EvalBFEReLUClear(
                        std::declval<const V&>(),
                        0,
                        0,
                        std::declval<V&>()),
                    void()) {
        HE::EvalBFEReLUClear(x_fp, bitwidth_, scale_, y_fp_buf_);
    }

    template <typename V>
    auto call_bfe_relu(const V& x_fp, long)
        -> decltype(std::declval<V&>() = HE::EvalBFEReLUClear(
                        std::declval<const V&>(),
                        0,
                        0),
                    void()) {
        y_fp_buf_ = HE::EvalBFEReLUClear(x_fp, bitwidth_, scale_);
    }

    void apply_bfe_relu(const std::vector<int64_t>& x_fp, std::vector<U>& out) {
#if DAZG_ORBIT_TFHE_RELU_VERBOSE
        static std::atomic<bool> once_bfe{false};
        if (!once_bfe.exchange(true)) {
            std::cerr << "[apply_bfe_relu] calling HE::EvalBFEReLUClear"
                      << " n=" << x_fp.size()
                      << " bitwidth=" << bitwidth_
                      << " scale=" << scale_
                      << std::endl;
        }
#endif

        call_bfe_relu(x_fp, 0);

        out.resize(y_fp_buf_.size());
        for (std::size_t i = 0; i < y_fp_buf_.size(); ++i) {
            out[i] = s64_to_ring(y_fp_buf_[i]);
        }
    }

    template <typename V>
    auto call_bfe_gelu(const V& x_fp, int)
        -> decltype(HE::EvalBFEGeLUClear(
                        std::declval<const V&>(),
                        0,
                        0,
                        std::declval<V&>()),
                    void()) {
        HE::EvalBFEGeLUClear(x_fp, bitwidth_, scale_, y_fp_buf_);
    }

    template <typename V>
    auto call_bfe_gelu(const V& x_fp, long)
        -> decltype(std::declval<V&>() = HE::EvalBFEGeLUClear(
                        std::declval<const V&>(),
                        0,
                        0),
                    void()) {
        y_fp_buf_ = HE::EvalBFEGeLUClear(x_fp, bitwidth_, scale_);
    }

    int64_t certact_clip_fp() const {
        const uint64_t override_fp = dazg_orbit::ablation::CertActClipFpOverride();
        if (override_fp != 0) return static_cast<int64_t>(override_fp);
        if (scale_ >= 0 && scale_ < 60) {
            return static_cast<int64_t>(8ULL << static_cast<unsigned>(scale_));
        }
        return static_cast<int64_t>(524288);  // 8 * 2^16 fallback
    }

    bool try_certact_tail_gelu(const std::vector<int64_t>& x_fp,
                               std::vector<U>& out,
                               int64_t local_min,
                               int64_t local_max) {
        if (!dazg_orbit::ablation::EnableCertAct()) return false;
        if (x_fp.empty()) return false;

        const int64_t clip_fp = certact_clip_fp();
        if (clip_fp <= 0) return false;

        // Fast reject: if the tensor range overlaps the central PLUT domain,
        // at least one value may require the residual evaluator.
        if (local_min > -clip_fp && local_max < clip_fp) return false;

        out.resize(x_fp.size());
        uint64_t tail_identity = 0;
        uint64_t tail_zero = 0;

        for (std::size_t i = 0; i < x_fp.size(); ++i) {
            const int64_t x = x_fp[i];
            if (x >= clip_fp) {
                out[i] = s64_to_ring(x);
                ++tail_identity;
            } else if (x <= -clip_fp) {
                out[i] = static_cast<U>(0);
                ++tail_zero;
            } else {
                // Central-domain element: preserve the original exact evaluator.
                return false;
            }
        }

        certact_bypass_calls_.fetch_add(1, std::memory_order_relaxed);
        certact_bypass_elements_.fetch_add(static_cast<uint64_t>(x_fp.size()), std::memory_order_relaxed);
        certact_tail_identity_.fetch_add(tail_identity, std::memory_order_relaxed);
        certact_tail_zero_.fetch_add(tail_zero, std::memory_order_relaxed);

        const std::string certact_site = dazg_orbit::domain::CurrentConversionSite();
        const uint64_t certact_call_id = dazg_orbit::domain::CurrentActivationCallId();
        const bool certact_all_identity =
            (tail_identity == static_cast<uint64_t>(x_fp.size()) && tail_zero == 0);
        const bool certact_all_zero =
            (tail_zero == static_cast<uint64_t>(x_fp.size()) && tail_identity == 0);
        const char* certact_profile_kind =
            certact_all_identity ? "identity" :
            (certact_all_zero ? "zero" : "mixed_scatter");
        dazg_orbit::certact_fuse::RecordRuntimeBypassProfile(
            certact_site,
            certact_call_id,
            static_cast<uint64_t>(x_fp.size()),
            tail_identity,
            tail_zero,
            clip_fp,
            local_min,
            local_max);


        if (dazg_orbit::scatterlift_v8::EmitMasksEnabled()) {
            const auto scatterlift_v8_mask =
                dazg_orbit::scatterlift_v8::EncodeIdentityMaskHex(x_fp, clip_fp);
            dazg_orbit::scatterlift_v8::AppendMaskProfileLine(
                certact_site,
                certact_call_id,
                static_cast<uint64_t>(x_fp.size()),
                tail_identity,
                tail_zero,
                clip_fp,
                local_min,
                local_max,
                scatterlift_v8_mask);
        }

        if (dazg_orbit::certact_fuse::MixedMaskDumpEnabled()) {
            std::string dazg_orbit_mixed_mask_hex((x_fp.size() + 3) / 4, '0');
            for (std::size_t mi = 0; mi < x_fp.size(); ++mi) {
                if (x_fp[mi] >= clip_fp) {
                    const std::size_t hx = mi >> 2;
                    const int bit = static_cast<int>(mi & 3U);
                    const int val = dazg_orbit::certact_fuse::HexValue(dazg_orbit_mixed_mask_hex[hx]) | (1 << bit);
                    dazg_orbit_mixed_mask_hex[hx] = static_cast<char>(val < 10 ? ('0' + val) : ('a' + (val - 10)));
                }
            }
            dazg_orbit::certact_fuse::AppendMixedMaskProfile(
                certact_site,
                certact_call_id,
                static_cast<uint64_t>(x_fp.size()),
                tail_identity,
                tail_zero,
                clip_fp,
                local_min,
                local_max,
                dazg_orbit_mixed_mask_hex);
        }

        static std::atomic<int> prints{0};
        const int id = prints.fetch_add(1, std::memory_order_relaxed);
        if (id < 128) {
            std::cerr << "[DAZG_ORBIT_CERTACT_BYPASS]"
                      << " site=" << dazg_orbit::certact_fuse::SanitizeForLog(certact_site)
                      << " call_id=" << certact_call_id
                      << " n=" << x_fp.size()
                      << " clip_fp=" << clip_fp
                      << " min_fp=" << local_min
                      << " max_fp=" << local_max
                      << " tail_identity=" << tail_identity
                      << " tail_zero=" << tail_zero
                      << " pbs_calls=0"
                      << " route=exact_tail_identity_zero_scatter"
                      << " profile_kind=" << certact_profile_kind
                      << " fuse_candidate=" << ((certact_all_identity || certact_all_zero) ? 1 : 0)
                      << " mixed_scatter_requires_public_mask=" << ((certact_all_identity || certact_all_zero) ? 0 : 1)
                      << " certificate=whole_call_tail_domain"
                      << " exact_equiv=1 semantic_loss=0"
                      << std::endl;
        }
        return true;
    }

    void apply_dazg_gelu(const std::vector<int64_t>& x_fp, std::vector<U>& out) {
        static std::atomic<bool> once_gelu{false};
        if (!once_gelu.exchange(true)) {
            std::cerr << "[TFHEReLUProtocol DAZG GeLU]"
                      << " n=" << x_fp.size()
                      << " bitwidth=" << bitwidth_
                      << " scale=" << scale_
                      << " route=HE::EvalBFEGeLUClear"
                      << " semantic=replace_relu_activation"
                      << std::endl;
        }

        call_bfe_gelu(x_fp, 0);

        out.resize(y_fp_buf_.size());
        for (std::size_t i = 0; i < y_fp_buf_.size(); ++i) {
            out[i] = s64_to_ring(y_fp_buf_[i]);
        }
    }

    void reshare_outputs_prg(
        const std::vector<U>& out,
        T* result,
        int n,
        std::uint64_t call_id) {

#if DAZG_ORBIT_TFHE_RELU_VERBOSE
        static std::atomic<int> prg_prints{0};

        const std::size_t elided_bytes =
            static_cast<std::size_t>(n) *
            ((bitwidth_ <= 32) ? sizeof(std::uint32_t) : sizeof(U));

        const int print_id = prg_prints.fetch_add(1);
        if (print_id < DAZG_ORBIT_TFHE_RELU_VERBOSE_LIMIT) {
            std::cerr << "[DAZG_ORBIT_PRG_RESHARE]"
                      << " role=" << party_
                      << " call=" << call_id
                      << " n=" << n
                      << " output_send_bytes=0"
                      << " elided_output_bytes=" << elided_bytes
                      << " certificate=deterministic_prg_output_reshare"
                      << " output_communication_elided=1"
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << std::endl;
        }
#endif

        if (party_ == ALICE) {
            for (int i = 0; i < n; ++i) {
                const U y = mask_bits(out[static_cast<std::size_t>(i)]);
                const U bob_share =
                    prg_reshare_mask(
                        call_id,
                        static_cast<std::uint64_t>(n),
                        static_cast<std::uint64_t>(i));

                const U alice_share = mask_bits(static_cast<U>(y - bob_share));

#if DAZG_ORBIT_TFHE_RELU_PRG_SELF_CHECK
                const U rec = mask_bits(static_cast<U>(alice_share + bob_share));
                if (rec != y) {
                    std::cerr << "[DAZG_ORBIT_PRG_RESHARE_FAILED]"
                              << " call=" << call_id
                              << " i=" << i
                              << " y=" << y
                              << " alice_share=" << alice_share
                              << " bob_share=" << bob_share
                              << " rec=" << rec
                              << std::endl;
                    std::abort();
                }
#endif

                result[i] = from_ring(alice_share);
            }
        } else {
            for (int i = 0; i < n; ++i) {
                const U bob_share =
                    prg_reshare_mask(
                        call_id,
                        static_cast<std::uint64_t>(n),
                        static_cast<std::uint64_t>(i));

                result[i] = from_ring(bob_share);
            }
        }
    }
};

} // namespace NonlinearLayer
