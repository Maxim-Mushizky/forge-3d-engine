#pragma once

#include <cstdint>
#include <random>

namespace forge {

// 64-bit random id for entities/assets. Not cryptographic — collision odds at
// editor scale (thousands of objects) are negligible.
using UUID = uint64_t;

inline UUID GenerateUUID()
{
    static std::mt19937_64 rng{std::random_device{}()};
    static std::uniform_int_distribution<uint64_t> dist{1, UINT64_MAX}; // 0 reserved = "no entity"
    return dist(rng);
}

} // namespace forge
