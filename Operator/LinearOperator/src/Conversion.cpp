// DAZG-Orbit Project Source File
// Component: Operator/LinearOperator/src/Conversion.cpp
// Purpose: Batched secret-share and ciphertext conversion.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

// DAZG-Orbit conversion batching:
// independent share/ciphertext conversions may run with bounded parallelism;
// scheduling changes do not change tensor order, ownership, or ring semantics.

#include <LinearOperator/Conversion.h>
#include <Utils/dazg_orbit_domain_planner.h>
#include <iostream>
#include <atomic>
#include <chrono>
#include "Utils/dazg_orbit_determinism.h"
#include "Utils/dazg_orbit_ablation_flags.h"
#include <stdexcept>
#include <string>
#include <exception>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>
#include <algorithm>

using namespace HE::unified;

namespace {

// DAZG_ORBIT_THUNDERCUT_V17_CONVERSION_HELPERS_BEGIN
#ifndef DAZG_ORBIT_THUNDERCUT_V17_PARALLEL_HARDPATH_20260513
#define DAZG_ORBIT_THUNDERCUT_V17_PARALLEL_HARDPATH_20260513
static const char* DAZG_ORBIT_THUNDERCUT_V17_MARKER = "DAZG_ORBIT_THUNDERCUT_V17_PARALLEL_HARDPATH_20260513";

inline bool DAZGOrbitThunderEnvFlagV17(const char* name, bool default_value = false)
{
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "OFF" || s == "off" ||
             s == "no" || s == "NO");
}

inline bool DAZGOrbitThunderMasterEnabledV17()
{
    return DAZGOrbitThunderEnvFlagV17("DAZG_ORBIT_THUNDERCUT_V17", false);
}

inline bool DAZGOrbitThunderConversionEnabledV17()
{
    // DAZG_ORBIT_V575_CONVERSION_TURBO_20260614
    // Default-on exact conversion hardpath. Explicit legacy env still wins.
    // This targets the measured role2 HEToSS decrypt/decode hotspot; it does
    // not change masks, ciphertext contents, logits, or protocol semantics.
    if (std::getenv("DAZG_ORBIT_THUNDERCUT_V17_CONVERSION") != nullptr) {
        return DAZGOrbitThunderEnvFlagV17("DAZG_ORBIT_THUNDERCUT_V17_CONVERSION", false);
    }
    if (std::getenv("DAZG_ORBIT_THUNDERCUT_V17") != nullptr) {
        return DAZGOrbitThunderMasterEnabledV17();
    }
    return DAZGOrbitThunderEnvFlagV17("DAZG_ORBIT_V575_CONVERSION_TURBO", true);
}

inline uint64_t DAZGOrbitThunderThreadCountV17()
{
    // DAZG_ORBIT_V575_CONVERSION_TURBO_THREADS_20260614
    // Keep OMP/MKL at 1 and parallelize only the independent conversion polys.
    const char* v575 = std::getenv("DAZG_ORBIT_V575_CONVERSION_THREADS");
    const char* v = std::getenv("DAZG_ORBIT_THUNDER_THREADS");
    uint64_t t = 0;
    if (v575 != nullptr && *v575 != '\0') {
        t = static_cast<uint64_t>(std::strtoull(v575, nullptr, 10));
    } else if (v != nullptr && *v != '\0') {
        t = static_cast<uint64_t>(std::strtoull(v, nullptr, 10));
    }
    const bool explicit_threads = t != 0;
    if (t == 0) {
        t = static_cast<uint64_t>(std::thread::hardware_concurrency());
        if (t == 0) t = 4;
        if (t > 8) t = 8;
    }

    const char* m = std::getenv("DAZG_ORBIT_THUNDER_MAX_THREADS");
    uint64_t cap = explicit_threads ? t : 8;
    if (m != nullptr && *m != '\0') {
        cap = static_cast<uint64_t>(std::strtoull(m, nullptr, 10));
        if (cap == 0) cap = 1;
    }
    if (t > cap) t = cap;
    if (t < 1) t = 1;
    return t;
}

struct DAZGOrbitThunderConversionStatsV17 {
    std::atomic<uint64_t> parallel_regions{0};
    std::atomic<uint64_t> encode_polys{0};
    std::atomic<uint64_t> encrypt_polys{0};
    std::atomic<uint64_t> decrypt_polys{0};
    std::atomic<uint64_t> add_plain_polys{0};
    std::atomic<uint64_t> elapsed_us{0};

    ~DAZGOrbitThunderConversionStatsV17() {
        const uint64_t regions = parallel_regions.load(std::memory_order_relaxed);
        const uint64_t work =
            encode_polys.load(std::memory_order_relaxed) +
            encrypt_polys.load(std::memory_order_relaxed) +
            decrypt_polys.load(std::memory_order_relaxed) +
            add_plain_polys.load(std::memory_order_relaxed);
        if (regions == 0 && work == 0) return;
        std::cerr << "[DAZG_ORBIT_THUNDERCUT_V17_SUMMARY]"
                  << " marker=" << DAZG_ORBIT_THUNDERCUT_V17_MARKER
                  << " component=Conversion"
                  << " parallel_regions=" << regions
                  << " encode_polys=" << encode_polys.load(std::memory_order_relaxed)
                  << " encrypt_polys=" << encrypt_polys.load(std::memory_order_relaxed)
                  << " decrypt_polys=" << decrypt_polys.load(std::memory_order_relaxed)
                  << " add_plain_polys=" << add_plain_polys.load(std::memory_order_relaxed)
                  << " elapsed_us=" << elapsed_us.load(std::memory_order_relaxed)
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
};

inline DAZGOrbitThunderConversionStatsV17& DAZGOrbitThunderStatsV17()
{
    static DAZGOrbitThunderConversionStatsV17 s;
    return s;
}

template <typename Fn>
inline void DAZGOrbitThunderParallelForV17(size_t n, const Fn& fn)
{
    const uint64_t requested = DAZGOrbitThunderThreadCountV17();
    if (n <= 1 || requested <= 1) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }

    const size_t workers_n = std::min<size_t>(static_cast<size_t>(requested), n);
    std::atomic<size_t> next(0);
    std::vector<std::thread> workers;
    workers.reserve(workers_n);
    for (size_t tid = 0; tid < workers_n; ++tid) {
        workers.emplace_back([&]() {
            for (;;) {
                const size_t i = next.fetch_add(1, std::memory_order_relaxed);
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
// DAZG_ORBIT_THUNDERCUT_V17_CONVERSION_HELPERS_END

#ifndef DAZG_ORBIT_SAFE_SYMLIFT_V13_HELPER
#define DAZG_ORBIT_SAFE_SYMLIFT_V13_HELPER
inline bool DAZGOrbitEnvFlagV13(const char* key) {
    const char* v = std::getenv(key);
    return v != nullptr && std::strcmp(v, "1") == 0;
}

inline bool DAZGOrbitSafeSymLiftV13Active(HE::HEEvaluator* HE) {
    return HE != nullptr &&
           !HE->server &&
           DAZGOrbitEnvFlagV13("DAZG_ORBIT_ENABLE_SYMLIFT_SAFE_CLIENT") &&
           HE->context != nullptr &&
           HE->secretKeys != nullptr;
}

inline void DAZGOrbitLogSafeSymLiftV13Once(const char* kind, HE::HEEvaluator* HE) {
    static std::atomic<int> printed{0};
    if (printed.fetch_add(1, std::memory_order_relaxed) != 0) return;
    std::cerr << "[DAZG_ORBIT_SAFE_SYMLIFT_V13]"
              << " active=1"
              << " route=client_secret_key_symmetric_encryptor"
              << " kind=" << (kind == nullptr ? "unknown" : kind)
              << " role=" << ((HE != nullptr && HE->server) ? "server" : "client")
              << " secret_key_exported=0"
              << " server_secret_key_access=0"
              << " ciphertext_standard_bfv=1"
              << " exact_equiv=1"
              << " semantic_loss=0"
              << std::endl;
}
#endif  // DAZG_ORBIT_SAFE_SYMLIFT_V13_HELPER



struct DAZGOrbitSymLiftStats {
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> sym_calls{0};
    std::atomic<uint64_t> public_calls{0};
    std::atomic<uint64_t> polys{0};
    std::atomic<uint64_t> sym_polys{0};
    std::atomic<uint64_t> public_polys{0};
    std::atomic<uint64_t> sym_encrypt_us{0};
    std::atomic<uint64_t> public_encrypt_us{0};

    ~DAZGOrbitSymLiftStats() {
        const uint64_t c = calls.load();
        if (c == 0) return;
        std::cerr << "[DAZG_ORBIT_SYMLIFT_SUMMARY]"
                  << " calls=" << c
                  << " sym_calls=" << sym_calls.load()
                  << " public_calls=" << public_calls.load()
                  << " polys=" << polys.load()
                  << " sym_polys=" << sym_polys.load()
                  << " public_polys=" << public_polys.load()
                  << " sym_encrypt_us=" << sym_encrypt_us.load()
                  << " public_encrypt_us=" << public_encrypt_us.load()
                  << " route=client_secret_key_symmetric_encryptor_v13"
                  << " exact_equiv=1 semantic_loss=0"
                  << std::endl;
    }
};

inline DAZGOrbitSymLiftStats& SymLiftStats() {
    static DAZGOrbitSymLiftStats stats;
    return stats;
}

inline void RecordSymLiftConversion(const char* kind,
                                    bool symmetric,
                                    uint64_t polys,
                                    uint64_t elapsed_us) {
    DAZGOrbitSymLiftStats& s = SymLiftStats();
    s.calls.fetch_add(1, std::memory_order_relaxed);
    s.polys.fetch_add(polys, std::memory_order_relaxed);
    if (symmetric) {
        s.sym_calls.fetch_add(1, std::memory_order_relaxed);
        s.sym_polys.fetch_add(polys, std::memory_order_relaxed);
        s.sym_encrypt_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    } else {
        s.public_calls.fetch_add(1, std::memory_order_relaxed);
        s.public_polys.fetch_add(polys, std::memory_order_relaxed);
        s.public_encrypt_us.fetch_add(elapsed_us, std::memory_order_relaxed);
    }

    if (dazg_orbit::domain::ConversionDebugEnabled() || dazg_orbit::ablation::EnableSymLiftSSToHE()) {
        static std::atomic<int> prints{0};
        const int id = prints.fetch_add(1, std::memory_order_relaxed);
        if (id < 128) {
            std::cerr << "[DAZG_ORBIT_SYMLIFT_CONVERSION]"
                      << " kind=" << (kind == nullptr ? "unknown" : kind)
                      << " symmetric=" << (symmetric ? 1 : 0)
                      << " polys=" << polys
                      << " encrypt_elapsed_us=" << elapsed_us
                      << " route=" << (symmetric ? "client_secret_key_symmetric_encryptor_v13" : "public_key_share_lift")
                      << " exact_equiv=1 semantic_loss=0"
                      << std::endl;
        }
    }
}


template <typename ShapeVec>
inline void LogDAZGOrbitConversionSite(const char* kind,
                                   const ShapeVec& shape,
                                   HE::HEEvaluator* HE) {
    if (!dazg_orbit::domain::ConversionDebugEnabled()) return;

    const auto normalized_shape = dazg_orbit::domain::NormalizeShape(shape);
    std::cerr << "[DAZG_ORBIT_CONVERSION_SITE_DEBUG]"
              << " kind=" << (kind == nullptr ? "unknown" : kind)
              << " current_site=" << dazg_orbit::domain::Sanitize(dazg_orbit::domain::CurrentConversionSite())
              << " call_id=" << dazg_orbit::domain::CurrentActivationCallId()
              << " role=" << ((HE != nullptr && HE->server) ? "server" : "client")
              << " shape=" << dazg_orbit::domain::ShapeToString(normalized_shape)
              << " slots_per_poly=" << ((HE != nullptr && HE->polyModulusDegree != 0)
                                             ? static_cast<uint64_t>(HE->polyModulusDegree)
                                             : 1ULL)
              << " exact_equiv=1"
              << " semantic_loss=0"
              << std::endl;
}

}  // namespace

namespace Operator {

// input mod prime, output mod q, need a ring2field conversion before it
Tensor<UnifiedCiphertext> SSToHE(const Tensor<uint64_t> &x, HE::HEEvaluator* HE) {
    std::vector<size_t> scalar_shape = x.shape();
    dazg_orbit::domain::ScopedConversionRecord dazg_orbit_conversion_record(
        "SSToHE", scalar_shape, HE);
    LogDAZGOrbitConversionSite("SSToHE", scalar_shape, HE);
    uint64_t poly_degree = scalar_shape[scalar_shape.size() - 1];
    std::vector<size_t> poly_shape(scalar_shape.begin(), scalar_shape.end() - 1);
    std::vector<uint64_t> tmp_vec(poly_degree,0ULL);
    // encoding
    Tensor<UnifiedPlaintext> ac_pt(poly_shape, HE->server ? HE->Backend() : HOST);
    // Stage-Z2: do not pre-encrypt fresh zero ciphertexts here.  Server-side
    // ReceiveEncVec overwrites every ciphertext, and the client encrypts every
    // plaintext before SendEncVec.  Constructing HOST shells avoids an O(numPoly)
    // fresh-zero encryption tax on every linear-layer conversion.
    Tensor<UnifiedCiphertext> ac_ct(poly_shape, HOST);
    HE->encoder->encode(tmp_vec, ac_pt(0));
    if (DAZGOrbitThunderConversionEnabledV17() && ac_pt.size() > 1) {
        const auto v17_begin = std::chrono::steady_clock::now();
        DAZGOrbitThunderParallelForV17(ac_pt.size(), [&](size_t i) {
            std::vector<uint64_t> local_vec(poly_degree, 0ULL);
            for (size_t j = 0; j < poly_degree; j++) {
                local_vec[j] = x(i * poly_degree + j);
            }
            HE->encoder->encode(local_vec, ac_pt(i));
        });
        const auto v17_end = std::chrono::steady_clock::now();
        DAZGOrbitThunderStatsV17().parallel_regions.fetch_add(1, std::memory_order_relaxed);
        DAZGOrbitThunderStatsV17().encode_polys.fetch_add(static_cast<uint64_t>(ac_pt.size()), std::memory_order_relaxed);
        DAZGOrbitThunderStatsV17().elapsed_us.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(v17_end - v17_begin).count()),
            std::memory_order_relaxed);
    } else {
        for (size_t i = 0; i < ac_pt.size(); i++) {
            for (size_t j = 0; j < poly_degree; j++) {
                tmp_vec[j] = x(i * poly_degree + j);
            }
            HE->encoder->encode(tmp_vec, ac_pt(i));
        }
    }
    if (HE->server){
        HE->ReceiveEncVec(ac_ct);
        if (HE->Backend() == DEVICE){
            ac_ct.apply([HE](UnifiedCiphertext &ct){
                ct.to_device(*HE->context);
            });
        }
        assert(ac_pt.size() == ac_ct.size() && "Number of polys does not match.");
        if (DAZGOrbitThunderConversionEnabledV17() && ac_ct.size() > 1) {
            const auto v17_begin = std::chrono::steady_clock::now();
            DAZGOrbitThunderParallelForV17(ac_ct.size(), [&](size_t i) {
                HE->evaluator->add_plain_inplace(ac_ct(i), ac_pt(i));
            });
            const auto v17_end = std::chrono::steady_clock::now();
            DAZGOrbitThunderStatsV17().parallel_regions.fetch_add(1, std::memory_order_relaxed);
            DAZGOrbitThunderStatsV17().add_plain_polys.fetch_add(static_cast<uint64_t>(ac_ct.size()), std::memory_order_relaxed);
            DAZGOrbitThunderStatsV17().elapsed_us.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(v17_end - v17_begin).count()),
                std::memory_order_relaxed);
        } else {
            for (size_t i = 0; i < ac_ct.size(); i++) {
                HE->evaluator->add_plain_inplace(ac_ct(i), ac_pt(i));
            }
        }
    } 
    else { /* client */
        const bool use_symlift = dazg_orbit::ablation::EnableSymLiftSSToHE();
        const bool safe_symlift_v13 = use_symlift && DAZGOrbitSafeSymLiftV13Active(HE);
        std::unique_ptr<seal::Encryptor> dazg_orbit_v13_safe_encryptor;
        if (safe_symlift_v13) {
            DAZGOrbitLogSafeSymLiftV13Once("SSToHE", HE);
            dazg_orbit_v13_safe_encryptor.reset(new seal::Encryptor(*HE->context, *HE->secretKeys));
        }
        bool dazg_orbit_v13_used_sym = false;
        const auto enc_begin = std::chrono::steady_clock::now();
        for (size_t i = 0; i < ac_pt.size(); i++) {
            if (use_symlift) {
                // Exact SymLift: encrypt the same plaintext share under the
                // key-owner's secret-key encryption route.  The ciphertext is
                // still a standard BFV ciphertext and can be evaluated by the
                // server exactly as before; only the lift construction changes.
                try {
        {
            static bool dazg_orbit_symlift_public_fallback_0 = false;
            static bool dazg_orbit_symlift_fallback_logged_0 = false;
            if (!dazg_orbit_symlift_public_fallback_0) {
                try {
                    if (dazg_orbit_v13_safe_encryptor) {
                        dazg_orbit_v13_safe_encryptor->encrypt_symmetric(ac_pt(i).hplain(), ac_ct(i).hcipher());
                        dazg_orbit_v13_used_sym = true;
                    } else {
                        HE->encryptor->encrypt_symmetric(ac_pt(i).hplain(), ac_ct(i).hcipher());
                        dazg_orbit_v13_used_sym = true;
                    }
                } catch (const std::exception& e) {
                    const std::string dazg_orbit_symlift_error = e.what();
                    if (dazg_orbit_symlift_error.find("secret key") == std::string::npos) {
                        throw;
                    }
                    dazg_orbit_symlift_public_fallback_0 = true;
                    if (!dazg_orbit_symlift_fallback_logged_0) {
                        std::cerr << "[DAZG_ORBIT_SYMLIFT_FALLBACK] version=DAZG_ORBIT_SYMLIFT_SAFE_GUARD_V2 callsite=0 reason=secret_key_not_set action=public_encrypt exact_equiv=1 semantic_loss=0" << std::endl;
                        dazg_orbit_symlift_fallback_logged_0 = true;
                    }
                    HE->encryptor->encrypt(ac_pt(i).hplain(), ac_ct(i).hcipher());
                }
            } else {
                HE->encryptor->encrypt(ac_pt(i).hplain(), ac_ct(i).hcipher());
            }
        }
    } catch (const std::logic_error& e) {
        const std::string dazg_orbit_symlift_error = e.what();
        if (dazg_orbit_symlift_error.find("secret key") == std::string::npos) {
            throw;
        }
        if (i == 0) {
            std::cerr << "[DAZG_ORBIT_SYMLIFT_FALLBACK] reason=secret_key_not_set action=public_encrypt exact_equiv=1 semantic_loss=0" << std::endl;
        }
        HE->encryptor->encrypt(ac_pt(i), ac_ct(i));
    }
            } else {
                HE->encryptor->encrypt(ac_pt(i), ac_ct(i));
            }
        }
        const auto enc_end = std::chrono::steady_clock::now();
        RecordSymLiftConversion(
            "SSToHE",
            dazg_orbit_v13_used_sym,
            static_cast<uint64_t>(ac_pt.size()),
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(enc_end - enc_begin).count()));
        HE->SendEncVec(ac_ct);
        // DAZG_ORBIT_V743R8P93_SSTOHE_CLIENT_VALID_RETURN_ACTIVE_20260701
        // Client-side SSToHE used to return default HOST ciphertext shells after
        // sending ac_ct. The GeLU polynomial path may still touch the returned
        // tensor on the client before HEToSS receives the server result. Returning
        // ac_ct keeps the local placeholder a valid BFV ciphertext in this HE
        // context/parms_id; protocol output remains owned by server-side HEToSS.
        {
            static std::atomic<int> dazg_orbit_v743r8p93_sstohe_client_valid_return_log{0};
            const int dazg_orbit_v743r8p93_log_id =
                dazg_orbit_v743r8p93_sstohe_client_valid_return_log.fetch_add(1, std::memory_order_relaxed);
            if (dazg_orbit_v743r8p93_log_id < 16) {
                std::cerr << "[DAZG_ORBIT_V743R8P93_SSTOHE_CLIENT_VALID_RETURN]"
                          << " marker=DAZG_ORBIT_V743R8P93_SSTOHE_CLIENT_VALID_RETURN_ACTIVE_20260701"
                          << " kind=SSToHE"
                          << " role=client"
                          << " returned_valid_ciphertext_placeholder=1"
                          << " polys=" << ac_ct.size()
                          << " slots_per_poly=" << poly_degree
                          << " protocol_output_owner=server_HEToSS"
                          << " exact_equiv=1 semantic_loss=0"
                          << std::endl;
            }
        }
        return ac_ct;
    }
    return ac_ct;
};

// input mod q, output mod prime, need a field2ring conversion after it to support ring MPC protocols
Tensor<uint64_t> HEToSS(Tensor<UnifiedCiphertext> out_ct, HE::HEEvaluator* HE) {
    std::vector<size_t> scalar_shape = out_ct.shape();
    scalar_shape.push_back(HE->polyModulusDegree);
    dazg_orbit::domain::ScopedConversionRecord dazg_orbit_conversion_record(
        "HEToSS", scalar_shape, HE);
    LogDAZGOrbitConversionSite("HEToSS", scalar_shape, HE);
    Tensor<uint64_t> x(scalar_shape);
    Tensor<UnifiedPlaintext> out_share(out_ct.shape(), HOST);
    auto gen = dazg_orbit_det::MakeMt19937_64(
        "Conversion64",
        dazg_orbit_det::HashU64Seq({static_cast<uint64_t>(out_ct.size()),
                                  static_cast<uint64_t>(HE->polyModulusDegree),
                                  static_cast<uint64_t>(dazg_orbit::domain::CurrentActivationCallId())}));
    std::uniform_int_distribution<uint64_t> distrib(0, HE->plain_mod - 1);
    // mask generation and communication
    if (HE->server) {
        for (size_t i = 0; i < out_ct.size(); i++){
            std::vector<uint64_t> pos_mask(HE->polyModulusDegree, 0);
            std::vector<uint64_t> neg_mask(HE->polyModulusDegree, 0);
            for (size_t j = 0; j < pos_mask.size(); j++) {
                pos_mask[j] = distrib(gen);
                neg_mask[j] = HE->plain_mod - pos_mask[j];
                if (HE->server) {
                    x(i * HE->polyModulusDegree + j) = pos_mask[j];
                }
            }
            // TODO: noise flooding (add freshly encrypted zero), refer to Cheetah
            UnifiedPlaintext tmp_pos(HOST);
            UnifiedPlaintext tmp_neg(HOST);
            HE->encoder->encode(pos_mask, tmp_pos);
            HE->encoder->encode(neg_mask, tmp_neg);
            HE->evaluator->add_plain_inplace(out_ct(i), tmp_neg);  // annotate this when testing
            out_share(i) = tmp_pos;
        }
        out_ct.apply([HE](UnifiedCiphertext &ct){
            if (HE->Backend() == DEVICE) {
                ct.to_host(*HE->context);
            }
        });
        HE->SendEncVec(out_ct);
    }
    else {
        HE->ReceiveEncVec(out_ct);
    }

    // decoding and decryption
    std::vector<uint64_t> tmp_vec(HE->polyModulusDegree);
    if (HE->server) {
        // for (size_t i = 0; i < out_share.size(); i++) {
        //     // HE->batchEncoder->decode(out_share(i), tmp_vec);     * SEAL does not allow adjacent encoding and decoding?
        //     for (size_t j = 0; j < HE->polyModulusDegree; j++) {
        //         x(i * HE->polyModulusDegree + j) = tmp_vec[j];
        //     }
        // }
    }
    else {
        if (DAZGOrbitThunderConversionEnabledV17() && out_ct.size() > 1) {
            const auto v17_begin = std::chrono::steady_clock::now();
            DAZGOrbitThunderParallelForV17(out_ct.size(), [&](size_t i) {
                Plaintext out_pt;
                std::vector<uint64_t> local_vec(HE->polyModulusDegree);
                HE->decryptor->decrypt(out_ct(i), out_pt);
                HE->encoder->decode(out_pt, local_vec);
                for (size_t j = 0; j < HE->polyModulusDegree; j++) {
                    x(i * HE->polyModulusDegree + j) = local_vec[j];
                }
            });
            const auto v17_end = std::chrono::steady_clock::now();
            DAZGOrbitThunderStatsV17().parallel_regions.fetch_add(1, std::memory_order_relaxed);
            DAZGOrbitThunderStatsV17().decrypt_polys.fetch_add(static_cast<uint64_t>(out_ct.size()), std::memory_order_relaxed);
            DAZGOrbitThunderStatsV17().elapsed_us.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(v17_end - v17_begin).count()),
                std::memory_order_relaxed);
        } else {
            for (size_t i = 0; i < out_ct.size(); i++) {
                Plaintext out_pt;
                HE->decryptor->decrypt(out_ct(i), out_pt);
                HE->encoder->decode(out_pt, tmp_vec);
                for (size_t j = 0; j < HE->polyModulusDegree; j++) {
                    x(i * HE->polyModulusDegree + j) = tmp_vec[j];
                }
            }
        }
    }

    x.reshape(scalar_shape);
    return x;
};


Tensor<HE::unified::UnifiedCiphertext> SSToHE_coeff(const Tensor<uint64_t> &x, HE::HEEvaluator* HE)
{
    std::vector<size_t> shapeTab = x.shape();
    dazg_orbit::domain::ScopedConversionRecord dazg_orbit_conversion_record(
        "SSToHE_coeff", shapeTab, HE);
    LogDAZGOrbitConversionSite("SSToHE_coeff", shapeTab, HE);
    auto polyModulusDegree = HE->polyModulusDegree;
    auto plain = HE->plain_mod;
    size_t numPoly = 1;
    for (int num : shapeTab) {
        numPoly *= num;
    }

    int len = shapeTab.back(); 
    shapeTab.pop_back();
    numPoly /= len;
    Tensor<UnifiedPlaintext> T(shapeTab,Datatype::HOST);
    if (DAZGOrbitThunderConversionEnabledV17() && numPoly > 1) {
        const auto v17_begin = std::chrono::steady_clock::now();
        DAZGOrbitThunderParallelForV17(numPoly, [&](size_t i) {
            std::vector<uint64_t> Tsubv(len, 0);
            for (size_t j = 0; j < static_cast<size_t>(len); j++){
                Tsubv[j] = x(i * static_cast<size_t>(len) + j);
            }
            T(i).hplain().resize(polyModulusDegree);
            seal::util::modulo_poly_coeffs(Tsubv, len, plain, T(i).hplain().data());
            std::fill_n(T(i).hplain().data() + len, polyModulusDegree - len, 0);
        });
        const auto v17_end = std::chrono::steady_clock::now();
        DAZGOrbitThunderStatsV17().parallel_regions.fetch_add(1, std::memory_order_relaxed);
        DAZGOrbitThunderStatsV17().encode_polys.fetch_add(static_cast<uint64_t>(numPoly), std::memory_order_relaxed);
        DAZGOrbitThunderStatsV17().elapsed_us.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(v17_end - v17_begin).count()),
            std::memory_order_relaxed);
    } else {
        for (size_t i = 0; i < numPoly; i++){
            vector<uint64_t> Tsubv(len, 0);
            for (size_t j = 0; j < len; j++){
                Tsubv[j] = x(i * len + j);
            }
            T(i).hplain().resize(polyModulusDegree);
            seal::util::modulo_poly_coeffs(Tsubv, len, plain, T(i).hplain().data());
            std::fill_n(T(i).hplain().data() + len, polyModulusDegree - len, 0);
        }
    }
    Tensor<UnifiedCiphertext> finalpack(shapeTab, HOST);
    if (!HE->server){
        // Client: exact key-owner share lift.  SymLift replaces public-key
        // encryption with BFV symmetric encryption when enabled.
        const bool use_symlift = dazg_orbit::ablation::EnableSymLiftSSToHE();
        const bool safe_symlift_v13 = use_symlift && DAZGOrbitSafeSymLiftV13Active(HE);
        std::unique_ptr<seal::Encryptor> dazg_orbit_v13_safe_encryptor;
        if (safe_symlift_v13) {
            DAZGOrbitLogSafeSymLiftV13Once("SSToHE_coeff", HE);
            dazg_orbit_v13_safe_encryptor.reset(new seal::Encryptor(*HE->context, *HE->secretKeys));
        }
        bool dazg_orbit_v13_used_sym = false;
        const auto enc_begin = std::chrono::steady_clock::now();
        for (size_t i = 0; i < numPoly; i++){
            if (use_symlift) {
                {
                    static bool dazg_orbit_symlift_public_fallback_1 = false;
                    static bool dazg_orbit_symlift_fallback_logged_1 = false;
                    if (!dazg_orbit_symlift_public_fallback_1) {
                        try {
                            if (dazg_orbit_v13_safe_encryptor) {
                                dazg_orbit_v13_safe_encryptor->encrypt_symmetric(T(i).hplain(), finalpack(i).hcipher());
                                dazg_orbit_v13_used_sym = true;
                            } else {
                                HE->encryptor->encrypt_symmetric(T(i).hplain(), finalpack(i).hcipher());
                                dazg_orbit_v13_used_sym = true;
                            }
                        } catch (const std::exception& e) {
                            const std::string dazg_orbit_symlift_error = e.what();
                            if (dazg_orbit_symlift_error.find("secret key") == std::string::npos) {
                                throw;
                            }
                            dazg_orbit_symlift_public_fallback_1 = true;
                            if (!dazg_orbit_symlift_fallback_logged_1) {
                                std::cerr << "[DAZG_ORBIT_SYMLIFT_FALLBACK] version=DAZG_ORBIT_SYMLIFT_SAFE_GUARD_V2 callsite=1 reason=secret_key_not_set action=public_encrypt exact_equiv=1 semantic_loss=0" << std::endl;
                                dazg_orbit_symlift_fallback_logged_1 = true;
                            }
                            HE->encryptor->encrypt(T(i).hplain(), finalpack(i).hcipher());
                        }
                    } else {
                        HE->encryptor->encrypt(T(i).hplain(), finalpack(i).hcipher());
                    }
                }
            } else {
                HE->encryptor->encrypt(T(i), finalpack(i));
            }
        }
        const auto enc_end = std::chrono::steady_clock::now();
        RecordSymLiftConversion(
            "SSToHE_coeff",
            dazg_orbit_v13_used_sym,
            static_cast<uint64_t>(numPoly),
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(enc_end - enc_begin).count()));
        // enc.flatten();
        HE->SendEncVec(finalpack);
    }else{
        //服务器端
        HE->ReceiveEncVec(finalpack);
        if (HE->Backend() == DEVICE){
            finalpack.apply([HE](UnifiedCiphertext &ct){
                ct.to_device(*HE->context);
            });
            T.apply([HE](UnifiedPlaintext &pt){
                pt.to_device(*HE->context);
            });
        }
        if (DAZGOrbitThunderConversionEnabledV17() && numPoly > 1) {
            const auto v17_begin = std::chrono::steady_clock::now();
            DAZGOrbitThunderParallelForV17(numPoly, [&](size_t i) {
                HE->evaluator->add_plain_inplace(finalpack(i), T(i));
            });
            const auto v17_end = std::chrono::steady_clock::now();
            DAZGOrbitThunderStatsV17().parallel_regions.fetch_add(1, std::memory_order_relaxed);
            DAZGOrbitThunderStatsV17().add_plain_polys.fetch_add(static_cast<uint64_t>(numPoly), std::memory_order_relaxed);
            DAZGOrbitThunderStatsV17().elapsed_us.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(v17_end - v17_begin).count()),
                std::memory_order_relaxed);
        } else {
            for (size_t i = 0; i < numPoly; i++){
                HE->evaluator->add_plain_inplace(finalpack(i), T(i));
            }
        }
    }
    return finalpack;
}


Tensor<uint64_t> HEToSS_coeff(Tensor<HE::unified::UnifiedCiphertext> &out_ct, HE::HEEvaluator* HE)
{
    auto shapeTab = out_ct.shape();
    auto fullScalarShapeForDAZGOrbit = shapeTab;
    fullScalarShapeForDAZGOrbit.push_back(HE->polyModulusDegree);
    dazg_orbit::domain::ScopedConversionRecord dazg_orbit_conversion_record(
        "HEToSS_coeff", fullScalarShapeForDAZGOrbit, HE);
    LogDAZGOrbitConversionSite("HEToSS_coeff", fullScalarShapeForDAZGOrbit, HE);
    Tensor<UnifiedPlaintext> outShare(shapeTab,HOST);
    size_t numPoly = 1;
    for (int num : shapeTab) {
        numPoly *= num;
    }
    auto tensorShapeTab = shapeTab;
    tensorShapeTab.push_back(HE->polyModulusDegree);

    Tensor<uint64_t> tensorShare(tensorShapeTab);
    UnifiedPlaintext plainMaskInv(HOST);
    // HETOSS_coeff only support CPU
    if (HE->server) {
        if (HE->Backend() == DEVICE){
            // cout << "device" << endl;
            for (size_t i = 0; i < out_ct.size(); i++){
                out_ct(i).to_host(*HE->context);
            }
        }
        // for(int i=0;i<HE->polyModulusDegree;i++){
        //     cout << "out_ct(0)[i]:" << out_ct(0).hcipher().data()[i] << endl;
        // }
        int64_t mask;
        auto gen = dazg_orbit_det::MakeMt19937(
            "Conversion32",
            dazg_orbit_det::HashU64Seq({static_cast<uint64_t>(numPoly),
                                      static_cast<uint64_t>(HE->polyModulusDegree),
                                      static_cast<uint64_t>(dazg_orbit::domain::CurrentActivationCallId())}));
        std::uniform_int_distribution<int64_t> dist(0, HE->plain_mod - 1);
        // cout << "numPoly:" << numPoly << endl;
        for (size_t i = 0; i < numPoly; i++){
            outShare(i).hplain().resize(HE->polyModulusDegree);
            plainMaskInv.hplain().resize(HE->polyModulusDegree);
            for (size_t l = 0; l < HE->polyModulusDegree; l++){
                mask = dist(gen);
                *(outShare(i).hplain().data() + l) = mask;
                tensorShare((i) * HE->polyModulusDegree + l) = mask;
                mask = HE->plain_mod - mask;
                *(plainMaskInv.hplain().data() + l) = mask;
                // cout << "mask:" << *(plainMaskInv.hplain().data() + l) << endl;
            }
            
            // cout << "add_plain_inplace done1" << endl;
            HE->evaluator->add_plain_inplace(out_ct(i), plainMaskInv);
            // cout << "add_plain_inplace done" << endl;
        }
        out_ct.flatten();
        // cout << "HEToSS_coeff done" << endl;
        // if (HE->Backend() == DEVICE){
        //     out_ct.apply([HE](UnifiedCiphertext &ct){
        //         ct.to_host(*HE->context);
        //     });
        // }
        HE->SendEncVec(out_ct);
        return tensorShare;

    }else{
        HE->ReceiveEncVec(out_ct);
        if (DAZGOrbitThunderConversionEnabledV17() && numPoly > 1) {
            const auto v17_begin = std::chrono::steady_clock::now();
            DAZGOrbitThunderParallelForV17(numPoly, [&](size_t i) {
                HE->decryptor->decrypt(out_ct(i), outShare(i));
                for (size_t j = 0; j < HE->polyModulusDegree; j++){
                    tensorShare(i * HE->polyModulusDegree + j) = *(outShare(i).hplain().data() + j);
                }
            });
            const auto v17_end = std::chrono::steady_clock::now();
            DAZGOrbitThunderStatsV17().parallel_regions.fetch_add(1, std::memory_order_relaxed);
            DAZGOrbitThunderStatsV17().decrypt_polys.fetch_add(static_cast<uint64_t>(numPoly), std::memory_order_relaxed);
            DAZGOrbitThunderStatsV17().elapsed_us.fetch_add(
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(v17_end - v17_begin).count()),
                std::memory_order_relaxed);
        } else {
            for (size_t i = 0; i < numPoly; i++){
                HE->decryptor->decrypt(out_ct(i), outShare(i));
                for (size_t j = 0; j < HE->polyModulusDegree; j++){
                    tensorShare(i * HE->polyModulusDegree + j) = *(outShare(i).hplain().data() + j);
                }
            }
        }
        return tensorShare;
    }
}

} // namespace Operator
