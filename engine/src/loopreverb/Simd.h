#pragma once

#include <array>
#include <cstddef>

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)
 #include <emmintrin.h>
#elif defined(_M_ARM64) || defined(__aarch64__)
 #include <arm_neon.h>
#endif

// Four floats at once, for the reverb's sixteen lines (DESIGN.md, revision 1.7). Only addition, subtraction and
// multiplication, each rounded on its own as in scalar code (no fused multiply-add), so every lane gives exactly the
// result the scalar expression gives, bit for bit, whichever path is compiled.

namespace dawg::reverb::simd
{

#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__)

/**
    Four floats.
*/
struct Float4
{
    __m128 v;
};

[[nodiscard]] inline Float4 load (const float* p) noexcept
{
    return { _mm_loadu_ps (p) };
}
inline void store (float* p, Float4 a) noexcept
{
    _mm_storeu_ps (p, a.v);
}
[[nodiscard]] inline Float4 broadcast (float x) noexcept
{
    return { _mm_set1_ps (x) };
}
[[nodiscard]] inline Float4 operator+ (Float4 a, Float4 b) noexcept
{
    return { _mm_add_ps (a.v, b.v) };
}
[[nodiscard]] inline Float4 operator- (Float4 a, Float4 b) noexcept
{
    return { _mm_sub_ps (a.v, b.v) };
}
[[nodiscard]] inline Float4 operator* (Float4 a, Float4 b) noexcept
{
    return { _mm_mul_ps (a.v, b.v) };
}
[[nodiscard]] inline Float4 negate (Float4 a) noexcept
{
    return { _mm_xor_ps (a.v, _mm_set1_ps (-0.0f)) };
}

/**
    Lanes 1, 0, 3, 2.
*/
[[nodiscard]] inline Float4 swapPairs (Float4 a) noexcept
{
    return { _mm_shuffle_ps (a.v, a.v, _MM_SHUFFLE (2, 3, 0, 1)) };
}

/**
    Lanes 2, 3, 0, 1.
*/
[[nodiscard]] inline Float4 swapHalves (Float4 a) noexcept
{
    return { _mm_shuffle_ps (a.v, a.v, _MM_SHUFFLE (1, 0, 3, 2)) };
}

/**
    Each lane truncated towards zero (static_cast<int>), stored to whole; returns them as floats again.
*/
[[nodiscard]] inline Float4 truncate (Float4 a, int* whole) noexcept
{
    const auto i = _mm_cvttps_epi32 (a.v);
    _mm_storeu_si128 (reinterpret_cast<__m128i*> (whole), i);
    return { _mm_cvtepi32_ps (i) };
}

#elif defined(_M_ARM64) || defined(__aarch64__)

struct Float4
{
    float32x4_t v;
};

[[nodiscard]] inline Float4 load (const float* p) noexcept
{
    return { vld1q_f32 (p) };
}
inline void store (float* p, Float4 a) noexcept
{
    vst1q_f32 (p, a.v);
}
[[nodiscard]] inline Float4 broadcast (float x) noexcept
{
    return { vdupq_n_f32 (x) };
}
[[nodiscard]] inline Float4 operator+ (Float4 a, Float4 b) noexcept
{
    return { vaddq_f32 (a.v, b.v) };
}
[[nodiscard]] inline Float4 operator- (Float4 a, Float4 b) noexcept
{
    return { vsubq_f32 (a.v, b.v) };
}
[[nodiscard]] inline Float4 operator* (Float4 a, Float4 b) noexcept
{
    return { vmulq_f32 (a.v, b.v) };
}
[[nodiscard]] inline Float4 negate (Float4 a) noexcept
{
    return { vnegq_f32 (a.v) };
}
[[nodiscard]] inline Float4 swapPairs (Float4 a) noexcept
{
    return { vrev64q_f32 (a.v) };
}
[[nodiscard]] inline Float4 swapHalves (Float4 a) noexcept
{
    return { vextq_f32 (a.v, a.v, 2) };
}

[[nodiscard]] inline Float4 truncate (Float4 a, int* whole) noexcept
{
    const auto i = vcvtq_s32_f32 (a.v); // rounds towards zero
    vst1q_s32 (whole, i);
    return { vcvtq_f32_s32 (i) };
}

#else

struct Float4
{
    std::array<float, 4> v;
};

[[nodiscard]] inline Float4 load (const float* p) noexcept
{
    return { { p[0], p[1], p[2], p[3] } };
}

inline void store (float* p, Float4 a) noexcept
{
    for (size_t i = 0; i < 4; ++i)
        p[i] = a.v[i];
}

[[nodiscard]] inline Float4 broadcast (float x) noexcept
{
    return { { x, x, x, x } };
}

template <typename Op>
[[nodiscard]] inline Float4 lanes (Float4 a, Float4 b, Op op) noexcept
{
    return { { op (a.v[0], b.v[0]), op (a.v[1], b.v[1]), op (a.v[2], b.v[2]), op (a.v[3], b.v[3]) } };
}

[[nodiscard]] inline Float4 operator+ (Float4 a, Float4 b) noexcept
{
    return lanes (a, b, [] (float x, float y) { return x + y; });
}

[[nodiscard]] inline Float4 operator- (Float4 a, Float4 b) noexcept
{
    return lanes (a, b, [] (float x, float y) { return x - y; });
}

[[nodiscard]] inline Float4 operator* (Float4 a, Float4 b) noexcept
{
    return lanes (a, b, [] (float x, float y) { return x * y; });
}

[[nodiscard]] inline Float4 negate (Float4 a) noexcept
{
    return { { -a.v[0], -a.v[1], -a.v[2], -a.v[3] } };
}
[[nodiscard]] inline Float4 swapPairs (Float4 a) noexcept
{
    return { { a.v[1], a.v[0], a.v[3], a.v[2] } };
}
[[nodiscard]] inline Float4 swapHalves (Float4 a) noexcept
{
    return { { a.v[2], a.v[3], a.v[0], a.v[1] } };
}

[[nodiscard]] inline Float4 truncate (Float4 a, int* whole) noexcept
{
    Float4 result {};

    for (size_t i = 0; i < 4; ++i)
    {
        whole[i] = static_cast<int> (a.v[i]);
        result.v[i] = static_cast<float> (whole[i]);
    }

    return result;
}

#endif

/**
    Four lanes of an array, starting at index i.
*/
template <size_t n>
[[nodiscard]] inline Float4 load (const std::array<float, n>& a, size_t i) noexcept
{
    return load (a.data() + i);
}

/**
    Stores four lanes into an array, starting at index i.
*/
template <size_t n>
inline void store (std::array<float, n>& a, size_t i, Float4 x) noexcept
{
    store (a.data() + i, x);
}

} // namespace dawg::reverb::simd
