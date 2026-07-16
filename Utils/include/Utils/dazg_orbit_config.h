// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_config.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once
#include <cstdlib>
#include <string>

namespace dazg_orbit {

inline bool EnvFlag(const char* name, bool default_value) {
    const char* v = std::getenv(name);
    if (!v) return default_value;
    std::string s(v);
    return !(s == "0" || s == "false" || s == "False" || s == "OFF");
}

struct Config {
    bool enable_stage_y = true;
    bool enable_sparse_schedule = true;
    bool enable_batched_conversion = true;
    bool enable_csr_classifier = true;
    bool enable_compact_k1s2 = true;
    bool enable_polyphase_k3s2 = true;
    bool enable_exact_tiled_k3s1 = true;
    bool enable_profiler = true;

    static Config FromEnv() {
        Config c;
        c.enable_stage_y = EnvFlag("DAZG_ORBIT_STAGE_Y", true);
        c.enable_sparse_schedule = EnvFlag("DAZG_ORBIT_SPARSE_SCHEDULE", true);
        c.enable_batched_conversion = EnvFlag("DAZG_ORBIT_BATCHED_CONVERSION", true);
        c.enable_csr_classifier = EnvFlag("DAZG_ORBIT_CSR_CLASSIFIER", true);
        c.enable_compact_k1s2 = EnvFlag("DAZG_ORBIT_COMPACT_K1S2", true);
        c.enable_polyphase_k3s2 = EnvFlag("DAZG_ORBIT_POLYPHASE_K3S2", true);
        c.enable_exact_tiled_k3s1 = EnvFlag("DAZG_ORBIT_EXACT_TILED_K3S1", true);
        c.enable_profiler = EnvFlag("DAZG_ORBIT_PROFILER", true);
        return c;
    }
};

}  // namespace dazg_orbit
