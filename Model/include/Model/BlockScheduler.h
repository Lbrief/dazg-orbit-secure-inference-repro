// DAZG-Orbit Project Source File
// Component: Model/include/Model/BlockScheduler.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <cstdint>

namespace BlockScheduler {

enum class BlockExecMode : uint8_t {
    BaselineExact = 0,
    TailFusionCandidate = 1
};

struct BlockExecPlan {
    BlockExecMode mode = BlockExecMode::BaselineExact;
    bool profile = false;
    bool downsample = false;
};

inline BlockExecPlan MakeBlockExecPlan(uint64_t in_planes,
                                       uint64_t planes,
                                       uint64_t stride,
                                       bool has_shortcut) {
    BlockExecPlan p;
    p.profile = (stride != 1 || has_shortcut || in_planes != planes);
    p.downsample = has_shortcut;

    // v1 先只把“可能值得做 block-level fuse 的块”标出来，
    // 但执行语义仍保持 baseline 不变。
    p.mode = has_shortcut ? BlockExecMode::TailFusionCandidate
                          : BlockExecMode::BaselineExact;
    return p;
}

} // namespace BlockScheduler
