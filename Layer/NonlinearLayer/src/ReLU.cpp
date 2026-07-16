// DAZG-Orbit Project Source File
// Component: Layer/NonlinearLayer/src/ReLU.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <cstdint>
#include <NonlinearLayer/ReLU.h>

// 这里必须是“定义”，不是 extern；并且要放在全局命名空间
int32_t kScale = 16;

namespace NonlinearLayer {

} // namespace NonlinearLayer
