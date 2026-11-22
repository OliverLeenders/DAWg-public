#include "dawg/AllocationGuard.h"
#include "dawg/Node.h"
#include "dawg/Realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace dawg;

TEST_CASE ("allocation guard counts allocations inside its scope", "[realtime]")
{
    if constexpr (! allocationGuardEnabled)
        SKIP ("Allocation guard is compiled out in this configuration");

    const auto before = allocationViolationCount();
    int value = 0;

    {
        const ScopedNoAllocation guard;
        const auto allocated = std::make_unique<int> (42); // one allocation and one deallocation
        value = *allocated;
    }

    // Catch2 itself may allocate, so only assert outside the guarded scope.
    CHECK (value == 42);
    CHECK (allocationViolationCount() == before + 2);

    const auto outside = std::make_unique<int> (1);
    CHECK (*outside == 1);
    CHECK (allocationViolationCount() == before + 2);
}

TEST_CASE ("snapshot exchange hands over the newest snapshot only", "[realtime]")
{
    SnapshotExchange<int> exchange;
    CHECK (exchange.acquire() == nullptr);

    exchange.publish (std::make_shared<const int> (1));
    exchange.publish (std::make_shared<const int> (2));
    REQUIRE (exchange.acquire() != nullptr);
    CHECK (*exchange.acquire() == 2);

    exchange.publish (std::make_shared<const int> (3));
    CHECK (*exchange.acquire() == 3);
    exchange.collectGarbage();
    CHECK (*exchange.acquire() == 3);
}

TEST_CASE ("spsc queue preserves order and reports full and empty", "[realtime]")
{
    SpscQueue<int, 4> queue;
    CHECK_FALSE (queue.pop().has_value());

    for (int i = 0; i < 4; ++i)
        REQUIRE (queue.push (i));

    CHECK_FALSE (queue.push (99));

    for (int i = 0; i < 4; ++i)
        CHECK (queue.pop() == i);

    CHECK_FALSE (queue.pop().has_value());
}

TEST_CASE ("event buffers stay sorted and drop on overflow", "[realtime]")
{
    EventBuffer buffer;
    REQUIRE (buffer.add ({ 10, NoteEventType::noteOn, 1, nullptr }));
    REQUIRE (buffer.add ({ 5, NoteEventType::noteOn, 2, nullptr }));
    REQUIRE (buffer.add ({ 10, NoteEventType::noteOff, 3, nullptr }));

    const auto events = buffer.events();
    CHECK (events[0].noteId == 2);
    CHECK (events[1].noteId == 1); // equal offsets keep insertion order
    CHECK (events[2].noteId == 3);

    for (size_t i = buffer.events().size(); i < EventBuffer::capacity; ++i)
        REQUIRE (buffer.add ({ 0, NoteEventType::noteOn, 0, nullptr }));

    CHECK_FALSE (buffer.add ({ 0, NoteEventType::noteOn, 0, nullptr }));
    CHECK (buffer.droppedCount() == 1);
}
