// DAZG-Orbit Project Source File
// Component: experiments/n100_checkpoint013/src/qahl_v645_src/qahl_v645_scheduler.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

#include "qahl_v645_inventory.hpp"
#include "qahl_v645_tensor_adapters.hpp"
#include "qahl_v645_extra_adapters.hpp"
#include "qahl_v645_sample_input.hpp"
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace dazg_orbit::qahl::v645;

static std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') { o.push_back('\\'); o.push_back(c); }
    else if (c == '\n') o += "\\n";
    else o.push_back(c);
  }
  return o;
}

static const PayloadEntry* find_ref_sched(const std::string& key) {
  for (const auto& p : kReferences) if (key == p.key) return &p;
  return nullptr;
}

int main(int argc, char** argv) {
  std::string out_report = "qahl_v645_scheduler_report.json";
  std::size_t sample_index = 0;
  for (int i=1; i<argc; ++i) {
    std::string a = argv[i];
    if ((a == "--out-report" || a == "--out") && i + 1 < argc) out_report = argv[++i];
    else if ((a == "--sample-index" || a == "--sample_index") && i + 1 < argc) sample_index = static_cast<std::size_t>(std::stoul(argv[++i]));
  }

  std::vector<std::string> issues;
  std::map<std::string, TensorU64> cache;
  bool sample_ready = false;
  bool bucket_scale_ready = false;
  try {
    for (const auto& p : kWeights) cache.emplace(p.key, load_tensor_u64(p.key, p.path, 16));
    const auto* input_p = find_ref_sched("qahl_ref_input_n10.npy");
    if (input_p) {
      auto x = load_input_sample_tensor(input_p->key, input_p->path, sample_index, false, 16);
      (void)x;
      sample_ready = true;
    } else {
      issues.push_back("missing qahl_ref_input_n10.npy");
    }
    bucket_scale_ready = cache.find("h8.1.bucket_scale") != cache.end();
  } catch (const std::exception& e) {
    issues.push_back(e.what());
  }

  std::ofstream out(out_report);
  out << "{\n";
  out << "  \"schema\": \"dazg_orbit.qahl.v645.scheduler_report\",\n";
  out << "  \"status\": \"" << (issues.empty() ? "scheduler_ready" : "blocked") << "\",\n";
  out << "  \"tensor_cache_entries\": " << cache.size() << ",\n";
  out << "  \"expected_weight_payloads\": " << kWeightCount << ",\n";
  out << "  \"sample_input_ready\": " << (sample_ready ? "true" : "false") << ",\n";
  out << "  \"bucket_scale_ready\": " << (bucket_scale_ready ? "true" : "false") << ",\n";
  out << "  \"he_exact_done\": false,\n";
  out << "  \"strict_hash_match_all\": null,\n";
  out << "  \"reference_echo\": false,\n";
  out << "  \"issues\": [";
  for (std::size_t i=0; i<issues.size(); ++i) { if (i) out << ", "; out << "\"" << esc(issues[i]) << "\""; }
  out << "]\n";
  out << "}\n";
  std::cout << "[v645][scheduler] status=" << (issues.empty() ? "scheduler_ready" : "blocked") << " cache=" << cache.size() << "\n";
  return issues.empty() ? 0 : 7;
}
