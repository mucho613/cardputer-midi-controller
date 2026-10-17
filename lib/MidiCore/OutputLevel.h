#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace midi {
using OutputBytes = std::array<uint8_t, 8>;
inline OutputBytes encodeOutput(int db) {
    const uint8_t attenuation = static_cast<uint8_t>(db >= -40 && db <= 0 ? -db : 40);
    return {{'O','U','T',1,attenuation,static_cast<uint8_t>(attenuation ^ 255),0x5A,0xA5}};
}
inline bool decodeOutput(const uint8_t* b, size_t size, int& db) {
    db = -40;
    if (size != 8 || b[0] != 'O' || b[1] != 'U' || b[2] != 'T' || b[3] != 1 ||
        b[4] > 40 || b[5] != (b[4] ^ 255) || b[6] != 0x5A || b[7] != 0xA5) return false;
    db = -static_cast<int>(b[4]); return true;
}
}
