#include "MidiCore.h"
#include "Preset.h"
#include "OutputLevel.h"
#include <cstdlib>
#include <iostream>
#include <vector>
#include <initializer_list>

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ << " failed: " #condition "\n"; std::exit(1); } } while (false)
using namespace midi;
using Bytes = std::vector<uint8_t>;
Bytes drain(Engine& e) {
    Bytes result; uint8_t b;
    while (e.popTx(b)) result.push_back(b);
    return result;
}
void feed(Engine& e, std::initializer_list<uint8_t> bytes, uint32_t time = 1000) {
    for (auto b : bytes) e.receive(b, time);
}
void initialize(Engine& e) {
    CHECK(e.restore(defaults())); e.tick(0); drain(e); e.tick(201); drain(e);
    CHECK(fullyKnown(e.state())); CHECK(equal(e.state(), defaults()));
    CHECK(e.gmModeKnown());
}
void recover(Engine& e, uint32_t time) {
    drain(e); e.tick(time); drain(e); e.tick(time + 1);
    CHECK(!e.recovering());
}
void channelState() {
    Engine e; initialize(e);
    for (uint8_t c = 0; c < 16; ++c) {
        feed(e, {uint8_t(0xB0 | c), 7, uint8_t(c + 30), 10, uint8_t(c + 40), 91, 50, 93, 60});
        feed(e, {uint8_t(0xC0 | c), c});
        CHECK(e.state()[c].value[Program] == c);
        CHECK(e.state()[c].value[Volume] == c + 30);
        CHECK(e.state()[c].value[Pan] == c + 40);
        CHECK(e.state()[c].value[Reverb] == 50);
        CHECK(e.state()[c].value[Chorus] == 60);
        CHECK(e.activitySeen() & (1u << c));
    }
    CHECK(e.activitySeen() == 0xFFFF);
    feed(e, {0xB2, 0, 8, 32, 1});
    CHECK(e.state()[2].value[BankMSB] == 0); // Bank is committed at Program Change.
    feed(e, {0xC2, 10});
    CHECK(e.state()[2].value[BankMSB] == 8 && e.state()[2].value[BankLSB] == 1);
    CHECK(e.edit(2, Program, 7)); e.tick(1000);
    CHECK(e.state()[2].value[BankMSB] == 0 && e.state()[2].value[Program] == 7);
    feed(e, {0xC2, 9}); CHECK(e.state()[2].value[Program] == 9);
    feed(e, {0xB2, 121, 0});
    CHECK(!(e.state()[2].known & (1u << Volume)));
    CHECK(e.state()[2].known & (1u << Program));
}
void runningAndRealtime() {
    Engine e;
    feed(e, {0x90, 60, 100});
    CHECK(drain(e) == Bytes({0x90, 60, 100}));
    CHECK(e.edit(1, Volume, 80)); e.tick(1000);
    CHECK(drain(e) == Bytes({0xB1, 7, 80}));
    feed(e, {61, 0xF8, 110});
    CHECK(drain(e) == Bytes({0xF8, 0x90, 61, 110}));
    feed(e, {62, 0}); CHECK(drain(e) == Bytes({62, 0}));
    feed(e, {0xA0, 60, 70, 0xD0, 40, 41, 0xE0, 0, 64});
    CHECK(drain(e) == Bytes({0xA0, 60, 70, 0xD0, 40, 41, 0xE0, 0, 64}));
    feed(e, {0xF2, 1, 0xFA, 2, 0xF1, 3, 0xF3, 4, 0xF6});
    CHECK(drain(e) == Bytes({0xFA, 0xF2, 1, 2, 0xF1, 3, 0xF3, 4, 0xF6}));
    feed(e, {0x90, 64});
    CHECK(e.edit(0, Volume, 55)); e.tick(1000); CHECK(drain(e).empty());
    feed(e, {100}); CHECK(drain(e) == Bytes({0x90, 64, 100, 0xB0, 7, 55}));
    feed(e, {65, 110}); CHECK(drain(e) == Bytes({0x90, 65, 110}));
}
void sysexAndFaults() {
    Engine e; initialize(e);
    feed(e, {0xF0, 0x41, 0x00});
    CHECK(!fullyKnown(e.state())); CHECK(e.inSysEx());
    CHECK(!e.gmModeKnown());
    CHECK(e.edit(0, Volume, 70)); e.tick(1000);
    CHECK(drain(e) == Bytes({0xF0, 0x41, 0x00}));
    feed(e, {0xF8, 0x12, 0xF7});
    CHECK(drain(e) == Bytes({0xF8, 0x12, 0xF7, 0xB0, 7, 70}));
    CHECK(e.state()[0].known == (1u << Volume));
    feed(e, {0xFF}); CHECK(!fullyKnown(e.state())); drain(e);
    feed(e, {0xF0, 1}, 1100); e.tick(1601);
    CHECK(e.recovering()); CHECK(e.errors() == 1);
    drain(e); e.tick(1602); auto panic = drain(e);
    CHECK(panic.front() == 0xF7); CHECK(panic.size() == 1 + 16 * 7);
    e.tick(1603); CHECK(!e.recovering());
    feed(e, {0x90, 60}, 1700); e.tick(1801); CHECK(e.recovering()); recover(e, 1802);
    feed(e, {0x90, 60, 100}, 1900); CHECK(drain(e) == Bytes({0x90, 60, 100}));
    feed(e, {0xF0, 1, 0x90}, 1900); CHECK(e.recovering()); recover(e, 1901);
    feed(e, {0xFE}, 2000); drain(e); e.tick(2331); CHECK(e.recovering()); recover(e, 2332);
    feed(e, {0x90, 60}, 0xFFFFFFF0u); e.tick(90); CHECK(e.recovering());
}
void overflowAndLongSysEx() {
    Engine e;
    feed(e, {0xF0}, 0);
    for (unsigned i = 0; i < 20000; ++i) {
        e.receive(uint8_t(i & 127), i); CHECK(drain(e).size() >= 1);
    }
    feed(e, {0xF7}, 20000); drain(e); CHECK(e.errors() == 0);
    for (size_t i = 0; i <= Engine::TxCapacity; ++i) e.receive(0xF8, 20000);
    CHECK(e.recovering()); CHECK(e.txPending() <= Engine::TxCapacity);
    recover(e, 21000);
    for (unsigned i = 0; i < 512; ++i) CHECK(e.edit(0, Volume, 50));
    CHECK(!e.edit(0, Volume, 60));
    e.panic(); e.tick(22000); CHECK(!e.pendingLocal()); drain(e);
    feed(e, {0x90, 60}, 23000); e.panic(); e.tick(23000);
    CHECK(!e.recovering()); CHECK(e.pendingLocal());
    feed(e, {100}, 23001); CHECK(!e.pendingLocal()); drain(e);
    const auto errors = e.errors();
    feed(e, {0xF0, 1}, 24000); e.panic();
    CHECK(e.recovering()); CHECK(e.errors() == errors); recover(e, 24001);
}
void presetsAndPreview() {
    Engine e; Snapshot s = defaults();
    for (uint8_t c = 0; c < 16; ++c) s[c].value[Program] = c;
    CHECK(e.restore(s)); e.tick(0);
    auto first = drain(e);
    CHECK(first.size() == 16 * 7 + 6); // Stop before settings, allowing GM reset to settle.
    CHECK(first.back() == 0xF7);
    e.tick(199); CHECK(drain(e).empty());
    e.tick(200); CHECK(!drain(e).empty()); CHECK(equal(s, e.state()));
    CHECK(e.preview(9)); CHECK(!e.preview(0)); e.tick(500);
    CHECK(drain(e) == Bytes({0x99, 36, 80}));
    e.tick(800); CHECK(drain(e) == Bytes({0x89, 36, 0}));
    CHECK(e.preview(0)); e.tick(900); drain(e); e.panic(); e.tick(901); drain(e);
    e.tick(1300); CHECK(drain(e).empty());
    PresetBytes encoded;
    CHECK(encodePreset(s, encoded)); Snapshot decoded;
    CHECK(decodePreset(encoded.data(), encoded.size(), decoded)); CHECK(equal(s, decoded));
    CHECK(!decodePreset(encoded.data(), encoded.size() - 1, decoded)); CHECK(equal(decoded, defaults()));
    CHECK(!decodePreset(nullptr, 0, decoded));
    for (size_t i = 0; i < encoded.size(); ++i) {
        auto broken = encoded; broken[i] ^= 1;
        CHECK(!decodePreset(broken.data(), broken.size(), decoded)); CHECK(equal(decoded, defaults()));
    }
    s[0].known = 0; CHECK(!encodePreset(s, encoded)); CHECK(!e.restore(s));
    s = defaults(); s[1].value[Pan] = 255; CHECK(!encodePreset(s, encoded)); CHECK(!e.restore(s));
    CHECK(!e.edit(16, Volume, 50)); CHECK(!e.edit(0, Volume, 128));
}
// Independent wire decoder for checking transaction ordering and selection.
std::vector<Bytes> messages(const Bytes& bytes) {
    std::vector<Bytes> result; Bytes m; uint8_t running = 0; size_t need = 0;
    for (auto b : bytes) {
        if (b >= 0xF8) continue;
        if (b & 0x80) {
            CHECK(m.empty()); CHECK(b < 0xF0);
            running = b; need = ((b & 0xF0) == 0xC0 || (b & 0xF0) == 0xD0) ? 2 : 3;
            m.push_back(b);
        } else {
            if (m.empty()) { CHECK(running != 0); m.push_back(running); }
            m.push_back(b);
        }
        if (m.size() == need) { result.push_back(m); m.clear(); }
    }
    CHECK(m.empty()); return result;
}
void toneParameters() {
    Engine e;
    CHECK(e.edit(0, Cutoff, 80)); e.tick(0);
    CHECK(drain(e) == Bytes({0xB0,99,1,98,0x20,6,80,99,127,98,127,101,127,100,127}));
    CHECK(e.state()[0].value[Cutoff] == 80);
    CHECK(e.state()[0].known == (1u << Cutoff));
    CHECK(!(e.state()[1].known & (1u << Cutoff)));
    // Both inactive and active selectors must survive a local edit.
    feed(e, {0xB0,99,1,98,0x66,101,0,100,0}); drain(e);
    CHECK(e.edit(0, Resonance, 90)); e.tick(1000);
    auto tx = drain(e);
    CHECK(tx == Bytes({99,1,98,0x21,6,90,99,1,98,0x66,101,0,100,0}));
    feed(e, {6,12}); drain(e); // RPN pitch range, not resonance/release.
    CHECK(e.state()[0].value[Resonance] == 90);
    CHECK(!(e.state()[0].known & (1u << Release)));
    feed(e, {99,1,6,75}); drain(e); // Reuse inactive NRPN LSB.
    CHECK(e.state()[0].value[Release] == 75);
    feed(e, {99,127,98,127,6,99}); drain(e);
    CHECK(e.state()[0].value[Release] == 75);

    Engine partial;
    feed(partial, {0xB2,99,1}); drain(partial);
    CHECK(partial.edit(2, Attack, 10)); partial.tick(1000);
    CHECK(drain(partial) == Bytes({99,1,98,0x63,6,10,101,127,100,127,99,1,98,127}));
    feed(partial, {6,30}); drain(partial); // LSB still unknown: do not guess.
    CHECK(!(partial.state()[2].known & (1u << Cutoff)));
    feed(partial, {98,0x20,6,45}); drain(partial);
    CHECK(partial.state()[2].value[Cutoff] == 45);
    CHECK(partial.state()[2].value[Attack] == 10);
    feed(partial, {38,1}); drain(partial);
    CHECK(!(partial.state()[2].known & (1u << Cutoff)));

    Engine all; initialize(all);
    const uint8_t ids[] = {0x20,0x21,0x63,0x66};
    for (uint8_t ch = 0; ch < 16; ++ch) {
        for (uint8_t i = 0; i < 4; ++i) {
            feed(all, {uint8_t(0xB0|ch),99,1,98,ids[i],6,uint8_t(ch+i)}); drain(all);
            CHECK(all.state()[ch].value[Cutoff+i] == ch+i);
        }
        feed(all, {uint8_t(0xC0|ch), 20});
        auto sent = messages(drain(all));
        CHECK(sent.front() == Bytes({uint8_t(0xC0|ch),20}));
        CHECK(sent.size() == 29); // Program + four atomic 7-CC transactions.
        for (uint8_t i = 0; i < 4; ++i)
            CHECK(sent[3+i*7] == Bytes({uint8_t(0xB0|ch),6,uint8_t(ch+i)}));
    }
    CHECK(all.edit(1, Program, 30)); all.tick(1000);
    auto local = messages(drain(all)); CHECK(local.size() == 31);
    CHECK(local[2] == Bytes({0xC1,30}));
    CHECK(all.state()[1].value[Attack] == 3);

    Engine sx; feed(sx, {0xF0,1});
    CHECK(sx.edit(3, Release, 120)); sx.tick(1000);
    CHECK(drain(sx) == Bytes({0xF0,1}));
    feed(sx, {0xF8,2,0xF7}); auto out = drain(sx);
    CHECK(out[0] == 0xF8 && out[1] == 2 && out[2] == 0xF7 && out[3] == 0xB3);
    CHECK(sx.state()[3].value[Release] == 120);

    Engine full;
    for (size_t i = 0; i < Engine::TxCapacity - 10; ++i) full.receive(0xF8, 0);
    CHECK(full.edit(0, Cutoff, 100)); full.tick(0);
    CHECK(full.pendingLocal()); CHECK(!(full.state()[0].known & (1u << Cutoff)));
    CHECK(drain(full).size() == Engine::TxCapacity - 10);
    full.tick(1); CHECK(!full.pendingLocal());
    CHECK(messages(drain(full)).size() == 7);
    for (unsigned i = 0; i < 511; ++i) CHECK(full.edit(0, Volume, 1));
    CHECK(!full.edit(0, Program, 2)); // No partially queued bank/program edit.
    CHECK(full.edit(0, Attack, 5)); CHECK(!full.edit(0, Release, 6));
    full.tick(2); drain(full);
    CHECK(!(full.state()[0].known & (1u << Program)));
    CHECK(full.state()[0].value[Attack] == 5);
}
void presetCompatibility() {
    // Frozen v1 fixture: programs 0..15; CRC generated independently with zlib.
    const char* hex = "434d4944011007000000006440280000000164402800000002644028000000036440280000000464402800000005644028000000066440280000000764402800000008644028000000096440280000000a6440280000000b6440280000000c6440280000000d6440280000000e6440280000000f6440280008252b0c";
    Bytes fixture;
    for (size_t i = 0; hex[i]; i += 2) { char pair[] = {hex[i],hex[i+1],0}; fixture.push_back(static_cast<uint8_t>(std::strtoul(pair,nullptr,16))); }
    CHECK(fixture.size() == LegacyPresetSize);
    Snapshot state; CHECK(decodePreset(fixture.data(), fixture.size(), state));
    for (uint8_t ch = 0; ch < 16; ++ch) {
        CHECK(state[ch].value[Program] == ch);
        CHECK(state[ch].value[Volume] == 100);
        for (uint8_t f = Cutoff; f < FieldCount; ++f) CHECK(state[ch].value[f] == 64);
    }
    for (size_t i = 0; i < fixture.size(); ++i) {
        auto bad = fixture; bad[i] ^= 1;
        Snapshot fallback; CHECK(!decodePreset(bad.data(),bad.size(),fallback)); CHECK(equal(fallback,defaults()));
    }
    for (uint8_t ch = 0; ch < 16; ++ch)
        for (uint8_t f = Cutoff; f < FieldCount; ++f) state[ch].value[f] = (ch * 7 + f) % 128;
    PresetBytes bytes; CHECK(encodePreset(state,bytes)); CHECK(bytes[4] == 2);
    Snapshot roundtrip; CHECK(decodePreset(bytes.data(),bytes.size(),roundtrip)); CHECK(equal(state,roundtrip));
    Engine restored; CHECK(restored.restore(roundtrip)); restored.tick(0); drain(restored); restored.tick(201); drain(restored);
    CHECK(equal(restored.state(), state)); CHECK(!restored.pendingLocal());
    state[0].value[Release] = 128; CHECK(!encodePreset(state,bytes)); CHECK(!restored.restore(state));
    state = defaults(); state[0].known &= ~(1u << Release); CHECK(!encodePreset(state,bytes));
    CHECK(encodePreset(defaults(),bytes));
    bytes[18] = 128; // Invalid release with independently recomputed valid CRC.
    const uint32_t badValueCrc = 0x207352F6;
    for (uint8_t i = 0; i < 4; ++i) bytes[bytes.size()-4+i] = static_cast<uint8_t>(badValueCrc >> (8*i));
    CHECK(!decodePreset(bytes.data(),bytes.size(),roundtrip)); CHECK(equal(roundtrip,defaults()));
}
void outputLevel() {
    const Bytes low{0xF0,0,0x20,0,0,0,0x12,0x33,0x77,0x12,1,11,5,1,0,0xF7};
    auto high = low; high[12] = 7; high[13] = 9;
    Engine e; initialize(e);
    const auto before = e.state();
    CHECK(!e.setOutputLevel(1)); CHECK(!e.setOutputLevel(-41));
    CHECK(e.setOutputLevel(-40)); e.tick(300); CHECK(drain(e) == low);
    CHECK(equal(e.state(), before)); CHECK(e.gmModeKnown());
    CHECK(e.setOutputLevel(0)); e.tick(301); CHECK(drain(e) == high);
    CHECK(e.setOutputLevel(-40));
    feed(e,{0x90,60},302); e.tick(302); CHECK(drain(e).empty());
    feed(e,{100},303); auto bytes = drain(e);
    CHECK(bytes.size() == 19 && bytes[0] == 0x90 && bytes[2] == 100);
    CHECK(Bytes(bytes.begin()+3,bytes.end()) == low);
    feed(e,{61,100},304); CHECK(drain(e) == Bytes({0x90,61,100}));
    // Changes remain outside a streamed SysEx, including realtime interruptions.
    feed(e,{0xF0,0x7D,1},305); CHECK(e.setOutputLevel(0));
    e.tick(305); CHECK(drain(e) == Bytes({0xF0,0x7D,1}));
    feed(e,{0xF8,2,0xF7},306); bytes = drain(e);
    CHECK(Bytes(bytes.begin(),bytes.begin()+3) == Bytes({0xF8,2,0xF7}));
    CHECK(Bytes(bytes.begin()+3,bytes.end()) == high);
    // Two reset boundaries retain independent delays; notes follow gain.
    feed(e,{0xFF,0xFF,0x90,60,100},400); e.tick(400);
    CHECK(drain(e) == Bytes({0xFF})); e.tick(599); CHECK(drain(e).empty());
    e.tick(600); bytes = drain(e); CHECK(bytes.size() == 17 && bytes.back() == 0xFF);
    CHECK(Bytes(bytes.begin(),bytes.end()-1) == high);
    e.tick(799); CHECK(drain(e).empty()); e.tick(800); bytes = drain(e);
    CHECK(Bytes(bytes.begin(),bytes.begin()+16) == high);
    CHECK(Bytes(bytes.begin()+16,bytes.end()) == Bytes({0x90,60,100}));
    for (const Bytes& reset : {Bytes{0xF0,0x7E,0x7F,9,1,0xF7},
        Bytes{0xF0,0x41,0x10,0x42,0x12,0x40,0,0x7F,0,0x41,0xF7},
        Bytes{0xF0,0x43,0x10,0x4C,0,0,0x7E,0,0xF7}}) {
        Engine r; CHECK(r.setOutputLevel(-40)); r.tick(0); drain(r);
        for (auto b : reset) r.receive(b,10);
        r.tick(10); CHECK(drain(r) == reset);
        r.tick(209); CHECK(drain(r).empty()); r.tick(210); CHECK(drain(r) == low);
    }
    Engine n; CHECK(n.setOutputLevel(-40)); n.tick(0); drain(n);
    feed(n,{0xB0,99,0x37,98,0x5F,6,0x45},10); n.tick(10);
    CHECK(drain(n) == Bytes({0xB0,99,0x37,98,0x5F,6,0x45}));
    n.tick(210); CHECK(drain(n) == low);
    // FF inside SysEx must not inject the codec message before F7.
    feed(n,{0xF0,0x7D,0xFF,1,0xF7},220); n.tick(220);
    CHECK(drain(n) == Bytes({0xF0,0x7D,0xFF,1,0xF7}));
    n.tick(420); CHECK(drain(n) == low);
    // Startup/preset restore applies gain again after GM reset without changing snapshots.
    Engine boot; CHECK(boot.restore(defaults())); CHECK(boot.setOutputLevel(-40));
    boot.tick(0); bytes = drain(boot);
    CHECK(Bytes(bytes.begin(),bytes.begin()+16) == low);
    CHECK(bytes.back() == 0xF7); boot.tick(200); bytes = drain(boot);
    CHECK(Bytes(bytes.begin(),bytes.begin()+16) == low);
    CHECK(equal(boot.state(), defaults()));
    // Full queue: the gain transaction is deferred whole, never truncated.
    Engine full; for (size_t i=0;i<Engine::TxCapacity;++i) full.receive(0xF8,0);
    CHECK(full.setOutputLevel(-40)); full.tick(0); CHECK(drain(full).size() == Engine::TxCapacity);
    full.tick(1); CHECK(drain(full) == low); CHECK(full.errors() == 0);
    for (int db=-40;db<=0;++db) {
        const auto record = encodeOutput(db); int restored = 99;
        CHECK(decodeOutput(record.data(),record.size(),restored)); CHECK(restored == db);
        for (size_t j=0;j<record.size();++j) {
            auto bad=record; bad[j]^=1;
            CHECK(!decodeOutput(bad.data(),bad.size(),restored)); CHECK(restored == -40);
        }
    }
    auto bad=encodeOutput(0); int restored;
    bad[4]=41; bad[5]=41^255; CHECK(!decodeOutput(bad.data(),bad.size(),restored)); CHECK(restored == -40);
    CHECK(!decodeOutput(bad.data(),7,restored));
}
int main() {
    channelState(); runningAndRealtime(); sysexAndFaults(); overflowAndLongSysEx(); presetsAndPreview();
    toneParameters(); presetCompatibility(); outputLevel();
    std::cout << "PASS: " << checks << " checks across 8 MIDI/preset/output scenarios\n";
}

