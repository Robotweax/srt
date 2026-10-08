#pragma once

#include "robotweax/srt/reliability.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include <variant>

namespace robotweax::srt::detail {

struct PendingPeerDrop {
    SequenceRange sequences {};
    std::uint64_t deadline_microseconds = 0;
};

// A construction-sized AVL tree indexes exact wire identities. Each subtree
// also records its earliest deadline, avoiding a second index or queue scans.
// Admission, duplicate lookup and retirement are worst-case logarithmic;
// deadline lookup is constant-time. No operation grows the allocated storage.
template <class IndexType> class BasicPeerDropQueue {
public:
    using Index = IndexType;
    static constexpr Index none = std::numeric_limits<Index>::max();

    explicit BasicPeerDropQueue(std::size_t capacity)
        : entries_(capacity)
    {
        if (capacity == 0 || capacity > none)
            throw std::invalid_argument("invalid peer-drop capacity");
        for (Index i = 0; i < capacity; ++i)
            entries_[i].left = i + 1 < capacity ? i + 1 : none;
        free_entry_ = 0;
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return size_ == 0;
    }
    [[nodiscard]] bool full() const noexcept
    {
        return size_ == entries_.size();
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return size_;
    }
    [[nodiscard]] const PendingPeerDrop& at(Index i) const noexcept
    {
        return entries_[i].pending;
    }
    [[nodiscard]] Index first_due() const noexcept
    {
        return empty() ? none : entries_[root_].earliest;
    }
    [[nodiscard]] std::optional<std::uint64_t> next_deadline() const noexcept
    {
        return empty() ? std::nullopt
                       : std::optional {at(first_due()).deadline_microseconds};
    }
    // One physical slot per inspection, including free slots. A caller can
    // reclaim already-passed future entries with a fixed amount of work.
    [[nodiscard]] Index next_to_inspect() noexcept
    {
        const auto current = cursor_;
        cursor_ = (cursor_ + 1U) % static_cast<Index>(entries_.size());
        return entries_[current].height == 0 ? none : current;
    }
    [[nodiscard]] bool contains(
        SequenceRange range, std::size_t* steps = nullptr) const noexcept
    {
        const auto key = identity(range);
        auto node = root_;
        while (node != none) {
            if (steps != nullptr)
                ++*steps;
            const auto indexed = identity(at(node).sequences);
            if (indexed == key)
                return true;
            node = key < indexed ? entries_[node].left : entries_[node].right;
        }
        return false;
    }
    [[nodiscard]] bool insert(SequenceRange range, std::uint64_t deadline,
        std::size_t* steps = nullptr) noexcept
    {
        if (contains(range, steps))
            return true; // Keep the original grace.
        if (full())
            return false;
        const Index added = free_entry_;
        free_entry_ = entries_[added].left;
        entries_[added] = {{range, deadline}, none, none, added, 1};
        root_ = insert_node(root_, added, steps);
        ++size_;
        return true;
    }
    void erase(Index index) noexcept
    {
        root_ = erase_node(root_, identity(at(index).sequences));
        entries_[index].height = 0;
        entries_[index].left = free_entry_;
        free_entry_ = index;
        --size_;
    }
    [[nodiscard]] std::size_t storage_bytes() const noexcept
    {
        return entries_.size() * sizeof(Entry);
    }

private:
    struct Entry {
        PendingPeerDrop pending;
        Index left = none;
        Index right = none;
        Index earliest = none;
        std::uint8_t height = 0;
    };
    static std::uint64_t identity(SequenceRange range) noexcept
    {
        return (static_cast<std::uint64_t>(range.first.value()) << 32U)
            | range.last.value();
    }
    [[nodiscard]] int height(Index i) const noexcept
    {
        return i == none ? 0 : entries_[i].height;
    }
    [[nodiscard]] bool earlier(Index a, Index b) const noexcept
    {
        const auto& left = at(a);
        const auto& right = at(b);
        return left.deadline_microseconds < right.deadline_microseconds
            || (left.deadline_microseconds == right.deadline_microseconds
                && identity(left.sequences) < identity(right.sequences));
    }
    void refresh(Index node) noexcept
    {
        auto& entry = entries_[node];
        entry.height = static_cast<std::uint8_t>(
            1 + std::max(height(entry.left), height(entry.right)));
        entry.earliest = node;
        for (const auto child : {entry.left, entry.right}) {
            if (child != none
                && earlier(entries_[child].earliest, entry.earliest))
                entry.earliest = entries_[child].earliest;
        }
    }
    [[nodiscard]] Index rotate_left(Index node) noexcept
    {
        const auto next = entries_[node].right;
        entries_[node].right = entries_[next].left;
        entries_[next].left = node;
        refresh(node);
        refresh(next);
        return next;
    }
    [[nodiscard]] Index rotate_right(Index node) noexcept
    {
        const auto next = entries_[node].left;
        entries_[node].left = entries_[next].right;
        entries_[next].right = node;
        refresh(node);
        refresh(next);
        return next;
    }
    [[nodiscard]] Index balance(Index node) noexcept
    {
        refresh(node);
        const auto& entry = entries_[node];
        const auto skew = height(entry.left) - height(entry.right);
        if (skew > 1) {
            const auto& left = entries_[entry.left];
            if (height(left.left) < height(left.right))
                entries_[node].left = rotate_left(entry.left);
            return rotate_right(node);
        }
        if (skew < -1) {
            const auto& right = entries_[entry.right];
            if (height(right.right) < height(right.left))
                entries_[node].right = rotate_right(entry.right);
            return rotate_left(node);
        }
        return node;
    }
    [[nodiscard]] Index insert_node(
        Index node, Index added, std::size_t* steps) noexcept
    {
        if (node == none)
            return added;
        if (steps != nullptr)
            ++*steps;
        if (identity(at(added).sequences) < identity(at(node).sequences))
            entries_[node].left =
                insert_node(entries_[node].left, added, steps);
        else
            entries_[node].right =
                insert_node(entries_[node].right, added, steps);
        return balance(node);
    }
    [[nodiscard]] Index detach_first(Index node, Index& detached) noexcept
    {
        if (entries_[node].left == none) {
            detached = node;
            return entries_[node].right;
        }
        entries_[node].left = detach_first(entries_[node].left, detached);
        return balance(node);
    }
    [[nodiscard]] Index erase_node(Index node, std::uint64_t key) noexcept
    {
        auto& entry = entries_[node];
        const auto indexed = identity(entry.pending.sequences);
        if (key < indexed)
            entry.left = erase_node(entry.left, key);
        else if (key > indexed)
            entry.right = erase_node(entry.right, key);
        else {
            if (entry.left == none)
                return entry.right;
            if (entry.right == none)
                return entry.left;
            Index replacement = none;
            const auto right = detach_first(entry.right, replacement);
            entries_[replacement].left = entry.left;
            entries_[replacement].right = right;
            return balance(replacement);
        }
        return balance(node);
    }
    std::vector<Entry> entries_;
    std::size_t size_ = 0;
    Index root_ = none;
    Index free_entry_ = none;
    Index cursor_ = 0;
};

// Compact indices keep the ordinary receive windows at the previous 24 bytes
// per deferred range. Larger windows use 32-bit indices without reducing capacity.
class PeerDropQueue {
    using Small = BasicPeerDropQueue<std::uint16_t>;
    using Large = BasicPeerDropQueue<std::uint32_t>;
    using Storage = std::variant<Small, Large>;
    static Storage make_storage(std::size_t capacity)
    {
        if (capacity <= Small::none)
            return Storage {std::in_place_type<Small>, capacity};
        return Storage {std::in_place_type<Large>, capacity};
    }

public:
    using Index = std::uint32_t;
    static constexpr Index none = Large::none;
    explicit PeerDropQueue(std::size_t capacity)
        : storage_(make_storage(capacity))
    {
    }
    [[nodiscard]] bool empty() const noexcept
    {
        return std::visit(
            [](const auto& q) {
                return q.empty();
            },
            storage_);
    }
    [[nodiscard]] bool full() const noexcept
    {
        return std::visit(
            [](const auto& q) {
                return q.full();
            },
            storage_);
    }
    [[nodiscard]] std::size_t size() const noexcept
    {
        return std::visit(
            [](const auto& q) {
                return q.size();
            },
            storage_);
    }
    [[nodiscard]] const PendingPeerDrop& at(Index i) const noexcept
    {
        return std::visit(
            [i](const auto& q) -> const PendingPeerDrop& {
                return q.at(
                    static_cast<typename std::decay_t<decltype(q)>::Index>(i));
            },
            storage_);
    }
    [[nodiscard]] Index first_due() const noexcept
    {
        return std::visit(
            [](const auto& q) -> Index {
                const auto i = q.first_due();
                return i == q.none ? none : i;
            },
            storage_);
    }
    [[nodiscard]] Index next_to_inspect() noexcept
    {
        return std::visit(
            [](auto& q) -> Index {
                const auto i = q.next_to_inspect();
                return i == q.none ? none : i;
            },
            storage_);
    }
    [[nodiscard]] std::optional<std::uint64_t> next_deadline() const noexcept
    {
        return std::visit(
            [](const auto& q) {
                return q.next_deadline();
            },
            storage_);
    }
    [[nodiscard]] bool contains(
        SequenceRange range, std::size_t* steps = nullptr) const noexcept
    {
        return std::visit(
            [&](const auto& q) {
                return q.contains(range, steps);
            },
            storage_);
    }
    [[nodiscard]] bool insert(SequenceRange range, std::uint64_t deadline,
        std::size_t* steps = nullptr) noexcept
    {
        return std::visit(
            [&](auto& q) {
                return q.insert(range, deadline, steps);
            },
            storage_);
    }
    void erase(Index i) noexcept
    {
        std::visit(
            [i](auto& q) {
                q.erase(
                    static_cast<typename std::decay_t<decltype(q)>::Index>(i));
            },
            storage_);
    }
    [[nodiscard]] std::size_t storage_bytes() const noexcept
    {
        return std::visit(
            [](const auto& q) {
                return q.storage_bytes();
            },
            storage_);
    }

private:
    Storage storage_;
};

} // namespace robotweax::srt::detail
