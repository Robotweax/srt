/* SPDX-License-Identifier: MIT */

#pragma once

#include "test.hpp"
#include "robotweax/srt/crypto.hpp"

#include <utility>

// Direct-session fixtures emulate the runtime KMREQ/KMRSP exchange after
// the initial handshake. Public socket tests must exercise the runtime itself.
inline void confirm_directional_test_keys(
    robotweax::srt::CryptoSession& first, robotweax::srt::CryptoSession& second)
{
    using namespace robotweax::srt;
    for (const auto pair :
        {std::pair {&first, &second}, std::pair {&second, &first}}) {
        REQUIRE(!pair.first->pending_key_material().empty());
        REQUIRE_EQ(pair.second->accept_key_material(
                       pair.first->pending_key_material(), false),
            Error::none);
        REQUIRE_EQ(pair.first->acknowledge_key_material(
                       pair.second->key_material_response(), false),
            Error::none);
        REQUIRE(pair.first->ready_to_send_data());
    }
}
