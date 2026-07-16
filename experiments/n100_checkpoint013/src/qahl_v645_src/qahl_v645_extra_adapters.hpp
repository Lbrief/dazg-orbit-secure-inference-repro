// DAZG-Orbit Project Source File
// Component: experiments/n100_checkpoint013/src/qahl_v645_src/qahl_v645_extra_adapters.hpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

#pragma once
#include "qahl_v645_tensor_adapters.hpp"
#include "qahl_v645_inventory.hpp"
#include <stdexcept>
#include <string>
#include <vector>

namespace dazg_orbit::qahl::v645 {

inline const PayloadEntry* find_weight_entry_extra(const std::string& key) {
  for (const auto& p : kWeights) if (key == p.key) return &p;
  return nullptr;
}

inline std::int64_t q16_i64(uint64_t x) {
  return static_cast<std::int64_t>(x);
}
inline uint64_t q16_u64(std::int64_t x) {
  return static_cast<uint64_t>(x);
}
inline uint64_t q16_mul(uint64_t a, uint64_t b) {
  __int128 prod = static_cast<__int128>(q16_i64(a)) * static_cast<__int128>(q16_i64(b));
  prod >>= 16;
  return q16_u64(static_cast<std::int64_t>(prod));
}

inline TensorU64 tensor_channel_slice(TensorU64& x, std::size_t bucket, std::size_t bucket_count) {
  auto shape = tensor_shape(x);
  if (shape.size() != 3) throw std::runtime_error("tensor_channel_slice expects CHW tensor");
  const std::size_t C = shape[0], H = shape[1], W = shape[2];
  if (bucket_count == 0 || C % bucket_count != 0 || bucket >= bucket_count) throw std::runtime_error("bad qchar bucket split");
  const std::size_t CB = C / bucket_count;
  std::vector<uint64_t> out(CB * H * W);
  auto flat = tensor_flatten_u64(x);
  for (std::size_t c=0; c<CB; ++c) {
    for (std::size_t h=0; h<H; ++h) {
      for (std::size_t w=0; w<W; ++w) {
        const std::size_t src = ((bucket * CB + c) * H + h) * W + w;
        const std::size_t dst = (c * H + h) * W + w;
        out[dst] = flat[src];
      }
    }
  }
  return make_tensor_from_u64({CB, H, W}, out, 16);
}

inline TensorU64 tensor_channel_concat4(TensorU64& a, TensorU64& b, TensorU64& c, TensorU64& d) {
  std::vector<TensorU64*> xs{&a, &b, &c, &d};
  auto s0 = tensor_shape(a);
  if (s0.size() != 3) throw std::runtime_error("concat expects CHW");
  const std::size_t CB = s0[0], H = s0[1], W = s0[2];
  std::vector<uint64_t> out(4 * CB * H * W);
  for (std::size_t bi=0; bi<4; ++bi) {
    auto s = tensor_shape(*xs[bi]);
    if (s != s0) throw std::runtime_error("concat bucket shape mismatch");
    auto flat = tensor_flatten_u64(*xs[bi]);
    for (std::size_t c=0; c<CB; ++c) {
      for (std::size_t h=0; h<H; ++h) {
        for (std::size_t w=0; w<W; ++w) {
          const std::size_t dst = (((bi * CB + c) * H + h) * W + w);
          const std::size_t src = ((c * H + h) * W + w);
          out[dst] = flat[src];
        }
      }
    }
  }
  return make_tensor_from_u64({4 * CB, H, W}, out, 16);
}

inline TensorU64 tensor_bucket_scale(TensorU64& x, std::size_t bucket) {
  const auto* p = find_weight_entry_extra("h8.1.bucket_scale");
  if (!p) throw std::runtime_error("missing h8.1.bucket_scale");
  auto scale = load_tensor_u64(p->key, p->path, 16);
  auto s = tensor_shape(x);
  if (s.size() != 3) throw std::runtime_error("bucket_scale expects CHW");
  const std::size_t C=s[0], H=s[1], W=s[2];
  auto vx = tensor_flatten_u64(x);
  std::vector<uint64_t> out(vx.size());
  auto ss = tensor_shape(scale);
  if (ss.size() != 4 || ss[0] <= bucket || ss[1] < C) throw std::runtime_error("bad bucket_scale shape");
  for (std::size_t c=0; c<C; ++c) {
    uint64_t sc = tensor_get_flat(scale, ss, ((bucket * ss[1] + c) * ss[2] + 0) * ss[3] + 0);
    for (std::size_t h=0; h<H; ++h) {
      for (std::size_t w=0; w<W; ++w) {
        const std::size_t idx = (c * H + h) * W + w;
        out[idx] = q16_mul(vx[idx], sc);
      }
    }
  }
  return make_tensor_from_u64(s, out, 16);
}

inline TensorU64 tensor_avgpool_hw(TensorU64& x) {
  auto s = tensor_shape(x);
  if (s.size() != 3) throw std::runtime_error("avgpool expects CHW");
  const std::size_t C=s[0], H=s[1], W=s[2];
  auto vx = tensor_flatten_u64(x);
  std::vector<uint64_t> out(C);
  const std::int64_t denom = static_cast<std::int64_t>(H * W);
  for (std::size_t c=0; c<C; ++c) {
    __int128 sum = 0;
    for (std::size_t h=0; h<H; ++h)
      for (std::size_t w=0; w<W; ++w)
        sum += static_cast<__int128>(q16_i64(vx[(c * H + h) * W + w]));
    out[c] = q16_u64(static_cast<std::int64_t>(sum / denom));
  }
  return make_tensor_from_u64({C}, out, 16);
}

inline TensorU64 tensor_as_linear_row(TensorU64& x) {
  auto s = tensor_shape(x);
  auto flat = tensor_flatten_u64(x);
  if (s.size() == 1) return make_tensor_from_u64({1, s[0]}, flat, 16);
  if (s.size() == 2) return tensor_identity(x);
  throw std::runtime_error("linear row adapter expects rank 1 or 2");
}

} // namespace dazg_orbit::qahl::v645
