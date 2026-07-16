// DAZG-Orbit Project Source File
// Component: experiments/n10_p60/src/qahl_v645_src/qahl_v645_sample_input.hpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

#pragma once
#include "qahl_v645_tensor_loader.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace dazg_orbit::qahl::v645 {

inline RawNpyU64 slice_nchw_sample(const RawNpyU64& batch, std::size_t sample_index, bool keep_batch_dim) {
  if (batch.shape.size() != 4) {
    throw std::runtime_error("slice_nchw_sample expected rank-4 NCHW input");
  }
  const std::size_t N = batch.shape[0];
  const std::size_t C = batch.shape[1];
  const std::size_t H = batch.shape[2];
  const std::size_t W = batch.shape[3];
  if (sample_index >= N) {
    throw std::runtime_error("sample_index out of range");
  }
  RawNpyU64 out;
  out.key = batch.key + std::string(keep_batch_dim ? ".sample_nchw1" : ".sample_chw");
  out.path = batch.path;
  out.descr = batch.descr;
  out.shape = keep_batch_dim ? std::vector<size_t>{1, C, H, W} : std::vector<size_t>{C, H, W};
  out.values.resize(C * H * W);
  const std::size_t base = sample_index * C * H * W;
  for (std::size_t i = 0; i < C * H * W; ++i) {
    out.values[i] = batch.values[base + i];
  }
  return out;
}

inline TensorU64 load_input_sample_tensor(const std::string& key,
                                          const std::string& path,
                                          std::size_t sample_index,
                                          bool keep_batch_dim,
                                          int scale_bits = 16) {
  auto batch = load_raw_npy_u64(key, path);
  auto sample = slice_nchw_sample(batch, sample_index, keep_batch_dim);
  return make_tensor_u64(sample, scale_bits);
}

} // namespace dazg_orbit::qahl::v645
