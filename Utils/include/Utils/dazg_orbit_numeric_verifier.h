// DAZG-Orbit Project Source File
// Component: Utils/include/Utils/dazg_orbit_numeric_verifier.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace dazg_orbit_numeric_verifier {

inline const char* EnvStr(const char* name, const char* default_value = "") {
    const char* v = std::getenv(name);
    return (v == nullptr || *v == '\0') ? default_value : v;
}

inline bool EnvBool(const char* name, bool default_value = false) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    return !(std::strcmp(v, "0") == 0 ||
             std::strcmp(v, "false") == 0 || std::strcmp(v, "False") == 0 || std::strcmp(v, "FALSE") == 0 ||
             std::strcmp(v, "off") == 0 || std::strcmp(v, "OFF") == 0 ||
             std::strcmp(v, "no") == 0 || std::strcmp(v, "NO") == 0);
}

inline int64_t EnvI64(const char* name, int64_t default_value = 0) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    char* end = nullptr;
    long long parsed = std::strtoll(v, &end, 10);
    if (end == v) return default_value;
    return static_cast<int64_t>(parsed);
}

inline uint64_t EnvU64(const char* name, uint64_t default_value = 0) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return default_value;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v) return default_value;
    return static_cast<uint64_t>(parsed);
}

inline uint64_t AbsI64ToU64(int64_t x) {
    if (x >= 0) return static_cast<uint64_t>(x);
    if (x == std::numeric_limits<int64_t>::min()) {
        return static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
    }
    return static_cast<uint64_t>(-x);
}

inline uint64_t SplitMix64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

inline uint64_t HashSignedVector(const std::vector<int64_t>& vals, uint64_t salt = 0) {
    uint64_t h = 1469598103934665603ULL ^ salt;
    for (size_t i = 0; i < vals.size(); ++i) {
        const uint64_t u = static_cast<uint64_t>(vals[i]);
        h ^= SplitMix64(u + 0x9e3779b97f4a7c15ULL * static_cast<uint64_t>(i + 1));
        h *= 1099511628211ULL;
    }
    return h;
}

inline std::vector<size_t> TopKIndices(const std::vector<int64_t>& vals, size_t k) {
    std::vector<size_t> idx(vals.size());
    for (size_t i = 0; i < vals.size(); ++i) idx[i] = i;
    if (k > idx.size()) k = idx.size();
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(),
        [&](size_t a, size_t b) {
            if (vals[a] != vals[b]) return vals[a] > vals[b];
            return a < b;
        });
    idx.resize(k);
    return idx;
}

inline std::string JoinIndices(const std::vector<size_t>& idx) {
    std::ostringstream oss;
    for (size_t i = 0; i < idx.size(); ++i) {
        if (i) oss << ',';
        oss << idx[i];
    }
    return oss.str();
}

struct Summary {
    size_t n = 0;
    int top1 = -1;
    int second_top1 = -1;
    int64_t best_fp = 0;
    int64_t second_best_fp = 0;
    int64_t top1_margin_fp = 0;
    uint64_t checksum = 0;
    int64_t min_fp = 0;
    int64_t max_fp = 0;
    uint64_t signed_hash = 0;
    uint64_t round8_hash = 0;
    uint64_t round12_hash = 0;
    uint64_t sum_abs_mod = 0;
    int64_t signed_sum_mod = 0;
    std::string top5;
};

inline Summary ComputeSummary(const std::vector<int64_t>& vals) {
    Summary s;
    s.n = vals.size();
    if (vals.empty()) return s;

    const std::vector<size_t> top5_idx = TopKIndices(vals, 5);
    if (!top5_idx.empty()) {
        s.top1 = static_cast<int>(top5_idx[0]);
        s.best_fp = vals[top5_idx[0]];
    }
    if (top5_idx.size() >= 2) {
        s.second_top1 = static_cast<int>(top5_idx[1]);
        s.second_best_fp = vals[top5_idx[1]];
        s.top1_margin_fp = s.best_fp - s.second_best_fp;
    }
    s.min_fp = vals[0];
    s.max_fp = vals[0];

    uint64_t checksum = 0;
    uint64_t sum_abs = 0;
    long long signed_sum = 0;
    std::vector<int64_t> r8(vals.size()), r12(vals.size());
    for (size_t i = 0; i < vals.size(); ++i) {
        const int64_t v = vals[i];
        s.min_fp = std::min(s.min_fp, v);
        s.max_fp = std::max(s.max_fp, v);
        checksum = (checksum + static_cast<uint64_t>(v & 0xffffffffULL) * static_cast<uint64_t>(i + 1)) % 1000000007ULL;
        sum_abs = (sum_abs + AbsI64ToU64(v)) % 1000000007ULL;
        signed_sum += v;
        r8[i] = v >> 8;
        r12[i] = v >> 12;
    }
    s.checksum = checksum;
    s.sum_abs_mod = sum_abs;
    s.signed_sum_mod = signed_sum % 1000000007LL;
    s.signed_hash = HashSignedVector(vals, 0);
    s.round8_hash = HashSignedVector(r8, 8);
    s.round12_hash = HashSignedVector(r12, 12);
    s.top5 = JoinIndices(top5_idx);
    return s;
}

inline bool SaveVector(const std::string& path, const std::vector<int64_t>& vals) {
    std::ofstream ofs(path.c_str());
    if (!ofs) return false;
    ofs << "n=" << vals.size() << "\n";
    for (size_t i = 0; i < vals.size(); ++i) ofs << vals[i] << "\n";
    return true;
}

inline bool LoadVector(const std::string& path, std::vector<int64_t>* vals) {
    if (vals == nullptr) return false;
    std::ifstream ifs(path.c_str());
    if (!ifs) return false;
    std::string line;
    size_t n = 0;
    if (!std::getline(ifs, line)) return false;
    if (line.rfind("n=", 0) == 0) {
        n = static_cast<size_t>(std::strtoull(line.c_str() + 2, nullptr, 10));
    } else {
        return false;
    }
    vals->clear();
    vals->reserve(n);
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        vals->push_back(std::strtoll(line.c_str(), nullptr, 10));
    }
    return vals->size() == n;
}

struct CompareResult {
    bool available = false;
    bool top1_match = false;
    bool top5_match = false;
    bool checksum_match = false;
    bool signed_hash_match = false;
    bool round8_hash_match = false;
    bool round12_hash_match = false;
    bool exact_match = false;
    bool tolerance_match = false;
    bool bounded_match = false;
    bool bias_corrected_match = false;
    bool classification_equiv_match = false;
    bool class_margin_guard_match = false;
    size_t diff_count = 0;
    int64_t linf_error = 0;
    uint64_t l1_error = 0;
    long double mean_abs_error = 0.0L;
    uint64_t normalized_l1_ppm = 0;
    int64_t signed_error_mean = 0;
    int64_t signed_error_median = 0;
    int64_t bias_corrected_linf_error = 0;
    uint64_t bias_corrected_l1_error = 0;
    uint64_t bias_corrected_normalized_l1_ppm = 0;
    int ref_top1 = -1;
    int cur_top1 = -1;
    int ref_second_top1 = -1;
    int cur_second_top1 = -1;
    int64_t ref_best_fp = 0;
    int64_t cur_best_fp = 0;
    int64_t ref_second_best_fp = 0;
    int64_t cur_second_best_fp = 0;
    int64_t ref_top1_margin_fp = 0;
    int64_t cur_top1_margin_fp = 0;
    int64_t top1_abs_error = 0;
    int64_t best_fp_abs_error = 0;
    uint64_t ref_checksum = 0;
    uint64_t cur_checksum = 0;
    uint64_t ref_signed_hash = 0;
    uint64_t cur_signed_hash = 0;
    uint64_t ref_round8_hash = 0;
    uint64_t cur_round8_hash = 0;
    uint64_t ref_round12_hash = 0;
    uint64_t cur_round12_hash = 0;
    std::string ref_top5;
    std::string cur_top5;
    int64_t linf_tolerance = 0;
    uint64_t normalized_l1_ppm_tolerance = 0;
    int64_t top1_abs_tolerance = 0;
    int64_t best_fp_tolerance = 0;
    int64_t bias_corrected_linf_tolerance = 0;
    uint64_t bias_corrected_normalized_l1_ppm_tolerance = 0;
    int64_t class_margin_safety_fp = 0;
    std::string equivalence_class;
    std::string error_class;
    bool pass = false;
};

inline CompareResult CompareVectors(const std::vector<int64_t>& ref,
                                    const std::vector<int64_t>& cur) {
    CompareResult cr;
    cr.available = (!ref.empty() && ref.size() == cur.size());
    if (!cr.available) {
        cr.error_class = "unavailable_or_size_mismatch";
        cr.equivalence_class = "unavailable";
        return cr;
    }

    const Summary ref_s = ComputeSummary(ref);
    const Summary cur_s = ComputeSummary(cur);
    cr.ref_top1 = ref_s.top1;
    cr.cur_top1 = cur_s.top1;
    cr.ref_second_top1 = ref_s.second_top1;
    cr.cur_second_top1 = cur_s.second_top1;
    cr.ref_best_fp = ref_s.best_fp;
    cr.cur_best_fp = cur_s.best_fp;
    cr.ref_second_best_fp = ref_s.second_best_fp;
    cr.cur_second_best_fp = cur_s.second_best_fp;
    cr.ref_top1_margin_fp = ref_s.top1_margin_fp;
    cr.cur_top1_margin_fp = cur_s.top1_margin_fp;
    cr.ref_checksum = ref_s.checksum;
    cr.cur_checksum = cur_s.checksum;
    cr.ref_signed_hash = ref_s.signed_hash;
    cr.cur_signed_hash = cur_s.signed_hash;
    cr.ref_round8_hash = ref_s.round8_hash;
    cr.cur_round8_hash = cur_s.round8_hash;
    cr.ref_round12_hash = ref_s.round12_hash;
    cr.cur_round12_hash = cur_s.round12_hash;
    cr.ref_top5 = ref_s.top5;
    cr.cur_top5 = cur_s.top5;
    cr.top1_match = (ref_s.top1 == cur_s.top1);
    cr.top5_match = (ref_s.top5 == cur_s.top5);
    cr.checksum_match = (ref_s.checksum == cur_s.checksum);
    cr.signed_hash_match = (ref_s.signed_hash == cur_s.signed_hash);
    cr.round8_hash_match = (ref_s.round8_hash == cur_s.round8_hash);
    cr.round12_hash_match = (ref_s.round12_hash == cur_s.round12_hash);

    uint64_t l1 = 0;
    uint64_t ref_abs = 0;
    int64_t linf = 0;
    size_t diff = 0;
    long double signed_sum = 0.0L;
    std::vector<int64_t> diffs;
    diffs.reserve(ref.size());

    for (size_t i = 0; i < ref.size(); ++i) {
        const int64_t d = cur[i] - ref[i];
        const uint64_t ad = AbsI64ToU64(d);
        const uint64_t ar = AbsI64ToU64(ref[i]);
        if (ad != 0) ++diff;
        if (ad > static_cast<uint64_t>(linf)) {
            linf = ad > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(ad);
        }
        l1 += ad;
        ref_abs += ar;
        signed_sum += static_cast<long double>(d);
        diffs.push_back(d);
    }

    cr.diff_count = diff;
    cr.linf_error = linf;
    cr.l1_error = l1;
    cr.mean_abs_error = static_cast<long double>(l1) / static_cast<long double>(ref.size());
    cr.normalized_l1_ppm = (ref_abs == 0) ? 0ULL : static_cast<uint64_t>((static_cast<long double>(l1) * 1000000.0L) / static_cast<long double>(ref_abs));

    std::sort(diffs.begin(), diffs.end());
    cr.signed_error_median = diffs[diffs.size() / 2];
    cr.signed_error_mean = static_cast<int64_t>(signed_sum / static_cast<long double>(diffs.size()));

    uint64_t bc_l1 = 0;
    int64_t bc_linf = 0;
    for (size_t i = 0; i < ref.size(); ++i) {
        const int64_t d = (cur[i] - ref[i]) - cr.signed_error_median;
        const uint64_t ad = AbsI64ToU64(d);
        if (ad > static_cast<uint64_t>(bc_linf)) {
            bc_linf = ad > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(ad);
        }
        bc_l1 += ad;
    }
    cr.bias_corrected_linf_error = bc_linf;
    cr.bias_corrected_l1_error = bc_l1;
    cr.bias_corrected_normalized_l1_ppm =
        (ref_abs == 0) ? 0ULL : static_cast<uint64_t>((static_cast<long double>(bc_l1) * 1000000.0L) / static_cast<long double>(ref_abs));

    if (ref_s.top1 >= 0 && static_cast<size_t>(ref_s.top1) < ref.size()) {
        cr.top1_abs_error = static_cast<int64_t>(AbsI64ToU64(cur[static_cast<size_t>(ref_s.top1)] - ref[static_cast<size_t>(ref_s.top1)]));
    }
    cr.best_fp_abs_error = static_cast<int64_t>(AbsI64ToU64(cur_s.best_fp - ref_s.best_fp));

    cr.linf_tolerance = std::max<int64_t>(0, EnvI64("DAZG_ORBIT_NUMERIC_LINF_TOLERANCE", 0));
    cr.normalized_l1_ppm_tolerance = EnvU64("DAZG_ORBIT_NUMERIC_NORMALIZED_L1_PPM_TOLERANCE", 0);
    cr.top1_abs_tolerance = std::max<int64_t>(0, EnvI64("DAZG_ORBIT_NUMERIC_TOP1_ABS_TOLERANCE", cr.linf_tolerance));
    cr.best_fp_tolerance = std::max<int64_t>(0, EnvI64("DAZG_ORBIT_NUMERIC_BEST_FP_TOLERANCE", cr.linf_tolerance));
    cr.bias_corrected_linf_tolerance = std::max<int64_t>(0, EnvI64("DAZG_ORBIT_NUMERIC_BIAS_CORRECTED_LINF_TOLERANCE", 0));
    cr.bias_corrected_normalized_l1_ppm_tolerance = EnvU64("DAZG_ORBIT_NUMERIC_BIAS_CORRECTED_NORMALIZED_L1_PPM_TOLERANCE", 0);
    cr.class_margin_safety_fp = std::max<int64_t>(0, EnvI64("DAZG_ORBIT_NUMERIC_CLASS_EQUIV_MARGIN_SAFETY", 0));

    cr.exact_match = cr.checksum_match && cr.signed_hash_match &&
                     cr.round8_hash_match && cr.round12_hash_match && cr.diff_count == 0;

    const bool allow_tolerance = EnvBool("DAZG_ORBIT_NUMERIC_ALLOW_TOLERANCE", false);
    const bool require_top5 = EnvBool("DAZG_ORBIT_NUMERIC_REQUIRE_TOP5", true);
    const bool topk_ok = cr.top1_match && (!require_top5 || cr.top5_match);
    const bool linf_ok = cr.linf_error <= cr.linf_tolerance;
    const bool norm_ok = cr.normalized_l1_ppm <= cr.normalized_l1_ppm_tolerance;
    const bool top1_ok = cr.top1_abs_error <= cr.top1_abs_tolerance;
    const bool best_ok = cr.best_fp_abs_error <= cr.best_fp_tolerance;

    cr.tolerance_match = linf_ok && norm_ok && top1_ok && best_ok;
    cr.bounded_match = topk_ok && cr.tolerance_match;

    const bool bc_linf_enabled = cr.bias_corrected_linf_tolerance > 0;
    const bool bc_norm_enabled = cr.bias_corrected_normalized_l1_ppm_tolerance > 0;
    cr.bias_corrected_match =
        topk_ok &&
        (!bc_linf_enabled || cr.bias_corrected_linf_error <= cr.bias_corrected_linf_tolerance) &&
        (!bc_norm_enabled || cr.bias_corrected_normalized_l1_ppm <= cr.bias_corrected_normalized_l1_ppm_tolerance);

    const bool allow_class_equiv = EnvBool("DAZG_ORBIT_NUMERIC_ALLOW_CLASS_EQUIV", false);
    const bool require_class_margin = EnvBool("DAZG_ORBIT_NUMERIC_CLASS_EQUIV_REQUIRE_MARGIN", false);
    const bool ref_margin_positive = cr.ref_top1_margin_fp > 0;
    const bool cur_margin_positive = cr.cur_top1_margin_fp > 0;
    cr.class_margin_guard_match =
        (!require_class_margin) ||
        (ref_margin_positive && cur_margin_positive &&
         cr.ref_top1_margin_fp > (2 * cr.linf_error + cr.class_margin_safety_fp) &&
         cr.cur_top1_margin_fp > cr.class_margin_safety_fp);
    cr.classification_equiv_match = allow_class_equiv && topk_ok && cr.class_margin_guard_match;

    cr.pass = cr.exact_match || (allow_tolerance && cr.bounded_match) || cr.classification_equiv_match;

    if (cr.exact_match) {
        cr.equivalence_class = "exact";
        cr.error_class = "exact_match";
    } else if (cr.bounded_match) {
        cr.equivalence_class = "bounded_numeric_topk";
        cr.error_class = "bounded_topk_numeric_match_not_exact";
    } else if (cr.classification_equiv_match) {
        cr.equivalence_class = "classification_topk_equiv";
        cr.error_class = require_class_margin ?
            "classification_topk_margin_certified_not_exact" :
            "classification_topk_preserved_not_exact";
    } else if (cr.bias_corrected_match) {
        cr.equivalence_class = "bias_corrected_diagnostic_only";
        cr.error_class = "bias_corrected_topk_match_not_pass";
    } else if (cr.top1_match && cr.top5_match) {
        cr.equivalence_class = "topk_only";
        cr.error_class = "topk_only_fail_numeric_bounds";
    } else if (cr.top1_match) {
        cr.equivalence_class = "top1_only";
        cr.error_class = "top1_only_fail_top5_and_numeric_bounds";
    } else {
        cr.equivalence_class = "mismatch";
        cr.error_class = "top1_mismatch";
    }

    return cr;
}

}  // namespace dazg_orbit_numeric_verifier
