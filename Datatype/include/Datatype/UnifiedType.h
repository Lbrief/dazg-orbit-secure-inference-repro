// DAZG-Orbit Project Source File
// Component: Datatype/include/Datatype/UnifiedType.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

namespace Datatype {

enum LOCATION { HOST = 0, DEVICE, HOST_AND_DEVICE, UNDEF };

class UnifiedBase {
public:
  UnifiedBase(LOCATION loc = UNDEF) : loc_(loc){};

  UnifiedBase(const UnifiedBase &) = default;

  UnifiedBase &operator=(const UnifiedBase &) = default;

  UnifiedBase(UnifiedBase &&) = default;

  UnifiedBase &operator=(UnifiedBase &&) = default;

  virtual bool on_host() const { return loc_ == HOST; }

  virtual bool on_device() const { return loc_ == DEVICE; }

  LOCATION location() const { return loc_; }

protected:
  LOCATION loc_;
};

} // namespace Datatype