#include "dawg/AllocationGuard.h"

#include <atomic>

#if DAWG_ALLOCATION_GUARD
 #include <cstdlib>
 #include <new>
#endif

namespace dawg
{

#if DAWG_ALLOCATION_GUARD

namespace
{
// Trivially initialised, so safe to touch from inside operator new during static initialisation.
thread_local int guardDepth = 0;
thread_local int exemptDepth = 0;
std::atomic<std::size_t> violations { 0 };
std::atomic<std::size_t> exempt { 0 };

void noteAllocation() noexcept
{
    // Counters only; no ordering with other data needed.
    if (guardDepth > 0)
        (exemptDepth > 0 ? exempt : violations).fetch_add (1, std::memory_order_relaxed);
}
} // namespace

ScopedNoAllocation::ScopedNoAllocation() noexcept
{
    ++guardDepth;
}

ScopedNoAllocation::~ScopedNoAllocation()
{
    --guardDepth;
}

ScopedAllowAllocation::ScopedAllowAllocation() noexcept
{
    ++exemptDepth;
}

ScopedAllowAllocation::~ScopedAllowAllocation()
{
    --exemptDepth;
}

std::size_t allocationViolationCount() noexcept
{
    return violations.load();
}

std::size_t exemptAllocationCount() noexcept
{
    return exempt.load();
}

#else

ScopedNoAllocation::ScopedNoAllocation() noexcept = default;
ScopedNoAllocation::~ScopedNoAllocation() = default;

ScopedAllowAllocation::ScopedAllowAllocation() noexcept = default;
ScopedAllowAllocation::~ScopedAllowAllocation() = default;

std::size_t allocationViolationCount() noexcept
{
    return 0;
}

std::size_t exemptAllocationCount() noexcept
{
    return 0;
}

#endif

} // namespace dawg

#if DAWG_ALLOCATION_GUARD

// -----------------------------------------------------------------------------------------------------------
// Replacement global allocation functions. They behave like the defaults (malloc/free based) and additionally
// report calls made while a ScopedNoAllocation is alive. This is the one place where malloc/free are allowed.
// NOLINTBEGIN(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory, readability-inconsistent-declaration-parameter-name)

namespace
{
[[nodiscard]] void* allocate (std::size_t size)
{
    dawg::noteAllocation();

    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;

    throw std::bad_alloc();
}

[[nodiscard]] void* allocateAligned (std::size_t size, std::align_val_t alignment)
{
    dawg::noteAllocation();
    const auto align = static_cast<std::size_t> (alignment);
    size = size == 0 ? 1 : size;

 #ifdef _MSC_VER
    if (void* p = _aligned_malloc (size, align))
        return p;
 #else
    void* p = nullptr;
    if (posix_memalign (&p, align < sizeof (void*) ? sizeof (void*) : align, size) == 0)
        return p;
 #endif

    throw std::bad_alloc();
}

void deallocate (void* p) noexcept
{
    if (p == nullptr)
        return;

    dawg::noteAllocation();
    std::free (p);
}

void deallocateAligned (void* p) noexcept
{
    if (p == nullptr)
        return;

    dawg::noteAllocation();

 #ifdef _MSC_VER
    _aligned_free (p);
 #else
    std::free (p);
 #endif
}
} // namespace

[[nodiscard]] void* operator new (std::size_t size)
{
    return allocate (size);
}
[[nodiscard]] void* operator new[] (std::size_t size)
{
    return allocate (size);
}
[[nodiscard]] void* operator new (std::size_t size, std::align_val_t a)
{
    return allocateAligned (size, a);
}
[[nodiscard]] void* operator new[] (std::size_t size, std::align_val_t a)
{
    return allocateAligned (size, a);
}

[[nodiscard]] void* operator new (std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    try
    {
        return allocate (size);
    }
    catch (...)
    {
        return nullptr;
    }
}

[[nodiscard]] void* operator new[] (std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    try
    {
        return allocate (size);
    }
    catch (...)
    {
        return nullptr;
    }
}

[[nodiscard]] void* operator new (std::size_t size, std::align_val_t a, const std::nothrow_t& /*tag*/) noexcept
{
    try
    {
        return allocateAligned (size, a);
    }
    catch (...)
    {
        return nullptr;
    }
}

[[nodiscard]] void* operator new[] (std::size_t size, std::align_val_t a, const std::nothrow_t& /*tag*/) noexcept
{
    try
    {
        return allocateAligned (size, a);
    }
    catch (...)
    {
        return nullptr;
    }
}

void operator delete (void* p) noexcept
{
    deallocate (p);
}
void operator delete[] (void* p) noexcept
{
    deallocate (p);
}
void operator delete (void* p, const std::nothrow_t& /*tag*/) noexcept
{
    deallocate (p);
}
void operator delete[] (void* p, const std::nothrow_t& /*tag*/) noexcept
{
    deallocate (p);
}
void operator delete (void* p, std::align_val_t /*alignment*/) noexcept
{
    deallocateAligned (p);
}
void operator delete[] (void* p, std::align_val_t /*alignment*/) noexcept
{
    deallocateAligned (p);
}
void operator delete (void* p, std::align_val_t /*alignment*/, const std::nothrow_t& /*tag*/) noexcept
{
    deallocateAligned (p);
}
void operator delete[] (void* p, std::align_val_t /*alignment*/, const std::nothrow_t& /*tag*/) noexcept
{
    deallocateAligned (p);
}

 // Sized deallocation is optional (e.g. off by default in some Clang versions); only replace it when enabled.
 #ifdef __cpp_sized_deallocation
void operator delete (void* p, std::size_t /*size*/) noexcept
{
    deallocate (p);
}
void operator delete[] (void* p, std::size_t /*size*/) noexcept
{
    deallocate (p);
}
void operator delete (void* p, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    deallocateAligned (p);
}
void operator delete[] (void* p, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept
{
    deallocateAligned (p);
}
 #endif

// NOLINTEND(cppcoreguidelines-no-malloc, cppcoreguidelines-owning-memory, readability-inconsistent-declaration-parameter-name)

#endif
