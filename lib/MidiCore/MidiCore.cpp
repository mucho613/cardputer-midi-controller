#include "MidiCore.h"

namespace midi {
namespace {
uint8_t length(uint8_t status) {
    if (status < 0x80) return 0;
    if (status < 0xF0) return (status & 0xE0) == 0xC0 ? 2 : 3;
    if (status == 0xF1 || status == 0xF3) return 2;
    if (status == 0xF2) return 3;
    return 1;
}
constexpr uint8_t controllers[] = {0, 32, 0, 7, 10, 91, 93};
constexpr uint8_t toneNrpn[] = {0x20, 0x21, 0x63, 0x66};
Message tone(uint8_t ch, Field field, uint8_t value) {
    Message m = cc(ch, 6, value); m.toneField = field; return m;
}
}
Snapshot defaults() { return Snapshot{}; }
bool fullyKnown(const Snapshot& state) {
    for (const auto& ch : state) if (ch.known != AllKnown) return false;
    return true;
}
bool equal(const Snapshot& a, const Snapshot& b) {
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].known != b[i].known || a[i].value != b[i].value) return false;
    return true;
}
Message shortMessage(uint8_t status, uint8_t a, uint8_t b) {
    Message m;
    m.data[0] = status; m.data[1] = a; m.data[2] = b; m.size = length(status);
    return m;
}
Message cc(uint8_t channel, uint8_t controller, uint8_t value) {
    return shortMessage(0xB0 | channel, controller, value);
}
Engine::Engine() { invalidate(); }
void Engine::invalidate() {
    for (auto& ch : state_) ch.known = 0;
    bankKnown_.fill(0);
    selection_.fill(Selection{});
    gmModeKnown_ = false;
    ++revision_;
}
void Engine::resetParser() {
    running_ = status_ = needed_ = received_ = 0; sysex_ = false;
}
bool Engine::put(uint8_t b) {
    if (txCount_ == tx_.size()) return false;
    pauseAfter_[(txRead_ + txCount_) % tx_.size()] = false;
    tx_[(txRead_ + txCount_) % tx_.size()] = b;
    ++txCount_; return true;
}
bool Engine::popTx(uint8_t& b) {
    if (holding_) {
        if (static_cast<int32_t>(now_ - holdUntil_) < 0) return false;
        holding_ = false;
    }
    if (!txCount_) return false;
    const bool pause = pauseAfter_[txRead_];
    b = tx_[txRead_]; txRead_ = (txRead_ + 1) % tx_.size(); --txCount_;
    if (pause) {
        holding_ = true;
        // At least 100 ms quiet time after the hardware TX FIFO tail (<=128 bytes).
        holdUntil_ = now_ + 200;
    }
    return true;
}
bool Engine::setOutputLevel(int db) {
    if (db < -40 || db > 0) return false;
    outputDb_ = db; outputEnabled_ = outputPending_ = true;
    return true;
}
bool Engine::emitOutput() {
    if (tx_.size() - txCount_ < 16) return false;
    const uint16_t value = 0x1B40 | (0x39 + outputDb_);
    const uint8_t bytes[] = {0xF0,0,0x20,0,0,0,0x12,0x33,0x77,0x12,
        static_cast<uint8_t>((value >> 12) & 15), static_cast<uint8_t>((value >> 8) & 15),
        static_cast<uint8_t>((value >> 4) & 15), static_cast<uint8_t>(value & 15),0,0xF7};
    for (auto b : bytes) put(b);
    outRunning_ = 0; outputPending_ = false;
    return true;
}
void Engine::resetTail() {
    pauseAfter_[(txRead_ + txCount_ - 1) % tx_.size()] = true;
    if (outputEnabled_) emitOutput(); // Caller reserves the entire tail.
}
bool Engine::receivedReset() const {
    const auto& p = sysexPrefix_;
    if (resetInSysEx_) return true;
    // Universal GM on/off/GM2, Roland GS reset, Yamaha XG reset.
    if (sysexLength_ == 6 && p[1] == 0x7E && p[3] == 9 && p[4] >= 1 && p[4] <= 3) return true;
    if (sysexLength_ == 11 && p[1] == 0x41 && p[3] == 0x42 && p[4] == 0x12 &&
        p[5] == 0x40 && p[6] == 0 && p[7] == 0x7F && p[8] == 0) return true;
    if (sysexLength_ == 9 && p[1] == 0x43 && p[3] == 0x4C && p[4] == 0 && p[5] == 0 && p[6] == 0x7E) return true;
    // Keep the local output setting authoritative over external codec writes.
    return sysexLength_ == 16 && p[1] == 0 && p[2] == 0x20 && p[3] == 0 && p[4] == 0 &&
        p[5] == 0 && p[6] == 0x12 && p[7] == 0x33 && p[8] == 0x77 && p[9] == 0x12;
}
void Engine::apply(const Message& m) {
    const uint8_t s = m.data[0];
    if (s == 0xFF || s == 0xF0) {
        invalidate();
        if (s == 0xF0 && m.size == 6 && m.data[3] == 0x09 && m.data[4] == 1) gmModeKnown_ = true;
        return;
    }
    if (s >= 0xF0) return;
    const uint8_t ch = s & 15, type = s & 0xF0;
    auto& c = state_[ch];
    if (type == 0xC0) {
        c.value[Program] = m.data[1]; c.known |= 1u << Program;
        for (uint8_t i = 0; i < 2; ++i) {
            c.value[i] = pendingBank_[ch][i];
            c.known = (c.known & ~(1u << i)) | ((bankKnown_[ch] & (1u << i)));
        }
        ++revision_;
    } else if (type == 0xB0) {
        const uint8_t controller = m.data[1], value = m.data[2];
        if (controller == 0 || controller == 32) {
            const uint8_t i = controller == 0 ? 0 : 1;
            pendingBank_[ch][i] = value; bankKnown_[ch] |= 1u << i;
        }
        auto& sel = selection_[ch];
        if (controller == 99 || controller == 98 || controller == 101 || controller == 100) {
            const uint8_t index = controller == 99 ? 0 : controller == 98 ? 1 : controller == 101 ? 2 : 3;
            sel.values[index] = value; sel.known |= 1u << index; sel.active = index & 2;
        }
        if ((controller == 6 || controller == 38 || controller == 96 || controller == 97) &&
            sel.active == 0 && (sel.known & 3) == 3 && sel.values[0] == 1) {
            for (uint8_t f = Cutoff; f < FieldCount; ++f) if (sel.values[1] == toneNrpn[f - Cutoff]) {
                if (controller == 6) { c.value[f] = value; c.known |= 1u << f; }
                else { c.known &= ~(1u << f); } // Unsupported precision/increment semantics.
                ++revision_;
            }
        }
        for (uint8_t f = Volume; f <= Chorus; ++f) {
            if (controllers[f] == controller) {
                c.value[f] = value; c.known |= 1u << f; ++revision_;
            }
        }
        if (controller == 121) { // Device-dependent controller reset: be conservative.
            sel = Selection{};
            c.known &= (1u << Volume) - 1; ++revision_;
        }
    }
}
bool Engine::emit(const Message& m) {
    if (m.toneField != 255) {
        // Seven CCs at most: select, data, restore both selector pairs. Reserve
        // the entire transaction so external input can never split it.
        if (tx_.size() - txCount_ < 21) return false;
        const uint8_t ch = m.data[0] & 15, f = m.toneField;
        const auto saved = selection_[ch];
        emit(cc(ch, 99, 1)); emit(cc(ch, 98, toneNrpn[f - Cutoff])); emit(cc(ch, 6, m.data[2]));
        for (uint8_t pass = 0; pass < 2; ++pass) {
            const uint8_t base = pass == 0 ? (saved.active ^ 2) : saved.active;
            // Preserve even a partially selected pair. Unknown bytes are 127
            // on wire and remain unknown to the state tracker.
            emit(cc(ch, base == 0 ? 99 : 101, (saved.known & (1u << base)) ? saved.values[base] : 127));
            emit(cc(ch, base == 0 ? 98 : 100, (saved.known & (2u << base)) ? saved.values[base + 1] : 127));
        }
        selection_[ch] = saved;
        return true;
    }
    const uint8_t s = m.data[0];
    const auto& sel = selection_[s & 15];
    const bool nrpnReset = (s & 0xF0) == 0xB0 && m.data[1] == 6 && m.data[2] == 0x45 &&
        sel.active == 0 && (sel.known & 3) == 3 && sel.values[0] == 0x37 && sel.values[1] == 0x5F;
    const bool reset = (s == 0xFF && !sysex_) || nrpnReset ||
        (s == 0xF0 && m.size == 6 && m.data[3] == 9);
    if (reset && tx_.size() - txCount_ < m.size + size_t(outputEnabled_ ? 16 : 0)) return false;
    const bool program = (s & 0xF0) == 0xC0;
    if (program && tx_.size() - txCount_ < 87) return false;
    const bool omit = s < 0xF0 && outRunning_ == s;
    if (tx_.size() - txCount_ < static_cast<size_t>(m.size - (omit ? 1 : 0))) return false;
    for (uint8_t i = omit ? 1 : 0; i < m.size; ++i) put(m.data[i]);
    if (s < 0xF0) outRunning_ = s;
    else if (s < 0xF8 || s == 0xFF) outRunning_ = 0;
    apply(m);
    if (nrpnReset) invalidate();
    if (reset) resetTail();
    if (program) {
        const uint8_t ch = s & 15;
        for (uint8_t f = Cutoff; f < FieldCount; ++f)
            if (state_[ch].known & (1u << f)) emit(tone(ch, static_cast<Field>(f), state_[ch].value[f]));
    }
    return true;
}
bool Engine::queue(const Message& m) {
    if (recovering_ || localCount_ == local_.size()) return false;
    local_[(localRead_ + localCount_) % local_.size()] = m; ++localCount_; return true;
}
bool Engine::edit(uint8_t ch, Field field, uint8_t value) {
    if (ch > 15 || field < Program || field >= FieldCount || value > 127) return false;
    if (field == Program) {
        // UI selects GM bank 0 explicitly; external bank changes remain supported.
        if (recovering_ || local_.size() - localCount_ < 3) return false;
        queue(cc(ch, 0, 0)); queue(cc(ch, 32, 0)); return queue(shortMessage(0xC0 | ch, value));
    }
    return queue(field >= Cutoff ? tone(ch, field, value) : cc(ch, controllers[field], value));
}
bool Engine::restore(const Snapshot& state) {
    if (!fullyKnown(state) || busy() || previewActive_ || previewPending_) return false;
    constexpr size_t restoreCommands = 48 + 1 + 16 * (3 + 4 + 4);
    if (local_.size() - localCount_ < restoreCommands) return false;
    for (const auto& ch : state) for (uint8_t v : ch.value) if (v > 127) return false;
    panic();
    Message reset; reset.size = 6; reset.data = {{0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7, 0, 0}};
    queue(reset);
    for (uint8_t ch = 0; ch < 16; ++ch) {
        const auto& v = state[ch].value;
        queue(cc(ch, 0, v[BankMSB])); queue(cc(ch, 32, v[BankLSB]));
        queue(shortMessage(0xC0 | ch, v[Program]));
        for (uint8_t f = Volume; f <= Chorus; ++f) queue(cc(ch, controllers[f], v[f]));
        for (uint8_t f = Cutoff; f < FieldCount; ++f) queue(tone(ch, static_cast<Field>(f), v[f]));
    }
    return true;
}
bool Engine::preview(uint8_t ch) {
    if (ch > 15 || previewActive_ || previewPending_) return false;
    Message m = shortMessage(0x90 | ch, ch == 9 ? 36 : 60, 80); m.preview = true;
    if (!queue(m)) return false;
    previewPending_ = true; return true;
}
void Engine::panic() {
    // Explicit panic may abort a stalled SysEx through the recovery path.
    if (sysex_) { fault(); --errors_; return; }
    localRead_ = localCount_ = 0;
    previewActive_ = previewPending_ = false;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        queue(cc(ch, 64, 0)); queue(cc(ch, 120, 0)); queue(cc(ch, 123, 0));
    }
}
void Engine::fault() {
    ++errors_; invalidate(); resetParser();
    if (outputEnabled_) outputPending_ = true;
    recovering_ = true; recoverySent_ = false; sensing_ = false;
    localRead_ = localCount_ = 0; previewActive_ = previewPending_ = false;
}
void Engine::emitPanic() {
    put(0xF7); outRunning_ = 0; // Close any already transmitted partial SysEx.
    for (uint8_t ch = 0; ch < 16; ++ch) {
        emit(cc(ch, 64, 0)); emit(cc(ch, 120, 0)); emit(cc(ch, 123, 0));
    }
}
void Engine::pump(uint32_t now) {
    if (sysex_ || needed_ || recovering_) return;
    if (outputPending_ && !emitOutput()) return;
    while (localCount_ && tx_.size() - txCount_ >= 8) {
        const auto m = local_[localRead_];
        if (!emit(m)) break;
        localRead_ = (localRead_ + 1) % local_.size(); --localCount_;
        if (m.preview) {
            previewPending_ = false; previewActive_ = true;
            previewChannel_ = m.data[0] & 15; previewNote_ = m.data[1];
            // Account for bytes ahead of the note in the UART queue.
            previewAt_ = now + 250 + static_cast<uint32_t>((txCount_ * 320 + 999) / 1000);
        }
    }
}
void Engine::tick(uint32_t now) {
    now_ = now;
    if (!recovering_ && ((sysex_ && now - lastData_ > 500) ||
        (needed_ && now - lastData_ > 100) || (sensing_ && now - lastByte_ > 330))) fault();
    if (recovering_) {
        if (txCount_) return;
        if (!recoverySent_) { emitPanic(); recoverySent_ = true; }
        else recovering_ = false;
        return;
    }
    if (previewActive_ && static_cast<int32_t>(now - previewAt_) >= 0) {
        if (queue(shortMessage(0x80 | previewChannel_, previewNote_, 0))) previewActive_ = false;
    }
    pump(now);
}
void Engine::receive(uint8_t b, uint32_t now) {
    if (recovering_) return;
    lastByte_ = now;
    if (b >= 0xF8) {
        if (b == 0xFF && sysex_) resetInSysEx_ = true;
        if (b == 0xFE) sensing_ = true;
        if (!emit(shortMessage(b))) fault();
        // Reset is a realtime byte; leave parser framing intact.
        return;
    }
    lastData_ = now;
    if (sysex_) {
        if (b < 0x80 || b == 0xF7) {
            if (sysexLength_ < sysexPrefix_.size()) sysexPrefix_[sysexLength_] = b;
            if (sysexLength_ <= sysexPrefix_.size()) ++sysexLength_;
            const bool reset = b == 0xF7 && receivedReset();
            if (reset && tx_.size() - txCount_ < size_t(outputEnabled_ ? 17 : 1)) { fault(); return; }
            if (!put(b)) { fault(); return; }
            if (b == 0xF7) {
                sysex_ = false;
                if (reset) resetTail();
                pump(now);
            }
            return;
        }
        fault(); return; // Malformed SysEx: close it before resuming.
    }
    if (b & 0x80) {
        if (needed_) { fault(); return; }
        if (b == 0xF0) {
            running_ = outRunning_ = 0; sysex_ = true; invalidate();
            sysexPrefix_[0] = 0xF0; sysexLength_ = 1; resetInSysEx_ = false;
            if (!put(b)) fault();
            return;
        }
        status_ = b; running_ = b < 0xF0 ? b : 0;
        needed_ = length(b) - 1; received_ = 0;
        if (!needed_) {
            if (!emit(shortMessage(b))) fault();
            else pump(now);
        }
        return;
    }
    if (!needed_) {
        if (!running_) { fault(); return; }
        status_ = running_; needed_ = length(status_) - 1; received_ = 0;
    }
    data_[received_++] = b;
    if (received_ == needed_) {
        Message m = shortMessage(status_, data_[0], data_[1]);
        needed_ = received_ = 0;
        if (!emit(m)) { fault(); return; }
        if (status_ < 0xF0) { activity_[status_ & 15] = now; activitySeen_ |= 1u << (status_ & 15); }
        pump(now);
    }
}
} // namespace midi
