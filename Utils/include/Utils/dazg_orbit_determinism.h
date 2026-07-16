// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_determinism.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <type_traits>
#include <atomic>

namespace dazg_orbit_det {

inline bool EnvFlag(const char* name, bool default_value) {
    const char* v = std::getenv(name);
    if (!v) return default_value;
    return !(std::strcmp(v, "0") == 0 ||
             std::strcmp(v, "false") == 0 ||
             std::strcmp(v, "False") == 0 ||
             std::strcmp(v, "FALSE") == 0 ||
             std::strcmp(v, "off") == 0 ||
             std::strcmp(v, "OFF") == 0 ||
             std::strcmp(v, "no") == 0 ||
             std::strcmp(v, "NO") == 0);
}

inline uint64_t EnvU64(const char* name, uint64_t default_value) {
    const char* v = std::getenv(name);
    if (!v || !*v) return default_value;

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v) return default_value;

    return static_cast<uint64_t>(parsed);
}

inline uint64_t EnvU64Any(const char* primary,
                          const char* alias1,
                          const char* alias2,
                          uint64_t default_value) {
    const char* names[3] = {primary, alias1, alias2};
    for (const char* name : names) {
        if (name == nullptr) continue;
        const char* v = std::getenv(name);
        if (v != nullptr && *v != '\0') return EnvU64(name, default_value);
    }
    return default_value;
}

inline int EnvInt(const char* name, int default_value) {
    const char* v = std::getenv(name);
    if (!v || !*v) return default_value;
    return std::atoi(v);
}

inline uint64_t Fnv1a64(const char* s) {
    uint64_t h = 1469598103934665603ULL;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        h ^= static_cast<uint64_t>(*p++);
        h *= 1099511628211ULL;
    }
    return h;
}

inline bool Enabled() {
    return EnvFlag("DAZG_ORBIT_DETERMINISTIC", false);
}

inline uint64_t BaseSeed() {
    return EnvU64("DAZG_ORBIT_SEED", 20260501ULL);
}

inline int Party() {
    return EnvInt("DAZG_ORBIT_DET_PARTY", 0);
}

inline uint64_t InputVariantFromEnv() {
    return EnvU64Any("DAZG_ORBIT_INPUT_VARIANT",
                     "DAZG_ORBIT_INPUT_VARIANT",
                     "DAZG_ORBIT_INPUT_VARIANT",
                     0ULL);
}

inline std::string BuildDeterministicTagWithVariant(const std::string& base,
                                                    uint64_t variant) {
    if (variant == 0ULL) return base;
    return base + ".variant." + std::to_string(static_cast<unsigned long long>(variant));
}

inline std::string BuildDeterministicTagWithVariant(const char* base,
                                                    uint64_t variant) {
    return BuildDeterministicTagWithVariant(std::string(base == nullptr ? "" : base), variant);
}

inline std::string BuildDeterministicTag(const std::string& base) {
    return BuildDeterministicTagWithVariant(base, InputVariantFromEnv());
}

inline std::string BuildDeterministicTag(const char* base) {
    return BuildDeterministicTagWithVariant(base, InputVariantFromEnv());
}

inline uint64_t MixSeed(const char* stream_name, uint64_t extra = 0) {
    uint64_t s = BaseSeed();
    s ^= Fnv1a64(stream_name);
    s ^= static_cast<uint64_t>(Party()) * 0x9e3779b97f4a7c15ULL;
    s ^= extra * 0xbf58476d1ce4e5b9ULL;

    s ^= (s >> 30);
    s *= 0xbf58476d1ce4e5b9ULL;
    s ^= (s >> 27);
    s *= 0x94d049bb133111ebULL;
    s ^= (s >> 31);

    return s;
}


inline uint64_t SplitMix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline uint64_t HashU64Seq(std::initializer_list<uint64_t> xs) {
    uint64_t h = 0x6a09e667f3bcc909ULL;
    for (uint64_t x : xs) {
        h = SplitMix64(h ^ x);
    }
    return h;
}

inline uint64_t NextDeterministicOrdinal() {
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

template <typename TensorLike>
inline void FillTensorDeterministic(TensorLike& tensor,
                                    const char* stream_name,
                                    uint64_t modulus,
                                    uint64_t extra = 0,
                                    bool add_call_ordinal = true) {
    if (!Enabled()) {
        tensor.randomize(modulus);
        return;
    }

    auto& buf = tensor.data();
    if (modulus == 0) {
        for (std::size_t i = 0; i < buf.size(); ++i) {
            buf[i] = 0;
        }
        return;
    }

    const uint64_t ordinal = add_call_ordinal ? NextDeterministicOrdinal() : 0ULL;
    uint64_t state = MixSeed(stream_name,
                             extra ^ SplitMix64(ordinal + 0xd1b54a32d192ed03ULL));

    for (std::size_t i = 0; i < buf.size(); ++i) {
        state = SplitMix64(state ^ (static_cast<uint64_t>(i) * 0x9e3779b97f4a7c15ULL));
        uint64_t v = state;
        if (modulus != std::numeric_limits<uint64_t>::max()) {
            v %= modulus;
        }
        buf[i] = static_cast<typename std::decay<decltype(buf[i])>::type>(v);
    }
}


inline std::mt19937 MakeMt19937(const char* stream_name, uint64_t extra = 0) {
    if (!Enabled()) {
        std::random_device rd;
        return std::mt19937(rd());
    }

    return std::mt19937(static_cast<uint32_t>(MixSeed(stream_name, extra) & 0xffffffffULL));
}

inline std::mt19937_64 MakeMt19937_64(const char* stream_name, uint64_t extra = 0) {
    if (!Enabled()) {
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
        return std::mt19937_64(s);
    }

    return std::mt19937_64(MixSeed(stream_name, extra));
}

inline void SeedCStd() {
    if (!Enabled()) return;
    std::srand(static_cast<unsigned int>(MixSeed("c_std_rand") & 0xffffffffULL));
}

inline void LogConfig(int role) {
    std::cerr
        << "[DAZG_ORBIT_DETERMINISM]"
        << " role=" << role
        << " deterministic=" << (Enabled() ? 1 : 0)
        << " seed=" << BaseSeed()
        << " input_variant=" << InputVariantFromEnv()
        << " det_party=" << Party()
        << std::endl;
}

}  // namespace dazg_orbit_det
