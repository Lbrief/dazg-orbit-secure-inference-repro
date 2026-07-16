// DAZG-Orbit Project Source File
// Component: Layer/LinearLayer/src/CirLinear.cpp
// Purpose: Packed homomorphic linear layer and diagonal/rotation scheduling.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.

// DAZG-Orbit linear-layer acceleration:
// active packed diagonals are evaluated with sparse BSGS; baby-step rotations
// are cached and reused, while inactive diagonals generate no rotation.

/**
 * CirLinearNest: Block Circulant Linear Layer with Nested Encoding
 * 
 * This is the block circulant version of LinearNest.
 * 
 * Key insight: Each circulant block is encoded using coefficient encoding (Theorem 1),
 * then transformed via cyclic NTT. Multiple blocks can be packed into one ciphertext
 * and processed with BSGS optimization.
 * 
 * Dimensions:
 *   Input: (dim_0, dim_1) = (batch, input_channels)
 *   Weight: (dim_1, dim_2) = (input_channels, output_channels), block circulant
 *   Output: (dim_0, dim_2) = (batch, output_channels)
 * 
 * Block structure:
 *   num_blocks_1 = dim_1 / block_size (input blocks)
 *   num_blocks_2 = dim_2 / block_size (output blocks)
 * 
 * Degenerate cases:
 *   - num_blocks_1 = num_blocks_2 = 1: Single block, no BSGS needed
 *   - tile_size = 1: Each ciphertext holds one block, simple accumulation
 *   - tile_size >= num_blocks: All blocks fit in one ciphertext per dimension
 */

#include <LinearLayer/CirLinear.h>
#include <Utils/CyclicNTT.h>
#include "Utils/dazg_orbit_ablation_flags.h"
#include "Utils/dazg_orbit_determinism.h"
#include <algorithm>
#include <cassert>
#include <hexl/hexl.hpp>
#include <chrono>
#include <cstdlib>

// DAZG_ORBIT_V666_NATIVE_CORE_MODP_PACK_REPAIR_BEGIN
#ifndef DAZG_ORBIT_V666_NATIVE_CORE_MODP_PACK_REPAIR
#define DAZG_ORBIT_V666_NATIVE_CORE_MODP_PACK_REPAIR
#include <cstdlib>
#include <cstdint>
static inline bool DAZGOrbitV666CoreRepairEnabled() {
  const char* e = std::getenv("DAZG_ORBIT_V666_CANON_PACK_REPAIR");
  return e != nullptr && e[0] != '\0' && e[0] != '0';
}
static inline uint64_t DAZGOrbitV666PlainMod() {
  const char* e = std::getenv("DAZG_ORBIT_V666_PLAIN_MOD");
  return e ? static_cast<uint64_t>(std::strtoull(e, nullptr, 10)) : 4294475777ULL;
}
#endif
// DAZG_ORBIT_V666_NATIVE_CORE_MODP_PACK_REPAIR_END



using namespace seal;
using namespace HE;
using namespace HE::unified;



namespace LinearLayer {

// DAZG_ORBIT_V9_FOLDED_BIAS_ADD_PATCH_BEGIN
// Add folded linear bias to exactly one secret share after HE-to-SS depacking.
static bool DAZGOrbitV9FoldedBiasAddEnabled()
{
    const char* v = std::getenv("DAZG_ORBIT_DISABLE_FOLDED_BIAS_ADD");
    if (v == nullptr || *v == '\0') return true;
    return !(v[0] == '1' || v[0] == 't' || v[0] == 'T' ||
             v[0] == 'y' || v[0] == 'Y');
}

static bool DAZGOrbitV9ShouldAddBias(HEEvaluator* he)
{
    return he != nullptr && he->server && DAZGOrbitV9FoldedBiasAddEnabled();
}

static void DAZGOrbitV9AddFoldedBiasLinear2D(Tensor<uint64_t>& y,
                                           const Tensor<uint64_t>& bias,
                                           HEEvaluator* he)
{
    if (!DAZGOrbitV9ShouldAddBias(he)) return;
    const std::vector<size_t>& ys = y.shape();
    const std::vector<size_t>& bs = bias.shape();
    if (ys.size() != 2 || bs.empty()) return;

    const size_t B = ys[0];
    const size_t O = ys[1];

    if (bs.size() == 1 && bs[0] == O) {
        for (size_t b = 0; b < B; ++b) {
            for (size_t o = 0; o < O; ++o) {
                y({b, o}) = y({b, o}) + bias({o});
            }
        }
        return;
    }

    if (bs.size() == 2 && bs[0] == B && bs[1] == O) {
        for (size_t b = 0; b < B; ++b) {
            for (size_t o = 0; o < O; ++o) {
                y({b, o}) = y({b, o}) + bias({b, o});
            }
        }
        return;
    }

    if (bs.size() == 2 && bs[0] == 1 && bs[1] == O) {
        for (size_t b = 0; b < B; ++b) {
            for (size_t o = 0; o < O; ++o) {
                y({b, o}) = y({b, o}) + bias({0, o});
            }
        }
    }
}
// DAZG_ORBIT_V9_FOLDED_BIAS_ADD_PATCH_END



namespace {
inline const char* DAZGOrbitStageZ2ScheduleName()
{
    return dazg_orbit::ablation::EnableStageZ2()
        ? "StageZ2-ExactSparseBSGSLinear"
        : "DenseLegacyLinear";
}
} // namespace

// ======================== CirLinearNest ========================

CirLinearNest::CirLinearNest(uint64_t dim_0, uint64_t block_size,
                             const Tensor<uint64_t>& weight, const Tensor<uint64_t>& bias,
                             HE::HEEvaluator* HE)
    : dim_0(dim_0),
      block_size(block_size),
      weight(weight),
      bias(bias),
      HE(HE)
{
    std::vector<size_t> weight_shape = weight.shape();
    dim_1 = weight_shape[0];
    dim_2 = weight_shape[1];
    
    assert(dim_1 % block_size == 0 && "dim_1 must be divisible by block_size");
    assert(dim_2 % block_size == 0 && "dim_2 must be divisible by block_size");
    
    num_blocks_1 = dim_1 / block_size;
    num_blocks_2 = dim_2 / block_size;
    
    compute_he_params();
    
    if (HE->server) {
        weight_pt = PackWeight();
    }
}

CirLinearNest::CirLinearNest(uint64_t dim_0, uint64_t dim_1, uint64_t dim_2, uint64_t block_size,
                             HE::HEEvaluator* HE)
    : dim_0(dim_0),
      dim_1(dim_1),
      dim_2(dim_2),
      block_size(block_size),
      HE(HE)
{
    assert(dim_1 % block_size == 0 && "dim_1 must be divisible by block_size");
    assert(dim_2 % block_size == 0 && "dim_2 must be divisible by block_size");
    
    num_blocks_1 = dim_1 / block_size;
    num_blocks_2 = dim_2 / block_size;
    
    if (HE->server) {
        this->weight = Tensor<uint64_t>({dim_1, dim_2});
        this->bias = Tensor<uint64_t>({dim_0, dim_2});
        dazg_orbit_det::FillTensorDeterministic(
            this->weight,
            "CirLinearNest.weight",
            16,
            dazg_orbit_det::HashU64Seq({dim_0, dim_1, dim_2, block_size, 1ULL}));
        dazg_orbit_det::FillTensorDeterministic(
            this->bias,
            "CirLinearNest.bias",
            16,
            dazg_orbit_det::HashU64Seq({dim_0, dim_1, dim_2, block_size, 2ULL}));
    }
    
    compute_he_params();
    
    if (HE->server) {
        weight_pt = PackWeight();
    }
}

void CirLinearNest::compute_he_params() {
    /**
     * Parameter computation for CirLinearNest.
     * 
     * Each circulant block needs ntt_size = padded_dim_0 * block_size slots.
     * We can pack multiple blocks per ciphertext (tile_size blocks per half).
     * 
     * For BSGS optimization:
     *   - tile_size blocks are arranged on an anti-diagonal
     *   - input_rot = sqrt(tile_size) rotations needed
     * 
     * Degenerate cases are handled by limiting tile_size appropriately.
     */
    
    // Pad dim_0 to power of 2
    padded_dim_0 = 1;
    while (padded_dim_0 < dim_0) {
        padded_dim_0 <<= 1;
    }
    
    // NTT size for one circulant block (per Theorem 1)
    ntt_size = padded_dim_0 * block_size;
    
    // Maximum blocks that can fit per ciphertext half
    uint64_t max_tile = HE->polyModulusDegree / (2 * ntt_size);
    if (max_tile < 1) max_tile = 1;
    
    // Actual tile_size: limited by the number of blocks we actually have
    // No point having tile_size larger than num_blocks
    tile_size = std::min(max_tile, std::max(num_blocks_1, num_blocks_2));
    
    // Tiled dimensions
    tiled_blocks_1 = (num_blocks_1 + tile_size - 1) / tile_size;
    tiled_blocks_2 = (num_blocks_2 + tile_size - 1) / tile_size;
    
    // Padded to tile boundaries
    padded_blocks_1 = tiled_blocks_1 * tile_size;
    padded_blocks_2 = tiled_blocks_2 * tile_size;
    
    // BSGS: input_rot = ceil(sqrt(tile_size))
    // Special case: tile_size=1 means no rotation needed
    if (tile_size <= 1) {
        input_rot = 1;
    } else {
        input_rot = 1;
        while (input_rot * input_rot < tile_size) {
            input_rot++;
        }
    }
    
    // Pad weight
    padded_weight = Tensor<uint64_t>({padded_blocks_1 * block_size, padded_blocks_2 * block_size});
    for (uint64_t i = 0; i < dim_1; i++) {
        for (uint64_t j = 0; j < dim_2; j++) {
            padded_weight({i, j}) = weight({i, j});
        }
    }
    
    std::cout << "CirLinearNest params: dim_0=" << dim_0 << " padded=" << padded_dim_0
              << ", block_size=" << block_size << ", ntt_size=" << ntt_size
              << ", num_blocks=(" << num_blocks_1 << "," << num_blocks_2 << ")"
              << ", tile_size=" << tile_size << ", input_rot=" << input_rot
              << ", tiled=(" << tiled_blocks_1 << "," << tiled_blocks_2 << ")" << std::endl;
}

Tensor<UnifiedPlaintext> CirLinearNest::PackWeight() {
    /**
     * Pack weights into plaintexts.
     * 
     * Convention (same as TestMatmul):
     *   weight shape: (dim_1=input, dim_2=output)
     *   W[out, in] = weight({in, out})
     *   First column of block W_block[:, 0]: W_block[i, 0] = weight({in_blk*b, out_blk*b + i})
     * 
     * For tile_size=1: simple encoding like CirLinearSimple
     * For tile_size>1: BSGS encoding with anti-diagonal pattern
     */
    Utils::CyclicNTT cyclic_ntt(ntt_size, HE->plain_mod);
    Tensor<UnifiedPlaintext> wpt({tiled_blocks_1, tiled_blocks_2, tile_size}, HE->Backend());
    const uint64_t half_degree = HE->polyModulusDegree / 2;
    std::vector<uint64_t> poly(HE->polyModulusDegree, 0);
    std::vector<uint64_t> w_coef(ntt_size, 0);

    pack_active = Tensor<uint64_t>({tiled_blocks_1, tiled_blocks_2, tile_size});
    sparse_entries.clear();
    sparse_active_input_tile.assign(tiled_blocks_1, 0);
    sparse_active_output_tile.assign(tiled_blocks_2, 0);
    sparse_max_rot_by_input_tile.assign(tiled_blocks_1, 0);
    sparse_total_packs = tiled_blocks_1 * tiled_blocks_2 * tile_size;
    sparse_active_packs = 0;
    sparse_zero_packs = 0;
    sparse_rotation_slots = 0;

    const bool stage_z2_enabled = dazg_orbit::ablation::EnableStageZ2();

    auto register_stagez2_entry = [&](uint64_t entry_ti,
                                      uint64_t entry_tj,
                                      uint64_t entry_k) {
        pack_active({entry_ti, entry_tj, entry_k}) = 1;
        sparse_active_packs++;
        sparse_active_input_tile[entry_ti] = 1;
        sparse_active_output_tile[entry_tj] = 1;

        CirLinearSparseEntry entry;
        entry.ti = entry_ti;
        entry.tj = entry_tj;
        entry.k = entry_k;
        entry.rot_idx = (tile_size == 1) ? 0 : (input_rot - 1 - (entry_k % input_rot));
        entry.group_idx = (tile_size == 1) ? 0 : (entry_k / input_rot);
        sparse_entries.push_back(entry);
        if (entry.rot_idx > sparse_max_rot_by_input_tile[entry_ti]) {
            sparse_max_rot_by_input_tile[entry_ti] = entry.rot_idx;
        }
    };

    for (uint64_t ti = 0; ti < tiled_blocks_1; ti++) {
        for (uint64_t tj = 0; tj < tiled_blocks_2; tj++) {
            for (uint64_t k = 0; k < tile_size; k++) {
                std::fill(poly.begin(), poly.end(), 0);
                const uint64_t k_mod = k % input_rot;
                const uint64_t k_div = k / input_rot;
                
                for (uint64_t l = 0; l < tile_size; l++) {
                    uint64_t in_blk, out_blk;
                    
                    if (tile_size == 1) {
                        in_blk = ti;
                        out_blk = tj;
                    } else {
                        // BSGS pattern from LinearNest
                        in_blk = ti * tile_size + (l + input_rot - 1 - k_mod) % tile_size;
                        out_blk = tj * tile_size + (tile_size - 1 - l - k_div + tile_size) % tile_size;
                    }
                    
                    if (in_blk >= num_blocks_1 || out_blk >= num_blocks_2) continue;
                    
                    // Coefficient encode the circulant block's first column
                    std::fill(w_coef.begin(), w_coef.end(), 0);
                    const uint64_t in_ch = in_blk * block_size;
                    const uint64_t out_blk_base = out_blk * block_size;
                    for (uint64_t i = 0; i < block_size; i++) {
                        uint64_t out_ch = out_blk_base + i;
                        if (in_ch < dim_1 && out_ch < dim_2) {
                            w_coef[i * padded_dim_0] = padded_weight({in_ch, out_ch});
                        }
                    }
                    
                    // Apply cyclic NTT (in-place)
                    cyclic_ntt.ComputeForward(w_coef.data(), w_coef.data());
                    
                    // Place in polynomial
                    // tile_size=1: only first half (like CirLinearSimple)
                    // tile_size>1: both halves for row batching
                    uint64_t offset = l * ntt_size;
                    for (uint64_t m = 0; m < ntt_size; m++) {
                        poly[offset + m] = w_coef[m];
                    }
                    if (tile_size > 1) {
                        uint64_t offset2 = l * ntt_size + half_degree;
                        for (uint64_t m = 0; m < ntt_size; m++) {
                            poly[offset2 + m] = w_coef[m];
                        }
                    }
                }
                
                bool all_zero = true;
                for (uint64_t m = 0; m < HE->polyModulusDegree && all_zero; ++m) {
                    all_zero = (poly[m] == 0);
                }

                if (all_zero) {
                    sparse_zero_packs++;
                    if (stage_z2_enabled) {
                        pack_active({ti, tj, k}) = 0;
                        poly[HE->polyModulusDegree - 1] = 1;
                    } else {
                        // A5 fallback: keep zero plaintext semantics but keep
                        // structurally valid packs in the worklist, disabling
                        // exact zero-pack elimination.
                        register_stagez2_entry(ti, tj, k);
                    }
                } else {
                    register_stagez2_entry(ti, tj, k);
                }

                HE->encoder->encode(poly, wpt({ti, tj, k}));
            }
        }
    }
    
    for (uint64_t ti = 0; ti < tiled_blocks_1; ++ti) {
        if (sparse_active_input_tile[ti]) {
            sparse_rotation_slots += sparse_max_rot_by_input_tile[ti];
        }
    }

    std::cout << "CirLinearNest PackWeight: zero_packs="
              << sparse_zero_packs << "/" << sparse_total_packs
              << ", active_packs=" << sparse_active_packs
              << ", sparse_entries=" << sparse_entries.size()
              << ", rotation_slots=" << sparse_rotation_slots
              << ", enable_stage_z2=" << (dazg_orbit::ablation::EnableStageZ2() ? 1 : 0)
              << ", schedule=" << DAZGOrbitStageZ2ScheduleName()
              << std::endl;

    return wpt;
}

Tensor<uint64_t> CirLinearNest::PackActivation(Tensor<uint64_t> &x) {

// DAZG_ORBIT_V666_ACTIVATION_CANON_ENTRY
if (DAZGOrbitV666CoreRepairEnabled()) {
  uint64_t __p = DAZGOrbitV666PlainMod();
  for (int64_t __i = 0; __i < x.size(); ++__i) { x.data()[__i] = x.data()[__i] % __p; }
}


    /**
     * Pack activations into message tensor for encryption.
     * 
     * For tile_size=1: like CirLinearSimple, only first half
     * For tile_size>1: both halves for row batching
     */
    Utils::CyclicNTT cyclic_ntt(ntt_size, HE->plain_mod);
    Tensor<uint64_t> ac_msg({tiled_blocks_1, HE->polyModulusDegree});
    const uint64_t half_degree = HE->polyModulusDegree / 2;
    std::vector<uint64_t> x_coef(ntt_size, 0);
    
    for (uint64_t ti = 0; ti < tiled_blocks_1; ti++) {
        for (uint64_t l = 0; l < tile_size; l++) {
            uint64_t blk = ti * tile_size + l;
            if (blk >= num_blocks_1) continue;
            
            // Coefficient encode: x̂[i * padded_dim_0 + j] = X[i, j]
            std::fill(x_coef.begin(), x_coef.end(), 0);
            const uint64_t blk_base = blk * block_size;
            for (uint64_t i = 0; i < block_size; i++) {
                for (uint64_t j = 0; j < dim_0; j++) {
                    uint64_t ch = blk_base + i;
                    if (ch < dim_1) {
                        x_coef[i * padded_dim_0 + j] = x({j, ch});
                    }
                }
            }
            
            // Apply cyclic NTT (in-place)
            cyclic_ntt.ComputeForward(x_coef.data(), x_coef.data());
            
            // Place in polynomial
            uint64_t offset = l * ntt_size;
            for (uint64_t m = 0; m < ntt_size; m++) {
                ac_msg({ti, offset + m}) = x_coef[m];
            }
            if (tile_size > 1) {
                uint64_t offset2 = l * ntt_size + half_degree;
                for (uint64_t m = 0; m < ntt_size; m++) {
                    ac_msg({ti, offset2 + m}) = x_coef[m];
                }
            }
        }
    }
    
    return ac_msg;
}

Tensor<UnifiedCiphertext> CirLinearNest::HECompute(
    const Tensor<UnifiedPlaintext> &wpt,
    Tensor<UnifiedCiphertext> &ac_ct) 
{
    const auto target = HE->server ? HE->Backend() : HOST;
    Tensor<UnifiedCiphertext> out_ct({tiled_blocks_2}, target);

    rotation_count = 0;
    multiply_count = 0;
    rotation_time_ms = 0;
    multiply_time_ms = 0;

    if (!HE->server) return out_ct;

    UnifiedGaloisKeys* keys = HE->galoisKeys;

    auto time_rotation = [&](auto&& func) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        rotation_time_ms += std::chrono::duration<double, std::milli>(end - start).count();
        rotation_count++;
    };

    auto time_multiply = [&](auto&& func) {
        auto start = std::chrono::high_resolution_clock::now();
        func();
        auto end = std::chrono::high_resolution_clock::now();
        multiply_time_ms += std::chrono::duration<double, std::milli>(end - start).count();
        multiply_count++;
    };

    std::vector<uint64_t> has_output(tiled_blocks_2, 0);

    auto fill_missing_output = [&]() {
        bool have_zero = false;
        UnifiedCiphertext zero_ct(target);
        for (uint64_t tj = 0; tj < tiled_blocks_2; ++tj) {
            if (has_output[tj]) continue;
            if (!have_zero) {
                zero_ct = HE->GenerateZeroCiphertext(target);
                have_zero = true;
            }
            out_ct(tj) = zero_ct;
        }
    };

    if (tile_size == 1) {
        for (const auto& e : sparse_entries) {
            UnifiedCiphertext tmp(target);
            time_multiply([&]() {
                HE->evaluator->multiply_plain(ac_ct(e.ti), wpt({e.ti, e.tj, 0}), tmp);
            });
            if (!has_output[e.tj]) {
                out_ct(e.tj) = tmp;
                has_output[e.tj] = 1;
            } else {
                HE->evaluator->add_inplace(out_ct(e.tj), tmp);
            }
        }
        fill_missing_output();
        return out_ct;
    }

    Tensor<UnifiedCiphertext> ac_rot({input_rot, tiled_blocks_1}, target);
    for (uint64_t ti = 0; ti < tiled_blocks_1; ++ti) {
        if (ti >= sparse_active_input_tile.size() || sparse_active_input_tile[ti] == 0) {
            continue;
        }
        ac_rot({0, ti}) = ac_ct(ti);
        const uint64_t max_rot = sparse_max_rot_by_input_tile[ti];
        for (uint64_t r = 1; r <= max_rot; ++r) {
            time_rotation([&]() {
                HE->evaluator->rotate_rows(ac_rot({r - 1, ti}), ntt_size, *keys, ac_rot({r, ti}));
            });
        }
    }

    Tensor<UnifiedCiphertext> int_ct({tiled_blocks_2, tile_size}, target);
    std::vector<uint64_t> has_partial(static_cast<size_t>(tiled_blocks_2 * tile_size), 0);
    auto partial_idx = [&](uint64_t tj, uint64_t k) -> size_t {
        return static_cast<size_t>(tj * tile_size + k);
    };

    for (const auto& e : sparse_entries) {
        UnifiedCiphertext tmp(target);
        time_multiply([&]() {
            HE->evaluator->multiply_plain(ac_rot({e.rot_idx, e.ti}), wpt({e.ti, e.tj, e.k}), tmp);
        });
        const size_t idx = partial_idx(e.tj, e.k);
        if (!has_partial[idx]) {
            int_ct({e.tj, e.k}) = tmp;
            has_partial[idx] = 1;
        } else {
            HE->evaluator->add_inplace(int_ct({e.tj, e.k}), tmp);
        }
    }

    const uint64_t num_groups = (tile_size + input_rot - 1) / input_rot;
    const uint64_t rotate_step = ntt_size * input_rot;
    std::vector<uint64_t> has_group(static_cast<size_t>(tiled_blocks_2 * num_groups), 0);
    auto group_idx = [&](uint64_t tj, uint64_t g) -> size_t {
        return static_cast<size_t>(tj * num_groups + g);
    };

    for (uint64_t tj = 0; tj < tiled_blocks_2; ++tj) {
        for (uint64_t k = 0; k < tile_size; ++k) {
            if (!has_partial[partial_idx(tj, k)]) continue;
            const uint64_t g = k / input_rot;
            const uint64_t head_k = g * input_rot;
            const size_t gi = group_idx(tj, g);
            if (!has_group[gi]) {
                if (k != head_k) {
                    int_ct({tj, head_k}) = int_ct({tj, k});
                }
                has_group[gi] = 1;
            } else {
                HE->evaluator->add_inplace(int_ct({tj, head_k}), int_ct({tj, k}));
            }
        }
    }

    for (uint64_t tj = 0; tj < tiled_blocks_2; ++tj) {
        uint64_t first_g = num_groups;
        for (uint64_t g = 0; g < num_groups; ++g) {
            if (!has_group[group_idx(tj, g)]) continue;
            first_g = g;
            break;
        }
        if (first_g == num_groups) continue;

        out_ct(tj) = int_ct({tj, first_g * input_rot});
        has_output[tj] = 1;

        // Keep the dense BSGS slot alignment: zero group additions can be
        // skipped, but trailing giant-step rotations are part of the exact
        // output layout expected by DepackResult.
        for (uint64_t g = first_g + 1; g < num_groups; ++g) {
            time_rotation([&]() {
                HE->evaluator->rotate_rows(out_ct(tj), rotate_step, *keys, out_ct(tj));
            });
            if (has_group[group_idx(tj, g)]) {
                HE->evaluator->add_inplace(out_ct(tj), int_ct({tj, g * input_rot}));
            }
        }
    }

    fill_missing_output();
    return out_ct;
}

Tensor<uint64_t> CirLinearNest::DepackResult(Tensor<uint64_t> &out_msg) {
    /**
     * Depack results from HE output.
     * 
     * Apply cyclic iNTT to each block, then extract using Theorem 1:
     *   Y[batch, out_ch] = ŷ[i * padded_dim_0 + batch]
     * where out_ch = out_blk * block_size + i
     */
    Utils::CyclicNTT cyclic_ntt(ntt_size, HE->plain_mod);
    Tensor<uint64_t> y({dim_0, dim_2});
    std::vector<uint64_t> y_ntt(ntt_size, 0);
    
    for (uint64_t tj = 0; tj < tiled_blocks_2; tj++) {
        for (uint64_t l = 0; l < tile_size; l++) {
            // Determine output block index
            // For tile_size=1: out_blk = tj
            // Otherwise: reverse of BSGS pattern
            uint64_t out_blk;
            if (tile_size == 1) {
                out_blk = tj;
            } else {
                // Result is at slot l, corresponding to anti-diagonal position
                // When k=0, out_blk = tj * tile_size + (tile_size - 1 - l) % tile_size
                out_blk = tj * tile_size + (tile_size - 1 - l + tile_size) % tile_size;
            }
            
            if (out_blk >= num_blocks_2) continue;
            
            // Extract NTT values for this block
            uint64_t offset = l * ntt_size;
            for (uint64_t m = 0; m < ntt_size; m++) {
                y_ntt[m] = out_msg({tj, offset + m});
            }
            
            // Apply cyclic iNTT (in-place)
            cyclic_ntt.ComputeInverse(y_ntt.data(), y_ntt.data());
            
            // Extract: Y[batch, out_ch] = ŷ[i * padded_dim_0 + batch]
            for (uint64_t i = 0; i < block_size; i++) {
                uint64_t out_ch = out_blk * block_size + i;
                if (out_ch >= dim_2) continue;
                
                for (uint64_t batch = 0; batch < dim_0; batch++) {
                    y({batch, out_ch}) = y_ntt[i * padded_dim_0 + batch];
                }
            }
        }
    }
    
    DAZGOrbitV9AddFoldedBiasLinear2D(y, this->bias, HE);
    return y;
}

Tensor<uint64_t> CirLinearNest::operator()(Tensor<uint64_t> &x) {

// DAZG_ORBIT_V666_ACTIVATION_CANON_ENTRY
if (DAZGOrbitV666CoreRepairEnabled()) {
  uint64_t __p = DAZGOrbitV666PlainMod();
  for (int64_t __i = 0; __i < x.size(); ++__i) { x.data()[__i] = x.data()[__i] % __p; }
}


    Tensor<uint64_t> ac_msg = PackActivation(x);
    Tensor<UnifiedCiphertext> ac_ct = Operator::SSToHE(ac_msg, HE);
    Tensor<UnifiedCiphertext> out_ct = HECompute(weight_pt, ac_ct);
    Tensor<uint64_t> out_msg = Operator::HEToSS(out_ct, HE);
    
// DAZG_ORBIT_V666_OUTMSG_CANON_BEFORE_DEPACK
if (DAZGOrbitV666CoreRepairEnabled()) {
  uint64_t __p = DAZGOrbitV666PlainMod();
  for (int64_t __i = 0; __i < out_msg.size(); ++__i) { out_msg.data()[__i] = out_msg.data()[__i] % __p; }
}

Tensor<uint64_t> y = DepackResult(out_msg);
    return y;
}

} // namespace LinearLayer
