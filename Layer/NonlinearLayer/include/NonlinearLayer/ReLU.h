// DAZG-Orbit Project Source File
// Component: Layer/NonlinearLayer/include/NonlinearLayer/ReLU.h
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <Datatype/Tensor.h>
#include "../../../Layer/Module.h"
#include <OTProtocol/aux-protocols.h>
#include <OTProtocol/millionaire.h>
#include <atomic>
#include <iostream>

#pragma once
using namespace Datatype;
using namespace Utils;

extern int32_t bitlength;
extern int32_t kScale;

#define RING 0
#define OFF_PLACE

namespace NonlinearLayer{

template <typename T, typename IO> class ReLUProtocol {
public:
  virtual ~ReLUProtocol() = default;

  virtual void relu(T *result, T *share, int num_relu,
                    uint8_t *msb = nullptr, bool skip_ot = false) = 0;

  virtual bool use_bulk_dispatch() const {
    return false;
  }
};


template <typename T, typename IO> 
class ReLURingProtocol : public ReLUProtocol<T, IO> {
public:
  OTPrimitive::OTPack<IO> *otpack;
  OTProtocol::TripleGenerator<IO> *triple_gen = nullptr;
  OTProtocol::MillionaireProtocol<IO> *millionaire = nullptr;
  OTProtocol::AuxProtocols *aux = nullptr;
  int party;
  int algeb_str;
  int l, b;
  int num_cmps;

  // Constructor, l is the bitlength of the input, b is the bitlength of each divided input, e.g., 4-bit base
  ReLURingProtocol(int party,int l, int b,
                   OTPack<IO> *otpack, OT_TYPE ot_type = Datatype::IKNP) {
    this->party = party;
    this->l = l;
    this->b = b;
    this->otpack = otpack;
    this->millionaire = new MillionaireProtocol<IO>(party, otpack->io, otpack,l,b,ot_type);
    this->triple_gen = this->millionaire->triple_gen;
    this->aux = new AuxProtocols(party, otpack->io, otpack);
  }

  // Destructor
  virtual ~ReLURingProtocol() { delete millionaire; }

  void relu(T *result, T *share, int num_relu,
                uint8_t *msb, bool skip_ot) {
        static std::atomic<bool> once{false};
        /*if (!once.exchange(true)) {
            std::cerr << "[DAZG-Orbit main ReLUProtocol hit] num_relu="
                    << num_relu << std::endl;
        }*/
        uint8_t *msb_tmp = new uint8_t[num_relu];
        if(msb!=nullptr){
            memcpy(msb_tmp,msb,num_relu*sizeof(uint8_t));
        }
        else{
            this->aux->template MSB<T>(share, msb_tmp, num_relu, this->l);
        }
        // std::cout << "MSB done" << std::endl;
        for (int i = 0; i < num_relu; i++) {
            if (this->party == ALICE) {
                msb_tmp[i] = msb_tmp[i] ^ 1;
            }
        }
        this->aux->template multiplexer<T>(msb_tmp, share, result, num_relu, this->l, this->l);
        delete[] msb_tmp;
        return;
    }
};

template <typename T, typename IO=Utils::NetIO>
class ReLU : public Module{
    public:
      int bitwidth;
      int num_threads;
      ReLU(ReLUProtocol<T, IO> **reluprotocol,int bitwidth=32, int num_threads=4){
        this->bitwidth = bitwidth;
        this->num_threads = num_threads;
        this->reluProtocol = reluprotocol;
      }

      void operator()(Tensor<T> &x){
        int dim = x.size();
        T* x_flatten = x.data().data();

        if (reluProtocol != nullptr &&
            reluProtocol[0] != nullptr &&
            reluProtocol[0]->use_bulk_dispatch()) {
            reluProtocol[0]->relu(x_flatten, x_flatten, dim);
            return;
        }

        std::thread relu_threads[num_threads];
        int chunk_size = dim / num_threads;
        for (int i = 0; i < num_threads; ++i) {
            int offset = i * chunk_size;
            int lnum_ops;
            if (i == (num_threads - 1)) {
                lnum_ops = dim - offset;
            } else {
                lnum_ops = chunk_size;
            }
            relu_threads[i] =
                std::thread(relu_thread, reluProtocol[i], x_flatten + offset, x_flatten + offset, lnum_ops);
        }
        for (int i = 0; i < num_threads; ++i) {
            relu_threads[i].join();
        }
      }

      
    private:
      ReLUProtocol<T, IO>** reluProtocol=nullptr;
      void static relu_thread(ReLUProtocol<T, IO>* reluProtocol, T* result, T* input, int lnum_ops){
        reluProtocol->relu(result, input, lnum_ops);
      }
};

}