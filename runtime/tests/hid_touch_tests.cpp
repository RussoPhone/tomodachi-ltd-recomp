#include "services/hid_touch.h"

#include <cassert>
#include <cstddef>

int main() {
    using namespace nxrt::hle;

    static_assert(sizeof(TouchState) == 0x28);
    static_assert(sizeof(TouchScreenState) == 0x290);
    static_assert(sizeof(TouchScreenLifo) == 0x2C38);

    TouchScreenLifo lifo{};
    TouchInputSampler sampler;

    sampler.Write(lifo, 1, true, 640, 360);
    auto state = sampler.Current(lifo);
    assert(lifo.entries[lifo.buffer_tail].sampling_number == 2);
    assert(state.entry_count == 1);
    assert(state.states[0].attribute == 0x1); // start_touch
    assert(state.states[0].position_x == 640);
    assert(state.states[0].position_y == 360);

    sampler.Write(lifo, 2, true, 700, 410);
    state = sampler.Current(lifo);
    assert(state.entry_count == 1);
    assert(state.states[0].attribute == 0);
    assert(state.states[0].position_x == 700);
    assert(state.states[0].position_y == 410);

    sampler.Write(lifo, 3, false, 700, 410);
    state = sampler.Current(lifo);
    assert(state.entry_count == 1);
    assert(state.states[0].attribute == 0x2); // end_touch

    sampler.Write(lifo, 4, false, 700, 410);
    state = sampler.Current(lifo);
    assert(state.entry_count == 0);
}
