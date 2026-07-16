// DAZG-Orbit Project Source File
// Component: Layer/LinearLayer/src/Stage2StaticConv.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <LinearLayer/Conv.h>
#include <seal/util/polyarithsmallmod.h>
#include <algorithm>
#include <random>
#include <thread>
#include <chrono>
#include <Utils/dazg_orbit_domain_planner.h>
#include "Utils/dazg_orbit_determinism.h"
#define PROFILE_STAGE1_STATIC

using namespace seal;
using namespace LinearLayer;

namespace {
using Clock = std::chrono::steady_clock;

inline long long UsBetween(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
}

thread_local uint64_t g_stage1_last_mul_plain = 0;
thread_local uint64_t g_stage1_last_add_inplace = 0;
thread_local uint64_t g_stage1_last_rotate_rows = 0;
} // namespace


// 计算上取整除法
int Conv2DStage2Static::DivUpper(int a, int b) {
    return ((a + b - 1) / b);
}

// 计算计算开销
int Conv2DStage2Static::CalculateCost(int H, int W, int h, int Hw, int Ww, int C, int N) {
    return (int)ceil((double)C / (N / (Hw * Ww))) *
           (int)ceil((double)(H - h + 1) / (Hw - h + 1)) *
           (int)ceil((double)(W - h + 1) / (Ww - h + 1));
}

// 查找最佳分块方式
void Conv2DStage2Static::FindOptimalPartition(int H, int W, int h, int C, int N, int* optimalHw, int* optimalWw) {
    int min_cost = (1 << 30);
    for (int Hw = h; Hw <= H; Hw++) {
        for (int Ww = h; Ww <= W; Ww++) {
            if (Hw * Ww > N) continue;
            int cost = CalculateCost(H, W, h, Hw, Ww, C, N);
            if (cost < min_cost) {
                min_cost = cost;
                *optimalHw = Hw;
                *optimalWw = Ww;
            }
        }
    }
}


void Conv2DStage2Static::compute_he_params(uint64_t raw_in_feature_size) {
    const uint64_t padded_in = raw_in_feature_size + 2 * padding;
    this->in_feature_size = padded_in;

    int optimalHw = padded_in, optimalWw = padded_in;
    FindOptimalPartition(padded_in, padded_in, kernel_size, in_channels,
                         polyModulusDegree, &optimalHw, &optimalWw);

    HW = optimalHw;
    WW = optimalWw;
    CW = min(in_channels, (polyModulusDegree / (HW * WW)));
    MW = min(out_channels, (polyModulusDegree / (CW * HW * WW)));
    dM = DivUpper(out_channels, MW);
    dC = DivUpper(in_channels, CW);
    dH = DivUpper(padded_in - kernel_size + 1, HW - kernel_size + 1);
    dW = DivUpper(padded_in - kernel_size + 1, WW - kernel_size + 1);
    OW = HW * WW * (MW * CW - 1) + WW * (kernel_size - 1) + kernel_size - 1;
    HOut = (padded_in - kernel_size + stride) / stride;
    WOut = (padded_in - kernel_size + stride) / stride;
    HWprime = (HW - kernel_size + stride) / stride;
    WWprime = (WW - kernel_size + stride) / stride;
    fused_bn = false;
}


Conv2DStage2Static::Conv2DStage2Static(uint64_t in_feature_size,
                                           uint64_t stride,
                                           uint64_t padding,
                                           const Tensor<uint64_t>& weight,
                                           const Tensor<uint64_t>& bias,
                                           HE::HEEvaluator* HE)
    : Conv2D(in_feature_size, stride, padding, weight, bias, HE)
{
    std::vector<size_t> shape = weight.shape();
    in_channels = shape[1];
    out_channels = shape[0];
    kernel_size = shape[2];
    polyModulusDegree = HE->polyModulusDegree;
    plain = HE->plain_mod;
    this->padding = padding;

    auto t_he0 = Clock::now();
    compute_he_params(in_feature_size);
    auto t_he1 = Clock::now();

    auto t_plan0 = Clock::now();
    PreparePlan();
    auto t_plan1 = Clock::now();

    long long pack_weight_us = 0;
    if (HE->server) {
        auto t_pack0 = Clock::now();
        weight_pt = PackWeight();
        auto t_pack1 = Clock::now();
        pack_weight_us = UsBetween(t_pack0, t_pack1);
    }

    fused_bn = false;

const bool hot_stage2 =
    (in_channels == 128 && out_channels == 128 &&
     kernel_size == 3 && stride == 1 &&
     padding == 1 && HOut == 28 && WOut == 28);

#ifdef PROFILE_STAGE1_STATIC
    if (hot_stage2) {
        std::cout << "[Stage2StaticCtor] "
                  << "he_param_us=" << UsBetween(t_he0, t_he1)
                  << ", prepare_plan_us=" << UsBetween(t_plan0, t_plan1)
                  << ", pack_weight_us=" << pack_weight_us
                  << ", dC=" << dC
                  << ", dH=" << dH
                  << ", dW=" << dW
                  << ", pack_len=" << pack_len_
                  << std::endl;
    }
#endif
}


Conv2DStage2Static::Conv2DStage2Static(
    uint64_t in_feature_size,
    uint64_t in_channels,
    uint64_t out_channels,
    uint64_t kernel_size,
    uint64_t stride,
    HE::HEEvaluator* HE)
    : Conv2D(in_feature_size, in_channels, out_channels, kernel_size, stride, HE)
{
    polyModulusDegree = HE->polyModulusDegree;
    plain = HE->plain_mod;

    auto t_he0 = Clock::now();
    compute_he_params(in_feature_size);
    auto t_he1 = Clock::now();

    auto t_plan0 = Clock::now();
    PreparePlan();
    auto t_plan1 = Clock::now();

    long long pack_weight_us = 0;
    if (HE->server) {
        auto t_pack0 = Clock::now();
        weight_pt = PackWeight();
        auto t_pack1 = Clock::now();
        pack_weight_us = UsBetween(t_pack0, t_pack1);
    }

    fused_bn = false;

const bool hot_stage2 =
    (in_channels == 128 && out_channels == 128 &&
     kernel_size == 3 && stride == 1 &&
     padding == 1 && HOut == 28 && WOut == 28);


#ifdef PROFILE_STAGE1_STATIC
    if (hot_stage2) {
        std::cout << "[Stage2StaticCtor] "
                  << "he_param_us=" << UsBetween(t_he0, t_he1)
                  << ", prepare_plan_us=" << UsBetween(t_plan0, t_plan1)
                  << ", pack_weight_us=" << pack_weight_us
                  << ", dC=" << dC
                  << ", dH=" << dH
                  << ", dW=" << dW
                  << ", pack_len=" << pack_len_
                  << std::endl;
    }
#endif
}




void Conv2DStage2Static::PreparePlan() {
    const size_t paddedH = in_feature_size;       // 已含 padding
    const size_t innerH  = paddedH - 2 * padding; // 原始输入边长

    pack_len_ = static_cast<size_t>(CW) * HW * WW;
    num_poly_ = static_cast<size_t>(dC) * dH * dW;

    pack_src_index_.assign(num_poly_ * pack_len_, -1);
    depack_src_index_.assign(static_cast<size_t>(out_channels) * HOut * WOut, -1);
    coeff_scratch_.assign(pack_len_, 0);

    for (size_t gama = 0; gama < dC; ++gama) {
        const size_t ch_base = gama * CW;
        for (size_t alpha = 0; alpha < dH; ++alpha) {
            const size_t row_base = alpha * (HW - kernel_size + 1);
            for (size_t beta = 0; beta < dW; ++beta) {
                const size_t col_base = beta * (WW - kernel_size + 1);
                const size_t block_base =
                    (((gama * dH) + alpha) * dW + beta) * pack_len_;

                for (size_t ic = 0; ic < CW; ++ic) {
                    const size_t ch = ch_base + ic;
                    const size_t ic_base = block_base + ic * HW * WW;

                    for (size_t jh = 0; jh < HW; ++jh) {
                        const size_t src_h_padded = row_base + jh;
                        const size_t j_base = ic_base + jh * WW;

                        for (size_t kw = 0; kw < WW; ++kw) {
                            const size_t src_w_padded = col_base + kw;
                            const size_t dst = j_base + kw;

                            if (ch < in_channels &&
                                src_h_padded >= padding &&
                                src_h_padded < padding + innerH &&
                                src_w_padded >= padding &&
                                src_w_padded < padding + innerH) {

                                const size_t x_h = src_h_padded - padding;
                                const size_t x_w = src_w_padded - padding;

                                pack_src_index_[dst] =
                                    static_cast<int>((ch * innerH + x_h) * innerH + x_w);
                            }
                        }
                    }
                }
            }
        }
    }

    for (size_t cprime = 0; cprime < out_channels; ++cprime) {
        for (size_t iprime = 0; iprime < HOut; ++iprime) {
            for (size_t jprime = 0; jprime < WOut; ++jprime) {
                size_t c = cprime % MW;
                size_t i = (iprime * stride) % (HW - kernel_size + 1);
                size_t j = (jprime * stride) % (WW - kernel_size + 1);
                size_t theta = cprime / MW;
                size_t alpha = (iprime * stride) / (HW - kernel_size + 1);
                size_t beta  = (jprime * stride) / (WW - kernel_size + 1);
                size_t des = OW - c * CW * HW * WW + i * WW + j;

                size_t dst = (cprime * HOut + iprime) * WOut + jprime;
                depack_src_index_[dst] =
                    static_cast<int>((((theta * dH) + alpha) * dW + beta) *
                                     polyModulusDegree + des);
            }
        }
    }

    plan_ready = true;
}


void Conv2DStage2Static::fuse_bn(Tensor<uint64_t> *gamma, Tensor<uint64_t> *beta){
    Tensor<uint64_t> kernelFuse({out_channels, in_channels, kernel_size, kernel_size}, 0);
    for (size_t i = 0; i < out_channels; i++){
        for (size_t j = 0; j < in_channels; j++){
            for (size_t k = 0; k < kernel_size; k++){
                for (size_t l = 0; l < kernel_size; l++){
                    kernelFuse({i, j, k, l}) = this->weight({i, j, k, l}) * (*gamma)({i});
                }
            }
        }
    }
    this->weight = kernelFuse;
    Tensor<uint64_t> biasFuse({out_channels, HOut, WOut}, 0);
    std::cout << "Hprime:" << HOut << std::endl;
    std::cout << "Wprime:" << WOut << std::endl; 

    for (size_t i = 0; i < out_channels; i++){
        for (size_t j = 0; j < HOut; j++){
            for (size_t k = 0; k < WOut; k++){
                biasFuse({i, j, k}) = this->bias({i, j, k}) * (*gamma)({i}) + (*beta)({i});
            }
        }
    }
    this->bias = biasFuse;
}


Conv2DStage2Static::Conv2DStage2Static(
    uint64_t in_feature_size,
    uint64_t stride,
    uint64_t padding,
    const Tensor<uint64_t>& weight,
    const Tensor<uint64_t>& bias,
    HE::HEEvaluator* HE,
    Tensor<uint64_t> *gamma,
    Tensor<uint64_t> *beta)
    : Conv2DStage2Static(in_feature_size, stride, padding, weight, bias, HE)
{
    this->fused_bn = true;
    this->fuse_bn(gamma, beta);

    if (HE->server) {
        weight_pt = this->PackWeight();
    }
}



// 加密张量
Tensor<UnifiedCiphertext> Conv2DStage2Static::EncryptTensor(Tensor<UnifiedPlaintext> plainTensor) {
    std::vector<size_t> shapeTab = {dC ,dH , dW};
    Tensor<UnifiedCiphertext> TalphabetaCipher(shapeTab, HE->GenerateZeroCiphertext());
    for (unsigned long gama = 0; gama < dC; gama++) {
        for (unsigned long alpha = 0; alpha < dH; alpha++) {
            for (unsigned long beta = 0; beta < dW; beta++) {
                HE->encryptor->encrypt(plainTensor(gama * dH * dW + alpha * dW + beta), TalphabetaCipher(gama * dH * dW + alpha * dW + beta));
            }
        }
    }
    return TalphabetaCipher;
}

Tensor<uint64_t> Conv2DStage2Static::HETOTensor (Tensor<UnifiedCiphertext> inputCipher){
    auto shapeTab = inputCipher.shape();
    Tensor<UnifiedCiphertext> cipherMask(shapeTab,HE->GenerateZeroCiphertext());
    Tensor<UnifiedPlaintext> plainMask(shapeTab,HOST);
    size_t numPoly = 1;
    for (int num : shapeTab) {
        numPoly *= num;
    }
    auto tensorShapeTab = shapeTab;
    tensorShapeTab.push_back(polyModulusDegree);

    Tensor<uint64_t> tensorMask(tensorShapeTab, 0);
    UnifiedPlaintext plainMaskInv(HOST);
    if (HE->server) {
        int64_t mask;
        auto gen = dazg_orbit_det::MakeMt19937("Stage2StaticConv");
        std::uniform_int_distribution<int64_t> dist(0, plain - 1);
        for (size_t i = 0; i < numPoly; i++){
            plainMask(i).hplain().resize(polyModulusDegree);
            plainMaskInv.hplain().resize(polyModulusDegree);
            for (size_t l = 0; l < polyModulusDegree; l++){
                mask = dist(gen);
                *(plainMask(i).hplain().data() + l) = mask;
                tensorMask((i) * polyModulusDegree + l) = mask;
                mask = plain - mask;
                *(plainMaskInv.hplain().data() + l) = mask;   
            }
            HE->evaluator->add_plain(inputCipher(i), plainMaskInv, cipherMask(i));
        }
        cipherMask.flatten();
        HE->SendEncVec(cipherMask);
        return tensorMask;

    }else{
        HE->ReceiveEncVec(cipherMask);
        for (size_t i = 0; i < numPoly; i++){
            this->HE->decryptor->decrypt(cipherMask(i), plainMask(i));
            for (size_t j = 0; j < polyModulusDegree; j++){
                tensorMask(i * polyModulusDegree + j) = *(plainMask(i).hplain().data() + j);
            }
        }
        return tensorMask;
    }
}

// 计算输入张量的 Pack 版本
Tensor<uint64_t> Conv2DStage2Static::PackActivation(Tensor<uint64_t> &x){
    Tensor<uint64_t> pack({dC, dH, dW, CW * HW * WW}, 0);
    for (size_t i = 0; i < pack_src_index_.size(); ++i) {
        int src = pack_src_index_[i];
        if (src >= 0) {
            pack(i) = x(static_cast<size_t>(src));
        }
    }
    return pack;
}

Tensor<UnifiedCiphertext> Conv2DStage2Static::TensorTOHE(Tensor<uint64_t> PackActivationTensor) {
    std::vector<size_t> shapeTab = PackActivationTensor.shape();
    size_t numPoly = 1;
    for (int num : shapeTab) {
        numPoly *= num;
    }

    int len = shapeTab.back(); 
    shapeTab.pop_back();
    numPoly /= len;
    Tensor<UnifiedPlaintext> T(shapeTab,Datatype::HOST);
    for (size_t i = 0; i < numPoly; i++){
        vector<uint64_t> Tsubv(len, 0);
        for (size_t j = 0; j < len; j++){
            Tsubv[j] = PackActivationTensor(i * len + j);
        }
        T(i).hplain().resize(polyModulusDegree);
        seal::util::modulo_poly_coeffs(Tsubv, len, plain, T(i).hplain().data());
        std::fill_n(T(i).hplain().data() + len, polyModulusDegree - len, 0);
    }
    Tensor<UnifiedCiphertext> finalpack(shapeTab, Datatype::HOST);
    if (!HE->server){
        //客服端
        Tensor<UnifiedCiphertext> enc(shapeTab, Datatype::HOST);
        for (size_t i = 0; i < numPoly; i++){
            this->HE->encryptor->encrypt(T(i), enc(i));
        }
        // enc.flatten();
        HE->SendEncVec(enc);
    }else{
        //服务器端
        Tensor<UnifiedCiphertext> encflatten({numPoly}, this->HE->GenerateZeroCiphertext());
        HE->ReceiveEncVec(encflatten);
        Tensor<UnifiedCiphertext> enc(shapeTab, Datatype::HOST);
        for (size_t i = 0; i < numPoly; i++){
            this->HE->evaluator->add_plain(encflatten(i), T(i), enc(i));
        }
        finalpack = enc;
    }
    return finalpack;
}

// 计算卷积核的 Pack 版本
Tensor<UnifiedPlaintext> Conv2DStage2Static::PackWeight() {
    cout << "pack weight begin" << endl;

    std::vector<size_t> shapeTab = {dM, dC};
    Tensor<UnifiedPlaintext> Ktg(shapeTab,HOST);
    size_t len = OW + 1;
    if (!HE->server){
        return Ktg;
    }

    for (unsigned long theta = 0; theta < dM; theta++){
        for (unsigned long gama = 0; gama < dC; gama++){
            vector<uint64_t> Tsubv (len,0); 
            for (unsigned long it = 0; it < MW; it++){
                for (unsigned long jg = 0; jg < CW; jg++){
                    if (((theta * MW + it) >= out_channels) || ((gama * CW + jg) >= in_channels)){
                        for (unsigned hr = 0; hr < kernel_size; hr++){
                            for (unsigned hc = 0; hc < kernel_size; hc++){
                                Tsubv[OW - it * CW * HW * WW - jg * HW * WW - hr * WW - hc] = 0;
                            }
                        }
                    }else{
                        for (unsigned hr = 0; hr < kernel_size; hr++){
                            for (unsigned hc = 0; hc < kernel_size; hc++){
                                int64_t element = this->weight({theta * MW + it, gama * CW + jg, hr, hc});
                                Tsubv[OW - it * CW * HW * WW - jg * HW * WW - hr * WW - hc] =
                                    (element >= 0) ? unsigned(element) : unsigned(element + plain);
                            }
                        }
                    }
                }
            }
            Ktg({theta,gama}).hplain().resize(polyModulusDegree);
            seal::util::modulo_poly_coeffs(Tsubv, len, plain, Ktg({theta, gama}).hplain().data());
            if (len < polyModulusDegree){
                std::fill_n(Ktg({theta,gama}).hplain().data() + len, polyModulusDegree - len, 0);
            }
        }
    }
    cout << "transfer weight to device" << endl;
    if (HE->Backend() == DEVICE){
        for (size_t i = 0; i < Ktg.size(); i++){
            Ktg(i).to_device(*HE->context);
        }
    }

    cout << "pack weight done" << endl;
    return Ktg;
}


Tensor<UnifiedCiphertext> Conv2DStage2Static::sumCP(Tensor<UnifiedCiphertext> cipherTensor, Tensor<UnifiedPlaintext> plainTensor){
    Tensor<UnifiedCiphertext> Talphabeta({dC, dH, dW}, HOST);
    for (size_t gama = 0; gama < dC; gama++){
        for (size_t alpha = 0; alpha < dH; alpha++){
            for (size_t beta = 0; beta < dW; beta++){
                HE->evaluator->add_plain(cipherTensor({gama,alpha,beta}), plainTensor({gama,alpha,beta}), Talphabeta({gama,alpha,beta}));
            }
        }
    }
    return Talphabeta;
}
   

Tensor<UnifiedCiphertext> Conv2DStage2Static::HECompute(
    const Tensor<UnifiedPlaintext> &weight_pt,
    Tensor<UnifiedCiphertext> &ac_ct)
{
    const auto target = HE->server ? HE->Backend() : HOST;
    std::vector<size_t> shapeTab = {
        static_cast<size_t>(dM),
        static_cast<size_t>(dH),
        static_cast<size_t>(dW)
    };
    Tensor<UnifiedCiphertext> out_ct(shapeTab, HE->GenerateZeroCiphertext(target));

    if (!HE->server) {
        return out_ct;
    }

    g_stage1_last_mul_plain = 0;
    g_stage1_last_add_inplace = 0;
    g_stage1_last_rotate_rows = 0;   // 这条核没有 rotation

    auto &eval = *HE->evaluator;
    UnifiedCiphertext interm(target);

    const size_t spatial_blocks = static_cast<size_t>(dH) * static_cast<size_t>(dW);
    const size_t dm_sz = static_cast<size_t>(dM);
    const size_t dc_sz = static_cast<size_t>(dC);
    const size_t weight_stride = dc_sz;

    // 目标：固定一个输入密文 ac_ct(gama, s)，连续复用到所有 theta
    // 比当前的 s -> theta -> gama 更像“输入驻留”
    for (size_t s = 0; s < spatial_blocks; ++s) {
        // gama = 0：先初始化所有 theta 的输出
        for (size_t theta = 0; theta < dm_sz; ++theta) {
            const size_t out_idx = theta * spatial_blocks + s;
            const size_t w_idx   = theta * weight_stride;

            eval.multiply_plain(
                ac_ct(s),
                weight_pt(w_idx),
                out_ct(out_idx)
            );
            ++g_stage1_last_mul_plain;
        }

        // gama = 1 ... dC-1：固定一个输入密文，扫完全部 theta
        for (size_t gama = 1; gama < dc_sz; ++gama) {
            const size_t src_idx = gama * spatial_blocks + s;

            for (size_t theta = 0; theta < dm_sz; ++theta) {
                const size_t out_idx = theta * spatial_blocks + s;
                const size_t w_idx   = theta * weight_stride + gama;

                eval.multiply_plain(
                    ac_ct(src_idx),
                    weight_pt(w_idx),
                    interm
                );
                ++g_stage1_last_mul_plain;

                eval.add_inplace(out_ct(out_idx), interm);
                ++g_stage1_last_add_inplace;
            }
        }
    }

    return out_ct;
}



Tensor<uint64_t> Conv2DStage2Static::DepackResult(Tensor<uint64_t> &out){
    if (!plan_ready) {
        PreparePlan();
    }

    Tensor<uint64_t> finalResult({out_channels, HOut, WOut}, 0);
    for (size_t i = 0; i < depack_src_index_.size(); ++i) {
        finalResult(i) = out(static_cast<size_t>(depack_src_index_[i]));
    }
    return finalResult;
}


Tensor<uint64_t> Conv2DStage2Static::operator()(Tensor<uint64_t> &x){
    long long plan_us = 0;
    if (!plan_ready) {
        auto tp0 = Clock::now();
        PreparePlan();
        auto tp1 = Clock::now();
        plan_us = UsBetween(tp0, tp1);
    }

    auto t0 = Clock::now();
    auto Cipher = this->PackActivationDirectToHE(x);
    auto t1 = Clock::now();

    auto ConvResult = this->HECompute(weight_pt, Cipher);
    auto t2 = Clock::now();

    auto share = Operator::HEToSS_coeff(ConvResult, HE);
    auto t3 = Clock::now();

    auto finalR = this->DepackResult(share);
    auto t4 = Clock::now();

const bool hot_stage2 =
    (in_channels == 128 && out_channels == 128 &&
     kernel_size == 3 && stride == 1 &&
     padding == 1 && HOut == 28 && WOut == 28);



#ifdef PROFILE_STAGE1_STATIC
if (hot_stage2) {
    std::cout << "[Stage2StaticOnline] "
              << "plan_us=" << plan_us
              << ", pack_to_he_us=" << UsBetween(t0, t1)
              << ", he_compute_us=" << UsBetween(t1, t2)
              << ", he_to_ss_us=" << UsBetween(t2, t3)
              << ", depack_us=" << UsBetween(t3, t4)
              << ", mul_plain=" << g_stage1_last_mul_plain
              << ", rotate_rows=" << g_stage1_last_rotate_rows
              << ", add_inplace=" << g_stage1_last_add_inplace
              << ", total_online_us=" << UsBetween(t0, t4)
              << std::endl;
}
#endif


    return finalR;
}



void Conv2DStage2Static::FillActivationPlaintexts(
    Tensor<uint64_t> &x,
    Tensor<UnifiedPlaintext> &T)
{
    if (!plan_ready) {
        PreparePlan();
    }

    for (size_t p = 0; p < num_poly_; ++p) {
        std::fill(coeff_scratch_.begin(), coeff_scratch_.end(), 0);

        const size_t base = p * pack_len_;
        for (size_t j = 0; j < pack_len_; ++j) {
            int src = pack_src_index_[base + j];
            if (src >= 0) {
                coeff_scratch_[j] = x(static_cast<size_t>(src));
            }
        }

        T(p).hplain().resize(polyModulusDegree);
        seal::util::modulo_poly_coeffs(
            coeff_scratch_,
            pack_len_,
            plain,
            T(p).hplain().data());

        if (pack_len_ < polyModulusDegree) {
            std::fill_n(
                T(p).hplain().data() + pack_len_,
                polyModulusDegree - pack_len_,
                0);
        }
    }
}

Tensor<UnifiedCiphertext> Conv2DStage2Static::PackActivationDirectToHE(Tensor<uint64_t> &x) {
    if (!plan_ready) {
        PreparePlan();
    }

    std::vector<size_t> shapeTab = {
        static_cast<size_t>(dC),
        static_cast<size_t>(dH),
        static_cast<size_t>(dW)
    };

    auto dazg_orbit_shape_with_slots = shapeTab;
    dazg_orbit_shape_with_slots.push_back(polyModulusDegree);
    dazg_orbit::domain::ScopedConversionRecord dazg_orbit_domain_record(
        "SSToHE_coeff_direct", dazg_orbit_shape_with_slots, HE, polyModulusDegree,
        "Conv2DStage2Static::PackActivationDirectToHE");

    Tensor<UnifiedPlaintext> T(shapeTab, Datatype::HOST);
    FillActivationPlaintexts(x, T);

    Tensor<UnifiedCiphertext> finalpack(shapeTab, Datatype::HOST);

    if (!HE->server) {
        Tensor<UnifiedCiphertext> enc(shapeTab, Datatype::HOST);
        for (size_t i = 0; i < num_poly_; ++i) {
            HE->encryptor->encrypt(T(i), enc(i));
        }
        HE->SendEncVec(enc);
    } else {
        Tensor<UnifiedCiphertext> encflatten({num_poly_}, Datatype::HOST);
        HE->ReceiveEncVec(encflatten);

        Tensor<UnifiedCiphertext> enc(shapeTab, Datatype::HOST);
        for (size_t i = 0; i < num_poly_; ++i) {
            HE->evaluator->add_plain(encflatten(i), T(i), enc(i));
        }
        finalpack = enc;
    }

    return finalpack;
}


 // namespace LinearLayer