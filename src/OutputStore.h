#pragma once
#include <Preferences.h>
#include <OutputLevel.h>
class OutputStore {
public:
    bool begin() { return ready_ = prefs_.begin("card-output", false); }
    bool load(int& db) {
        db = -40;
        if (!ready_ || prefs_.getBytesLength("level") != 8) return false;
        midi::OutputBytes bytes{};
        return prefs_.getBytes("level", bytes.data(), bytes.size()) == bytes.size() &&
            midi::decodeOutput(bytes.data(), bytes.size(), db);
    }
    bool save(int db) {
        if (!ready_ || db < -40 || db > 0) return false;
        const auto bytes = midi::encodeOutput(db);
        if (prefs_.putBytes("level", bytes.data(), bytes.size()) != bytes.size()) return false;
        int verify; return load(verify) && verify == db;
    }
private:
    Preferences prefs_;
    bool ready_ = false;
};
