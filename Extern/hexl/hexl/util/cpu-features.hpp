// Portable DAZG reproducibility patch: AVX-512 disabled.
#pragma once
namespace intel { namespace hexl {
static const bool disable_avx512dq = true;
static const bool disable_avx512ifma = true;
static const bool disable_avx512vbmi2 = true;
static const bool has_avx512dq = false;
static const bool has_avx512ifma = false;
static const bool has_avx512vbmi2 = false;
}} // namespace intel::hexl
