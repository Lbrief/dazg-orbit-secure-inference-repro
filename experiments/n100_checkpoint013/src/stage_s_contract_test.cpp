// DAZG-Orbit Project Source File
// Component: experiments/n100_checkpoint013/src/stage_s_contract_test.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static const std::vector<std::int64_t>& load_table() {
  static const std::vector<std::int64_t> table = []() {
    constexpr std::size_t n = 1048577;
    const char* path = std::getenv("DAZG_ORBIT_STAGE_S_Q16_TABLE");
    if (path == nullptr || *path == '\0') {
      throw std::runtime_error("DAZG_ORBIT_STAGE_S_Q16_TABLE is not set");
    }
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() != static_cast<std::streamoff>(n * sizeof(std::int64_t))) {
      throw std::runtime_error("Stage-S table file/size mismatch");
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::int64_t> v(n);
    in.read(reinterpret_cast<char*>(v.data()),
            static_cast<std::streamsize>(n * sizeof(std::int64_t)));
    if (!in) throw std::runtime_error("Stage-S table read failed");
    return v;
  }();
  return table;
}

static std::int64_t eval(std::int64_t q) {
  constexpr std::int64_t clip = 524288;
  if (q <= -clip) return 0;
  if (q >= clip) return q;
  return load_table().at(static_cast<std::size_t>(q + clip));
}

int main() {
  const std::vector<std::int64_t> q = {
      -600000, -524289, -524288, -524287, -400000, -65536, -1,
      0, 1, 65536, 400000, 524287, 524288, 524289,
      600000, 663548, 1716707};
  const std::vector<std::int64_t> expected = {
      0, 0, 0, 0, 0, -10397, -1,
      0, 1, 55139, 400000, 524287, 524288, 524289,
      600000, 663548, 1716707};
  bool ok = true;
  std::vector<std::int64_t> y;
  y.reserve(q.size());
  for (std::size_t i = 0; i < q.size(); ++i) {
    y.push_back(eval(q[i]));
    ok = ok && y.back() == expected[i];
  }
  std::cout << "{\"schema\":\"dazg.stage_s.frozen_table.contract.v1\","
            << "\"status\":\"" << (ok ? "PASS" : "FAIL") << "\","
            << "\"central_minus1\":" << y[6] << ","
            << "\"central_plus1\":" << y[8] << ","
            << "\"positive_600000\":" << y[14] << ","
            << "\"positive_1716707\":" << y[16] << "}"
            << std::endl;
  return ok ? 0 : 21;
}
