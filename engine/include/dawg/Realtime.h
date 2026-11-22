#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

namespace dawg
{

#ifdef _MSC_VER
 #pragma warning(push)
 #pragma warning(disable : 4324) // structure padded due to alignment specifier (intentional, avoids false sharing)
#endif

/**
    Wait-free single-producer / single-consumer queue with a fixed capacity.

    Never locks or allocates, so either end may be the audio thread.

    @tparam T         Element type. Should be cheap to move and nothrow-movable.
    @tparam Capacity  Number of slots; must be a power of two.
    @note  Exactly one thread may call push() and exactly one (other) thread may call pop().
*/
template <typename T, size_t Capacity>
class SpscQueue final
{
    static_assert ((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    /**
        Appends an element.

        @param value  The element to append.
        @return       False if the queue was full and the element was not added.
        @note  Producer thread only.
    */
    [[nodiscard]] bool push (T value) noexcept
    {
        // Relaxed: only the producer writes m_writeIndex. Acquire pairs with the consumer's release so the
        // slot we are about to overwrite has been fully read.
        const auto w = m_writeIndex.load (std::memory_order_relaxed);

        if (w - m_readIndex.load (std::memory_order_acquire) == Capacity)
            return false;

        m_slots[w & (Capacity - 1)] = std::move (value);
        m_writeIndex.store (w + 1, std::memory_order_release); // publishes the slot to the consumer
        return true;
    }

    /**
        Removes the oldest element.

        @return  The element, or std::nullopt if the queue was empty.
        @note  Consumer thread only.
    */
    [[nodiscard]] std::optional<T> pop() noexcept
    {
        // Mirror image of push(): relaxed for our own index, acquire to see the producer's slot write.
        const auto r = m_readIndex.load (std::memory_order_relaxed);

        if (r == m_writeIndex.load (std::memory_order_acquire))
            return std::nullopt;

        auto value = std::move (m_slots[r & (Capacity - 1)]);
        m_readIndex.store (r + 1, std::memory_order_release); // hands the slot back to the producer
        return value;
    }

private:
    std::array<T, Capacity> m_slots {};
    alignas (64) std::atomic<size_t> m_writeIndex { 0 };
    alignas (64) std::atomic<size_t> m_readIndex { 0 };
};

#ifdef _MSC_VER
 #pragma warning(pop)
#endif

/**
    Hands immutable snapshots from the message thread to the audio thread.

    The message thread owns every snapshot (shared_ptr). The audio thread only sees raw pointers: it takes
    the newest one from a single atomic slot in acquire() and hands the one it replaced back through a
    queue. The message thread releases retired snapshots in collectGarbage(), so nothing is ever freed on
    the audio thread.

    @tparam T  The snapshot type.
    @note  publish() and collectGarbage(): message thread. acquire(): audio thread.
           The audio thread must have stopped calling acquire() before this object is destroyed.
*/
template <typename T>
class SnapshotExchange final
{
public:
    /**
        Makes a new snapshot available to the audio thread.

        @param snapshot  The snapshot. Must not be null.
        @note  Message thread only.
    */
    void publish (std::shared_ptr<const T> snapshot)
    {
        collectGarbage();
        const T* raw = snapshot.get();
        m_owned.push_back (std::move (snapshot));

        // A snapshot still sitting in the slot was never seen by the audio thread, so it can be freed.
        if (const T* superseded = m_latest.exchange (raw))
            release (superseded);
    }

    /**
        Frees snapshots the audio thread no longer uses.

        @note  Message thread only. Call regularly, e.g. from a timer.
    */
    void collectGarbage()
    {
        while (const auto retired = m_retired.pop())
            release (*retired);
    }

    /**
        Returns the newest published snapshot.

        @return  The snapshot, or nullptr before the first publish(). Valid until the next acquire().
        @note  Audio thread only. Never blocks or allocates.
    */
    [[nodiscard]] const T* acquire() noexcept
    {
        if (const T* next = m_latest.exchange (nullptr))
        {
            // If the retired queue is full the old snapshot simply stays owned: a small leak, never a free here.
            if (m_active != nullptr)
                std::ignore = m_retired.push (m_active);

            m_active = next;
        }

        return m_active;
    }

private:
    void release (const T* raw)
    {
        std::erase_if (m_owned, [raw] (const auto& p) { return p.get() == raw; });
    }

    std::atomic<const T*> m_latest { nullptr };
    SpscQueue<const T*, 64> m_retired;
    std::vector<std::shared_ptr<const T>> m_owned;
    const T* m_active = nullptr;
};

} // namespace dawg
