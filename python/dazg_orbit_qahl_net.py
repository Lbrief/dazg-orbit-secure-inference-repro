# DAZG-Orbit Project Source File
# Component: python/dazg_orbit_qahl_net.py
# Purpose: Fail-closed experiment, verification, or Q16 oracle component.
# Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
# Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
# Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
# Security boundary: reveal correctness backend; security_claim=0.
"""
DAZGOrbit-QAHLNet: Q-CHAR / ARHC-L guided HE-friendly CIFAR-100 architecture.

This is NOT a drop-in replacement for old DAZG-Orbit weights.  It must be
trained, exported to a new bridge, and then validated with plaintext-vs-HE exactness.

Design identity:
- Q-CHAR phase-bucket channel routing at H8.
- ARHC-L anchor tails: late spatial residuals are 1x1-only.
- No H4 K3 residual body by default; H8 is the terminal spatial anchor.
- Downsample blocks use a single K3 stride-2 transition plus a 1x1 skip.

This deliberately avoids copying generic HE networks such as CryptoNets/LoLa,
SHE/HEMET/mobile-style HE nets, or TTnet/TFHE lookup-table designs.
"""
from __future__ import annotations

from dataclasses import dataclass, asdict
from typing import Dict, Iterable, Literal, Sequence, Tuple

import torch
import torch.nn as nn
import torch.nn.functional as F

# DAZG_ORBIT_V743R6_STAGE_S_Q16_MODEL_CONTRACT
from dazg_orbit_stage_s_q16 import stage_s_gelu_q16_ste


ActName = Literal["gelu", "square", "poly2", "bfe_poly2", "dazg_bfe", "dazg_stage_s_q16", "identity"]


@dataclass
class QAHLConfig:
    widths: Tuple[int, int, int] = (64, 128, 256)
    num_classes: int = 100
    k3_cells: Tuple[int, int, int] = (1, 1, 1)
    phase_buckets_h8: int = 4
    activation: ActName = "gelu"
    terminal_anchor: str = "h8"
    stem_width: int = 64
    dropout: float = 0.0

    def to_dict(self) -> Dict:
        return asdict(self)


class HEAct(nn.Module):
    def __init__(self, kind: ActName = "gelu"):
        super().__init__()
        self.kind = kind

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if self.kind == "identity":
            return x
        if self.kind == "square":
            return x * x
        if self.kind == "poly2":
            # Mild degree-2 proxy.  During bridge export this should be mapped
            # to the same polynomial path as the HE runtime.
            return 0.5 * x + 0.125 * x * x
        if self.kind == "dazg_stage_s_q16":
            # Bit-exact active DAZG-Orbit Stage-S/Q16 forward; GELU-gradient STE backward.
            return stage_s_gelu_q16_ste(x)
        if self.kind in {"bfe_poly2", "dazg_bfe"}:
            # V742 deployment surrogate: train under the same low-degree activation family used by DAZG-Orbit BFE/Q32 lowering.
            return 0.5 * x + 0.125 * x * x
        return F.gelu(x)


class Conv1x1(nn.Module):
    def __init__(self, in_ch: int, out_ch: int, bias: bool = True):
        super().__init__()
        self.conv = nn.Conv2d(in_ch, out_ch, kernel_size=1, bias=bias)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.conv(x)


class QCharPhaseMixer(nn.Module):
    """Channel routing aligned to Q-CHAR phase buckets.

    The module is cheap for HE because it uses only 1x1 convolutions.  It gives
    the network a phase-bucket structure that the runtime can map to fused
    anchor orbits instead of late K3 residual tiles.
    """

    def __init__(self, channels: int, buckets: int = 4, act: ActName = "gelu"):
        super().__init__()
        assert channels % buckets == 0, "channels must be divisible by phase buckets"
        self.channels = channels
        self.buckets = buckets
        per = channels // buckets
        self.local = nn.ModuleList([Conv1x1(per, per) for _ in range(buckets)])
        self.mix = Conv1x1(channels, channels)
        self.act = HEAct(act)
        self.bucket_scale = nn.Parameter(torch.ones(buckets, per, 1, 1))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        chunks = torch.chunk(x, self.buckets, dim=1)
        out = []
        for i, c in enumerate(chunks):
            out.append(self.local[i](c) * self.bucket_scale[i])
        y = torch.cat(out, dim=1)
        return self.mix(self.act(y))


class ARHCLAnchorTail(nn.Module):
    """ARHC-L-inspired anchor tail: residual capacity without late K3 rotation cost."""

    def __init__(self, channels: int, act: ActName = "gelu", dropout: float = 0.0):
        super().__init__()
        hidden = max(channels, int(channels * 1.25))
        self.net = nn.Sequential(
            Conv1x1(channels, hidden),
            HEAct(act),
            nn.Dropout2d(dropout) if dropout > 0 else nn.Identity(),
            Conv1x1(hidden, channels),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return x + self.net(x)


class OrbitK3Cell(nn.Module):
    """One K3 residual cell followed by 1x1 orbit mixing.

    Keep K3 only where it is useful for accuracy.  The late network avoids a
    stack of K3 residual bodies because those create HE rotations without enough
    marginal accuracy benefit.
    """

    def __init__(self, channels: int, act: ActName = "gelu", dropout: float = 0.0):
        super().__init__()
        self.body = nn.Sequential(
            nn.Conv2d(channels, channels, kernel_size=3, padding=1, bias=True),
            HEAct(act),
            nn.Dropout2d(dropout) if dropout > 0 else nn.Identity(),
            Conv1x1(channels, channels),
        )
        self.anchor = ARHCLAnchorTail(channels, act=act, dropout=dropout)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.anchor(x + self.body(x))


class AnchorTransition(nn.Module):
    """Stride-2 transition with one K3 main path and a 1x1 skip."""

    def __init__(self, in_ch: int, out_ch: int, act: ActName = "gelu", dropout: float = 0.0):
        super().__init__()
        self.main = nn.Sequential(
            nn.Conv2d(in_ch, out_ch, kernel_size=3, stride=2, padding=1, bias=True),
            HEAct(act),
            nn.Dropout2d(dropout) if dropout > 0 else nn.Identity(),
            Conv1x1(out_ch, out_ch),
        )
        self.skip = nn.Conv2d(in_ch, out_ch, kernel_size=1, stride=2, bias=True)
        self.tail = ARHCLAnchorTail(out_ch, act=act, dropout=dropout)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.tail(self.main(x) + self.skip(x))


class DAZGOrbitQAHLNet(nn.Module):
    def __init__(self, cfg: QAHLConfig = QAHLConfig()):
        super().__init__()
        self.cfg = cfg
        w0, w1, w2 = cfg.widths
        assert w2 % cfg.phase_buckets_h8 == 0, "H8 width must be divisible by phase bucket count"

        self.stem = nn.Sequential(
            nn.Conv2d(3, w0, kernel_size=3, padding=1, bias=True),
            HEAct(cfg.activation),
            Conv1x1(w0, w0),
            ARHCLAnchorTail(w0, act=cfg.activation, dropout=cfg.dropout),
        )

        h32 = []
        for _ in range(cfg.k3_cells[0]):
            h32.append(OrbitK3Cell(w0, act=cfg.activation, dropout=cfg.dropout))
        h32.append(ARHCLAnchorTail(w0, act=cfg.activation, dropout=cfg.dropout))
        self.h32 = nn.Sequential(*h32)

        self.to_h16 = AnchorTransition(w0, w1, act=cfg.activation, dropout=cfg.dropout)
        h16 = []
        for _ in range(cfg.k3_cells[1]):
            h16.append(OrbitK3Cell(w1, act=cfg.activation, dropout=cfg.dropout))
        h16.append(ARHCLAnchorTail(w1, act=cfg.activation, dropout=cfg.dropout))
        self.h16 = nn.Sequential(*h16)

        self.to_h8 = AnchorTransition(w1, w2, act=cfg.activation, dropout=cfg.dropout)
        h8 = []
        for _ in range(cfg.k3_cells[2]):
            h8.append(OrbitK3Cell(w2, act=cfg.activation, dropout=cfg.dropout))
        h8.append(QCharPhaseMixer(w2, buckets=cfg.phase_buckets_h8, act=cfg.activation))
        h8.append(ARHCLAnchorTail(w2, act=cfg.activation, dropout=cfg.dropout))
        self.h8 = nn.Sequential(*h8)

        # Terminal H8 anchor pool: no H4 K3 residual stage by default.
        self.head = nn.Sequential(
            nn.AdaptiveAvgPool2d(1),
            nn.Flatten(),
            nn.Linear(w2, cfg.num_classes),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = self.stem(x)
        x = self.h32(x)
        x = self.to_h16(x)
        x = self.h16(x)
        x = self.to_h8(x)
        x = self.h8(x)
        return self.head(x)

    def he_cost_proxy(self) -> Dict[str, int]:
        # Conservative symbolic proxy.  Exact values must be validated by HE runtime.
        k3_h32 = self.cfg.k3_cells[0]
        k3_h16 = self.cfg.k3_cells[1]
        k3_h8 = self.cfg.k3_cells[2]
        return {
            "terminal_spatial_anchor": 8,
            "has_h4_k3_residual": 0,
            "k3_cells_h32": k3_h32,
            "k3_cells_h16": k3_h16,
            "k3_cells_h8": k3_h8,
            "qchar_phase_buckets_h8": self.cfg.phase_buckets_h8,
            "target_policy_rotation_per_sample_low": 240,
            "target_policy_rotation_per_sample_high": 300,
        }


def qahl_s(num_classes: int = 100, activation: ActName = "gelu") -> DAZGOrbitQAHLNet:
    return DAZGOrbitQAHLNet(QAHLConfig(widths=(64, 128, 256), k3_cells=(1, 1, 1), num_classes=num_classes, activation=activation))


def qahl_m(num_classes: int = 100, activation: ActName = "gelu") -> DAZGOrbitQAHLNet:
    return DAZGOrbitQAHLNet(QAHLConfig(widths=(64, 160, 320), k3_cells=(1, 1, 2), num_classes=num_classes, activation=activation))


def qahl_t(num_classes: int = 100, activation: ActName = "gelu") -> DAZGOrbitQAHLNet:
    return DAZGOrbitQAHLNet(QAHLConfig(widths=(48, 96, 192), k3_cells=(1, 1, 1), num_classes=num_classes, activation=activation))


def build_model(name: str = "qahl_s", num_classes: int = 100, activation: ActName = "gelu") -> DAZGOrbitQAHLNet:
    name = name.lower()
    if name in {"qahl_s", "s", "dazg_orbit-qahlnet-s"}:
        return qahl_s(num_classes=num_classes, activation=activation)
    if name in {"qahl_m", "m", "dazg_orbit-qahlnet-m"}:
        return qahl_m(num_classes=num_classes, activation=activation)
    if name in {"qahl_t", "t", "dazg_orbit-qahlnet-t"}:
        return qahl_t(num_classes=num_classes, activation=activation)
    raise ValueError(f"unknown model: {name}")


if __name__ == "__main__":
    model = qahl_s()
    x = torch.randn(2, 3, 32, 32)
    y = model(x)
    print(y.shape)
    print(model.he_cost_proxy())


# DAZG_ORBIT_V742_DAZG_QAT_EXPORT_CONTRACT: model accepts bfe_poly2/dazg_bfe activation for deployment-aligned fine-tuning.
