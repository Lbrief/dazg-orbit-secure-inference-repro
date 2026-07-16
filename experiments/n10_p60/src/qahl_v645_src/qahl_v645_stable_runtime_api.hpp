// DAZG-Orbit Project Source File
// Component: experiments/n10_p60/src/qahl_v645_src/qahl_v645_stable_runtime_api.hpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once
#include <cstdint>
#include <vector>
#include <seal/seal.h>
#if __has_include("Datatype/Tensor.h")
#include "Datatype/Tensor.h"
#elif __has_include("Tensor.h")
#include "Tensor.h"
#endif
#include "LinearLayer/Conv.h"
#include "LinearLayer/CirLinear.h"
#include "NonlinearLayer/TFHEGeLU.h"
#if __has_include("HE/unified/UnifiedEvaluator.h")
#include "HE/unified/UnifiedEvaluator.h"
#elif __has_include("HE/UnifiedEvaluator.h")
#include "HE/UnifiedEvaluator.h"
#elif __has_include("UnifiedEvaluator.h")
#include "UnifiedEvaluator.h"
#endif

namespace dazg_orbit::qahl::v645::resolved {
using Conv2D = LinearLayer::CirConv2D;
using LinearNest = LinearLayer::CirLinearNest;
using TFHEGeLU = dazg_orbit::tfhe::TFHEGeLU;
using RuntimeEvaluator = HE::unified::UnifiedEvaluator;
using TensorU64 = Tensor<uint64_t>;
using HEEvaluator = HE::HEEvaluator;
using CirLayoutPlan = LinearLayer::CirLayoutPlan;
using AddCipher = seal::Ciphertext;
using AddRhs = seal::Ciphertext;
using MulCipher = seal::Ciphertext;
using MulPlain = seal::Plaintext;

inline TensorU64 conv_forward(Conv2D& op, TensorU64& x) { return op(x); }
inline TensorU64 linear_forward(LinearNest& op, TensorU64& x) { return op(x); }
inline std::vector<std::int64_t> gelu_forward_fixed(TFHEGeLU& gelu, const std::vector<std::int64_t>& q16) { return gelu.forward_fixed(q16); }
inline void residual_add_inplace(RuntimeEvaluator& ev, AddCipher& lhs, const AddRhs& rhs) { ev.add_inplace(lhs, rhs); }
inline void multiply_plain_inplace(RuntimeEvaluator& ev, MulCipher& ct, const MulPlain& pt) { ev.multiply_plain_inplace(ct, pt, seal::MemoryManager::GetPool()); }
} // namespace dazg_orbit::qahl::v645::resolved
