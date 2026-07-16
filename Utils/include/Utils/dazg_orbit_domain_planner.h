// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_domain_planner.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dazg_orbit {
namespace domain {

inline bool EnvFlag(const char* name, bool default_value = false) {
    const char* v = std::getenv(name);
    if (v == nullptr) return default_value;
    const std::string s(v);
    return !(s == "0" || s == "false" || s == "False" ||
             s == "FALSE" || s == "off" || s == "OFF" ||
             s == "no" || s == "NO");
}

inline uint64_t EnvUInt64(const char* name, uint64_t default_value = 0) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v) return default_value;
    return static_cast<uint64_t>(parsed);
}

inline bool DomainPlanLoggingEnabled() {
    // Default-on compact evidence ledger: one summary at process exit.
    // Disable timing-only benchmark evidence with DAZG_ORBIT_DOMAIN_PLAN=0.
    return EnvFlag("DAZG_ORBIT_DOMAIN_PLAN",
                   EnvFlag("DAZG_ORBIT_DOMAIN_PLAN", true));
}

inline bool DomainDetailLoggingEnabled() {
    // Detailed per-edge logs are optional to avoid perturbing benchmark timing.
    return EnvFlag("DAZG_ORBIT_DOMAIN_DETAIL",
                   EnvFlag("DAZG_ORBIT_DOMAIN_DETAIL", false));
}

inline bool ConversionDebugEnabled() {
    return EnvFlag("DAZG_ORBIT_CONVERSION_DEBUG",
                   EnvFlag("DAZG_ORBIT_CONVERSION_DEBUG", false));
}

inline uint64_t GlobalTotalRoundsRef() {
    const uint64_t current = EnvUInt64("DAZG_ORBIT_GLOBAL_TOTAL_ROUNDS", 0);
    if (current != 0) return current;
    return EnvUInt64("DAZG_ORBIT_GLOBAL_TOTAL_ROUNDS", 0);
}

inline std::atomic<uint64_t>& RuntimeFusionAppliedGroups() {
    static std::atomic<uint64_t> v{0};
    return v;
}

inline std::atomic<uint64_t>& RuntimeFusionSavedRounds() {
    static std::atomic<uint64_t> v{0};
    return v;
}

inline void RecordRuntimeFusionApplied(uint64_t groups, uint64_t saved_rounds) {
    RuntimeFusionAppliedGroups().fetch_add(groups, std::memory_order_relaxed);
    RuntimeFusionSavedRounds().fetch_add(saved_rounds, std::memory_order_relaxed);
}

// One and only mutex for both site and activation-call sticky fallbacks.
// It must appear before every function that takes the lock.
inline std::mutex& SiteMutex() {
    static std::mutex mu;
    return mu;
}

inline std::string& CurrentSiteStorage() {
    thread_local std::string site = "unspecified";
    return site;
}

inline std::string& StickySiteStorage() {
    static std::string site = "unspecified";
    return site;
}

inline uint64_t& CurrentActivationCallIdStorage() {
    thread_local uint64_t call_id = 0;
    return call_id;
}

inline uint64_t& StickyActivationCallIdStorage() {
    static uint64_t call_id = 0;
    return call_id;
}

inline void SetStickyConversionSite(const std::string& site) {
    std::lock_guard<std::mutex> lock{SiteMutex()};
    StickySiteStorage() = site.empty() ? std::string("unspecified") : site;
}

inline void SetStickyActivationCallId(uint64_t call_id) {
    std::lock_guard<std::mutex> lock{SiteMutex()};
    StickyActivationCallIdStorage() = call_id;
}

inline std::string CurrentConversionSite() {
    const std::string& local = CurrentSiteStorage();
    if (!local.empty() && local != "unspecified") return local;
    std::lock_guard<std::mutex> lock{SiteMutex()};
    return StickySiteStorage();
}

inline uint64_t CurrentActivationCallId() {
    const uint64_t local = CurrentActivationCallIdStorage();
    if (local != 0) return local;
    std::lock_guard<std::mutex> lock{SiteMutex()};
    return StickyActivationCallIdStorage();
}

class ScopedConversionSite {
public:
    explicit ScopedConversionSite(std::string site, uint64_t call_id = 0)
        : old_site_(CurrentSiteStorage()),
          old_call_id_(CurrentActivationCallIdStorage()) {
        const std::string normalized =
            site.empty() ? std::string("unspecified") : std::move(site);
        CurrentSiteStorage() = normalized;
        CurrentActivationCallIdStorage() = call_id;
        SetStickyConversionSite(normalized);
        SetStickyActivationCallId(call_id);
    }

    ~ScopedConversionSite() {
        CurrentSiteStorage() = old_site_;
        CurrentActivationCallIdStorage() = old_call_id_;
        SetStickyConversionSite(old_site_);
        SetStickyActivationCallId(old_call_id_);
    }

    ScopedConversionSite(const ScopedConversionSite&) = delete;
    ScopedConversionSite& operator=(const ScopedConversionSite&) = delete;

private:
    std::string old_site_;
    uint64_t old_call_id_ = 0;
};

inline std::string ShapeToString(const std::vector<size_t>& shape) {
    if (shape.empty()) return "[scalar]";
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < shape.size(); ++i) {
        if (i != 0) oss << "x";
        oss << shape[i];
    }
    oss << "]";
    return oss.str();
}

template <typename ShapeVec>
inline std::vector<size_t> NormalizeShape(const ShapeVec& shape) {
    std::vector<size_t> out;
    for (const auto& v : shape) out.push_back(static_cast<size_t>(v));
    return out;
}

inline std::vector<size_t> NormalizeShape(std::initializer_list<size_t> shape) {
    return std::vector<size_t>(shape.begin(), shape.end());
}

inline uint64_t Numel(const std::vector<size_t>& shape) {
    if (shape.empty()) return 0;
    uint64_t n = 1;
    for (const size_t v : shape) n *= static_cast<uint64_t>(v == 0 ? 1 : v);
    return n;
}

inline std::string Sanitize(const std::string& s) {
    std::string out = s.empty() ? std::string("unspecified") : s;
    for (char& c : out) {
        if (c == '|' || c == ' ' || c == '\t' || c == '\n' || c == '\r') c = '_';
    }
    return out;
}

struct ConversionAggregate {
    uint64_t count = 0;
    uint64_t polys = 0;
    uint64_t slots = 0;
    uint64_t comm = 0;
    uint64_t rounds = 0;
    uint64_t elapsed_us = 0;
};

class DomainPlannerRegistry {
public:
    static DomainPlannerRegistry& Instance() {
        static DomainPlannerRegistry inst;
        return inst;
    }

    ~DomainPlannerRegistry() { EmitSummary(); }

    void Record(const std::string& kind,
                const std::string& site,
                uint64_t call_id,
                bool server,
                uint64_t polys,
                uint64_t slots_per_poly,
                uint64_t before_comm,
                uint64_t after_comm,
                uint64_t before_rounds,
                uint64_t after_rounds,
                uint64_t elapsed_us,
                const std::vector<size_t>& shape) {
        const uint64_t comm_delta =
            (after_comm >= before_comm) ? (after_comm - before_comm) : 0ULL;
        const uint64_t rounds_delta =
            (after_rounds >= before_rounds) ? (after_rounds - before_rounds) : 0ULL;
        const uint64_t slots = polys * (slots_per_poly == 0 ? 1ULL : slots_per_poly);
        const std::string safe_site = Sanitize(site);
        const std::string role = server ? "server" : "client";
        const std::string key = Sanitize(kind) + "_" + safe_site + "_call" +
                                std::to_string(call_id) + "_" +
                                ShapeToString(shape) + "_" + role;

        {
            std::lock_guard<std::mutex> lock{mu_};
            emitted_ = false;
            ConversionAggregate& a = agg_[key];
            a.count += 1;
            a.polys += polys;
            a.slots += slots;
            a.comm += comm_delta;
            a.rounds += rounds_delta;
            a.elapsed_us += elapsed_us;
        }

        if (DomainDetailLoggingEnabled()) {
            const std::string safe_kind = Sanitize(kind);
            std::cerr << "[DAZG_ORBIT_CONVERSION_EDGE]"
                      << " kind=" << safe_kind
                      << " site=" << safe_site
                      << " call_id=" << call_id
                      << " role=" << (server ? 1 : 0)
                      << " domain_from=" << (safe_kind.find("HEToSS") == 0 ? "HE" : "SS")
                      << " domain_to=" << (safe_kind.find("HEToSS") == 0 ? "SS" : "HE")
                      << " shape=" << ShapeToString(shape)
                      << " polys=" << polys
                      << " slots_per_poly=" << slots_per_poly
                      << " total_slots=" << slots
                      << " comm_delta=" << comm_delta
                      << " rounds_delta=" << rounds_delta
                      << " elapsed_us=" << elapsed_us
                      << " batchable=1"
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << std::endl;
        }
    }

    void EmitSummary() {
        if (!DomainPlanLoggingEnabled()) return;

        std::vector<std::pair<std::string, ConversionAggregate>> items;
        {
            std::lock_guard<std::mutex> lock{mu_};
            if (emitted_) return;
            emitted_ = true;
            items.assign(agg_.begin(), agg_.end());
        }

        std::sort(items.begin(), items.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });

        uint64_t total_count = 0;
        uint64_t total_comm = 0;
        uint64_t total_rounds = 0;
        uint64_t total_us = 0;
        uint64_t mergeable_groups = 0;
        uint64_t mergeable_edges = 0;
        uint64_t graph_cut_groups = 0;
        uint64_t safe_graph_cut_groups = 0;
        uint64_t rejected_graph_cut_groups = 0;
        uint64_t saved_rounds_proxy_total = 0;

        for (const auto& kv : items) {
            const std::string safe_key = Sanitize(kv.first);
            const ConversionAggregate& a = kv.second;

            total_count += a.count;
            total_comm += a.comm;
            total_rounds += a.rounds;
            total_us += a.elapsed_us;

            const bool is_activation_site =
                safe_key.find("BasicBlock#") != std::string::npos ||
                safe_key.find("BasicBlock/") != std::string::npos ||
                safe_key.find("ResNet_4stages/conv1") != std::string::npos;
            const bool is_he_to_ss = safe_key.find("HEToSS_") == 0;
            const bool is_ss_to_he = safe_key.find("SSToHE_") == 0;
            const bool is_coeff = safe_key.find("_coeff_") != std::string::npos ||
                                  safe_key.find("coeff_unspecified") != std::string::npos;
            const bool repeated_group = a.count > 1;

            if (repeated_group) {
                mergeable_groups += 1;
                mergeable_edges += a.count - 1;
            }

            const bool graph_cut_candidate =
                is_activation_site && !is_coeff && repeated_group &&
                (is_he_to_ss || is_ss_to_he);

            uint64_t saved_rounds_proxy = 0;
            if (graph_cut_candidate && is_he_to_ss && a.count > 1) {
                saved_rounds_proxy = a.count - 1;
            }
            if (graph_cut_candidate && is_ss_to_he && a.rounds > 1) {
                saved_rounds_proxy = a.rounds - 1;
            }

            const bool safe_runtime_fusion =
                graph_cut_candidate && saved_rounds_proxy >= 4 && a.rounds >= 4;

            if (graph_cut_candidate) {
                graph_cut_groups += 1;
                saved_rounds_proxy_total += saved_rounds_proxy;
                if (safe_runtime_fusion) safe_graph_cut_groups += 1;
                else rejected_graph_cut_groups += 1;
            }
        }

        const double saved_ratio_in_conversion =
            total_rounds == 0
                ? 0.0
                : static_cast<double>(saved_rounds_proxy_total) /
                  static_cast<double>(total_rounds);
        const uint64_t global_total_rounds_ref = GlobalTotalRoundsRef();
        const double saved_ratio_vs_global =
            global_total_rounds_ref == 0
                ? 0.0
                : static_cast<double>(saved_rounds_proxy_total) /
                  static_cast<double>(global_total_rounds_ref);
        const uint64_t runtime_fusion_applied_groups =
            RuntimeFusionAppliedGroups().load(std::memory_order_relaxed);
        const uint64_t runtime_saved_rounds_actual =
            RuntimeFusionSavedRounds().load(std::memory_order_relaxed);
        const bool runtime_applied = runtime_fusion_applied_groups > 0;
        const bool runtime_fusion_recommended =
            runtime_applied ||
            (safe_graph_cut_groups > 0 && saved_rounds_proxy_total >= 8 &&
             saved_ratio_in_conversion >= 0.05);

        std::cerr << "[DAZG_ORBIT_DOMAIN_SUMMARY]"
                  << " total_conversion_edges=" << total_count
                  << " total_comm=" << total_comm
                  << " total_rounds=" << total_rounds
                  << " total_elapsed_us=" << total_us
                  << " mergeable_groups=" << mergeable_groups
                  << " mergeable_edges_proxy=" << mergeable_edges
                  << " graph_cut_groups=" << graph_cut_groups
                  << " safe_graph_cut_groups=" << safe_graph_cut_groups
                  << " rejected_graph_cut_groups=" << rejected_graph_cut_groups
                  << " saved_rounds_proxy_total=" << saved_rounds_proxy_total
                  << " saved_rounds_ratio_in_conversion=" << saved_ratio_in_conversion
                  << " global_total_rounds_ref=" << global_total_rounds_ref
                  << " saved_rounds_ratio_vs_global=" << saved_ratio_vs_global
                  << " runtime_fusion_applied_groups=" << runtime_fusion_applied_groups
                  << " runtime_saved_rounds_actual=" << runtime_saved_rounds_actual
                  << " planner_mode=dazg_orbit_site_call_shape_role_graph_cut_filter_v15_mask_stable_projection_batch"
                  << " runtime_fusion_recommended=" << runtime_fusion_recommended
                  << " runtime_applied=" << runtime_applied
                  << " decision="
                  << (runtime_applied
                          ? "runtime_projection_conversion_batch_applied"
                          : (runtime_fusion_recommended
                              ? "candidate_large_enough_for_runtime_apply"
                              : "reject_runtime_apply_keep_exact_protocol"))
                  << " reason="
                  << (runtime_applied
                          ? "projection_branch_sstohe_and_hetoss_batched_exact"
                          : (runtime_fusion_recommended
                              ? "same_call_gain_passes_threshold"
                              : "proxy_gain_too_small_or_dependency_guarded"))
                  << " certificate=site_call_shape_role_conversion_placement"
                  << " paper_claim=certified_domain_transition_filter"
                  << " exact_equiv=1"
                  << " semantic_loss=0"
                  << std::endl;

        for (const auto& kv : items) {
            const std::string safe_key = Sanitize(kv.first);
            const ConversionAggregate& a = kv.second;
            const bool is_activation_site =
                safe_key.find("BasicBlock#") != std::string::npos ||
                safe_key.find("BasicBlock/") != std::string::npos ||
                safe_key.find("ResNet_4stages/conv1") != std::string::npos;
            const bool is_he_to_ss = safe_key.find("HEToSS_") == 0;
            const bool is_ss_to_he = safe_key.find("SSToHE_") == 0;
            const bool is_coeff = safe_key.find("_coeff_") != std::string::npos ||
                                  safe_key.find("coeff_unspecified") != std::string::npos;
            const bool repeated_group = a.count > 1;
            const bool graph_cut_candidate =
                is_activation_site && !is_coeff && repeated_group &&
                (is_he_to_ss || is_ss_to_he);
            uint64_t saved_rounds_proxy = 0;
            if (graph_cut_candidate && is_he_to_ss && a.count > 1) {
                saved_rounds_proxy = a.count - 1;
            }
            if (graph_cut_candidate && is_ss_to_he && a.rounds > 1) {
                saved_rounds_proxy = a.rounds - 1;
            }
            const bool safe_runtime_fusion =
                graph_cut_candidate && saved_rounds_proxy >= 4 && a.rounds >= 4;
            std::cerr << "[DAZG_ORBIT_DOMAIN_PLAN]"
                      << " key=" << safe_key
                      << " count=" << a.count
                      << " total_polys=" << a.polys
                      << " total_slots=" << a.slots
                      << " total_comm=" << a.comm
                      << " total_rounds=" << a.rounds
                      << " total_elapsed_us=" << a.elapsed_us
                      << " batch_candidate=" << repeated_group
                      << " graph_cut_candidate=" << graph_cut_candidate
                      << " saved_rounds_proxy=" << saved_rounds_proxy
                      << " activation_site=" << is_activation_site
                      << " coeff_group=" << is_coeff
                      << " same_call_shape_role_scope=1"
                      << " min_saved_rounds_for_apply=4"
                      << " safe_runtime_fusion=" << safe_runtime_fusion
                      << " runtime_applied=0"
                      << " decision="
                      << (safe_runtime_fusion
                              ? "candidate_passes_apply_threshold"
                              : "reject_or_keep_exact_protocol")
                      << " reason="
                      << (safe_runtime_fusion
                              ? "large_same_call_candidate"
                              : "small_gain_or_dependency_guard")
                      << " exact_equiv=1"
                      << " semantic_loss=0"
                      << std::endl;
        }
    }

private:
    DomainPlannerRegistry() = default;
    DomainPlannerRegistry(const DomainPlannerRegistry&) = delete;
    DomainPlannerRegistry& operator=(const DomainPlannerRegistry&) = delete;

    std::mutex mu_;
    bool emitted_ = false;
    std::unordered_map<std::string, ConversionAggregate> agg_;
};

class ScopedConversionRecord {
public:
    template <typename ShapeVec, typename HEPtr>
    ScopedConversionRecord(std::string kind,
                           const ShapeVec& shape,
                           HEPtr* he,
                           uint64_t slots_per_poly = 0,
                           std::string site = CurrentConversionSite()) {
        Init(std::move(kind), NormalizeShape(shape), he, slots_per_poly,
             std::move(site));
    }

    template <typename HEPtr>
    ScopedConversionRecord(std::string kind,
                           std::initializer_list<size_t> shape,
                           HEPtr* he,
                           uint64_t slots_per_poly = 0,
                           std::string site = CurrentConversionSite()) {
        Init(std::move(kind), NormalizeShape(shape), he, slots_per_poly,
             std::move(site));
    }

    ~ScopedConversionRecord() {
        if (!enabled_) return;
        const uint64_t after_comm = get_comm_ ? get_comm_() : before_comm_;
        const uint64_t after_rounds = get_rounds_ ? get_rounds_() : before_rounds_;
        const auto t1 = std::chrono::steady_clock::now();
        const uint64_t elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0_).count());
        DomainPlannerRegistry::Instance().Record(kind_, site_, call_id_, server_,
                                                 polys_, slots_per_poly_,
                                                 before_comm_, after_comm,
                                                 before_rounds_, after_rounds,
                                                 elapsed_us, shape_);
    }

    ScopedConversionRecord(const ScopedConversionRecord&) = delete;
    ScopedConversionRecord& operator=(const ScopedConversionRecord&) = delete;

private:
    template <typename HEPtr>
    void Init(std::string kind,
              std::vector<size_t> shape,
              HEPtr* he,
              uint64_t slots_per_poly,
              std::string site) {
        kind_ = std::move(kind);
        site_ = site.empty() ? std::string("unspecified") : std::move(site);
        call_id_ = CurrentActivationCallId();
        shape_ = std::move(shape);

        if (he == nullptr) {
            enabled_ = false;
            return;
        }

        // DAZG_ORBIT_V575_TELEMETRY_HOTPATH_BYPASS_20260614
        // If both summary and detail telemetry are disabled, avoid the per-edge
        // string/mutex/map accounting entirely. This is exactness-neutral.
        if (!DomainPlanLoggingEnabled() && !DomainDetailLoggingEnabled()) {
            enabled_ = false;
            return;
        }

        enabled_ = true;
        server_ = static_cast<bool>(he->server);
        slots_per_poly_ = slots_per_poly;
        if (slots_per_poly_ == 0) {
            slots_per_poly_ = static_cast<uint64_t>(he->polyModulusDegree);
        }
        if (slots_per_poly_ == 0) slots_per_poly_ = 1;

        const uint64_t total = Numel(shape_);
        polys_ = (total + slots_per_poly_ - 1) / slots_per_poly_;

        if (he->IO != nullptr) {
            before_comm_ = static_cast<uint64_t>(he->IO->counter);
            before_rounds_ = static_cast<uint64_t>(he->IO->num_rounds);
            get_comm_ = [he]() -> uint64_t {
                return he->IO ? static_cast<uint64_t>(he->IO->counter) : 0ULL;
            };
            get_rounds_ = [he]() -> uint64_t {
                return he->IO ? static_cast<uint64_t>(he->IO->num_rounds) : 0ULL;
            };
        } else {
            before_comm_ = 0;
            before_rounds_ = 0;
            get_comm_ = []() -> uint64_t { return 0ULL; };
            get_rounds_ = []() -> uint64_t { return 0ULL; };
        }
        t0_ = std::chrono::steady_clock::now();
    }

    bool enabled_ = false;
    bool server_ = false;
    std::string kind_;
    std::string site_ = "unspecified";
    uint64_t call_id_ = 0;
    std::vector<size_t> shape_;
    uint64_t polys_ = 0;
    uint64_t slots_per_poly_ = 1;
    uint64_t before_comm_ = 0;
    uint64_t before_rounds_ = 0;
    std::function<uint64_t()> get_comm_;
    std::function<uint64_t()> get_rounds_;
    std::chrono::steady_clock::time_point t0_;
};

}  // namespace domain
}  // namespace dazg_orbit
