#pragma once

#include "ZunMath.hpp"
#include "inttypes.hpp"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof(x[0]))

union AnyArg {
    i32 i;
    u32 u;
    f32 f;
    i16 s[2];
    u16 us[2];
    i8 c[4];
    u8 b[4];
};
static_assert(sizeof(AnyArg) == 4);

namespace utils
{
f32 AddNormalizeAngle(f32 a, f32 b);
void Rotate(ZunVec3 *out, ZunVec3 *point, f32 angle);

inline f32 NormalizeAngle(f32 a)
{
    return AddNormalizeAngle(a, 0.0f);
}
} // namespace utils
