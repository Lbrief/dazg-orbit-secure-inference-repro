// DAZG-Orbit Project Source File
// Component: Layer/LinearLayer/src/ConvCheetah.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <LinearLayer/Conv.h>
#include <seal/util/polyarithsmallmod.h>
#include <algorithm>
#include <chrono>
#include "Utils/dazg_orbit_determinism.h"
#include <cstdlib>

using namespace seal;
using namespace LinearLayer;

// DAZG_ORBIT_V9_FOLDED_BIAS_ADD_PATCH_BEGIN
// Add folded Conv/BN bias to exactly one secret share after HE-to-SS depacking.
// The DAZG-GELU bias^2 bridge stores folded_bias at the pre-truncation scale.
// This changes no HE rotations; baseline and policy both use the same layer code.
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

static void DAZGOrbitV9AddFoldedBiasConv3D(Tensor<uint64_t>& y,
                                         const Tensor<uint64_t>& bias,
                                         HEEvaluator* he)
{
    if (!DAZGOrbitV9ShouldAddBias(he)) return;
    const std::vector<size_t>& ys = y.shape();
    const std::vector<size_t>& bs = bias.shape();
    if (ys.size() != 3 || bs.empty()) return;

    const size_t C = ys[0];
    const size_t H = ys[1];
    const size_t W = ys[2];

    if (bs.size() == 1 && bs[0] == C) {
        for (size_t c = 0; c < C; ++c) {
            const uint64_t b = bias({c});
            for (size_t i = 0; i < H; ++i) {
                for (size_t j = 0; j < W; ++j) {
                    y({c, i, j}) = y({c, i, j}) + b;
                }
            }
        }
        return;
    }

    if (bs.size() == 3 && bs[0] == C && bs[1] == H && bs[2] == W) {
        for (size_t c = 0; c < C; ++c) {
            for (size_t i = 0; i < H; ++i) {
                for (size_t j = 0; j < W; ++j) {
                    y({c, i, j}) = y({c, i, j}) + bias({c, i, j});
                }
            }
        }
    }
}
// DAZG_ORBIT_V9_FOLDED_BIAS_ADD_PATCH_END


namespace {
inline long long UsBetween(std::chrono::steady_clock::time_point a,
                           std::chrono::steady_clock::time_point b) {
        return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    }
}



// 计算上取整除法
int Conv2DCheetah::DivUpper(int a, int b) {
    return ((a + b - 1) / b);
}

// 计算计算开销
int Conv2DCheetah::CalculateCost(int H, int W, int h, int Hw, int Ww, int C, int N) {
    return (int)ceil((double)C / (N / (Hw * Ww))) *
           (int)ceil((double)(H - h + 1) / (Hw - h + 1)) *
           (int)ceil((double)(W - h + 1) / (Ww - h + 1));
}

// 查找最佳分块方式
void Conv2DCheetah::FindOptimalPartition(int H, int W, int h, int C, int N, int* optimalHw, int* optimalWw) {
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


void Conv2DCheetah::compute_he_params(uint64_t raw_in_feature_size) {
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


Conv2DCheetah::Conv2DCheetah(uint64_t in_feature_size, uint64_t stride, uint64_t padding, const Tensor<uint64_t>& weight, const Tensor<uint64_t>& bias, HE::HEEvaluator* HE)
    : Conv2D(in_feature_size + 2 * padding, stride, padding, weight, bias, HE)
{
    std::vector<size_t> shape = weight.shape();
    in_channels = shape[1];
    out_channels = shape[0];
    kernel_size = shape[2];
    polyModulusDegree = HE->polyModulusDegree;
    this->padding = padding;
    //in_feature_size = in_feature_size + 2 * padding;
    int optimalHw = this->in_feature_size, optimalWw = this->in_feature_size;
    cout << "in_feature_size:" << this->in_feature_size << endl;
    FindOptimalPartition(this->in_feature_size, this->in_feature_size, kernel_size, in_channels, polyModulusDegree, &optimalHw, &optimalWw);
    cout << "optimalHw:" << optimalHw << endl;
    HW = optimalHw;
    WW = optimalWw;
    CW = min(in_channels, (polyModulusDegree / (HW * WW)));
    MW = min(out_channels, (polyModulusDegree / (CW * HW * WW)));
    dM = DivUpper(out_channels,MW);
    dC = DivUpper(in_channels,CW);
    cout << "dC,dM:" << dC << "," << dM << endl;
    dH = DivUpper(this->in_feature_size - kernel_size + 1 , HW - kernel_size + 1);
    dW = DivUpper(this->in_feature_size - kernel_size + 1 , WW - kernel_size + 1);
    OW = HW * WW * (MW * CW - 1) + WW * (kernel_size - 1) + kernel_size - 1;
    HOut = (this->in_feature_size - kernel_size + stride) / stride;
    WOut = (this->in_feature_size - kernel_size + stride) / stride;
    HWprime = (HW - kernel_size + stride) / stride;
    WWprime = (WW - kernel_size + stride) / stride;
    polyModulusDegree = HE->polyModulusDegree;
    plain = HE->plain_mod;
    // std::cout << "plain" << plain;
    weight_pt = this->PackWeight();
    this->fused_bn = false;
    cout << "feature_size:" << this->in_feature_size << endl;
    cout << "Hprime:" << HOut << endl;
    cout << "Wprime:" << WOut << endl;
    cout << "HWPrime:" << HWprime << endl;
    cout << "WWprime:" << WWprime << endl;
    cout << "Conv2DCheetah constructor done" << endl;
};

Conv2DCheetah::Conv2DCheetah(
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
    compute_he_params(in_feature_size);

    hot56_fastpath =
        (in_feature_size == 56 &&
         in_channels == 64 &&
         out_channels == 64 &&
         kernel_size == 3 &&
         stride == 1 &&
         padding == 1);

    if (hot56_fastpath) {
        PrepareHot56Tables();
    }

    if (HE->server) {
        weight_pt = PackWeight();
    }

    cout << "padding:" << padding << endl;
}


void Conv2DCheetah::PrepareHot56Tables() {
    const size_t paddedH = in_feature_size;          // 已含 padding
    const size_t innerH  = paddedH - 2 * padding;    // 原始输入边长
    const size_t pack_len = CW * HW * WW;

    hot56_pack_src_index.assign(dC * dH * dW * pack_len, -1);
    hot56_depack_src_index.assign(out_channels * HOut * WOut, -1);

    for (size_t gama = 0; gama < dC; ++gama) {
        const size_t ch_base = gama * CW;
        for (size_t alpha = 0; alpha < dH; ++alpha) {
            const size_t row_base = alpha * (HW - kernel_size + 1);
            for (size_t beta = 0; beta < dW; ++beta) {
                const size_t col_base = beta * (WW - kernel_size + 1);
                const size_t block_base = (((gama * dH) + alpha) * dW + beta) * pack_len;

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

                                hot56_pack_src_index[dst] =
                                    static_cast<int>((ch * innerH + x_h) * innerH + x_w);
                            }
                        }
                    }
                }
            }
        }
    }

    // 预计算 DepackResult 的 source index
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
                hot56_depack_src_index[dst] =
                    static_cast<int>((((theta * dH) + alpha) * dW + beta) * polyModulusDegree + des);
            }
        }
    }

    hot56_tables_ready = true;
}

Tensor<uint64_t> Conv2DCheetah::PackActivationHot56(Tensor<uint64_t> &x) {
    const size_t pack_len = CW * HW * WW;
    Tensor<uint64_t> pack({dC, dH, dW, pack_len}, 0);

    for (size_t i = 0; i < hot56_pack_src_index.size(); ++i) {
        const int src = hot56_pack_src_index[i];
        if (src >= 0) {
            pack(i) = x(static_cast<size_t>(src));
        }
    }

    return pack;
}


Tensor<uint64_t> Conv2DCheetah::DepackResultHot56(Tensor<uint64_t> &out) {
    Tensor<uint64_t> finalResult({out_channels, HOut, WOut}, 0);

    const size_t total = out_channels * HOut * WOut;
    for (size_t i = 0; i < total; ++i) {
        int src = hot56_depack_src_index[i];
        finalResult(i) = out(static_cast<size_t>(src));
    }

    DAZGOrbitV9AddFoldedBiasConv3D(finalResult, this->bias, HE);
    return finalResult;
}




void Conv2DCheetah::fuse_bn(Tensor<uint64_t> *gamma, Tensor<uint64_t> *beta){
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


Conv2DCheetah::Conv2DCheetah (uint64_t in_feature_size, uint64_t stride, uint64_t padding, const Tensor<uint64_t>& weight, const Tensor<uint64_t>& bias, HE::HEEvaluator* HE, Tensor<uint64_t> *gamma, Tensor<uint64_t> *beta)
    : Conv2DCheetah(in_feature_size, stride, padding, weight, bias, HE)
{
    this->fused_bn = true;
    this->fuse_bn(gamma, beta);
    weight_pt = this->PackWeight();
};


// 加密张量
Tensor<UnifiedCiphertext> Conv2DCheetah::EncryptTensor(Tensor<UnifiedPlaintext> plainTensor) {
    std::vector<size_t> shapeTab = {dC ,dH , dW};
    Tensor<UnifiedCiphertext> TalphabetaCipher(shapeTab, HE->GenerateZeroCiphertext());
    for (unsigned long gama = 0; gama < dC; gama++) {
        for (unsigned long alpha = 0; alpha < dH; alpha++) {
            for (unsigned long beta = 0; beta < dW; beta++) {
                HE->encryptor->encrypt(plainTensor(gama * dH * dW + alpha + beta), TalphabetaCipher(gama * dH * dW + alpha * dW + beta));
            }
        }
    }
    return TalphabetaCipher;
}

Tensor<uint64_t> Conv2DCheetah::HETOTensor (Tensor<UnifiedCiphertext> inputCipher){
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
        auto gen = dazg_orbit_det::MakeMt19937("ConvCheetah");
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
Tensor<uint64_t> Conv2DCheetah::PackActivation(Tensor<uint64_t> &x){
        if (hot56_fastpath && hot56_tables_ready) {
        return PackActivationHot56(x);
    }
    Tensor<uint64_t> padded_x ({in_channels, in_feature_size, in_feature_size} ,0);
    for (size_t i = 0; i < in_channels; i++){
        for (size_t j = 0; j < (in_feature_size - 2 * padding); j++){
            for (size_t k = 0; k < (in_feature_size - 2 * padding); k++){
                padded_x({i, j + padding, k + padding}) = x({i, j, k});
            }
        }
    }
    size_t len = CW * HW * WW;
    Tensor<uint64_t> Tsub ({CW, HW, WW});
    Tensor<uint64_t> PackActivationTensor({dC, dH, dW, len},0);
    for (unsigned long gama = 0; gama < dC; gama++){
        for (unsigned long alpha = 0; alpha < dH; alpha++){
            for (unsigned long beta = 0; beta < dW; beta++){
                //traverse 
                for (unsigned long ic = 0; ic < CW; ic++){
                    if ((ic + gama * CW) >= in_channels){
                        for (unsigned long jh = 0; jh < HW; jh++){
                            for (unsigned long kw = 0; kw < WW; kw++){
                                Tsub({ic,jh,kw}) = 0;
                            }
                        }
                        //对于超出的channel部分应该设置为0
                    }
                    else{
                        for (unsigned long jh = 0; jh < HW; jh++){
                            if ((jh + alpha * (HW - kernel_size + 1)) >= in_feature_size){
                                for (unsigned long kw = 0; kw < WW; kw++){
                                    Tsub({ic,jh,kw}) = 0;
                                }
                                //超出的HW部分应该为0
                            }
                            else{
                                for (unsigned long kw = 0; kw <WW; kw++){
                                    if ((kw + beta * (WW - kernel_size + 1)) >= in_feature_size){
                                        Tsub({ic,jh,kw}) = 0;
                                    }
                                    else{
                                        int64_t element = padded_x({gama * CW + ic, alpha * (HW - kernel_size + 1) + jh, beta * (WW - kernel_size + 1) + kw});
                                        Tsub({ic,jh,kw}) = (element >= 0) ? unsigned(element) : unsigned(element + plain);
                                    }
                                }
                            }
                        }
                    }
                }
                Tensor<uint64_t> Tsubflatten = Tsub;
                Tsubflatten.flatten();
                vector<uint64_t> Tsubv = Tsubflatten.data(); 
                for (size_t i = 0; i < len; i++){
                    PackActivationTensor({gama, alpha, beta, i}) = Tsubv[i];
                }
            }
        }
    }
    return PackActivationTensor;
}

Tensor<UnifiedCiphertext> Conv2DCheetah::TensorTOHE(Tensor<uint64_t> PackActivationTensor) {
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
    Tensor<UnifiedCiphertext> finalpack(shapeTab, HE->GenerateZeroCiphertext());
    if (!HE->server){
        //客服端
        Tensor<UnifiedCiphertext> enc(shapeTab, HE->GenerateZeroCiphertext());
        for (size_t i = 0; i < numPoly; i++){
            this->HE->encryptor->encrypt(T(i), enc(i));
        }
        // enc.flatten();
        HE->SendEncVec(enc);
    }else{
        //服务器端
        Tensor<UnifiedCiphertext> encflatten({numPoly}, this->HE->GenerateZeroCiphertext());
        HE->ReceiveEncVec(encflatten);
        Tensor<UnifiedCiphertext> enc(shapeTab, HE->GenerateZeroCiphertext());
        for (size_t i = 0; i < numPoly; i++){
            this->HE->evaluator->add_plain(encflatten(i), T(i), enc(i));
        }
        finalpack = enc;
    }
    return finalpack;
}

// 计算卷积核的 Pack 版本
Tensor<UnifiedPlaintext> Conv2DCheetah::PackWeight() {
    auto t_pack0 = std::chrono::steady_clock::now();
    cout << "pack weight begin" << endl;

    std::vector<size_t> shapeTab = {dM, dC};
    Tensor<UnifiedPlaintext> Ktg(shapeTab,HOST);
    size_t len = OW + 1;

    if (!HE->server){
        pack_weight_us_last = 0;
        transfer_us_last = 0;
        packed_weight_ready = false;
        device_weight_ready = false;
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

    auto t_pack1 = std::chrono::steady_clock::now();
    pack_weight_us_last = UsBetween(t_pack0, t_pack1);
    packed_weight_ready = true;

    cout << "transfer weight to device" << endl;

    if (HE->Backend() == DEVICE){
        for (size_t i = 0; i < Ktg.size(); i++){
            Ktg(i).to_device(*HE->context);
        }
        device_weight_ready = true;
    } else {
        device_weight_ready = false;
    }

    auto t_pack2 = std::chrono::steady_clock::now();
    transfer_us_last = UsBetween(t_pack1, t_pack2);

    cout << "pack weight done" << endl;
    return Ktg;
}


Tensor<UnifiedCiphertext> Conv2DCheetah::sumCP(Tensor<UnifiedCiphertext> cipherTensor, Tensor<UnifiedPlaintext> plainTensor){
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
   

// 计算同态卷积
// #define MULTI_STRAEM
Tensor<UnifiedCiphertext> Conv2DCheetah::HECompute(const Tensor<UnifiedPlaintext> &weight_pt, Tensor<UnifiedCiphertext> &ac_ct)
{
    const auto target = HE->server ? HE->Backend() : HOST;
    std::vector<size_t> shapeTab = {dM, dH, dW};
    Tensor<UnifiedCiphertext> out_ct(shapeTab,HE->GenerateZeroCiphertext(target));
    if (!HE->server){
        return out_ct;
    }

    #ifndef MULTI_STRAEM
    UnifiedCiphertext interm(target);
    for (size_t theta = 0; theta < dM; theta++) {
        for (size_t alpha = 0; alpha < dH; alpha++) {
            for (size_t beta = 0; beta < dW; beta++) {
                HE->evaluator->multiply_plain(ac_ct({0, alpha, beta}), weight_pt({theta, 0}), out_ct({theta, alpha, beta}));
                for (size_t gama = 1; gama < dC; gama++) {
                    HE->evaluator->multiply_plain(ac_ct({gama, alpha, beta}), weight_pt({theta, gama}), interm);
                    HE->evaluator->add_inplace(out_ct({theta, alpha, beta}), interm);
                }
            }
        }
    }
    #else
    // 定义线程工作函数
    auto worker = [&](size_t theta_start, size_t theta_end) {
        UnifiedCiphertext interm(target);
        for (size_t theta = theta_start; theta < theta_end; theta++) {
            for (size_t alpha = 0; alpha < dH; alpha++) {
                for (size_t beta = 0; beta < dW; beta++) {
                    HE->evaluator->multiply_plain(ac_ct({0, alpha, beta}), 
                                                weight_pt({theta, 0}), 
                                                out_ct({theta, alpha, beta}));
                    for (size_t gama = 1; gama < dC; gama++) {
                        HE->evaluator->multiply_plain(ac_ct({gama, alpha, beta}), 
                                                    weight_pt({theta, gama}), 
                                                    interm);
                        HE->evaluator->add_inplace(out_ct({theta, alpha, beta}), interm);
                    }
                }
            }
        }
    };

    const size_t num_threads = 4; // [可修改]
    const size_t theta_per_thread = (dM + num_threads - 1) / num_threads;

    std::vector<std::thread> threads;
    for (size_t t = 0; t < num_threads; t++) {
        size_t start = t * theta_per_thread;
        size_t end = std::min(start + theta_per_thread, dM);
        if (start < dM) {
            threads.emplace_back(worker, start, end);
        }
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }
    #endif
    return out_ct;
}

Tensor<uint64_t> Conv2DCheetah::DepackResult(Tensor<uint64_t> &out){
        if (hot56_fastpath && hot56_tables_ready) {
        return DepackResultHot56(out);
    }
    Tensor<uint64_t> finalResult ({out_channels, HOut, WOut});
    int checkl = 0;

    for (size_t cprime = 0; cprime < out_channels; cprime++){
        for (size_t iprime = 0; iprime < HOut; iprime++){
            for (size_t jprime = 0; jprime < WOut; jprime++){
                size_t c = cprime % MW;
                size_t i = (iprime * stride) % (HW - kernel_size + 1);
                size_t j = (jprime * stride) % (WW - kernel_size + 1);
                size_t theta = cprime / MW;
                size_t alpha = (iprime * stride) / (HW - kernel_size + 1);
                size_t beta = (jprime * stride) / (WW - kernel_size + 1);
                size_t des = OW - c * CW * HW * WW + i  * WW + j;
                finalResult({cprime, iprime, jprime}) = out({theta, alpha, beta, des});
            }
        }
    }
    DAZGOrbitV9AddFoldedBiasConv3D(finalResult, this->bias, HE);
    return finalResult;

}

Tensor<uint64_t> Conv2DCheetah::operator()(Tensor<uint64_t> &x){
    auto pack = this->PackActivation(x);
    auto Cipher = Operator::SSToHE_coeff(pack, HE);
    auto ConvResult = this->HECompute(weight_pt, Cipher);
    auto share = Operator::HEToSS_coeff(ConvResult, HE);
    auto finalR = this->DepackResult(share);
    return finalR;
}



 // namespace LinearLayer
