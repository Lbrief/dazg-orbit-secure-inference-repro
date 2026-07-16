// DAZG-Orbit Project Source File
// Component: experiments/n100_checkpoint013/src/qahl_v645_src/qahl_v645_exec_inventory.hpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once
#include <cstddef>
namespace dazg_orbit::qahl::v645 {
struct ExecEntry { int index; int seq; const char* id; const char* name; const char* type; const char* weight; const char* bias; int in_ch; int out_ch; int feature; int stride; int padding; int kernel; int channel_block; int block_size; int input_elems; };
static constexpr std::size_t kExecEntryCount = 38;
static const ExecEntry kExecEntries[] = {
  {0, 1, "n0000", "stem.0", "conv2d", "stem.0.weight", "stem.0.bias", 3, 48, 32, 1, 1, 3, 1, 0, 3072},
  {1, 3, "n0002", "stem.2.conv", "conv2d", "stem.2.conv.weight", "stem.2.conv.bias", 48, 48, 32, 1, 0, 1, 16, 0, 49152},
  {2, 4, "n0003", "stem.3.net.0.conv", "conv2d", "stem.3.net.0.conv.weight", "stem.3.net.0.conv.bias", 48, 60, 32, 1, 0, 1, 4, 0, 49152},
  {3, 7, "n0006", "stem.3.net.3.conv", "conv2d", "stem.3.net.3.conv.weight", "stem.3.net.3.conv.bias", 60, 48, 32, 1, 0, 1, 4, 0, 61440},
  {4, 9, "n0008", "h32.0.body.0", "conv2d", "h32.0.body.0.weight", "h32.0.body.0.bias", 48, 48, 32, 1, 1, 3, 16, 0, 49152},
  {5, 12, "n0011", "h32.0.body.3.conv", "conv2d", "h32.0.body.3.conv.weight", "h32.0.body.3.conv.bias", 48, 48, 32, 1, 0, 1, 16, 0, 49152},
  {6, 14, "n0013", "h32.0.anchor.net.0.conv", "conv2d", "h32.0.anchor.net.0.conv.weight", "h32.0.anchor.net.0.conv.bias", 48, 60, 32, 1, 0, 1, 4, 0, 49152},
  {7, 17, "n0016", "h32.0.anchor.net.3.conv", "conv2d", "h32.0.anchor.net.3.conv.weight", "h32.0.anchor.net.3.conv.bias", 60, 48, 32, 1, 0, 1, 4, 0, 61440},
  {8, 19, "n0018", "h32.1.net.0.conv", "conv2d", "h32.1.net.0.conv.weight", "h32.1.net.0.conv.bias", 48, 60, 32, 1, 0, 1, 4, 0, 49152},
  {9, 22, "n0021", "h32.1.net.3.conv", "conv2d", "h32.1.net.3.conv.weight", "h32.1.net.3.conv.bias", 60, 48, 32, 1, 0, 1, 4, 0, 61440},
  {10, 24, "n0023", "to_h16.main.0", "conv2d", "to_h16.main.0.weight", "to_h16.main.0.bias", 48, 96, 32, 2, 1, 3, 16, 0, 49152},
  {11, 27, "n0026", "to_h16.main.3.conv", "conv2d", "to_h16.main.3.conv.weight", "to_h16.main.3.conv.bias", 96, 96, 16, 1, 0, 1, 32, 0, 24576},
  {12, 28, "n0027", "to_h16.skip", "conv2d", "to_h16.skip.weight", "to_h16.skip.bias", 48, 96, 32, 2, 0, 1, 16, 0, 49152},
  {13, 30, "n0029", "to_h16.tail.net.0.conv", "conv2d", "to_h16.tail.net.0.conv.weight", "to_h16.tail.net.0.conv.bias", 96, 120, 16, 1, 0, 1, 8, 0, 24576},
  {14, 33, "n0032", "to_h16.tail.net.3.conv", "conv2d", "to_h16.tail.net.3.conv.weight", "to_h16.tail.net.3.conv.bias", 120, 96, 16, 1, 0, 1, 8, 0, 30720},
  {15, 35, "n0034", "h16.0.body.0", "conv2d", "h16.0.body.0.weight", "h16.0.body.0.bias", 96, 96, 16, 1, 1, 3, 32, 0, 24576},
  {16, 38, "n0037", "h16.0.body.3.conv", "conv2d", "h16.0.body.3.conv.weight", "h16.0.body.3.conv.bias", 96, 96, 16, 1, 0, 1, 32, 0, 24576},
  {17, 40, "n0039", "h16.0.anchor.net.0.conv", "conv2d", "h16.0.anchor.net.0.conv.weight", "h16.0.anchor.net.0.conv.bias", 96, 120, 16, 1, 0, 1, 8, 0, 24576},
  {18, 43, "n0042", "h16.0.anchor.net.3.conv", "conv2d", "h16.0.anchor.net.3.conv.weight", "h16.0.anchor.net.3.conv.bias", 120, 96, 16, 1, 0, 1, 8, 0, 30720},
  {19, 45, "n0044", "h16.1.net.0.conv", "conv2d", "h16.1.net.0.conv.weight", "h16.1.net.0.conv.bias", 96, 120, 16, 1, 0, 1, 8, 0, 24576},
  {20, 48, "n0047", "h16.1.net.3.conv", "conv2d", "h16.1.net.3.conv.weight", "h16.1.net.3.conv.bias", 120, 96, 16, 1, 0, 1, 8, 0, 30720},
  {21, 50, "n0049", "to_h8.main.0", "conv2d", "to_h8.main.0.weight", "to_h8.main.0.bias", 96, 192, 16, 2, 1, 3, 32, 0, 24576},
  {22, 53, "n0052", "to_h8.main.3.conv", "conv2d", "to_h8.main.3.conv.weight", "to_h8.main.3.conv.bias", 192, 192, 8, 1, 0, 1, 64, 0, 12288},
  {23, 54, "n0053", "to_h8.skip", "conv2d", "to_h8.skip.weight", "to_h8.skip.bias", 96, 192, 16, 2, 0, 1, 32, 0, 24576},
  {24, 56, "n0055", "to_h8.tail.net.0.conv", "conv2d", "to_h8.tail.net.0.conv.weight", "to_h8.tail.net.0.conv.bias", 192, 240, 8, 1, 0, 1, 16, 0, 12288},
  {25, 59, "n0058", "to_h8.tail.net.3.conv", "conv2d", "to_h8.tail.net.3.conv.weight", "to_h8.tail.net.3.conv.bias", 240, 192, 8, 1, 0, 1, 16, 0, 15360},
  {26, 61, "n0060", "h8.0.body.0", "conv2d", "h8.0.body.0.weight", "h8.0.body.0.bias", 192, 192, 8, 1, 1, 3, 64, 0, 12288},
  {27, 64, "n0063", "h8.0.body.3.conv", "conv2d", "h8.0.body.3.conv.weight", "h8.0.body.3.conv.bias", 192, 192, 8, 1, 0, 1, 64, 0, 12288},
  {28, 66, "n0065", "h8.0.anchor.net.0.conv", "conv2d", "h8.0.anchor.net.0.conv.weight", "h8.0.anchor.net.0.conv.bias", 192, 240, 8, 1, 0, 1, 16, 0, 12288},
  {29, 69, "n0068", "h8.0.anchor.net.3.conv", "conv2d", "h8.0.anchor.net.3.conv.weight", "h8.0.anchor.net.3.conv.bias", 240, 192, 8, 1, 0, 1, 16, 0, 15360},
  {30, 75, "n0071", "h8.1.local.0.conv", "conv2d", "h8.1.local.0.conv.weight", "h8.1.local.0.conv.bias", 48, 48, 8, 1, 0, 1, 16, 0, 3072},
  {31, 77, "n0073", "h8.1.local.1.conv", "conv2d", "h8.1.local.1.conv.weight", "h8.1.local.1.conv.bias", 48, 48, 8, 1, 0, 1, 16, 0, 3072},
  {32, 79, "n0075", "h8.1.local.2.conv", "conv2d", "h8.1.local.2.conv.weight", "h8.1.local.2.conv.bias", 48, 48, 8, 1, 0, 1, 16, 0, 3072},
  {33, 81, "n0077", "h8.1.local.3.conv", "conv2d", "h8.1.local.3.conv.weight", "h8.1.local.3.conv.bias", 48, 48, 8, 1, 0, 1, 16, 0, 3072},
  {34, 85, "n0081", "h8.1.mix.conv", "conv2d", "h8.1.mix.conv.weight", "h8.1.mix.conv.bias", 192, 192, 8, 1, 0, 1, 64, 0, 12288},
  {35, 86, "n0082", "h8.2.net.0.conv", "conv2d", "h8.2.net.0.conv.weight", "h8.2.net.0.conv.bias", 192, 240, 8, 1, 0, 1, 16, 0, 12288},
  {36, 89, "n0085", "h8.2.net.3.conv", "conv2d", "h8.2.net.3.conv.weight", "h8.2.net.3.conv.bias", 240, 192, 8, 1, 0, 1, 16, 0, 15360},
  {37, 93, "n0089", "head.2", "linear", "head.2.weight", "head.2.bias", 192, 100, 1, 0, 0, 0, 0, 4, 192},
};
} // namespace dazg_orbit::qahl::v645
