#include <Arduino.h>
#include <M5Cardputer.h>
#include <MidiCore.h>
#include <atomic>
#include "PresetStore.h"
#include "OutputStore.h"
#include "GmNames.h"

namespace {
constexpr int MidiRx = 1, MidiTx = 2;
constexpr uint16_t Bg = 0x0841, Muted = 0x8C71, Accent = 0x07FF;
midi::Engine engine;
PresetStore presets;
OutputStore outputStore;
bool outputPage = false, outputSaved = false;
int savedOutput = -40;
M5Canvas screen(&M5Cardputer.Display);
std::atomic<bool> uartError{false};
midi::Snapshot baseline = midi::defaults(), loadTarget = midi::defaults();
uint8_t channel = 0, fieldIndex = 0, slot = 0;
bool tonePage = false;
uint8_t pageSelection[2] = {0, 5};
int activeSlot = -1;
bool presetPage = false, helpPage = false, confirmSave = false, loadPending = false;
bool loadSlotValid = false;
bool frameReady = false;
std::array<PresetStore::Status, 8> slotStatus{};
std::array<bool, 256> previousKeys{};
char notification[40] = "Starting...";
uint32_t notificationUntil = 0, lastFrame = 0, repeatAt = 0;
char repeatKey = 0;
std::array<std::array<int, 9>, 16> queuedValues{};
std::array<std::array<bool, 9>, 16> hasQueuedValue{};
const midi::Field fields[] = {midi::Program, midi::Volume, midi::Pan, midi::Reverb, midi::Chorus,
    midi::Cutoff, midi::Resonance, midi::Attack, midi::Release};
const char* fieldNames[] = {"PROGRAM", "VOLUME", "PAN", "REVERB", "CHORUS", "CUTOFF", "RESONANCE", "ATTACK", "RELEASE"};
const char* fieldShort[] = {"PG", "V", "P", "R", "C", "CF", "RS", "AT", "RL"};

void notice(const char* text) {
    snprintf(notification, sizeof(notification), "%s", text);
    notificationUntil = millis() + 3500;
    Serial.println(text);
}
void refreshSlots() {
    midi::Snapshot scratch;
    for (uint8_t i = 0; i < 8; ++i) slotStatus[i] = presets.load(i, scratch);
}
void serviceMidi() {
    if (uartError.exchange(false)) engine.fault();
    // Bounded work keeps keyboard/UI responsive even with a continuous stream.
    for (int i = 0; i < 256 && Serial1.available(); ++i) {
        const int b = Serial1.read();
        if (b >= 0) engine.receive(static_cast<uint8_t>(b), millis());
    }
    engine.tick(millis());
    uint8_t b;
    for (int i = 0; i < 256 && Serial1.availableForWrite() > 0; ++i) {
        if (!engine.popTx(b)) break;
        Serial1.write(b);
    }
}
bool hardwareIdle() {
    // Includes the hardware TX buffer; a little UART tail is harmless for saving.
    return !Serial1.available() && !engine.busy();
}
void requestLoad() {
    if (loadPending) { notice("Load already pending"); return; }
    const auto status = presets.load(slot, loadTarget);
    loadSlotValid = status == PresetStore::Status::Valid;
    slotStatus[slot] = status;
    if (status != PresetStore::Status::Valid) {
        loadTarget = midi::defaults();
        notice(status == PresetStore::Status::Empty ? "Empty: loading defaults" : "Invalid: loading defaults");
    } else notice("Load pending; stop notes");
    loadPending = true; confirmSave = false;
}
void savePreset() {
    if (loadPending || !hardwareIdle() || engine.pendingLocal()) { notice("Busy: retry save"); return; }
    if (!midi::fullyKnown(engine.state()) || !engine.gmModeKnown()) { notice("Unknown state: load preset first"); return; }
    if (!presets.save(slot, engine.state())) { notice("Save failed"); confirmSave = false; return; }
    baseline = engine.state(); activeSlot = slot;
    const bool selected = presets.select(slot);
    refreshSlots(); confirmSave = false;
    notice(selected ? "Preset saved" : "Saved; boot selection failed");
}
void changeValue(int delta) {
    if (loadPending) { notice("Load pending"); return; }
    const auto f = fields[fieldIndex];
    const auto& ch = engine.state()[channel];
    const int base = hasQueuedValue[channel][fieldIndex] ? queuedValues[channel][fieldIndex] :
        ((ch.known & (1u << f)) ? ch.value[f] : midi::Channel{}.value[f]);
    const int next = constrain(base + delta, 0, 127);
    if (!engine.edit(channel, f, static_cast<uint8_t>(next))) notice("Command queue full: retry");
    else { queuedValues[channel][fieldIndex] = next; hasQueuedValue[channel][fieldIndex] = true; }
}
void key(char c, bool shift) {
    if (c == KEY_BACKSPACE) { loadPending = false; confirmSave = false; engine.panic(); notice("All sounds off"); return; }
    if (confirmSave) {
        if (c == KEY_ENTER || c == 'y') savePreset();
        else if (c == 'n' || c == 'p' || c == '`') { confirmSave = false; notice("Save cancelled"); }
        return;
    }
    if (c == 'o') { outputPage = !outputPage; helpPage = presetPage = false; return; }
    if (c == 'h') { helpPage = !helpPage; return; }
    if (c == 'p') { presetPage = !presetPage; helpPage = outputPage = false; return; }
    if (helpPage) return;
    if (outputPage) {
        if (c == ',' || c == '/') {
            const int delta = (c == ',' ? -1 : 1) * (shift ? 10 : 1);
            engine.setOutputLevel(constrain(engine.outputLevel() + delta, -40, 0));
        }
        if (c == KEY_ENTER) { engine.setOutputLevel(engine.outputLevel()); notice("Output level queued"); }
        if (c == 's') {
            if (!hardwareIdle()) notice("Busy: retry output save");
            else if (outputStore.save(engine.outputLevel())) {
                savedOutput = engine.outputLevel(); outputSaved = true; notice("Output level saved");
            } else notice("Output save failed");
        }
        if (c == ' ' && !loadPending && !engine.preview(channel)) notice("Preview busy: retry");
        return;
    }
    if (presetPage) {
        if (loadPending) { notice("Load pending; Backspace cancels"); return; }
        if (c == '[' || c == ',') slot = (slot + 7) % 8;
        if (c == ']' || c == '/') slot = (slot + 1) % 8;
        if (c == KEY_ENTER) requestLoad();
        if (c == 's') {
            if (slotStatus[slot] == PresetStore::Status::Empty) savePreset();
            else confirmSave = true;
        }
        return;
    }
    if (c == '[') channel = (channel + 15) % 16;
    if (c == ']') channel = (channel + 1) % 16;
    if (c == 't') {
        pageSelection[tonePage ? 1 : 0] = fieldIndex;
        tonePage = !tonePage; fieldIndex = pageSelection[tonePage ? 1 : 0];
    }
    const uint8_t start = tonePage ? 5 : 0, count = tonePage ? 4 : 5;
    if (c == ';') fieldIndex = start + (fieldIndex - start + count - 1) % count;
    if (c == '.') fieldIndex = start + (fieldIndex - start + 1) % count;
    if (c == ',') changeValue(shift ? -10 : -1);
    if (c == '/') changeValue(shift ? 10 : 1);
    if (c == ' ' && !loadPending && !engine.preview(channel)) notice("Preview busy: retry");
}
void keyboard(uint32_t now) {
    M5Cardputer.update();
    std::array<bool, 256> down{};
    // Physical first-layer keys avoid Shift changing ',' to '<', etc.
    for (const auto& pos : M5Cardputer.Keyboard.keyList())
        down[static_cast<uint8_t>(M5Cardputer.Keyboard.getKeyValue(pos).value_first)] = true;
    const bool shift = M5Cardputer.Keyboard.keysState().shift;
    if (down[KEY_BACKSPACE] && !previousKeys[KEY_BACKSPACE]) {
        key(KEY_BACKSPACE, shift); previousKeys = down; repeatKey = 0; return;
    }
    for (size_t i = 0; i < down.size(); ++i) {
        if (down[i] && !previousKeys[i]) {
            key(static_cast<char>(i), shift);
            if (i == ',' || i == '/') { repeatKey = static_cast<char>(i); repeatAt = now + 400; }
        }
    }
    if (repeatKey && !down[static_cast<uint8_t>(repeatKey)]) repeatKey = 0;
    if (repeatKey && static_cast<int32_t>(now - repeatAt) >= 0) {
        key(repeatKey, shift); repeatAt = now + 100;
    }
    previousKeys = down;
}
void text(int x, int y, const char* s, uint16_t color = TFT_WHITE, int scale = 1) {
    screen.setTextColor(color, Bg); screen.setTextSize(scale); screen.setCursor(x, y); screen.print(s);
}
const char* statusName(PresetStore::Status s) {
    switch (s) {
        case PresetStore::Status::Empty: return "EMPTY";
        case PresetStore::Status::Valid: return "SAVED";
        case PresetStore::Status::Invalid: return "INVALID";
        default: return "NO STORAGE";
    }
}
void draw(uint32_t now) {
    if (!frameReady) return;
    screen.fillSprite(Bg);
    char line[64];
    if (helpPage) {
        text(4, 4, "KEY GUIDE", Accent, 2);
        text(4, 28, "[ ] channel   ; . select field");
        text(4, 42, ", / value     Shift: +/-10");
        text(4, 56, "Space: preview (250ms)");
        text(4, 70, "Backspace: all sounds off");
        text(4, 84, "P: presets    H: close help");
        text(4, 98, "Preset: [ ] choose / Enter load");
        text(4, 112, "S save / Enter yes / N cancel");
        text(4, 124, "T basic/tone   O output level", Accent);
    } else if (outputPage) {
        text(4, 4, "OUTPUT LEVEL", Accent, 2);
        snprintf(line, sizeof(line), "%d dB", engine.outputLevel());
        text(30, 34, line, TFT_WHITE, 3);
        text(4, 66, "DAC gain: -40 to 0 dB", Muted);
        text(4, 80, outputSaved && savedOutput == engine.outputLevel() ? "SAVED (separate from presets)" : "UNSAVED - S to save", Accent);
        text(4, 94, ", / adjust  Shift: +/-10");
        text(4, 108, "Enter reapply  S save  O close");
    } else if (presetPage) {
        text(4, 4, "PRESETS", Accent, 2);
        for (uint8_t i = 0; i < 8; ++i) {
            const int x = (i % 4) * 59 + 3, y = 30 + (i / 4) * 30;
            screen.drawRect(x, y, 56, 26, i == slot ? Accent : Muted);
            snprintf(line, sizeof(line), "%u%s", i + 1, activeSlot == i ? "*" : "");
            text(x + 4, y + 3, line, i == slot ? Accent : TFT_WHITE);
            const char* s = statusName(slotStatus[i]);
            text(x + 4, y + 14, slotStatus[i] == PresetStore::Status::Unavailable ? "ERROR" : s, Muted);
        }
        text(4, 94, confirmSave ? "Overwrite? Enter/Y=yes N=no" : "[ ] select  Enter load  S save", Accent);
        text(4, 106, "P close / Backspace panic", Muted);
    } else {
        const auto& ch = engine.state()[channel];
        const bool dirty = !engine.gmModeKnown() || !midi::equal(engine.state(), baseline);
        const char* mode = !engine.gmModeKnown() ? " ?" : (channel == 9 ? " DRUM" : "");
        if (activeSlot < 0) snprintf(line, sizeof(line), "CH %02u%s  INIT%s", channel + 1, mode, dirty ? "*" : "");
        else snprintf(line, sizeof(line), "CH %02u%s  P%u%s", channel + 1, mode, activeSlot + 1, dirty ? "*" : "");
        text(4, 3, line, Accent);
        text(202, 3, tonePage ? "TONE" : "BASIC", Accent);
        const bool bankKnown = (ch.known & 7) == 7;
        if (!(ch.known & (1u << midi::Program))) snprintf(line, sizeof(line), "Program: ?");
        else if (engine.gmModeKnown() && bankKnown && ch.value[midi::BankMSB] == 0 && ch.value[midi::BankLSB] == 0 && channel != 9)
            snprintf(line, sizeof(line), "%03u %s", ch.value[midi::Program] + 1, gmNames[ch.value[midi::Program]]);
        else if (bankKnown)
            snprintf(line, sizeof(line), "%s P%03u Bank %u:%u", channel == 9 && engine.gmModeKnown() ? "Drum" : "", ch.value[midi::Program] + 1, ch.value[0], ch.value[1]);
        else snprintf(line, sizeof(line), "P%03u Bank ?:?", ch.value[midi::Program] + 1);
        text(4, 19, line);
        const auto f = fields[fieldIndex];
        text(4, 40, fieldNames[fieldIndex], Muted);
        if (tonePage) text(4, 55, "64 = original", Muted);
        if (ch.known & (1u << f)) snprintf(line, sizeof(line), "%03u", ch.value[f] + (f == midi::Program ? 1 : 0));
        else snprintf(line, sizeof(line), " ? ");
        text(130, 36, line, TFT_WHITE, 3);
        for (uint8_t column = 0; column < 4; ++column) {
            const uint8_t i = (tonePage ? 5 : 1) + column;
            const auto cf = fields[i];
            if (ch.known & (1u << cf)) snprintf(line, sizeof(line), "%s:%03u", fieldShort[i], ch.value[cf]);
            else snprintf(line, sizeof(line), "%s: ? ", fieldShort[i]);
            text(4 + column * 59, 69, line, i == fieldIndex ? Accent : Muted);
        }
        for (uint8_t i = 0; i < 16; ++i) {
            const bool active = (engine.activitySeen() & (1u << i)) && now - engine.activity()[i] < 180;
            screen.fillRect(4 + i * 14, 88, 12, 10, active ? TFT_GREEN : 0x2104);
            if (i == channel) screen.drawRect(4 + i * 14, 87, 12, 12, Accent);
        }
        text(4, 102, "T tone  O output  P preset  H help", Muted);
    }
    if (!helpPage) {
        if (static_cast<int32_t>(notificationUntil - now) > 0) text(4, 123, notification, TFT_YELLOW);
        else if (engine.errors()) {
            snprintf(line, sizeof(line), "MIDI ERR %lu%s", static_cast<unsigned long>(engine.errors()), engine.recovering() ? " recovering" : (engine.gmModeKnown() ? " (history)" : " - reload preset"));
            text(4, 123, line, TFT_ORANGE);
        } else if (loadPending) text(4, 123, "Load pending; waiting for MIDI idle", TFT_YELLOW);
        else if (engine.inSysEx()) text(4, 123, "SysEx: local changes queued", TFT_YELLOW);
        else if (engine.pendingLocal()) text(4, 123, "Sending settings...", Muted);
        else text(4, 123, "UART 31250 / tracked state", Muted);
    }
    screen.pushSprite(0, 0);
}
} // namespace

void setup() {
    Serial.begin(115200);
    auto cfg = M5.config(); cfg.internal_spk = false; cfg.internal_mic = false;
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(100);
    screen.setColorDepth(16); frameReady = screen.createSprite(240, 135) != nullptr;
    screen.setTextWrap(false);
    if (!frameReady) { M5Cardputer.Display.println("Framebuffer allocation failed"); }
    // No TX ring: availableForWrite reports actual FIFO space, so single-byte
    // writes cannot block waiting for a ring-buffer item allocation.
    Serial1.setRxBufferSize(2048); Serial1.setTxBufferSize(0);
    Serial1.begin(31250, SERIAL_8N1, MidiRx, MidiTx);
    Serial1.onReceiveError([](hardwareSerial_error_t) { uartError.store(true); });
    const bool storageReady = presets.begin(); refreshSlots();
    const uint8_t last = presets.last();
    midi::Snapshot initial = midi::defaults();
    if (last < 8 && presets.load(last, initial) == PresetStore::Status::Valid) {
        activeSlot = last; slot = last; notice("Restoring saved preset");
    } else if (!storageReady) notice("Storage unavailable: defaults");
    else if (last < 8) notice("Invalid boot preset: defaults");
    else notice("Using initial settings");
    baseline = initial;
    outputStore.begin();
    outputSaved = outputStore.load(savedOutput);
    if (!outputSaved) notice("Output default -40dB; S to save");
    // Let the externally powered synth start before initializing it.
    delay(300);
    engine.restore(initial);
    engine.setOutputLevel(savedOutput);
    // Apply gain before accepting any external notes at startup.
    while (engine.busy()) {
        engine.tick(millis());
        uint8_t b;
        while (Serial1.availableForWrite() > 0 && engine.popTx(b)) Serial1.write(b);
        delay(1);
    }
    Serial1.flush();
}
void loop() {
    serviceMidi();
    if (!engine.pendingLocal()) for (auto& ch : hasQueuedValue) ch.fill(false);
    const uint32_t now = millis();
    keyboard(now);
    serviceMidi();
    if (loadPending && hardwareIdle() && engine.restore(loadTarget)) {
        loadPending = false; baseline = loadTarget;
        if (loadSlotValid) {
            activeSlot = slot;
            notice(presets.select(slot) ? "Preset loaded" : "Loaded; boot selection failed");
        } else { activeSlot = -1; notice("Defaults loaded; S to save"); }
    }
    serviceMidi();
    if (now - lastFrame >= 50) { lastFrame = now; draw(now); }
    // Yield to the ESP32 system tasks without a blocking MIDI delay.
    delay(1);
}
