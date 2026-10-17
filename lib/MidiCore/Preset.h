#pragma once
#include "MidiCore.h"

namespace midi {
// Explicit byte format, independent of C++ padding/endian or compiler version.
constexpr size_t PresetSize = 8 + 16 * FieldCount + 4;
constexpr size_t LegacyPresetSize = 8 + 16 * 7 + 4;
using PresetBytes = std::array<uint8_t, PresetSize>;
bool encodePreset(const Snapshot& state, PresetBytes& bytes);
bool decodePreset(const uint8_t* bytes, size_t size, Snapshot& state);
} // namespace midi
