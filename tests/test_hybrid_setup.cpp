// SPDX-License-Identifier: MIT
#include "test.hpp"
#include "compat/hybrid_setup.hpp"
#ifdef ROBOTWEAX_SRT_EXPERIMENTAL_HYBRID_CONTROL
#include "compat/socket_registry.hpp"
#include "compat/group_registry.hpp"
using namespace robotweax::srt;
using namespace robotweax::srt::compat;
namespace h = robotweax::srt::compat::hybrid;
namespace {
struct ProvisionFixture {
    std::vector<SRTSOCKET> groups, sockets;
    ~ProvisionFixture()
    {
        for (auto id : groups)
            GroupRegistry::instance().close(id);
        for (auto id : sockets)
            SocketRegistry::instance().close(id);
    }
    SRTSOCKET group()
    {
        auto id =
            GroupRegistry::instance().create(ROBOTWEAX_SRT_GTYPE_MULTICAST);
        groups.push_back(id);
        auto g = GroupRegistry::instance().find(id);
        g->member_public_options.multicast_independent = true;
        return id;
    }
    SRTSOCKET socket()
    {
        auto id = SocketRegistry::instance().create();
        sockets.push_back(id);
        auto r = SocketRegistry::instance().find(id);
        r->public_options.multicast_independent =
            r->public_options.multicast_coordinated = true;
        return id;
    }
    static h::StartupOptions options()
    {
        h::StartupOptions c;
        c.native.owner = std::make_shared<std::byte>();
        c.native.resolve = [](void*, const h::CredentialRequest&,
                               h::Authorization&,
                               std::span<std::byte, 32>) noexcept {
            return false;
        };
        c.native.revalidate = [](void*, const h::CredentialRequest&,
                                  const h::Authorization&,
                                  h::Authorization&) noexcept {
            return false;
        };
        c.native.enroll = [](void*, const h::Context&, const h::Authorization&,
                              std::uint64_t) noexcept {
            return false;
        };
        c.native.policy.supported |= h::startup_fallback | h::policy_query;
        c.native.policy.required |= h::startup_fallback | h::policy_query;
        c.native.timeout_us = 300'000;
        c.admission_timeout_us = 1'000'000;
        c.poll_interval_us = 1000;
        c.membership = {IpEndpoint::any(), 2, 200'000, 10'000, 20'000};
        return c;
    }
};
}
TEST(
    hybrid_setup_provisioning_requires_explicit_policy_callbacks_and_bounded_configuration)
{
    ProvisionFixture f;
    for (unsigned i = 0; i < 12; ++i) {
        auto c = f.options();
        switch (i) {
        case 0:
            c.admission_timeout_us = 0;
            break;
        case 1:
            c.admission_timeout_us = h::maximum_age_us + 1;
            break;
        case 2:
            c.poll_interval_us = 0;
            break;
        case 3:
            c.poll_interval_us = 50'001;
            break;
        case 4:
            c.native.policy.required &= ~h::startup_fallback;
            break;
        case 5:
            c.native.owner.reset();
            break;
        case 6:
            c.native.timeout_us = c.admission_timeout_us + 1;
            break;
        case 7:
            c.membership.maximum_attempts = 0;
            break;
        case 8:
            c.receiver.maximum_rounds = 9;
            break;
        case 9:
            c.source.maximum_attempts = 0;
            break;
        case 10:
            c.join.owner = c.native.owner;
            break;
        case 11:
            c.native.enroll = nullptr;
            break;
        }
        auto id = f.group();
        REQUIRE(!h::provision_startup(id, std::move(c)));
        REQUIRE(!GroupRegistry::instance().find(id)->hybrid_startup_options);
    }
}
TEST(hybrid_setup_provisioning_is_immutable_and_inherited_only_before_opening)
{
    ProvisionFixture f;
    auto group = f.group();
    auto c = f.options();
    REQUIRE(h::provision_startup(group, c));
    REQUIRE(!h::provision_startup(group, c));
    GroupRegistry::ConnectDescription d;
    REQUIRE(GroupRegistry::instance().describe_connect(group, d));
    REQUIRE(d.hybrid_startup_options);
    REQUIRE_EQ(d.hybrid_startup_options->native.owner, c.native.owner);
    auto opened = f.group();
    GroupRegistry::instance().find(opened)->opened = true;
    REQUIRE(!h::provision_startup(opened, c));
    auto socket = f.socket();
    REQUIRE(h::provision_startup(socket, c));
    REQUIRE(!h::provision_startup(socket, c));
    auto listening = f.socket();
    SocketRegistry::instance().find(listening)->state = SRTS_LISTENING;
    REQUIRE(!h::provision_startup(listening, c));
    auto legacy = f.socket();
    SocketRegistry::instance()
        .find(legacy)
        ->public_options.multicast_independent = false;
    REQUIRE(!h::provision_startup(legacy, c));
    REQUIRE(!h::provision_startup(SRT_INVALID_SOCK, c));
}
#endif
