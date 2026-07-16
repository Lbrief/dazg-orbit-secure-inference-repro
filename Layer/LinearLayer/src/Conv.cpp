// DAZG-Orbit Project Source File
// Component: Layer/LinearLayer/src/Conv.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <LinearLayer/Conv.h>
#include <cassert>
#include "Utils/dazg_orbit_determinism.h"

using namespace seal;
using namespace HE;
using namespace HE::unified;

namespace LinearLayer {
// Extract shared parameters. Let dim(w) = {Co, Ci, k, k}
Conv2D::Conv2D(uint64_t in_feature_size, uint64_t stride, uint64_t padding, const Tensor<uint64_t>& weight, const Tensor<uint64_t>& bias, HEEvaluator* HE)
    : in_feature_size(in_feature_size), 
      stride(stride),
      padding(padding),
      weight(weight), 
      bias(bias), 
      HE(HE)
{
    std::vector<size_t> weight_shape = weight.shape();

    assert(weight_shape[0] == bias.shape()[0] && "Output channel does not match.");
    assert(weight_shape[2] == weight_shape[3] && "Input feature map is not a square.");
    assert(in_feature_size - weight_shape[2] + 2 * padding >= 0 && "Input feature map is too small.");

    out_channels = weight_shape[0];
    in_channels = weight_shape[1];
    kernel_size = weight_shape[2];
    out_feature_size = (in_feature_size + 2 * padding - kernel_size) / stride + 1;
};

Conv2D::Conv2D(uint64_t in_feature_size, uint64_t in_channels, uint64_t out_channels, uint64_t kernel_size, uint64_t stride, HEEvaluator* HE)
    : in_feature_size(in_feature_size),
      in_channels(in_channels),
      out_channels(out_channels),
      kernel_size(kernel_size),
      stride(stride),
      HE(HE)
      {
        cout << "---------in Conv2D constructor-----------" << endl;
        cout << "in_feature_size, in_channels, out_channels, kernel_size, stride: " << in_feature_size << ", " << in_channels << ", " << out_channels << ", " << kernel_size << ", " << stride << endl;
        this->padding = (kernel_size - 1) / 2;
        if(HE->server) {
            // cout << "server Conv2D constructor called" << endl;
            this->weight = Tensor<uint64_t>({out_channels, in_channels, kernel_size, kernel_size});
            this->bias = Tensor<uint64_t>({out_channels});
            dazg_orbit_det::FillTensorDeterministic(
                this->weight,
                "Conv2D.weight",
                16,
                dazg_orbit_det::HashU64Seq({in_feature_size, in_channels, out_channels,
                                          kernel_size, stride, 1ULL}));
            dazg_orbit_det::FillTensorDeterministic(
                this->bias,
                "Conv2D.bias",
                16,
                dazg_orbit_det::HashU64Seq({in_feature_size, in_channels, out_channels,
                                          kernel_size, stride, 2ULL}));
            // cout << "server Conv2D constructor done" << endl;
        }
      }

Conv2D::Conv2D(uint64_t in_feature_size, uint64_t in_channels, uint64_t out_channels,
               uint64_t kernel_size, uint64_t stride, uint64_t padding,
               HEEvaluator* HE, bool metadata_only)
    : in_channels(in_channels),
      out_channels(out_channels),
      in_feature_size(in_feature_size),
      kernel_size(kernel_size),
      stride(stride),
      padding(padding),
      HE(HE)
{
    (void)metadata_only;
    out_feature_size = (in_feature_size + 2 * padding - kernel_size) / stride + 1;
}

} // namespace LinearLayer
