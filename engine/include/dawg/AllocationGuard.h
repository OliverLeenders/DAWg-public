#pragma once

#include <cstddef>

namespace dawg
{

/**
    True when the allocation guard is compiled in (CMake option DAWG_ALLOCATION_GUARD, on by default for
    Debug builds).
*/
#if DAWG_ALLOCATION_GUARD
inline constexpr bool allocationGuardEnabled = true;
#else
inline constexpr bool allocationGuardEnabled = false;
#endif

/**
    Marks the current thread as real-time for the lifetime of this object.

    While a guard is alive on a thread, every global operator new / delete called on that thread is counted
    as a violation (see allocationViolationCount()). Put one at the top of the audio callback.

    Violations are counted rather than asserted because asserting or logging would itself allocate. The
    message thread checks the counter and reports it.

    Guards nest. When the guard is compiled out, this class does nothing.

    @note  Construct and destroy on the same thread.
*/
class ScopedNoAllocation final
{
public:
    /**
        Starts guarding the current thread.
    */
    ScopedNoAllocation() noexcept;

    /**
        Stops guarding the current thread (unless an outer guard is still alive).
    */
    ~ScopedNoAllocation();

    ScopedNoAllocation (const ScopedNoAllocation&) = delete;
    ScopedNoAllocation& operator= (const ScopedNoAllocation&) = delete;
    ScopedNoAllocation (ScopedNoAllocation&&) = delete;
    ScopedNoAllocation& operator= (ScopedNoAllocation&&) = delete;
};

/**
    Exempts code we don't control (plugins) from the guard on the current thread for the lifetime of this
    object. Allocations made meanwhile inside a ScopedNoAllocation are counted separately (see
    exemptAllocationCount()) instead of as violations.

    Exemptions nest. When the guard is compiled out, this class does nothing.

    @note  Construct and destroy on the same thread.
*/
class ScopedAllowAllocation final
{
public:
    /**
        Starts the exemption.
    */
    ScopedAllowAllocation() noexcept;

    /**
        Ends the exemption (unless an outer one is still alive).
    */
    ~ScopedAllowAllocation();

    ScopedAllowAllocation (const ScopedAllowAllocation&) = delete;
    ScopedAllowAllocation& operator= (const ScopedAllowAllocation&) = delete;
    ScopedAllowAllocation (ScopedAllowAllocation&&) = delete;
    ScopedAllowAllocation& operator= (ScopedAllowAllocation&&) = delete;
};

/**
    Returns the number of allocations and deallocations made inside a ScopedNoAllocation so far, on any
    thread, excluding exempted code. Always 0 when the guard is compiled out.

    @note  Any thread.
*/
[[nodiscard]] std::size_t allocationViolationCount() noexcept;

/**
    Returns the number of allocations and deallocations made by exempted code (ScopedAllowAllocation) inside a
    ScopedNoAllocation so far, on any thread. Always 0 when the guard is compiled out.

    @note  Any thread.
*/
[[nodiscard]] std::size_t exemptAllocationCount() noexcept;

} // namespace dawg
