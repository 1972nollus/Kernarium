// SPDX-License-Identifier: GPL-2.0-or-later
#include "motion_protocol.hpp"
#include "require.hpp"

using uml::testing::require;

int main()
{
    // Independent wire fixture: +1, -2, +0.5 radians/second.
    std::array<uint8_t, 20> bytes{
        'A', 'N', 'S', '1', 4, 0, 0, 0,
        0, 0, 0x80, 0x3f, 0, 0, 0, 0xc0, 0, 0, 0, 0x3f};
    const auto sample = andarium::motion::decode(bytes);
    require(sample.has_value());
    require(sample->type == 4);
    require(sample->values == std::array<float, 3>{1, -2, 0.5f});
    require(!andarium::motion::decode(std::span(bytes).first(19)));
    bytes[0] = 0;
    require(!andarium::motion::decode(bytes));
    bytes[0] = 'A';
    bytes[4] = 99;
    require(!andarium::motion::decode(bytes));
    bytes[4] = 1;
    require(andarium::motion::decode(bytes).has_value());
    bytes[10] = 0x80;
    bytes[11] = 0x7f; // Infinity is never a physical measurement.
    require(!andarium::motion::decode(bytes));
    bytes[10] = 0xc0; // Neither is NaN.
    require(!andarium::motion::decode(bytes));
}
