#include "Preset.h"

namespace midi {
namespace {
uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFF;
    while (n--) {
        crc ^= *p++;
        for (uint8_t i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
}
bool encodePreset(const Snapshot& state, PresetBytes& out) {
    if (!fullyKnown(state)) return false;
    for (const auto& ch : state) for (auto v : ch.value) if (v > 127) return false;
    out = {}; out[0] = 'C'; out[1] = 'M'; out[2] = 'I'; out[3] = 'D';
    out[4] = 2; out[5] = 16; out[6] = FieldCount;
    size_t pos = 8;
    for (const auto& ch : state) for (auto v : ch.value) out[pos++] = v;
    uint32_t crc = crc32(out.data(), pos);
    for (uint8_t i = 0; i < 4; ++i) out[pos++] = static_cast<uint8_t>(crc >> (8 * i));
    return true;
}
bool decodePreset(const uint8_t* bytes, size_t size, Snapshot& state) {
    state = defaults();
    if (!bytes || (size != PresetSize && size != LegacyPresetSize)) return false;
    const bool legacy = bytes[4] == 1 && size == LegacyPresetSize && bytes[6] == 7;
    const bool current = bytes[4] == 2 && size == PresetSize && bytes[6] == FieldCount;
    if ((!legacy && !current) || bytes[0] != 'C' || bytes[1] != 'M' ||
        bytes[2] != 'I' || bytes[3] != 'D' || bytes[5] != 16 || bytes[7] != 0) return false;
    uint32_t stored = 0;
    for (uint8_t i = 0; i < 4; ++i) stored |= uint32_t(bytes[size - 4 + i]) << (8 * i);
    if (stored != crc32(bytes, size - 4)) return false;
    Snapshot decoded = defaults(); size_t pos = 8;
    for (auto& ch : decoded) for (uint8_t f = 0; f < bytes[6]; ++f) {
        ch.value[f] = bytes[pos++]; if (ch.value[f] > 127) return false;
    }
    state = decoded; return true;
}
} // namespace midi
