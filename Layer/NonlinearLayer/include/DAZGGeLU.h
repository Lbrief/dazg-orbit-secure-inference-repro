// DAZG-Orbit Project Source File
// Component: Layer/NonlinearLayer/include/DAZGGeLU.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#pragma once

#include <Datatype/Tensor.h>
#include <HE/tfhe/DAZGRLut.h>
#include <Utils/dazg_orbit_dazg_variant.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace NonlinearLayer {

template <typename T>
class DAZGGeLU {
public:
    explicit DAZGGeLU(int bitwidth = 32, int scale = 16)
        : bitwidth_(bitwidth), scale_(scale) {}

    void operator()(Datatype::Tensor<T>& x) const {
        const int dim = x.size();
        T* x_flatten = x.data().data();

        std::vector<std::int64_t> input_fp(dim);
        std::vector<std::int64_t> output_fp;

        for (int i = 0; i < dim; ++i) {
            input_fp[i] = static_cast<std::int64_t>(
                static_cast<std::uint64_t>(x_flatten[i])
            );
        }

        dazg_orbit::dazg::EmitVariantLineOnce("NonlinearLayer::DAZGGeLU");
        dazg_orbit::tfhe::EvalDAZGGeLUFp(input_fp, bitwidth_, scale_, output_fp);

        if (static_cast<int>(output_fp.size()) != dim) {
            throw std::runtime_error("DAZGGeLU output size mismatch");
        }

        for (int i = 0; i < dim; ++i) {
            x_flatten[i] = static_cast<T>(
                static_cast<std::uint64_t>(output_fp[i])
            );
        }

        static bool once = false;
        if (!once) {
            once = true;
            std::cerr << "[DAZGGeLU layer hit] n="
                      << dim
                      << " bitwidth=" << bitwidth_
                      << " scale=" << scale_
                      << " variant=" << dazg_orbit::dazg::CurrentVariantConfig().name
                      << " route=dazg_orbit::tfhe::EvalDAZGGeLUFp"
                      << std::endl;
        }
    }

private:
    int bitwidth_;
    int scale_;
};

}  // namespace NonlinearLayer
