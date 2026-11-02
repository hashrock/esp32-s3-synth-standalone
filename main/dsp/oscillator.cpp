#include "dsp/oscillator.h"

#include <cmath>

namespace dsp {

float g_sine[kSineSize + 1];

void init_sine_table() {
    for (int i = 0; i <= kSineSize; ++i) {
        g_sine[i] = static_cast<float>(std::sin(2.0 * M_PI * i / kSineSize));
    }
}

}  // namespace dsp
