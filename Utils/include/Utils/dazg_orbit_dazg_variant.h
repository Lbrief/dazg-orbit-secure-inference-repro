// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_dazg_variant.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cctype>
#include <iostream>
#include <string>

namespace dazg_orbit {
namespace dazg {

enum class Variant {
    Full,
    NoZeroGuard,
    NoResidual,
    NoQPolyBank,
    PlainLut,
    PolyApprox,
    Unknown
};

struct VariantConfig {
    Variant kind = Variant::Full;
    std::string name = "full";
    bool zero_guard_enabled = true;
    bool residual_enabled = true;
    bool qpoly_bank_enabled = true;
    bool plain_lut = false;
    bool poly_approx = false;
    bool known = true;
    bool strict_process_gate = true;
};

inline std::string NormalizeVariantName(std::string v) {
    if (v.empty()) return "full";
    std::replace(v.begin(), v.end(), '-', '_');
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "a0" || v == "baseline" || v == "prod" || v == "production") return "full";
    if (v == "nozero" || v == "no_zero" || v == "nozeroguard") return "no_zero_guard";
    if (v == "noresidual" || v == "no_res") return "no_residual";
    if (v == "noqpoly" || v == "no_qpoly" || v == "no_qpolybank") return "no_qpoly_bank";
    if (v == "lut" || v == "plain") return "plain_lut";
    if (v == "poly" || v == "polyapprox") return "poly_approx";
    return v;
}

inline const char* GetEnvFirst(const char* a, const char* b, const char* c = nullptr) {
    const char* x = std::getenv(a);
    if (x && *x) return x;
    x = std::getenv(b);
    if (x && *x) return x;
    if (c) {
        x = std::getenv(c);
        if (x && *x) return x;
    }
    return nullptr;
}

inline bool EnvFlagEnabled(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return false;
    std::string v = NormalizeVariantName(raw);
    return !(v == "0" || v == "false" || v == "off" || v == "no");
}

inline VariantConfig MakeConfigFromName(const std::string& normalized) {
    VariantConfig cfg;
    cfg.name = normalized;
    cfg.known = true;
    cfg.strict_process_gate = false;

    if (normalized == "full") {
        cfg.kind = Variant::Full;
        cfg.strict_process_gate = true;
    } else if (normalized == "no_zero_guard") {
        cfg.kind = Variant::NoZeroGuard;
        cfg.zero_guard_enabled = false;
    } else if (normalized == "no_residual") {
        cfg.kind = Variant::NoResidual;
        cfg.residual_enabled = false;
    } else if (normalized == "no_qpoly_bank") {
        cfg.kind = Variant::NoQPolyBank;
        cfg.qpoly_bank_enabled = false;
    } else if (normalized == "plain_lut") {
        cfg.kind = Variant::PlainLut;
        cfg.qpoly_bank_enabled = false;
        cfg.plain_lut = true;
    } else if (normalized == "poly_approx") {
        cfg.kind = Variant::PolyApprox;
        cfg.qpoly_bank_enabled = false;
        cfg.poly_approx = true;
    } else {
        cfg.kind = Variant::Unknown;
        cfg.name = normalized.empty() ? "unknown" : normalized;
        cfg.known = false;
    }

    if (EnvFlagEnabled("DAZG_ORBIT_DAZG_DISABLE_ZERO_GUARD") ||
        EnvFlagEnabled("DAZG_ORBIT_DAZG_DISABLE_ZERO_GUARD")) {
        cfg.zero_guard_enabled = false;
        if (cfg.kind == Variant::Full) {
            cfg.kind = Variant::NoZeroGuard;
            cfg.name = "no_zero_guard";
            cfg.strict_process_gate = false;
        }
    }
    if (EnvFlagEnabled("DAZG_ORBIT_DAZG_DISABLE_RESIDUAL") ||
        EnvFlagEnabled("DAZG_ORBIT_DAZG_DISABLE_RESIDUAL")) {
        cfg.residual_enabled = false;
        if (cfg.kind == Variant::Full) {
            cfg.kind = Variant::NoResidual;
            cfg.name = "no_residual";
            cfg.strict_process_gate = false;
        }
    }
    if (EnvFlagEnabled("DAZG_ORBIT_DAZG_DISABLE_QPOLY_BANK") ||
        EnvFlagEnabled("DAZG_ORBIT_DAZG_DISABLE_QPOLY_BANK") ||
        EnvFlagEnabled("DAZG_ORBIT_DAZG_DISABLE_QPOLY")) {
        cfg.qpoly_bank_enabled = false;
        if (cfg.kind == Variant::Full) {
            cfg.kind = Variant::NoQPolyBank;
            cfg.name = "no_qpoly_bank";
            cfg.strict_process_gate = false;
        }
    }
    if (EnvFlagEnabled("DAZG_ORBIT_DAZG_FORCE_PLAIN_LUT") ||
        EnvFlagEnabled("DAZG_ORBIT_DAZG_FORCE_PLAIN_LUT")) {
        cfg.kind = Variant::PlainLut;
        cfg.name = "plain_lut";
        cfg.qpoly_bank_enabled = false;
        cfg.plain_lut = true;
        cfg.poly_approx = false;
        cfg.strict_process_gate = false;
    }
    if (EnvFlagEnabled("DAZG_ORBIT_DAZG_FORCE_POLY_APPROX") ||
        EnvFlagEnabled("DAZG_ORBIT_DAZG_FORCE_POLY_APPROX")) {
        cfg.kind = Variant::PolyApprox;
        cfg.name = "poly_approx";
        cfg.qpoly_bank_enabled = false;
        cfg.plain_lut = false;
        cfg.poly_approx = true;
        cfg.strict_process_gate = false;
    }
    return cfg;
}

inline VariantConfig CurrentVariantConfig() {
    const char* raw = GetEnvFirst("DAZG_ORBIT_DAZG_VARIANT", "DAZG_ORBIT_DAZG_VARIANT", "DAZG_ORBIT_DAZG_VARIANT");
    return MakeConfigFromName(NormalizeVariantName(raw ? raw : "full"));
}

inline Variant RuntimeVariant() {
    return CurrentVariantConfig().kind;
}

inline const char* VariantName(Variant v) {
    switch (v) {
        case Variant::Full: return "full";
        case Variant::NoZeroGuard: return "no_zero_guard";
        case Variant::NoResidual: return "no_residual";
        case Variant::NoQPolyBank: return "no_qpoly_bank";
        case Variant::PlainLut: return "plain_lut";
        case Variant::PolyApprox: return "poly_approx";
        default: return "unknown";
    }
}

inline bool IsProductionVariant() { return CurrentVariantConfig().kind == Variant::Full; }
inline bool IsFullVariant() { return IsProductionVariant(); }
inline bool StrictProcessGate() { return CurrentVariantConfig().strict_process_gate; }
inline bool DisableZeroGuard() { return !CurrentVariantConfig().zero_guard_enabled; }
inline bool DisableResidual() { return !CurrentVariantConfig().residual_enabled; }
inline bool DisableIntegerQPolyPath() { return !CurrentVariantConfig().qpoly_bank_enabled; }
inline bool ForcePlainLut() { return CurrentVariantConfig().plain_lut; }
inline bool ForcePolyApprox() { return CurrentVariantConfig().poly_approx; }

inline int VariantMaxErrorThresholdFp(int production_threshold_fp) {
    const auto cfg = CurrentVariantConfig();
    switch (cfg.kind) {
        case Variant::Full: return production_threshold_fp;
        case Variant::NoZeroGuard: return std::max(production_threshold_fp, production_threshold_fp + 16);
        case Variant::NoResidual: return std::max(production_threshold_fp, 128);
        case Variant::NoQPolyBank:
        case Variant::PlainLut: return std::max(production_threshold_fp, 4096);
        case Variant::PolyApprox: return std::max(production_threshold_fp, 1048576);
        default: return std::max(production_threshold_fp, 1048576);
    }
}

inline const char* CertStatus(bool full_pass, bool variant_metric_ok) {
    const auto cfg = CurrentVariantConfig();
    if (cfg.kind == Variant::Full) return full_pass ? "pass" : "fail";
    return variant_metric_ok ? "ablation_measured" : "ablation_degraded";
}

inline const char* ExecutionMode() {
    const auto cfg = CurrentVariantConfig();
    switch (cfg.kind) {
        case Variant::Full: return "integer_qpoly_residual_zero_guard";
        case Variant::NoZeroGuard: return "integer_qpoly_residual_no_zero_guard";
        case Variant::NoResidual: return "integer_qpoly_no_residual";
        case Variant::NoQPolyBank: return "floating_poly_no_qpoly_bank";
        case Variant::PlainLut: return "plain_lut_bucket_center";
        case Variant::PolyApprox: return "poly_approx_global";
        default: return "unknown";
    }
}

inline void PrintVariantOnce(const char* component) {
    static std::atomic<bool> printed{false};
    bool expected = false;
    if (!printed.compare_exchange_strong(expected, true)) return;
    const auto cfg = CurrentVariantConfig();
    std::cerr << "[DAZG_ORBIT_DAZG_VARIANT]"
              << " component=" << (component ? component : "unknown")
              << " variant=" << cfg.name
              << " dazg_variant=" << cfg.name
              << " zero_guard_enabled=" << (cfg.zero_guard_enabled ? 1 : 0)
              << " residual_enabled=" << (cfg.residual_enabled ? 1 : 0)
              << " qpoly_bank_enabled=" << (cfg.qpoly_bank_enabled ? 1 : 0)
              << " plain_lut=" << (cfg.plain_lut ? 1 : 0)
              << " poly_approx=" << (cfg.poly_approx ? 1 : 0)
              << " strict_process_gate=" << (cfg.strict_process_gate ? 1 : 0)
              << std::endl;
}

inline void EmitVariantLineOnce(const char* component) {
    PrintVariantOnce(component);
}

}  // namespace dazg
}  // namespace dazg_orbit
