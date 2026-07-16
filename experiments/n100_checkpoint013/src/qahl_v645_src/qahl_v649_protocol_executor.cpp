// DAZG-Orbit Project Source File
// Component: experiments/n100_checkpoint013/src/qahl_v645_src/qahl_v649_protocol_executor.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#define DAZG_ORBIT_V673_FULLGRAPH_STEM0_PATCH_ACTIVE 1
#define main qahl_v645_original_main
#include "qahl_v645_paired_fullstage_executor.cpp"
#undef main

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <cstdlib>

using namespace dazg_orbit::qahl::v645;
using namespace dazg_orbit::qahl::v645::resolved;
namespace fs = std::filesystem;

static Utils::NetIO* v649_io = nullptr;
static std::string v649_role = "server";
static std::string v649_adapter_policy = "native";
static uint64_t v649_plain_mod = 0;
static int v649_reveal_count = 0;
static uint64_t v649_reveal_elements = 0;

template <class T, class = void>
struct v649_has_plain_mod_field : std::false_type {};
template <class T>
struct v649_has_plain_mod_field<T, std::void_t<decltype(std::declval<T&>().plain_mod)>>
    : std::true_type {};

static std::string v649_esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') { o.push_back('\\'); o.push_back(c); }
    else if (c == '\n') o += "\\n";
    else o.push_back(c);
  }
  return o;
}

static std::string v649_shape_json(const std::vector<size_t>& s) {
  std::ostringstream oss;
  oss << "[";
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (i) oss << ", ";
    oss << s[i];
  }
  oss << "]";
  return oss.str();
}

static std::size_t v649_nelem(const std::vector<size_t>& s) {
  if (s.empty()) return 0;
  std::size_t n = 1;
  for (auto v : s) n *= v;
  return n;
}

static uint64_t v649_context_plain_mod(HEEvaluator* he) {
  if constexpr (v649_has_plain_mod_field<HEEvaluator>::value) {
    const uint64_t from_context = static_cast<uint64_t>(he->plain_mod);
    if (from_context != 0) return from_context;
  }
  const int bits = dazg_orbit_v675_selected_plain_bits > 0
      ? dazg_orbit_v675_selected_plain_bits
      : 32;
  return static_cast<uint64_t>(seal::PlainModulus::Batching(8192, bits).value());
}

static uint64_t v649_residue_from_i64(std::int64_t x, uint64_t p) {
  const __int128 m = static_cast<__int128>(p);
  __int128 r = static_cast<__int128>(x) % m;
  if (r < 0) r += m;
  return static_cast<uint64_t>(r);
}

static std::int64_t v649_centered_i64(uint64_t x, uint64_t p) {
  x %= p;
  if (x > p / 2) {
    return static_cast<std::int64_t>(
        static_cast<__int128>(x) - static_cast<__int128>(p));
  }
  return static_cast<std::int64_t>(x);
}

static void v649_write_npy_u64(const std::string& path,
                               const std::vector<size_t>& shape,
                               const std::vector<uint64_t>& data) {
  std::ofstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot write npy: " + path);
  std::ostringstream hs;
  hs << "{'descr': '<u8', 'fortran_order': False, 'shape': (";
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i) hs << ", ";
    hs << shape[i];
  }
  if (shape.size() == 1) hs << ",";
  hs << "), }";
  std::string header = hs.str();
  std::size_t pad = (16 - ((10 + header.size() + 1) % 16)) % 16;
  header.append(pad, ' ');
  header.push_back('\n');
  const char magic[6] = {
      static_cast<char>(0x93), 'N', 'U', 'M', 'P', 'Y'
  };
  f.write(magic, 6);
  char ver[2] = {1, 0};
  f.write(ver, 2);
  uint16_t h = static_cast<uint16_t>(header.size());
  char hb[2] = {
      static_cast<char>(h & 0xff),
      static_cast<char>((h >> 8) & 0xff)
  };
  f.write(hb, 2);
  f.write(header.data(), static_cast<std::streamsize>(header.size()));
  if (!data.empty()) {
    f.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size() * sizeof(uint64_t)));
  }
}

static TensorU64 v649_make_server_owned_tensor(
    const std::vector<size_t>& shape,
    const std::vector<uint64_t>& signed_raw,
    const std::string& role,
    uint64_t p) {
  std::vector<uint64_t> share(signed_raw.size(), 0);
  if (role == "server") {
    for (std::size_t i = 0; i < signed_raw.size(); ++i) {
      share[i] = v649_residue_from_i64(
          static_cast<std::int64_t>(signed_raw[i]), p);
    }
  }
  return make_tensor_from_u64(shape, share, 16);
}

// DAZG_ORBIT_V677_WEIGHT_MODP_NORMALIZATION_PATCH_BEGIN
static bool v677_weight_modp_enabled() {
  const char* e = std::getenv("DAZG_ORBIT_V677_DISABLE_WEIGHT_MODP");
  return !(e && (std::string(e) == "1" || std::string(e) == "true" ||
                 std::string(e) == "TRUE" || std::string(e) == "yes"));
}

// DAZG_ORBIT_V678_BIAS_Q32_PROMOTION_PATCH_BEGIN
static bool v678_bias_q32_promotion_enabled() {
  const char* e = std::getenv("DAZG_ORBIT_V678_DISABLE_BIAS_Q32_PROMOTION");
  return !(e && (std::string(e) == "1" || std::string(e) == "true" ||
                 std::string(e) == "TRUE" || std::string(e) == "yes"));
}

static bool v678_key_ends_with(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static bool v678_q32_bridge_requested_by_env() {
  const char* e = std::getenv("DAZG_ORBIT_V671_ENABLE_Q32_TRUNC_BRIDGE");
  return e && std::string(e) != "0" && std::string(e) != "false" &&
         std::string(e) != "FALSE";
}

static uint64_t v678_residue_from_i128(__int128 x, uint64_t p) {
  const __int128 m = static_cast<__int128>(p);
  __int128 r = x % m;
  if (r < 0) r += m;
  return static_cast<uint64_t>(r);
}

static bool v678_promote_this_bias_to_q32(const std::string& key) {
  return v678_bias_q32_promotion_enabled() &&
         v678_q32_bridge_requested_by_env() &&
         v678_key_ends_with(key, ".bias");
}
// DAZG_ORBIT_V678_BIAS_Q32_PROMOTION_PATCH_END

static TensorU64 v677_normalize_signed_raw_tensor_to_plain_mod(
    TensorU64& raw_tensor,
    const std::string& key) {
  auto s = tensor_shape(raw_tensor);
  auto raw = tensor_flatten_u64(raw_tensor);
  if (!v677_weight_modp_enabled() || v649_plain_mod == 0) {
    return make_tensor_from_u64(s, raw, 16);
  }
  const bool promote_bias = v678_promote_this_bias_to_q32(key);
  std::vector<uint64_t> out(raw.size(), 0);

  // v678: Conv2D/Linear emit Q32 accumulators and the v671 bridge divides by
  // 2^16 afterwards. A Q16 bias must therefore enter those encrypted operators
  // as bias_q16 << 16. In this diagnostic reveal path, bridge outputs are
  // server-owned, so the public bias constant is injected on the server side
  // only; the client keeps a zero bias share. This repairs the stable v677
  // first-conv missing-bias offset without creating a double constant.
  if (promote_bias && v649_role != "server") {
    return make_tensor_from_u64(s, out, 16);
  }

  for (std::size_t i = 0; i < raw.size(); ++i) {
    __int128 signed_raw = static_cast<std::int64_t>(raw[i]);
    if (promote_bias) signed_raw *= static_cast<__int128>(65536);
    out[i] = v678_residue_from_i128(signed_raw, v649_plain_mod);
  }
  return make_tensor_from_u64(s, out, 16);
}

static TensorU64 v677_load_weight_tensor_modp(const std::string& key) {
  auto raw_tensor = load_weight_tensor(key);
  return v677_normalize_signed_raw_tensor_to_plain_mod(raw_tensor, key);
}

static TensorU64 v677_run_conv_named(
    HEEvaluator* he,
    TensorU64& x,
    const std::string& name,
    int feature,
    int stride,
    int padding,
    int kernel) {
  auto w = v677_load_weight_tensor_modp(name + ".weight");
  auto b = v677_load_weight_tensor_modp(name + ".bias");
  const int block = adaptive_channel_block_for(name.c_str(), 1);
  CirLayoutPlan layout;
  Conv2D op(static_cast<uint64_t>(feature),
            static_cast<uint64_t>(stride),
            static_cast<uint64_t>(padding),
            static_cast<uint64_t>(block),
            w, b, he,
            stride == 2 && kernel == 3,
            stride == 1 && kernel == 3,
            layout);
  return conv_forward(op, x);
}
// DAZG_ORBIT_V677_WEIGHT_MODP_NORMALIZATION_PATCH_END

static TensorU64 v649_load_input_server_owned(
    const std::string& key,
    const std::string& path,
    std::size_t sample_index,
    const std::string& role,
    uint64_t p) {
  auto batch = load_raw_npy_u64(key, path);
  auto sample = slice_nchw_sample(batch, sample_index, false);
  return v649_make_server_owned_tensor(
      sample.shape, sample.values, role, p);
}

static TensorU64 v649_synthetic_feature_server_owned(
    const std::string& role, uint64_t p) {
  std::vector<uint64_t> raw(192);
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const std::int64_t q =
        static_cast<std::int64_t>((static_cast<int>(i % 17) - 8) * 257);
    raw[i] = static_cast<uint64_t>(q);
  }
  return v649_make_server_owned_tensor({192}, raw, role, p);
}

static TensorU64 v649_mod_add(TensorU64& a, TensorU64& b) {
  auto sa = tensor_shape(a);
  auto sb = tensor_shape(b);
  if (sa != sb) throw std::runtime_error("v649 modular add shape mismatch");
  auto va = tensor_flatten_u64(a);
  auto vb = tensor_flatten_u64(b);
  for (std::size_t i = 0; i < va.size(); ++i) {
    const __int128 z =
        static_cast<__int128>(va[i] % v649_plain_mod) +
        static_cast<__int128>(vb[i] % v649_plain_mod);
    va[i] = static_cast<uint64_t>(
        z % static_cast<__int128>(v649_plain_mod));
  }
  return make_tensor_from_u64(sa, va, 16);
}

template <class Fn>
static TensorU64 v649_reveal_apply_reshare(
    TensorU64& local_share,
    const std::string& stage,
    Fn&& fn) {
  if (!v649_io) throw std::runtime_error("reveal adapter missing NetIO");
  auto in_shape = tensor_shape(local_share);
  auto local = tensor_flatten_u64(local_share);
  const uint64_t n = static_cast<uint64_t>(local.size());
  if (n > static_cast<uint64_t>(
              std::numeric_limits<int>::max() / sizeof(uint64_t))) {
    throw std::runtime_error("reveal tensor too large");
  }

  ++v649_reveal_count;
  v649_reveal_elements += n;

  if (v649_role == "client") {
    v649_io->send_data(&n, static_cast<int>(sizeof(n)));
    if (n) {
      v649_io->send_data(
          local.data(),
          static_cast<int>(n * sizeof(uint64_t)));
    }
    v649_io->flush();

    std::vector<uint64_t> zero_in(static_cast<std::size_t>(n), 0);
    auto zero_tensor = make_tensor_from_u64(in_shape, zero_in, 16);
    auto zero_out = fn(zero_tensor);
    auto out_shape = tensor_shape(zero_out);
    std::vector<uint64_t> zero_out_flat(v649_nelem(out_shape), 0);
    return make_tensor_from_u64(out_shape, zero_out_flat, 16);
  }

  uint64_t peer_n = 0;
  v649_io->recv_data(&peer_n, static_cast<int>(sizeof(peer_n)));
  if (peer_n != n) {
    throw std::runtime_error(
        "reveal share length mismatch at " + stage);
  }
  std::vector<uint64_t> peer(static_cast<std::size_t>(n), 0);
  if (n) {
    v649_io->recv_data(
        peer.data(),
        static_cast<int>(n * sizeof(uint64_t)));
  }

  std::vector<uint64_t> signed_plain(static_cast<std::size_t>(n), 0);
  for (std::size_t i = 0; i < signed_plain.size(); ++i) {
    const __int128 z =
        static_cast<__int128>(local[i] % v649_plain_mod) +
        static_cast<__int128>(peer[i] % v649_plain_mod);
    const uint64_t r = static_cast<uint64_t>(
        z % static_cast<__int128>(v649_plain_mod));
    signed_plain[i] =
        static_cast<uint64_t>(v649_centered_i64(r, v649_plain_mod));
  }

  auto plain_tensor =
      make_tensor_from_u64(in_shape, signed_plain, 16);
  auto plain_out = fn(plain_tensor);
  auto out_shape = tensor_shape(plain_out);
  auto raw_out = tensor_flatten_u64(plain_out);
  for (auto& x : raw_out) {
    x = v649_residue_from_i64(
        static_cast<std::int64_t>(x), v649_plain_mod);
  }
  return make_tensor_from_u64(out_shape, raw_out, 16);
}

static TensorU64 v649_gelu(TensorU64& x, const std::string& stage) {
  if (v649_adapter_policy == "reveal") {
    return v649_reveal_apply_reshare(
        x, stage,
        [](TensorU64& p) { return tensor_gelu_adapter(p); });
  }
  return tensor_gelu_adapter(x);
}

static TensorU64 v649_bucket_scale(
    TensorU64& x, std::size_t bucket, const std::string& stage) {
  if (v649_adapter_policy == "reveal") {
    return v649_reveal_apply_reshare(
        x, stage,
        [bucket](TensorU64& p) {
          return tensor_bucket_scale(p, bucket);
        });
  }
  return tensor_bucket_scale(x, bucket);
}

static TensorU64 v649_avgpool(TensorU64& x, const std::string& stage) {
  if (v649_adapter_policy == "reveal") {
    return v649_reveal_apply_reshare(
        x, stage,
        [](TensorU64& p) { return tensor_avgpool_hw(p); });
  }
  return tensor_avgpool_hw(x);
}

static TensorU64 v649_transposed_head_weight() {
  auto w = v677_load_weight_tensor_modp("head.2.weight");
  auto s = tensor_shape(w);
  if (s.size() != 2 || s[0] != 100 || s[1] != 192) {
    throw std::runtime_error("bad head weight shape");
  }
  auto flat = tensor_flatten_u64(w);
  std::vector<uint64_t> out(192 * 100);
  for (std::size_t o = 0; o < 100; ++o) {
    for (std::size_t i = 0; i < 192; ++i) {
      out[i * 100 + o] = flat[o * 192 + i];
    }
  }
  return make_tensor_from_u64({192, 100}, out, 16);
}

// DAZG_ORBIT_V671_Q32_TRUNC_BRIDGE_PATCH_BEGIN
static bool v671_q32_trunc_bridge_enabled() {
  const char* e = std::getenv("DAZG_ORBIT_V671_ENABLE_Q32_TRUNC_BRIDGE");
  return e && std::string(e) != "0" && std::string(e) != "false" &&
         std::string(e) != "FALSE";
}

static std::int64_t v671_div_q16(std::int64_t x) {
  const std::int64_t q = 65536LL;
  const char* mode = std::getenv("DAZG_ORBIT_V671_TRUNC_MODE");
  const bool use_floor = mode && std::string(mode) == "floor";
  if (!use_floor) return x / q;
  if (x >= 0) return x / q;
  return -(((-x) + q - 1) / q);
}

static TensorU64 v671_q32_to_q16_plain_tensor(TensorU64& x) {
  auto s = tensor_shape(x);
  auto raw = tensor_flatten_u64(x);
  std::vector<uint64_t> out(raw.size(), 0);

  // DAZG_ORBIT_V676_SIGNED_PLAIN_Q32_BRIDGE_FIX:
  // v649_reveal_apply_reshare() gives this plain adapter two's-complement
  // signed int64 values, not p-residues.  The v675 code re-centered a
  // uint64 raw negative through the plaintext modulus, corrupting negatives
  // because uint64_t(-x) % p depends on 2^64 mod p.  Interpret raw[i] as
  // int64 first, divide Q32 -> Q16, then return signed raw.  The surrounding
  // reveal/reshare wrapper converts signed raw output back into the HE
  // plaintext modulus exactly once.
  for (std::size_t i = 0; i < raw.size(); ++i) {
    const std::int64_t signed_q32 =
        static_cast<std::int64_t>(raw[i]);
    const std::int64_t y = v671_div_q16(signed_q32);
    out[i] = static_cast<uint64_t>(y);
  }
  return make_tensor_from_u64(s, out, 16);
}

static TensorU64 v671_q32_boundary_bridge(
    TensorU64& x,
    const std::string& stage) {
  if (!v671_q32_trunc_bridge_enabled()) return x;
  return v649_reveal_apply_reshare(
      x,
      std::string("v671.q32_to_q16.") + stage,
      [](TensorU64& p) { return v671_q32_to_q16_plain_tensor(p); });
}
// DAZG_ORBIT_V671_Q32_TRUNC_BRIDGE_PATCH_END

static TensorU64 v649_linear_head(HEEvaluator* he, TensorU64& feature) {
  auto flat = tensor_flatten_u64(feature);
  if (flat.size() != 192) {
    throw std::runtime_error("head feature must have 192 elements");
  }
  auto row = make_tensor_from_u64({1, 192}, flat, 16);
  auto w = v649_transposed_head_weight();
  auto b = v677_load_weight_tensor_modp("head.2.bias");
  LinearNest op(1, 4, w, b, he);
  auto v671_head_logits = linear_forward(op, row);
  // DAZG_ORBIT_V671_LINEAR_HEAD_PATCH
  return v671_q32_boundary_bridge(v671_head_logits, "head.linear");
}

static TensorU64 v649_unit_block(
    HEEvaluator* he,
    TensorU64& input,
    const std::string& conv0,
    const std::string& conv1,
    int feature,
    int stride0,
    int padding0,
    int kernel0,
    const std::string& stage) {
  auto residual = tensor_identity(input);
  auto branch = v677_run_conv_named(
      he, input, conv0, feature, stride0, padding0, kernel0);
  branch = v671_q32_boundary_bridge(branch, "conv_000");
  branch = v649_gelu(branch, stage + ".gelu");
  branch = v677_run_conv_named(
      he, branch, conv1, feature, 1, 0, 1);
  branch = v671_q32_boundary_bridge(branch, "conv_001");
  return v649_mod_add(branch, residual);
}

static TensorU64 v649_transition_block(
    HEEvaluator* he,
    TensorU64& input,
    const std::string& main0,
    const std::string& main3,
    const std::string& skip_name,
    const std::string& tail0,
    const std::string& tail3,
    int in_feature,
    int out_feature,
    const std::string& stage) {
  auto base = tensor_identity(input);
  auto main = v677_run_conv_named(
      he, base, main0, in_feature, 2, 1, 3);
  main = v671_q32_boundary_bridge(main, "conv_002");
  main = v649_gelu(main, stage + ".main_gelu");
  main = v677_run_conv_named(
      he, main, main3, out_feature, 1, 0, 1);
  main = v671_q32_boundary_bridge(main, "conv_003");
  auto skip = v677_run_conv_named(
      he, base, skip_name, in_feature, 2, 0, 1);
  skip = v671_q32_boundary_bridge(skip, "conv_004");
  auto current = v649_mod_add(main, skip);
  current = v649_unit_block(
      he, current, tail0, tail3, out_feature,
      1, 0, 1, stage + ".tail");
  return current;
}

static TensorU64 v649_h8_qchar(
    HEEvaluator* he, TensorU64& input) {
  auto b0 = tensor_channel_slice(input, 0, 4);
  auto b1 = tensor_channel_slice(input, 1, 4);
  auto b2 = tensor_channel_slice(input, 2, 4);
  auto b3 = tensor_channel_slice(input, 3, 4);

  b0 = v677_run_conv_named(
      he, b0, "h8.1.local.0.conv", 8, 1, 0, 1);
  b0 = v671_q32_boundary_bridge(b0, "conv_005");
  b0 = v649_bucket_scale(b0, 0, "h8.1.bucket0.scale");

  b1 = v677_run_conv_named(
      he, b1, "h8.1.local.1.conv", 8, 1, 0, 1);
  b1 = v671_q32_boundary_bridge(b1, "conv_006");
  b1 = v649_bucket_scale(b1, 1, "h8.1.bucket1.scale");

  b2 = v677_run_conv_named(
      he, b2, "h8.1.local.2.conv", 8, 1, 0, 1);
  b2 = v671_q32_boundary_bridge(b2, "conv_007");
  b2 = v649_bucket_scale(b2, 2, "h8.1.bucket2.scale");

  b3 = v677_run_conv_named(
      he, b3, "h8.1.local.3.conv", 8, 1, 0, 1);
  b3 = v671_q32_boundary_bridge(b3, "conv_008");
  b3 = v649_bucket_scale(b3, 3, "h8.1.bucket3.scale");

  auto cat = tensor_channel_concat4(b0, b1, b2, b3);
  cat = v649_gelu(cat, "h8.1.qchar.gelu");
  auto v671_conv_ret_009 = v677_run_conv_named(
      he, cat, "h8.1.mix.conv", 8, 1, 0, 1);
  return v671_q32_boundary_bridge(v671_conv_ret_009, "conv_009");
}

static TensorU64 v649_run_full_prefix(
    HEEvaluator* he,
    TensorU64& x0,
    int& conv_count,
    int& gelu_count,
    int& add_count,
    int& qsplit_count,
    int& bscale_count,
    int& avgpool_count) {
  g_phase = "conv_stem_0";
  auto current =
      v677_run_conv_named(he, x0, "stem.0", 32, 1, 1, 3);
  // DAZG_ORBIT_V673_STEM0_FULLPREFIX_BRIDGE: full-sample stem.0 must cross Q32->Q16 before GeLU.
  current = v671_q32_boundary_bridge(current, "conv_000_stem0_fullprefix");
  ++conv_count;

  g_phase = "gelu_stem_1";
  current = v649_gelu(current, "stem.1.gelu");
  ++gelu_count;

  g_phase = "conv_stem_2";
  current = v677_run_conv_named(
      he, current, "stem.2.conv", 32, 1, 0, 1);
  current = v671_q32_boundary_bridge(current, "conv_010");
  ++conv_count;

  g_phase = "stem_3_block";
  current = v649_unit_block(
      he, current,
      "stem.3.net.0.conv", "stem.3.net.3.conv",
      32, 1, 0, 1, "stem.3");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h32_0_body_block";
  current = v649_unit_block(
      he, current,
      "h32.0.body.0", "h32.0.body.3.conv",
      32, 1, 1, 3, "h32.0.body");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h32_0_anchor_block";
  current = v649_unit_block(
      he, current,
      "h32.0.anchor.net.0.conv",
      "h32.0.anchor.net.3.conv",
      32, 1, 0, 1, "h32.0.anchor");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h32_1_block";
  current = v649_unit_block(
      he, current,
      "h32.1.net.0.conv", "h32.1.net.3.conv",
      32, 1, 0, 1, "h32.1");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "to_h16_transition";
  current = v649_transition_block(
      he, current,
      "to_h16.main.0", "to_h16.main.3.conv",
      "to_h16.skip",
      "to_h16.tail.net.0.conv",
      "to_h16.tail.net.3.conv",
      32, 16, "to_h16");
  conv_count += 5; gelu_count += 2; add_count += 2;

  g_phase = "h16_0_body_block";
  current = v649_unit_block(
      he, current,
      "h16.0.body.0", "h16.0.body.3.conv",
      16, 1, 1, 3, "h16.0.body");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h16_0_anchor_block";
  current = v649_unit_block(
      he, current,
      "h16.0.anchor.net.0.conv",
      "h16.0.anchor.net.3.conv",
      16, 1, 0, 1, "h16.0.anchor");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h16_1_block";
  current = v649_unit_block(
      he, current,
      "h16.1.net.0.conv", "h16.1.net.3.conv",
      16, 1, 0, 1, "h16.1");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "to_h8_transition";
  current = v649_transition_block(
      he, current,
      "to_h8.main.0", "to_h8.main.3.conv",
      "to_h8.skip",
      "to_h8.tail.net.0.conv",
      "to_h8.tail.net.3.conv",
      16, 8, "to_h8");
  conv_count += 5; gelu_count += 2; add_count += 2;

  g_phase = "h8_0_body_block";
  current = v649_unit_block(
      he, current,
      "h8.0.body.0", "h8.0.body.3.conv",
      8, 1, 1, 3, "h8.0.body");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h8_0_anchor_block";
  current = v649_unit_block(
      he, current,
      "h8.0.anchor.net.0.conv",
      "h8.0.anchor.net.3.conv",
      8, 1, 0, 1, "h8.0.anchor");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h8_1_qchar_block";
  current = v649_h8_qchar(he, current);
  conv_count += 5; gelu_count += 1;
  qsplit_count += 4; bscale_count += 4;

  g_phase = "h8_2_block";
  current = v649_unit_block(
      he, current,
      "h8.2.net.0.conv", "h8.2.net.3.conv",
      8, 1, 0, 1, "h8.2");
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "head_avgpool";
  current = v649_avgpool(current, "head.avgpool");
  ++avgpool_count;
  return current;
}

int main(int argc, char** argv) {
  std::string out_report =
      "qahl_v649_protocol_executor_report.json";
  std::string out_tensor;
  std::string out_logits;
  std::string out_feature;
  std::string input_tensor;
  std::string role = "server";
  std::string address = "127.0.0.1";
  std::string mode = "full-sample";
  std::string input_owner = "server";
  std::string adapter_policy = "native";
  int party = 1;
  int port = 42480;
  std::size_t sample_index = 0;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if ((a == "--out-report" || a == "--out") &&
        i + 1 < argc) out_report = argv[++i];
    else if (a == "--out-tensor" && i + 1 < argc)
      out_tensor = argv[++i];
    else if (a == "--out-logits" && i + 1 < argc)
      out_logits = argv[++i];
    else if (a == "--out-feature" && i + 1 < argc)
      out_feature = argv[++i];
    else if (a == "--input-tensor" && i + 1 < argc)
      input_tensor = argv[++i];
    else if (a == "--role" && i + 1 < argc)
      role = argv[++i];
    else if (a == "--party" && i + 1 < argc)
      party = std::stoi(argv[++i]);
    else if (a == "--address" && i + 1 < argc)
      address = argv[++i];
    else if (a == "--port" && i + 1 < argc)
      port = std::stoi(argv[++i]);
    else if (a == "--mode" && i + 1 < argc)
      mode = argv[++i];
    else if (a == "--input-owner" && i + 1 < argc)
      input_owner = argv[++i];
    else if (a == "--adapter-policy" && i + 1 < argc)
      adapter_policy = argv[++i];
    else if ((a == "--sample-index" ||
              a == "--sample_index") &&
             i + 1 < argc)
      sample_index =
          static_cast<std::size_t>(std::stoul(argv[++i]));
  }

  g_report_path = out_report;
  g_role = role;
  g_prefix = mode;
  v649_role = role;
  v649_adapter_policy = adapter_policy;
  install_signal_handlers();

  auto t0 = std::chrono::steady_clock::now();
  std::vector<std::string> issues;
  bool netio_ok = false;
  bool context_ok = false;
  bool keygen_ok = false;
  bool input_ok = false;
  bool output_ready = false;
  bool wrote_tensor = false;
  bool wrote_logits = false;
  bool wrote_feature = false;
  bool party_contract_ok = false;
  bool input_owner_ok = false;
  bool plain_mod_matches_deterministic = false;
  std::string plain_mod_source = "not_initialized";
  std::string status = "blocked";
  std::vector<size_t> output_shape;
  std::vector<size_t> feature_shape;
  std::vector<uint64_t> output_values;
  std::vector<uint64_t> feature_values;
  int conv_count = 0;
  int gelu_count = 0;
  int add_count = 0;
  int qsplit_count = 0;
  int bscale_count = 0;
  int avgpool_count = 0;
  int linear_count = 0;

  try {
    if (mode == "plain-gelu-file") {
      if (input_tensor.empty()) {
        throw std::runtime_error(
            "--input-tensor required for plain-gelu-file");
      }
      v649_plain_mod = static_cast<uint64_t>(
          seal::PlainModulus::Batching(8192, 32).value());
      plain_mod_source = "seal_plain_modulus_batching_8192_32";
      plain_mod_matches_deterministic = true;
      auto raw = load_raw_npy_u64(
          "v649_plain_gelu_input", input_tensor);
      auto x = make_tensor_u64(raw, 16);
      input_ok = true;
      g_phase = "plain_gelu_eval";
      auto y = tensor_gelu_adapter(x);
      ++gelu_count;
      output_shape = tensor_shape(y);
      output_values = tensor_flatten_u64(y);
      output_ready = true;
      status = "plain_gelu_ready";
    } else {
      const int required_party =
          role == "server" ? 1 : (role == "client" ? 2 : -1);
      if (required_party < 0 || party != required_party) {
        throw std::runtime_error(
            "party contract violation: server must be 1 and client must be 2");
      }
      party_contract_ok = true;
      if (input_owner != "server") {
        throw std::runtime_error(
            "v649 only accepts server-owned real input; client must be zero");
      }
      input_owner_ok = true;
      if (adapter_policy != "native" &&
          adapter_policy != "reveal") {
        throw std::runtime_error(
            "adapter policy must be native or reveal");
      }

      g_phase = "construct_netio";
      auto io = make_io(role, address, port);
      v649_io = io.get();
      netio_ok = true;

      g_phase = "construct_he_context";
      auto he = make_he(io.get(), party);
      context_ok = true;
      v649_plain_mod = v649_context_plain_mod(he.get());
      const int v675_bits = dazg_orbit_v675_selected_plain_bits > 0
          ? dazg_orbit_v675_selected_plain_bits
          : 32;
      const uint64_t deterministic =
          static_cast<uint64_t>(
              seal::PlainModulus::Batching(8192, v675_bits).value());
      plain_mod_matches_deterministic =
          v649_plain_mod == deterministic;
      plain_mod_source =
          v649_has_plain_mod_field<HEEvaluator>::value
              ? "he_context_plain_mod"
              : "constructor_equivalent_seal_batching_v675";

      g_phase = "GenerateNewKey";
      he->GenerateNewKey();
      keygen_ok = true;

      if (mode == "head-contract") {
        auto feature = v649_synthetic_feature_server_owned(
            role, v649_plain_mod);
        feature_shape = tensor_shape(feature);
        input_ok = true;
        g_phase = "head_contract_he_linear";
        auto y = v649_linear_head(he.get(), feature);
        ++linear_count;
        output_shape = tensor_shape(y);
        output_values = tensor_flatten_u64(y);
        output_ready = v649_nelem(output_shape) == 100;
        status = output_ready
            ? "head_share_ready"
            : "head_shape_not_100";
      } else {
        const auto* input_p =
            find_ref("qahl_ref_input_n10.npy");
        if (!input_p) {
          throw std::runtime_error(
              "missing qahl_ref_input_n10.npy");
        }
        auto x0 = v649_load_input_server_owned(
            input_p->key, input_p->path,
            sample_index, role, v649_plain_mod);
        input_ok = true;

        if (mode == "first-conv") {
          g_phase = "first_conv";
          auto y = v677_run_conv_named(
              he.get(), x0, "stem.0", 32, 1, 1, 3);
          y = v671_q32_boundary_bridge(y, "conv_011");
          ++conv_count;
          output_shape = tensor_shape(y);
          output_values = tensor_flatten_u64(y);
          output_ready = true;
          status = "first_conv_share_ready";
        } else if (mode == "first-gelu") {
          g_phase = "first_conv";
          auto y = v677_run_conv_named(
              he.get(), x0, "stem.0", 32, 1, 1, 3);
          y = v671_q32_boundary_bridge(y, "conv_012");
          ++conv_count;
          g_phase = "first_gelu";
          y = v649_gelu(y, "stem.1.gelu");
          ++gelu_count;
          output_shape = tensor_shape(y);
          output_values = tensor_flatten_u64(y);
          output_ready = true;
          status = "first_gelu_share_ready";
        } else if (mode == "full-sample") {
          auto feature = v649_run_full_prefix(
              he.get(), x0,
              conv_count, gelu_count, add_count,
              qsplit_count, bscale_count, avgpool_count);
          feature_shape = tensor_shape(feature);
          feature_values = tensor_flatten_u64(feature);
          if (!out_feature.empty()) {
            v649_write_npy_u64(
                out_feature, feature_shape, feature_values);
            wrote_feature = true;
          }
          g_phase = "head_he_linear";
          auto y = v649_linear_head(he.get(), feature);
          ++linear_count;
          output_shape = tensor_shape(y);
          output_values = tensor_flatten_u64(y);
          output_ready = v649_nelem(output_shape) == 100;
          status = output_ready
              ? "full_sample_share_logits_ready"
              : "full_sample_head_shape_not_100";
        } else {
          throw std::runtime_error(
              "unknown mode: " + mode);
        }
      }
    }

    if (output_ready && !out_tensor.empty()) {
      v649_write_npy_u64(
          out_tensor, output_shape, output_values);
      wrote_tensor = true;
    }
    if (output_ready &&
        v649_nelem(output_shape) == 100 &&
        !out_logits.empty()) {
      v649_write_npy_u64(
          out_logits, output_shape, output_values);
      wrote_logits = true;
    }
  } catch (const std::exception& e) {
    issues.push_back(e.what());
    status = "blocked";
  }

  auto t1 = std::chrono::steady_clock::now();
  const double wall_ms =
      std::chrono::duration<double, std::milli>(
          t1 - t0).count();

  std::ofstream out(out_report);
  out << "{\n";
  out << "  \"schema\": \"dazg_orbit.qahl.v649.protocol_executor_report\",\n";
  out << "  \"status\": \"" << v649_esc(status) << "\",\n";
  out << "  \"role\": \"" << v649_esc(role) << "\",\n";
  out << "  \"party\": " << party << ",\n";
  out << "  \"party_contract_ok\": "
      << (party_contract_ok ? "true" : "false") << ",\n";
  out << "  \"mode\": \"" << v649_esc(mode) << "\",\n";
  out << "  \"input_owner\": \"" << v649_esc(input_owner) << "\",\n";
  out << "  \"input_owner_ok\": "
      << (input_owner_ok ? "true" : "false") << ",\n";
  out << "  \"adapter_policy\": \""
      << v649_esc(adapter_policy) << "\",\n";
  out << "  \"diagnostic_reveal\": "
      << (adapter_policy == "reveal" ? "true" : "false")
      << ",\n";
  out << "  \"plain_mod\": " << v649_plain_mod << ",\n";
  out << "  \"v677_weight_modp_normalization\": "
      << (v677_weight_modp_enabled() ? "true" : "false")
      << ",\n";
  out << "  \"v678_bias_q32_promotion\": "
      << (v678_bias_q32_promotion_enabled() ? "true" : "false")
      << ",\n";
  out << "  \"v678_bias_q32_server_owned\": true,\n";
  out << "  \"plain_mod_source\": \""
      << v649_esc(plain_mod_source) << "\",\n";
  out << "  \"plain_mod_matches_seal_batching\": "
      << (plain_mod_matches_deterministic ? "true" : "false")
      << ",\n";
  out << "  \"sample_index\": " << sample_index << ",\n";
  out << "  \"netio_ok\": "
      << (netio_ok ? "true" : "false") << ",\n";
  out << "  \"context_ok\": "
      << (context_ok ? "true" : "false") << ",\n";
  out << "  \"keygen_ok\": "
      << (keygen_ok ? "true" : "false") << ",\n";
  out << "  \"input_ok\": "
      << (input_ok ? "true" : "false") << ",\n";
  out << "  \"conv_count\": " << conv_count << ",\n";
  out << "  \"gelu_count\": " << gelu_count << ",\n";
  out << "  \"residual_add_count\": " << add_count << ",\n";
  out << "  \"qchar_split_count\": " << qsplit_count << ",\n";
  out << "  \"bucket_scale_count\": " << bscale_count << ",\n";
  out << "  \"avgpool_count\": " << avgpool_count << ",\n";
  out << "  \"linear_count\": " << linear_count << ",\n";
  out << "  \"diagnostic_reveal_count\": "
      << v649_reveal_count << ",\n";
  out << "  \"diagnostic_reveal_elements\": "
      << v649_reveal_elements << ",\n";
  out << "  \"last_phase\": \""
      << v649_esc(g_phase) << "\",\n";
  out << "  \"feature_shape\": "
      << v649_shape_json(feature_shape) << ",\n";
  out << "  \"output_shape\": "
      << v649_shape_json(output_shape) << ",\n";
  out << "  \"output_elements\": "
      << v649_nelem(output_shape) << ",\n";
  out << "  \"output_ready\": "
      << (output_ready ? "true" : "false") << ",\n";
  out << "  \"wrote_tensor\": "
      << (wrote_tensor ? "true" : "false") << ",\n";
  out << "  \"wrote_logits\": "
      << (wrote_logits ? "true" : "false") << ",\n";
  out << "  \"wrote_feature\": "
      << (wrote_feature ? "true" : "false") << ",\n";
  out << "  \"wall_ms\": " << wall_ms << ",\n";
  out << "  \"output_share_only\": "
      << (mode == "plain-gelu-file" ? "false" : "true")
      << ",\n";
  out << "  \"single_party_reference_compare_forbidden\": true,\n";
  out << "  \"he_exact_done\": false,\n";
  out << "  \"strict_hash_match_all\": null,\n";
  out << "  \"reference_echo\": false,\n";
  out << "  \"issues\": [";
  for (std::size_t i = 0; i < issues.size(); ++i) {
    if (i) out << ", ";
    out << "\"" << v649_esc(issues[i]) << "\"";
  }
  out << "]\n";
  out << "}\n";

  std::cout
      << "[v649] role=" << role
      << " party=" << party
      << " mode=" << mode
      << " policy=" << adapter_policy
      << " p=" << v649_plain_mod
      << " status=" << status
      << " shape=" << v649_shape_json(output_shape)
      << " wall_ms=" << wall_ms
      << "\n";

  return issues.empty() && output_ready ? 0 : 7;
}
