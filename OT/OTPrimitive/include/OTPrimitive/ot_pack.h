// DAZG-Orbit Project Source File
// Component: OT/OTPrimitive/include/OTPrimitive/ot_pack.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once
#define KKOT_TYPES 8
// #include "cf2_ot_pack.h"
#include "split_kkot.h"
#include "split_iknp.h"
namespace OTPrimitive {
template <typename IO>
class OTPack {
 public:
  OTPrimitive::OT<IO> *kkot[KKOT_TYPES];

  // iknp_straight and iknp_reversed: party
  // acts as sender in straight and receiver in reversed.
  // Needed for MUX calls.
  OTPrimitive::OT<IO> *iknp_straight;
  OTPrimitive::OT<IO> *iknp_reversed;
  IO *io;
  int party;
  bool do_setup = false;

  OTPack(IO *io, int party, bool do_setup = true) {
    // std::cout << "OTPack constructor" << std::endl;
  };

  ~OTPack() {
  };

  void SetupBaseOTs() {};

  /*
   * DISCLAIMER:
   * OTPack copy method avoids computing setup keys for each OT instance by
   * reusing the keys generated (through base OTs) for another OT instance.
   * Ideally, the PRGs within OT instances, using the same keys, should use
   * mutually exclusive counters for security. However, the current
   * implementation does not support this.
   */

  void copy(OTPack<IO> *copy_from) {};
};

}  // namespace OTPrimitive