// DAZG-Orbit Project Source File
// Component: experiments/n10_p60/src/qahl_v645_src/qahl_v645_paired_fullstage_executor.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

#include "qahl_v645_inventory.hpp"
#include "qahl_v645_tensor_adapters.hpp"
#include "qahl_v645_extra_adapters.hpp"
#include "qahl_v645_sample_input.hpp"
#include "qahl_v645_adaptive_route.hpp"

#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace dazg_orbit::qahl::v645;
using namespace dazg_orbit::qahl::v645::resolved;


#ifndef DAZG_ORBIT_V665_SERVER_OWNED_INPUT_HELPER
#define DAZG_ORBIT_V665_SERVER_OWNED_INPUT_HELPER
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <vector>
static std::size_t dazg_orbit_v665_numel_shape(const std::vector<size_t>& s) {
  std::size_t n = 1;
  for (auto v : s) n *= static_cast<std::size_t>(v);
  return n;
}
static TensorU64 dazg_orbit_v665_zero_tensor_like(TensorU64& t) {
  auto sh = tensor_shape(t);
  std::vector<uint64_t> z(dazg_orbit_v665_numel_shape(sh), 0);
  return make_tensor_from_u64(sh, z, 16);
}
#endif  // DAZG_ORBIT_V665_SERVER_OWNED_INPUT_HELPER


static std::string g_report_path;
static std::string g_role;
static std::string g_phase = "startup";
static std::string g_prefix = "full";

static std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '\\' || c == '"') { o.push_back('\\'); o.push_back(c); }
    else if (c == '\n') o += "\\n";
    else o.push_back(c);
  }
  return o;
}
static void signal_json(int sig) {
  std::string msg = "{\n"
    "  \"schema\": \"dazg_orbit.qahl.v645.prefix_signal_report\",\n"
    "  \"status\": \"signal_crash\",\n"
    "  \"role\": \"" + esc(g_role) + "\",\n"
    "  \"prefix\": \"" + esc(g_prefix) + "\",\n"
    "  \"phase\": \"" + esc(g_phase) + "\",\n"
    "  \"signal\": " + std::to_string(sig) + ",\n"
    "  \"he_exact_done\": false,\n"
    "  \"strict_hash_match_all\": null,\n"
    "  \"reference_echo\": false\n"
    "}\n";
  int fd = ::open(g_report_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd >= 0) {
    (void)::write(fd, msg.c_str(), msg.size());
    ::close(fd);
  }
  _Exit(128 + sig);
}
static void install_signal_handlers() {
  std::signal(SIGSEGV, signal_json);
  std::signal(SIGABRT, signal_json);
  std::signal(SIGFPE, signal_json);
  std::signal(SIGILL, signal_json);
}

static const PayloadEntry* find_weight(const std::string& key) {
  for (const auto& p : kWeights) if (key == p.key) return &p;
  return nullptr;
}
static const PayloadEntry* find_ref(const std::string& key) {
  for (const auto& p : kReferences) if (key == p.key) return &p;
  return nullptr;
}
static TensorU64 load_weight_tensor(const std::string& key) {
  const auto* p = find_weight(key);
  if (!p) throw std::runtime_error("missing weight payload: " + key);
  return load_tensor_u64(p->key, p->path, 16);
}
static std::unique_ptr<Utils::NetIO> make_io(const std::string& role, const std::string& address, int port) {
  if (role == "server") return std::unique_ptr<Utils::NetIO>(new Utils::NetIO(nullptr, port));
  return std::unique_ptr<Utils::NetIO>(new Utils::NetIO(address.c_str(), port));
}
// DAZG_ORBIT_V675_HE_CONTEXT_SAFE_LADDER_BEGIN
static int dazg_orbit_v675_selected_plain_bits = 0;

static std::vector<int> dazg_orbit_v675_coeff_chain_for_bits(int plain_bits) {
  // v674 used {60,50,50} at plain_bits=50.  A 50-bit plaintext prime can be
  // larger than a 50-bit coefficient prime, so SEAL rejects the BFV parameters
  // before graph computation starts.  Keep coeff primes strictly above the
  // requested plaintext-prime bit size while staying within the 8192-degree
  // coefficient-modulus budget used by the project.
  if (plain_bits >= 50) return std::vector<int>{60, 60, 60};
  if (plain_bits >= 48) return std::vector<int>{60, 60, 58};
  if (plain_bits >= 45) return std::vector<int>{60, 58, 58};
  if (plain_bits >= 40) return std::vector<int>{60, 50, 50};
  return std::vector<int>{60, 33, 33};
}

static std::unique_ptr<HEEvaluator> make_he(Utils::NetIO* io, int party) {
  const char* bits_env_v675 = std::getenv("DAZG_ORBIT_V675_FORCE_PLAIN_BITS");
  const char* bits_env_v671 = std::getenv("DAZG_ORBIT_V671_FORCE_PLAIN_BITS");
  const int plain_bits = bits_env_v675 ? std::atoi(bits_env_v675)
                         : (bits_env_v671 ? std::atoi(bits_env_v671) : 50);
  const std::vector<int> coeff = dazg_orbit_v675_coeff_chain_for_bits(plain_bits);
  std::cerr << "[v675-he] plain_bits=" << plain_bits << " coeff_bits=";
  for (std::size_t i = 0; i < coeff.size(); ++i) {
    if (i) std::cerr << ",";
    std::cerr << coeff[i];
  }
  std::cerr << "\n";
  auto ptr = std::unique_ptr<HEEvaluator>(
      new HEEvaluator(io, party, 8192, plain_bits, HOST, coeff));
  dazg_orbit_v675_selected_plain_bits = plain_bits;
  return ptr;
}
// DAZG_ORBIT_V675_HE_CONTEXT_SAFE_LADDER_END

static TensorU64 run_conv_named(HEEvaluator* he, TensorU64& x, const std::string& name,
                                int feature, int stride, int padding, int kernel) {
  auto w = load_weight_tensor(name + ".weight");
  auto b = load_weight_tensor(name + ".bias");
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

static TensorU64 run_linear_head(HEEvaluator* he, TensorU64& x) {
  auto row = tensor_as_linear_row(x);
  auto w = load_weight_tensor("head.2.weight");
  auto b = load_weight_tensor("head.2.bias");
  LinearNest op(1, 4, w, b, he);
  return linear_forward(op, row);
}

static TensorU64 run_unit_block(HEEvaluator* he, TensorU64& input,
                                const std::string& conv0,
                                const std::string& conv1,
                                int feature,
                                int stride0,
                                int padding0,
                                int kernel0) {
  TensorU64 residual_base = input;
  auto branch = run_conv_named(he, input, conv0, feature, stride0, padding0, kernel0);
  branch = tensor_gelu_adapter(branch);
  branch = run_conv_named(he, branch, conv1, feature, 1, 0, 1);
  return tensor_modular_add(branch, residual_base);
}

static TensorU64 run_transition_block(HEEvaluator* he, TensorU64& input,
                                      const std::string& main0,
                                      const std::string& main3,
                                      const std::string& skip_name,
                                      const std::string& tail0,
                                      const std::string& tail3,
                                      int in_feature,
                                      int out_feature) {
  TensorU64 base = input;
  auto main = run_conv_named(he, base, main0, in_feature, 2, 1, 3);
  main = tensor_gelu_adapter(main);
  main = run_conv_named(he, main, main3, out_feature, 1, 0, 1);
  auto skip = run_conv_named(he, base, skip_name, in_feature, 2, 0, 1);
  auto current = tensor_modular_add(main, skip);
  current = run_unit_block(he, current, tail0, tail3, out_feature, 1, 0, 1);
  return current;
}

static TensorU64 run_h8_qchar_block(HEEvaluator* he, TensorU64& input) {
  auto b0 = tensor_channel_slice(input, 0, 4);
  auto b1 = tensor_channel_slice(input, 1, 4);
  auto b2 = tensor_channel_slice(input, 2, 4);
  auto b3 = tensor_channel_slice(input, 3, 4);
  b0 = run_conv_named(he, b0, "h8.1.local.0.conv", 8, 1, 0, 1);
  b0 = tensor_bucket_scale(b0, 0);
  b1 = run_conv_named(he, b1, "h8.1.local.1.conv", 8, 1, 0, 1);
  b1 = tensor_bucket_scale(b1, 1);
  b2 = run_conv_named(he, b2, "h8.1.local.2.conv", 8, 1, 0, 1);
  b2 = tensor_bucket_scale(b2, 2);
  b3 = run_conv_named(he, b3, "h8.1.local.3.conv", 8, 1, 0, 1);
  b3 = tensor_bucket_scale(b3, 3);
  auto cat = tensor_channel_concat4(b0, b1, b2, b3);
  cat = tensor_gelu_adapter(cat);
  cat = run_conv_named(he, cat, "h8.1.mix.conv", 8, 1, 0, 1);
  return cat;
}

static TensorU64 run_to_h16_prefix(HEEvaluator* he, TensorU64& x0, int& conv_count, int& gelu_count, int& add_count) {
  g_phase = "conv_stem_0";
  auto current = run_conv_named(he, x0, "stem.0", 32, 1, 1, 3);
  ++conv_count;

  g_phase = "gelu_stem_1";
  current = tensor_gelu_adapter(current);
  ++gelu_count;

  g_phase = "conv_stem_2";
  current = run_conv_named(he, current, "stem.2.conv", 32, 1, 0, 1);
  ++conv_count;

  g_phase = "stem_3_block";
  current = run_unit_block(he, current, "stem.3.net.0.conv", "stem.3.net.3.conv", 32, 1, 0, 1);
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h32_0_body_block";
  current = run_unit_block(he, current, "h32.0.body.0", "h32.0.body.3.conv", 32, 1, 1, 3);
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h32_0_anchor_block";
  current = run_unit_block(he, current, "h32.0.anchor.net.0.conv", "h32.0.anchor.net.3.conv", 32, 1, 0, 1);
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "h32_1_block";
  current = run_unit_block(he, current, "h32.1.net.0.conv", "h32.1.net.3.conv", 32, 1, 0, 1);
  conv_count += 2; gelu_count += 1; add_count += 1;

  g_phase = "to_h16_transition";
  current = run_transition_block(he, current,
                                "to_h16.main.0", "to_h16.main.3.conv", "to_h16.skip",
                                "to_h16.tail.net.0.conv", "to_h16.tail.net.3.conv",
                                32, 16);
  conv_count += 5; gelu_count += 2; add_count += 2;
  return current;
}

int main(int argc, char** argv) {
  std::string out_report = "qahl_v645_paired_fullstage_executor_report.json";
  std::string role = "server";
  std::string address = "127.0.0.1";
  int party = 1;
  int port = 36480;
  std::size_t sample_index = 0;
  std::string prefix = "full";

  for (int i=1; i<argc; ++i) {
    std::string a = argv[i];
    if ((a == "--out-report" || a == "--out") && i + 1 < argc) out_report = argv[++i];
    else if (a == "--role" && i + 1 < argc) role = argv[++i];
    else if (a == "--party" && i + 1 < argc) party = std::stoi(argv[++i]);
    else if (a == "--address" && i + 1 < argc) address = argv[++i];
    else if (a == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
    else if ((a == "--sample-index" || a == "--sample_index") && i + 1 < argc) sample_index = static_cast<std::size_t>(std::stoul(argv[++i]));
    else if (a == "--prefix" && i + 1 < argc) prefix = argv[++i];
  }

  g_report_path = out_report;
  g_role = role;
  g_prefix = prefix;
  install_signal_handlers();

  std::vector<std::string> issues;
  bool netio_ok=false, context_ok=false, keygen_ok=false, input_ok=false;
  int conv_count=0, gelu_count=0, add_count=0, qchar_split_count=0, bucket_scale_count=0, avgpool_count=0, linear_count=0;
  std::string status = "blocked";
  std::vector<size_t> final_shape;

  try {
    g_phase = "construct_netio";
    auto io = make_io(role, address, port);
    netio_ok = true;

    g_phase = "construct_he_context";
    auto he = make_he(io.get(), party);
    context_ok = true;

    g_phase = "GenerateNewKey";
    he->GenerateNewKey();
    keygen_ok = true;

    g_phase = "load_input";
    const auto* input_p = find_ref("qahl_ref_input_n10.npy");
    if (!input_p) throw std::runtime_error("missing qahl_ref_input_n10.npy");
    auto x0 = load_input_sample_tensor(input_p->key, input_p->path, sample_index, false, 16);
    if (role == "client" && std::getenv("DAZG_ORBIT_V665_FORCE_CLIENT_ZERO_INPUT")) {
      x0 = dazg_orbit_v665_zero_tensor_like(x0);
      std::cerr << "[v665-input] client zeroed after load_input_sample_tensor\n";
    } else if (role == "server" && std::getenv("DAZG_ORBIT_V665_FORCE_SERVER_OWNS_INPUT")) {
      std::cerr << "[v665-input] server keeps real input after load_input_sample_tensor\n";
    }
    input_ok = true;

    auto current = run_to_h16_prefix(he.get(), x0, conv_count, gelu_count, add_count);

    if (prefix == "to_h16") {
      status = "prefix_to_h16_ok";
    } else {
      g_phase = "h16_0_body_block";
      current = run_unit_block(he.get(), current, "h16.0.body.0", "h16.0.body.3.conv", 16, 1, 1, 3);
      conv_count += 2; gelu_count += 1; add_count += 1;
      if (prefix == "h16_body") {
        status = "prefix_h16_body_ok";
      } else {
        g_phase = "h16_0_anchor_block";
        current = run_unit_block(he.get(), current, "h16.0.anchor.net.0.conv", "h16.0.anchor.net.3.conv", 16, 1, 0, 1);
        conv_count += 2; gelu_count += 1; add_count += 1;
        if (prefix == "h16_anchor") {
          status = "prefix_h16_anchor_ok";
        } else {
          g_phase = "h16_1_block";
          current = run_unit_block(he.get(), current, "h16.1.net.0.conv", "h16.1.net.3.conv", 16, 1, 0, 1);
          conv_count += 2; gelu_count += 1; add_count += 1;
          if (prefix == "h16_all") {
            status = "prefix_h16_all_ok";
          } else {
            g_phase = "to_h8_transition";
            current = run_transition_block(he.get(), current,
                                          "to_h8.main.0", "to_h8.main.3.conv", "to_h8.skip",
                                          "to_h8.tail.net.0.conv", "to_h8.tail.net.3.conv",
                                          16, 8);
            conv_count += 5; gelu_count += 2; add_count += 2;
            if (prefix == "to_h8") {
              status = "prefix_to_h8_ok";
            } else {
              g_phase = "h8_0_body_block";
              current = run_unit_block(he.get(), current, "h8.0.body.0", "h8.0.body.3.conv", 8, 1, 1, 3);
              conv_count += 2; gelu_count += 1; add_count += 1;
              if (prefix == "h8_body") {
                status = "prefix_h8_body_ok";
              } else {
                g_phase = "h8_0_anchor_block";
                current = run_unit_block(he.get(), current, "h8.0.anchor.net.0.conv", "h8.0.anchor.net.3.conv", 8, 1, 0, 1);
                conv_count += 2; gelu_count += 1; add_count += 1;
                if (prefix == "h8_anchor") {
                  status = "prefix_h8_anchor_ok";
                } else {
                  g_phase = "h8_1_qchar_block";
                  current = run_h8_qchar_block(he.get(), current);
                  conv_count += 5; gelu_count += 1; qchar_split_count += 4; bucket_scale_count += 4;
                  if (prefix == "h8_qchar") {
                    status = "prefix_h8_qchar_ok";
                  } else {
                    g_phase = "h8_2_block";
                    current = run_unit_block(he.get(), current, "h8.2.net.0.conv", "h8.2.net.3.conv", 8, 1, 0, 1);
                    conv_count += 2; gelu_count += 1; add_count += 1;
                    if (prefix == "h8_all") {
                      status = "prefix_h8_all_ok";
                    } else {
                      g_phase = "head_avgpool";
                      current = tensor_avgpool_hw(current);
                      ++avgpool_count;
                      g_phase = "head_linear";
                      current = run_linear_head(he.get(), current);
                      ++linear_count;
                      status = (prefix == "head" || prefix == "full") ? "prefix_full_head_ok" : "prefix_full_head_ok";
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
    final_shape = tensor_shape(current);
  } catch (const std::exception& e) {
    issues.push_back(e.what());
    status = "blocked";
  }

  std::ofstream out(out_report);
  out << "{\n";
  out << "  \"schema\": \"dazg_orbit.qahl.v645.paired_fullstage_executor_report\",\n";
  out << "  \"status\": \"" << status << "\",\n";
  out << "  \"role\": \"" << esc(role) << "\",\n";
  out << "  \"party\": " << party << ",\n";
  out << "  \"prefix\": \"" << esc(prefix) << "\",\n";
  out << "  \"netio_ok\": " << (netio_ok ? "true" : "false") << ",\n";
  out << "  \"context_ok\": " << (context_ok ? "true" : "false") << ",\n";
  out << "  \"keygen_ok\": " << (keygen_ok ? "true" : "false") << ",\n";
  out << "  \"input_ok\": " << (input_ok ? "true" : "false") << ",\n";
  out << "  \"conv_count\": " << conv_count << ",\n";
  out << "  \"gelu_count\": " << gelu_count << ",\n";
  out << "  \"residual_add_count\": " << add_count << ",\n";
  out << "  \"qchar_split_count\": " << qchar_split_count << ",\n";
  out << "  \"bucket_scale_count\": " << bucket_scale_count << ",\n";
  out << "  \"avgpool_count\": " << avgpool_count << ",\n";
  out << "  \"linear_count\": " << linear_count << ",\n";
  out << "  \"last_phase\": \"" << esc(g_phase) << "\",\n";
  out << "  \"final_shape\": [";
  for (std::size_t i=0; i<final_shape.size(); ++i) { if (i) out << ", "; out << final_shape[i]; }
  out << "],\n";
  out << "  \"sample_logits_ready\": " << ((linear_count > 0 && issues.empty()) ? "true" : "false") << ",\n";
  out << "  \"he_exact_done\": false,\n";
  out << "  \"strict_hash_match_all\": null,\n";
  out << "  \"reference_echo\": false,\n";
  out << "  \"issues\": [";
  for (std::size_t i=0; i<issues.size(); ++i) { if (i) out << ", "; out << "\"" << esc(issues[i]) << "\""; }
  out << "]\n";
  out << "}\n";
  std::cout << "[v645][prefix] role=" << role << " prefix=" << prefix << " status=" << status
            << " conv=" << conv_count << " gelu=" << gelu_count << " add=" << add_count
            << " qsplit=" << qchar_split_count << " bscale=" << bucket_scale_count
            << " linear=" << linear_count << " phase=" << g_phase << "\n";
  return issues.empty() ? 0 : 7;
}
