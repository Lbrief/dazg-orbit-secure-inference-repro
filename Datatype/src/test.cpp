// DAZG-Orbit Project Source File
// Component: Datatype/src/test.cpp
// Purpose: DAZG-Orbit runtime, protocol, model, or experiment component.
// Maintenance: DAZG-Orbit integration, acceleration, and reproducibility layer.
// Provenance and licenses: see NOTICE_UPSTREAM.md and LICENSE_NOTICE.md.
// Validated scope: frozen N=10 and checkpoint-013 balanced N=100.
// Security boundary: reveal correctness backend; security_claim=0.
#include <Datatype/Tensor.h>
using namespace Datatype;


int main(){
    Tensor<double> tensor({2, 3}, {1.1, 2.2, 3.3, 4.4, 5.5, 6.6});
    tensor.print();
    return 0;
}