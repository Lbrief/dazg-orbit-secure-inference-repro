// DAZG-Orbit Project Source File
// Component: experiments/n10_p60/src/qahl_v645_src/qahl_v645_tensor_loader.hpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

#pragma once
#include "qahl_v645_stable_runtime_api.hpp"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace dazg_orbit::qahl::v645 {

using TensorU64 = resolved::TensorU64;

struct RawNpyU64 {
  std::string key;
  std::string path;
  std::string descr;
  std::vector<size_t> shape;
  std::vector<std::uint64_t> values;
};

inline std::string between(const std::string& s, const std::string& a, const std::string& b) {
  auto i = s.find(a);
  if (i == std::string::npos) return "";
  i += a.size();
  auto j = s.find(b, i);
  if (j == std::string::npos) return "";
  return s.substr(i, j - i);
}

inline std::vector<size_t> parse_shape(const std::string& header) {
  std::vector<size_t> out;
  auto p = header.find("'shape'");
  if (p == std::string::npos) p = header.find("\"shape\"");
  auto l = header.find('(', p);
  auto r = header.find(')', l);
  if (l == std::string::npos || r == std::string::npos) return out;
  std::stringstream ss(header.substr(l + 1, r - l - 1));
  while (ss.good()) {
    std::string tok;
    std::getline(ss, tok, ',');
    std::stringstream ts(tok);
    size_t v = 0;
    if (ts >> v) out.push_back(v);
  }
  return out;
}

inline std::size_t elem_count(const std::vector<size_t>& shape) {
  std::size_t n = 1;
  for (auto v : shape) n *= v;
  return n;
}

inline RawNpyU64 load_raw_npy_u64(const std::string& key, const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot open npy " + path);
  char magic[6];
  f.read(magic, 6);
  const unsigned char expected[6] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
  for (int i=0; i<6; ++i) {
    if (static_cast<unsigned char>(magic[i]) != expected[i]) throw std::runtime_error("bad npy magic " + path);
  }
  unsigned char ver[2]; f.read(reinterpret_cast<char*>(ver), 2);
  std::uint32_t hlen = 0;
  if (ver[0] == 1) {
    unsigned char b[2]; f.read(reinterpret_cast<char*>(b), 2);
    hlen = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8);
  } else if (ver[0] == 2) {
    unsigned char b[4]; f.read(reinterpret_cast<char*>(b), 4);
    hlen = static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) |
           (static_cast<std::uint32_t>(b[2]) << 16) | (static_cast<std::uint32_t>(b[3]) << 24);
  } else {
    throw std::runtime_error("unsupported npy version " + path);
  }
  std::string header(hlen, '\0');
  f.read(&header[0], hlen);
  RawNpyU64 out;
  out.key = key;
  out.path = path;
  out.descr = between(header, "'descr': '", "'");
  if (out.descr.empty()) out.descr = between(header, "\"descr\": \"", "\"");
  out.shape = parse_shape(header);
  auto n = elem_count(out.shape);
  out.values.resize(n);
  if (n) {
    f.read(reinterpret_cast<char*>(out.values.data()), static_cast<std::streamsize>(n * sizeof(std::uint64_t)));
    if (!f) throw std::runtime_error("truncated npy payload " + path);
  }
  return out;
}


inline void assign_flat(TensorU64& t, const std::vector<size_t>& shape, const std::vector<std::uint64_t>& values) {
  std::vector<size_t> idx(shape.size(), 0);
  for (std::size_t flat = 0; flat < values.size(); ++flat) {
    std::size_t rem = flat;
    for (std::size_t d = shape.size(); d-- > 0;) {
      idx[d] = rem % shape[d];
      rem /= shape[d];
    }
    t(idx) = values[flat];
  }
}


inline TensorU64 make_tensor_u64(const RawNpyU64& raw, int scale_bits = 16) {
  const auto& shape = raw.shape;
  TensorU64 t(shape, 64, scale_bits);
  assign_flat(t, shape, raw.values);
  return t;
}

inline TensorU64 load_tensor_u64(const std::string& key, const std::string& path, int scale_bits = 16) {
  return make_tensor_u64(load_raw_npy_u64(key, path), scale_bits);
}

} // namespace dazg_orbit::qahl::v645
