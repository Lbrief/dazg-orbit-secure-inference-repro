// DAZG-Orbit Project Source File
// Component: experiments/n100_checkpoint013/src/qahl_v645_src/qahl_v645_tensor_adapters.hpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

#pragma once
#include "qahl_v645_tensor_loader.hpp"
#include "qahl_v645_stable_runtime_api.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace dazg_orbit::qahl::v645 {

using TensorU64 = resolved::TensorU64;

inline std::vector<size_t> tensor_shape(TensorU64& t) {
  auto s = t.shape();
  std::vector<size_t> out;
  for (auto v : s) out.push_back(static_cast<size_t>(v));
  return out;
}

inline uint64_t tensor_get_flat(TensorU64& t, const std::vector<size_t>& shape, std::size_t flat) {
  std::vector<size_t> idx(shape.size(), 0);
  std::size_t rem = flat;
  for (std::size_t d = shape.size(); d-- > 0;) {
    idx[d] = rem % shape[d];
    rem /= shape[d];
  }
  return t(idx);
}

inline void tensor_set_flat(TensorU64& t, const std::vector<size_t>& shape, std::size_t flat, uint64_t value) {
  std::vector<size_t> idx(shape.size(), 0);
  std::size_t rem = flat;
  for (std::size_t d = shape.size(); d-- > 0;) {
    idx[d] = rem % shape[d];
    rem /= shape[d];
  }
  t(idx) = value;
}

inline std::vector<uint64_t> tensor_flatten_u64(TensorU64& t) {
  auto shape = tensor_shape(t);
  const std::size_t n = elem_count(shape);
  std::vector<uint64_t> out(n);
  for (std::size_t i=0; i<n; ++i) out[i] = tensor_get_flat(t, shape, i);
  return out;
}

inline TensorU64 make_tensor_from_u64(const std::vector<size_t>& shape, const std::vector<uint64_t>& values, int scale_bits = 16) {
  RawNpyU64 raw;
  raw.key = "generated_tensor";
  raw.descr = "<u8";
  raw.shape = shape;
  raw.values = values;
  return make_tensor_u64(raw, scale_bits);
}

inline TensorU64 tensor_identity(TensorU64& x) {
  auto shape = tensor_shape(x);
  return make_tensor_from_u64(shape, tensor_flatten_u64(x), 16);
}

inline TensorU64 tensor_modular_add(TensorU64& a, TensorU64& b) {
  auto shape_a = tensor_shape(a);
  auto shape_b = tensor_shape(b);
  if (shape_a != shape_b) throw std::runtime_error("tensor_modular_add shape mismatch");
  auto va = tensor_flatten_u64(a);
  auto vb = tensor_flatten_u64(b);
  for (std::size_t i=0; i<va.size(); ++i) va[i] = va[i] + vb[i];
  return make_tensor_from_u64(shape_a, va, 16);
}

inline TensorU64 tensor_gelu_adapter(TensorU64& x) {
  auto shape = tensor_shape(x);
  auto vx = tensor_flatten_u64(x);
  std::vector<std::int64_t> q(vx.size());
  for (std::size_t i=0; i<vx.size(); ++i) q[i] = static_cast<std::int64_t>(vx[i]);
  resolved::TFHEGeLU gelu;
  auto y = resolved::gelu_forward_fixed(gelu, q);
  std::vector<uint64_t> out(y.size());
  for (std::size_t i=0; i<y.size(); ++i) out[i] = static_cast<uint64_t>(y[i]);
  return make_tensor_from_u64(shape, out, 16);
}

} // namespace dazg_orbit::qahl::v645
