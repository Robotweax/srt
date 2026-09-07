#include "compat/group_config.hpp"

#include <cstring>

namespace robotweax::srt::compat {

namespace {

template <std::size_t Size>
void secure_clear(std::array<GroupOptionValue, Size>& options) noexcept
{
    auto* bytes = reinterpret_cast<volatile unsigned char*>(options.data());
    for (std::size_t index = 0; index < sizeof(options); ++index) {
        bytes[index] = 0;
    }
}

} // namespace

GroupConfigSnapshot::~GroupConfigSnapshot() noexcept
{
    secure_clear(options);
    size = 0;
}

GroupSocketConfiguration::~GroupSocketConfiguration() noexcept
{
    secure_clear(options_);
    size_ = 0;
}

bool is_group_member_option(SRT_SOCKOPT option) noexcept
{
    switch (option) {
    case SRTO_BINDTODEVICE:
    case SRTO_CONNTIMEO:
    case SRTO_DRIFTTRACER:
    case SRTO_GROUPMINSTABLETIMEO:
    case SRTO_IPTOS:
    case SRTO_IPTTL:
    case SRTO_PASSPHRASE:
    case SRTO_PBKEYLEN:
    case SRTO_KMREFRESHRATE:
    case SRTO_KMPREANNOUNCE:
    case SRTO_ENFORCEDENCRYPTION:
#ifdef ENABLE_AEAD_API_PREVIEW
    case SRTO_CRYPTOMODE:
#endif
    case SRTO_LOSSMAXTTL:
    case SRTO_NAKREPORT:
    case SRTO_PEERIDLETIMEO:
    case SRTO_RCVBUF:
    case SRTO_SNDBUF:
    case SRTO_SNDDROPDELAY:
    case SRTO_UDP_RCVBUF:
    case SRTO_UDP_SNDBUF:
        return true;
    default:
        return false;
    }
}

bool GroupSocketConfiguration::add(
    SRT_SOCKOPT option, const void* value, int value_size) noexcept
{
    if (!is_group_member_option(option)
        || value == nullptr
        || value_size <= 0
        || static_cast<std::size_t>(value_size) > maximum_group_option_bytes) {
        return false;
    }

    std::lock_guard lock(mutex_);
    if (size_ >= options_.size()) {
        return false;
    }
    auto& destination = options_[size_];
    destination.option = option;
    destination.size = static_cast<std::uint16_t>(value_size);
    std::memcpy(destination.bytes.data(), value,
        static_cast<std::size_t>(value_size));
    ++size_;
    return true;
}

GroupConfigSnapshot GroupSocketConfiguration::snapshot() const noexcept
{
    std::lock_guard lock(mutex_);
    GroupConfigSnapshot result;
    result.size = size_;
    for (std::size_t index = 0; index < size_; ++index) {
        result.options[index] = options_[index];
    }
    return result;
}

} // namespace robotweax::srt::compat
