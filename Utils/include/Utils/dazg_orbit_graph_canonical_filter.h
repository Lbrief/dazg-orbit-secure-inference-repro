// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_graph_canonical_filter.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <string>

namespace dazg_orbit {
namespace graph {

inline bool IsCanonicalSummaryExcluded(const std::string& source,
                                       const std::string& benefit_class,
                                       const std::string& op_id) {
    if (benefit_class == "CSR_CLASSIFIER_WRAPPER") return true;
    if (source == "StageNResonantClassifierWrapper") return true;
    if (op_id.find("StageNResonantClassifierWrapper") != std::string::npos) return true;
    if (op_id.find("CSR_CLASSIFIER_WRAPPER") != std::string::npos) return true;
    return false;
}

template <class OpPlanLike>
inline bool IsCanonicalSummaryExcluded(const OpPlanLike& plan) {
    return IsCanonicalSummaryExcluded(plan.source, plan.benefit_class, plan.op_id);
}

}  // namespace graph
}  // namespace dazg_orbit
