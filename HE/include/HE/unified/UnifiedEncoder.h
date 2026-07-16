// DAZG-Orbit Project Source File
// Component: HE/include/HE/unified/UnifiedEncoder.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include "HE/unified/UnifiedContext.h"
#include "HE/unified/UnifiedPlaintext.h"
#include <seal/batchencoder.h>

#ifdef USE_HE_GPU
#include <phantom/batchencoder.h>
#endif

namespace HE {
namespace unified {

class UnifiedBatchEncoder : public seal::BatchEncoder {
public:
  UnifiedBatchEncoder(const UnifiedContext &context)
      : seal::BatchEncoder::BatchEncoder(context), context_(context) {
#ifdef USE_HE_GPU
    if (context.is_gpu_enable()) {
      device_encoder_ = std::make_unique<PhantomBatchEncoder>(context);
    }
#endif
  }

  ~UnifiedBatchEncoder() = default;

  void encode(const std::vector<std::uint64_t> &values,
              UnifiedPlaintext &destination) const {
    if (destination.on_host()) {
      seal::BatchEncoder::encode(values, destination);
    }
#ifdef USE_HE_GPU
    if (destination.on_device()) {
      device_encoder_->encode(context_, values, destination);
    }
#endif
  }

private:
  const UnifiedContext &context_;
#ifdef USE_HE_GPU
  std::unique_ptr<PhantomBatchEncoder> device_encoder_ = nullptr;
#endif
};

} // namespace unified
} // namespace HE