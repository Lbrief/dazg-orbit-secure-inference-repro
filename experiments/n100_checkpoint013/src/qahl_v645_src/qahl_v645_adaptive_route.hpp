// DAZG-Orbit Project Source File
// Component: experiments/n100_checkpoint013/src/qahl_v645_src/qahl_v645_adaptive_route.hpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once
#include <cstring>
#include <string>
namespace dazg_orbit::qahl::v645 {
struct AdaptiveRouteEntry { const char* name; int channel_block; int linear_block; const char* linear_rank; };
static const AdaptiveRouteEntry kAdaptiveRoutes[] = {
  {"stem.0", 1, 0, ""},
  {"stem.2.conv", 1, 0, ""},
  {"stem.3.net.0.conv", 2, 0, ""},
  {"stem.3.net.3.conv", 1, 0, ""},
  {"h32.0.body.0", 1, 0, ""},
  {"h32.0.body.3.conv", 1, 0, ""},
  {"h32.0.anchor.net.0.conv", 2, 0, ""},
  {"h32.0.anchor.net.3.conv", 2, 0, ""},
  {"h32.1.net.0.conv", 1, 0, ""},
  {"h32.1.net.3.conv", 2, 0, ""},
  {"to_h16.main.0", 1, 0, ""},
  {"to_h16.main.3.conv", 1, 0, ""},
  {"to_h16.skip", 1, 0, ""},
  {"to_h16.tail.net.0.conv", 1, 0, ""},
  {"to_h16.tail.net.3.conv", 1, 0, ""},
  {"h16.0.body.0", 1, 0, ""},
  {"h16.0.body.3.conv", 1, 0, ""},
  {"h16.0.anchor.net.0.conv", 2, 0, ""},
  {"h16.0.anchor.net.3.conv", 1, 0, ""},
  {"h16.1.net.0.conv", 2, 0, ""},
  {"h16.1.net.3.conv", 1, 0, ""},
  {"to_h8.main.0", 1, 0, ""},
  {"to_h8.main.3.conv", 1, 0, ""},
  {"to_h8.skip", 1, 0, ""},
  {"to_h8.tail.net.0.conv", 1, 0, ""},
  {"to_h8.tail.net.3.conv", 2, 0, ""},
  {"h8.0.body.0", 1, 0, ""},
  {"h8.0.body.3.conv", 1, 0, ""},
  {"h8.0.anchor.net.0.conv", 1, 0, ""},
  {"h8.0.anchor.net.3.conv", 1, 0, ""},
  {"h8.1.local.0.conv", 1, 0, ""},
  {"h8.1.local.1.conv", 1, 0, ""},
  {"h8.1.local.2.conv", 1, 0, ""},
  {"h8.1.local.3.conv", 1, 0, ""},
  {"h8.1.mix.conv", 1, 0, ""},
  {"h8.2.net.0.conv", 1, 0, ""},
  {"h8.2.net.3.conv", 2, 0, ""},
  {"head.2", 0, 1, "row"},
};
inline int adaptive_channel_block_for(const char* name, int fallback) {
  for (const auto& r : kAdaptiveRoutes)
    if (std::strcmp(r.name, name) == 0 && r.channel_block > 0) return r.channel_block;
  return fallback;
}
inline int adaptive_linear_block_for(const char* name, int fallback) {
  for (const auto& r : kAdaptiveRoutes)
    if (std::strcmp(r.name, name) == 0 && r.linear_block > 0) return r.linear_block;
  return fallback;
}
} // namespace dazg_orbit::qahl::v645
