#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace midi {
enum Field : uint8_t { BankMSB, BankLSB, Program, Volume, Pan, Reverb, Chorus, Cutoff, Resonance, Attack, Release, FieldCount };
constexpr uint16_t AllKnown = (1u << FieldCount) - 1;
struct Channel {
    std::array<uint8_t, FieldCount> value{{0, 0, 0, 100, 64, 40, 0, 64, 64, 64, 64}};
    uint16_t known = AllKnown;
};
using Snapshot = std::array<Channel, 16>;
Snapshot defaults();
bool fullyKnown(const Snapshot& state);
bool equal(const Snapshot& a, const Snapshot& b);

struct Message {
    std::array<uint8_t, 8> data{};
    uint8_t size = 0;
    bool preview = false;
    uint8_t toneField = 255; // Atomic NRPN operation, expanded only when emitted.
};

// Platform-independent MIDI router. No heap allocation or hardware calls.
class Engine {
public:
    static constexpr size_t TxCapacity = 4096;
    Engine();
    void receive(uint8_t byte, uint32_t now);
    void tick(uint32_t now);
    bool popTx(uint8_t& byte);
    size_t txPending() const { return txCount_; }
    bool busy() const { return outputPending_ || localCount_ || txCount_ || recovering_ || sysex_ || needed_; }
    bool pendingLocal() const { return localCount_ != 0; }
    bool inSysEx() const { return sysex_; }
    bool recovering() const { return recovering_; }
    bool gmModeKnown() const { return gmModeKnown_; }
    void fault();
    bool setOutputLevel(int db);
    int outputLevel() const { return outputDb_; }
    bool edit(uint8_t channel, Field field, uint8_t value);
    bool restore(const Snapshot& state);
    bool preview(uint8_t channel);
    void panic();
    const Snapshot& state() const { return state_; }
    const std::array<uint32_t, 16>& activity() const { return activity_; }
    uint16_t activitySeen() const { return activitySeen_; }
    uint32_t errors() const { return errors_; }
    uint32_t revision() const { return revision_; }

private:
    Snapshot state_{};
    std::array<std::array<uint8_t, 2>, 16> pendingBank_{};
    std::array<uint8_t, 16> bankKnown_{};
    std::array<uint32_t, 16> activity_{};
    uint16_t activitySeen_ = 0;
    std::array<uint8_t, TxCapacity> tx_{};
    std::array<bool, TxCapacity> pauseAfter_{};
    int outputDb_ = -40;
    bool outputEnabled_ = false, outputPending_ = false;
    std::array<uint8_t, 16> sysexPrefix_{};
    size_t sysexLength_ = 0;
    bool resetInSysEx_ = false;
    bool emitOutput();
    void resetTail();
    bool receivedReset() const;
    size_t txRead_ = 0, txCount_ = 0;
    struct Selection {
        std::array<uint8_t, 4> values{{127,127,127,127}}; // NRPN MSB/LSB, RPN MSB/LSB
        uint8_t known = 0;
        uint8_t active = 2; // 0=NRPN, 2=RPN
    };
    std::array<Selection, 16> selection_{};
    std::array<Message, 512> local_{};
    size_t localRead_ = 0, localCount_ = 0;
    uint8_t running_ = 0, status_ = 0, needed_ = 0, received_ = 0, outRunning_ = 0;
    std::array<uint8_t, 2> data_{};
    bool sysex_ = false, recovering_ = false, recoverySent_ = false;
    bool gmModeKnown_ = false;
    bool sensing_ = false, previewPending_ = false, previewActive_ = false;
    uint8_t previewChannel_ = 0, previewNote_ = 60;
    uint32_t lastData_ = 0, lastByte_ = 0, previewAt_ = 0, errors_ = 0, revision_ = 0;
    uint32_t now_ = 0, holdUntil_ = 0;
    bool holding_ = false;
    bool put(uint8_t byte);
    bool emit(const Message& message);
    bool queue(const Message& message);
    void apply(const Message& message);
    void invalidate();
    void resetParser();
    void pump(uint32_t now);
    void emitPanic();
};
Message cc(uint8_t channel, uint8_t controller, uint8_t value);
Message shortMessage(uint8_t status, uint8_t a = 0, uint8_t b = 0);
} // namespace midi
