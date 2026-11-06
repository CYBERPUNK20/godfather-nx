#pragma once
#include <bit>
#include <cstdint>

// Four independent MMX words. The same GNU vector expressions run on the
// development host and devkitA64. Never use signed word overflow.
namespace gfmovie {
using U16 = uint16_t __attribute__((vector_size(8)));
using S16 = int16_t __attribute__((vector_size(8)));
using S32 = int32_t __attribute__((vector_size(16)));
using Bytes4 = uint8_t __attribute__((vector_size(4)));
inline S32 wide(uint64_t x) { return __builtin_convertvector(std::bit_cast<S16>(x), S32); }
inline uint64_t sat16(S32 x) {
    x = x < -32768 ? -32768 : x;
    x = x > 32767 ? 32767 : x;
    return std::bit_cast<uint64_t>(__builtin_convertvector(x, S16));
}
inline uint64_t add_sat(uint64_t a, uint64_t b) { return sat16(wide(a) + wide(b)); }
inline uint64_t sub_sat(uint64_t a, uint64_t b) { return sat16(wide(a) - wide(b)); }
inline uint64_t mul_hi(uint64_t a, uint64_t b) {
    return std::bit_cast<uint64_t>(__builtin_convertvector((wide(a) * wide(b)) >> 16, S16));
}
inline uint64_t add(uint64_t a, uint64_t b) { return std::bit_cast<uint64_t>(std::bit_cast<U16>(a) + std::bit_cast<U16>(b)); }
inline uint64_t mul_lo(uint64_t a, uint64_t b) { return std::bit_cast<uint64_t>(std::bit_cast<U16>(a) * std::bit_cast<U16>(b)); }
inline uint32_t pack4(uint64_t x) {
    S16 s = std::bit_cast<S16>(x);
    s = s < 0 ? 0 : s;
    s = s > 255 ? 255 : s;
    return std::bit_cast<uint32_t>(__builtin_convertvector(s, Bytes4));
}
inline uint64_t pack8(uint64_t lo, uint64_t hi) { return uint64_t(pack4(lo)) | uint64_t(pack4(hi)) << 32; }
inline uint64_t expand4(uint32_t x) {
    return std::bit_cast<uint64_t>(__builtin_convertvector(std::bit_cast<Bytes4>(x), U16));
}
}
