#pragma once
// What the probe records about MPC: every dispatcher call, the process calls (block sizes, in
// place or not, gaps), the transport, the host's own answers, every parameter write and every MIDI
// event. Writers (the audio thread, MPC's UI side) only touch atomics and lock-free rings
// (plugin/ring.h); a monitor thread per instance reads them every 100 ms and writes what changed to
// the log (plugin/trace.h), so nothing on the audio thread does file I/O.
//
// No 64-bit atomics: they aren't lock-free everywhere on 32-bit ARM. Times are int32 ms.
#include "parameters.h"
#include "ring.h"
#include "vst2.h"

#include <atomic>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace ef {

int32_t nowMs();       // steady clock, ms since the plugin was loaded
int32_t threadId();    // the kernel's thread id

// The clock the write log stamps parameter writes and MIDI with (the gaps it prints are differences
// of these): nowMs(), unless a test has taken it over to make a gap exact. MPC's run never does.
int32_t stampMs();
void useManualStampClock(int32_t startMs);   // stampMs() stands still at startMs until advanced; < 0: nowMs() again
void advanceStampClock(int32_t ms);

constexpr int kOps = 96;        // dispatcher opcodes counted one by one; higher ones together
constexpr int kCanDoMax = 24;   // distinct canDo strings MPC asks the plugin
constexpr int kAudioTids = 8;   // MPC's audio worker threads seen in process

// The host's canDo strings the probe asks MPC about once.
constexpr const char* kHostCanDo[] = {
    "sendVstEvents", "sendVstMidiEvent", "sendVstTimeInfo", "receiveVstEvents", "receiveVstMidiEvent",
    "reportConnectionChanges", "acceptIOChanges", "sizeWindow", "offline", "openFileSelector",
    "startStopProcess", "shellCategory", "sendVstMidiEventFlagIsRealtime", "supplyIdle",
};
constexpr int kHostCanDos = static_cast<int>(sizeof kHostCanDo / sizeof kHostCanDo[0]);

// Asked once by the audio thread on its first block (host callbacks only from process), then
// published with ProbeState::hostReady.
struct HostCensus {
    int32_t version = 0, sampleRate = 0, blockSize = 0, processLevel = 0, vendorVersion = 0;
    char vendor[64] = {}, product[64] = {};
    int32_t canDo[kHostCanDos] = {};
};

// Where MPC's transport was when something happened: the latest block's (host callbacks are made
// from process only), so a write on the UI thread is placed to within a block or two.
struct Where {
    int32_t playing = 0;
    float beats = 0.0f;
};

// One parameter write, as MPC made it (any thread); the monitor logs it.
struct WriteEntry {
    int32_t id = 0;
    float value = 0.0f;       // as MPC sent it, 0..1
    int32_t atMs = 0;         // stampMs()
    int32_t audio = 0;        // written on one of the audio threads (automation playback would be)
    Where where;
};

// One MIDI event delivered by effProcessEvents.
struct MidiEntry {
    uint8_t bytes[3] = {};
    int32_t deltaFrames = 0;
    int32_t audio = 0;
    Where where;
};

struct ProbeState {
    // processReplacing (and the legacy process)
    std::atomic<uint32_t> calls{0}, legacyCalls{0};
    std::atomic<int32_t> lastCallMs{0}, frames{0}, minFrames{INT32_MAX}, maxFrames{0};
    std::atomic<int32_t> inPlace{-1};       // -1 not seen yet, 0 separate buffers, 1 in == out
    std::atomic<int32_t> nullInputs{0};     // blocks without input buffers
    std::atomic<int32_t> stereoSeen{0};     // an input block whose channels differed
    std::atomic<int32_t> lastAudibleMs{-1}; // input above -80 dBFS
    std::atomic<int32_t> audioTid[kAudioTids]{};
    std::atomic<int32_t> nAudioTids{0};

    // the transport, from the latest block
    std::atomic<int32_t> timeFlags{INT32_MIN};   // INT32_MIN: not asked yet; -1: getTime returned null
    std::atomic<float> tempo{0.0f}, ppq{0.0f};
    std::atomic<int32_t> sigNum{0}, sigDen{0};

    // the dispatcher: count, last index and value, which thread called first
    std::atomic<uint32_t> opCount[kOps]{};
    std::atomic<int32_t> opIdx[kOps]{}, opFirstTid[kOps]{};
    std::atomic<intptr_t> opVal[kOps]{};
    std::atomic<uint32_t> opOther{0};
    std::atomic<int32_t> opOtherLast{0};
    std::atomic<int32_t> spkIn{-1}, spkOut{-1};   // effSetSpeakerArrangement's channel counts

    // canDo strings MPC asked us
    char canDo[kCanDoMax][48] = {};
    std::atomic<int32_t> canDoAnswer[kCanDoMax]{};
    std::atomic<int32_t> canDoReady[kCanDoMax]{};
    std::atomic<int32_t> nCanDo{0};

    // setParameter: how many; each write goes to the ring for the log
    std::atomic<uint32_t> paramSets{0};
    Ring<WriteEntry, 256> writes;

    // effProcessEvents: the MIDI events counted, the last one's bytes (b0 << 16 | b1 << 8 | b2), and a
    // ring of them for the log
    std::atomic<uint32_t> midiCount{0}, midiLast{0};
    Ring<MidiEntry, 256> midi;

    HostCensus host;
    std::atomic<int32_t> hostReady{0};

    // the meter, published every 0.5 s by the audio thread
    std::atomic<float> inDb{-200.0f}, outDb{-200.0f}, cpuPct{0.0f}, callsPerSec{0.0f};

    void noteOp(int32_t op, int32_t idx, intptr_t val);
    void noteCanDo(const char* s, int32_t answer);
    void noteAudioThread();
    bool isAudioTid(int32_t tid) const;
    Where where() const;                          // from the latest block: stopped until there is one
    void noteWrite(int id, float value);          // MPC set a parameter (any thread); a readout or a bad index is skipped
    void noteMidi(const VstEvents* events);       // MPC delivered events (effProcessEvents); only MIDI ones count
};

// Writes what changed in a ProbeState to the log every 100 ms, from its own thread.
class Monitor {
public:
    Monitor(int instance, ProbeState& st, const Params& params, int latency);   // st: it empties the rings
    ~Monitor();   // logs the instance's end, stops and joins the thread
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;

private:
    void run();
    void tick();

    const int instance_;
    ProbeState& st_;
    const Params& params_;
    const int32_t createdMs_;

    std::mutex mtx_;
    std::condition_variable cv_;
    bool stop_ = false;

    // the monitor thread's own view of what it has logged
    uint32_t calls_ = 0;
    bool running_ = false, everRan_ = false, silentLogged_ = false;
    int32_t silentMark_ = 0;      // seconds of silence already reported (1, 10, 60)
    int32_t startedMs_ = 0, stoppedMs_ = 0, beatMs_ = 0;   // first call seen, last stop, last heartbeat
    int32_t flags_ = INT32_MIN, sigNum_ = 0, sigDen_ = 0;
    float tempo_ = 0.0f;
    int32_t minFrames_ = INT32_MAX, maxFrames_ = 0, inPlace_ = -1, stereo_ = 0, nullInputs_ = 0, nTids_ = 0;
    uint32_t opCount_[kOps] = {}, opCountAtBeat_[kOps] = {}, opOther_ = 0, legacy_ = 0;
    int32_t spkIn_ = -1, spkOut_ = -1, nCanDo_ = 0;
    bool hostLogged_ = false;
    float logged_[P_COUNT];                  // the value last logged for a parameter that is logged on change
    int32_t wroteMs_[P_COUNT];               // when a button or tile was last written (-1: not yet)
    uint32_t writesDropped_ = 0, midiDropped_ = 0;

    std::thread thread_;   // last: starts once everything above is set
};

} // namespace ef
