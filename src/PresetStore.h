#pragma once
#include <Preferences.h>
#include <Preset.h>

class PresetStore {
public:
    enum class Status { Empty, Valid, Invalid, Unavailable };
    bool begin() { ready_ = prefs_.begin("card-midi", false); return ready_; }
    Status load(uint8_t slot, midi::Snapshot& state) {
        state = midi::defaults();
        if (!ready_ || slot >= 8) return Status::Unavailable;
        char key[8]; keyFor(slot, key);
        const size_t size = prefs_.getBytesLength(key);
        if (!size) return Status::Empty;
        if (size != midi::PresetSize && size != midi::LegacyPresetSize) return Status::Invalid;
        midi::PresetBytes bytes{};
        if (prefs_.getBytes(key, bytes.data(), size) != size ||
            !midi::decodePreset(bytes.data(), size, state)) return Status::Invalid;
        return Status::Valid;
    }
    bool save(uint8_t slot, const midi::Snapshot& state) {
        if (!ready_ || slot >= 8) return false;
        midi::PresetBytes bytes{};
        if (!midi::encodePreset(state, bytes)) return false;
        char key[8]; keyFor(slot, key);
        if (prefs_.putBytes(key, bytes.data(), bytes.size()) != bytes.size()) return false;
        midi::Snapshot verify;
        return load(slot, verify) == Status::Valid && midi::equal(verify, state);
    }
    uint8_t last() { return ready_ ? prefs_.getUChar("last", 255) : 255; }
    bool select(uint8_t slot) {
        return ready_ && slot < 8 && prefs_.putUChar("last", slot) == 1;
    }
private:
    static void keyFor(uint8_t slot, char* key) { snprintf(key, 8, "slot%u", slot); }
    Preferences prefs_;
    bool ready_ = false;
};
