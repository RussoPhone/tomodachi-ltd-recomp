// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <span>

#include <sirit/sirit.h>

#include "stream.h"

int main() {
    std::uint32_t bound = 0;
    Sirit::Stream stream{&bound};

    stream.Reserve(8);
    assert(stream.LocalAddress() == 0);

    stream << spv::Op::OpNop << Sirit::EndOp{};
    assert(stream.LocalAddress() == 1);

    stream.Reserve(4);
    assert(stream.LocalAddress() == 1);

    std::cout << "Sirit stream local address tests passed\n";
}
