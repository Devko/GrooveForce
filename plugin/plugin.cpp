// GrooveForce as a VST2 insert effect for MPC OS (Force / MPC standalone).
//
// The groove engine (plugin/engine.h) behind the touchscreen pages generated from surface/surface.py;
// plugin/surface.* decides what every parameter does. The status line carries a CPU meter, so the
// cost can be read on the device, and how many of the four parts (Shape, Pump, Echo, Time) are running. What MPC
// does with an insert effect was measured first (EffectForce's docs/PROBE.md): it calls
// processReplacing nonstop in place with 128 frames, suspends and resumes on transport Stop (~100 ms)
// and for its slot's ON button (as long as it is off), and never calls effSetBypass. A short suspend
// keeps every state (a Pump mid-duck goes on from there, a Move goes on playing); only one over kLongSuspendMs clears
// them. MIDI into an insert is not asked for until GrooveForce's probe says MPC sends it (docs/PROBE.md), so canDo says no.
//
// Threads: processReplacing runs on one of MPC's audio workers (which one changes between calls,
// instances run concurrently); parameters, display text and chunks come from MPC's UI side. Host
// callbacks are only made from processReplacing. What the UI side wants of the audio side goes through the surface: the
// sound's parameters as snapshots, and the Moves' calls (a tile, GO, the Move parameter, a load) as a queue that runBlock
// hands to the engine before each block. The memory the Echo and Time need is made on the UI side (provide()) as the switch
// is set, or the Move that needs it is selected, and the audio side takes it up when it has arrived.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "vst2.h"
#include "engine.h"
#include "moves.h"
#include "param_ids.h"
#include "state.h"
#include "surface.h"
#include "trace.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace {

using namespace gf;

constexpr size_t kTextCap = 128;          // JUCE reads names/display text into 256 bytes; stay well inside
constexpr float kSampleRate = 44100.0f;   // MPC OS always runs 44.1 kHz
constexpr int kScratch = 512;
// Suspended longer than this, the plugin was switched off (its slot's ON button), not stopped with
// the transport: every tail is cleared, so switching it back on never replays old audio.
constexpr uint32_t kLongSuspendMs = 250;

// Denormals (decaying tails) are slow on the VFP unit. Flush them to zero for our own arithmetic
// only: MPC's callbacks run in its worker's own FP mode, handed back afterwards.
class FlushDenormals {
public:
    FlushDenormals() {
#if defined(__arm__)
        asm volatile("vmrs %0, fpscr" : "=r"(saved_));
        asm volatile("vmsr fpscr, %0" : : "r"(saved_ | (1u << 24)));   // FZ
#elif defined(__SSE__) || defined(__x86_64__)
        saved_ = _mm_getcsr();
        _mm_setcsr(saved_ | 0x8040);   // FTZ | DAZ
#endif
    }
    ~FlushDenormals() {
#if defined(__arm__)
        asm volatile("vmsr fpscr, %0" : : "r"(saved_));
#elif defined(__SSE__) || defined(__x86_64__)
        _mm_setcsr(saved_);
#endif
    }
    FlushDenormals(const FlushDenormals&) = delete;
    FlushDenormals& operator=(const FlushDenormals&) = delete;

private:
    uint32_t saved_ = 0;
};

double threadCpuUs() {
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e6 + static_cast<double>(ts.tv_nsec) * 1e-3;
}

// Milliseconds since the plugin was loaded, wrapping every 49 days (32 bits: no 64-bit atomics on ARMv7;
// differences are unsigned, so the wrap does no harm), on the surface's clock when the tests set one.
const auto kLoaded = std::chrono::steady_clock::now();
uint32_t nowMs() {
    if (Surface::clock) return static_cast<uint32_t>(Surface::clock());
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - kLoaded).count());
}

struct Plugin {
    AEffect             fx;          // must stay the first member: MPC hands us &fx back
    audioMasterCallback master = nullptr;
    Surface             surface;
    Engine              engine;
    std::string         chunk;       // effGetChunk buffer: must outlive the call
    std::atomic<bool>   clear{false};          // the next block starts with every tail cleared
    std::atomic<bool>   suspended{false};      // between effMainsChanged(0) and (1)
    std::atomic<uint32_t> suspendedAt{0};      // the first effMainsChanged(0) of that, ms
    std::atomic<int32_t> tail{0};              // effGetTailSize's answer, kept by the audio thread

    // audio thread only
    float    snapshot[P_COUNT] = {};
    bool     havePatch = false;
    uint32_t seenWrites = 0;
    float    scratch[2][kScratch] = {};
    int      ppqOffset = 0;   // process(): this sub-block starts this many samples into the host's block

    // CPU meter: the audio thread sums its own CPU time against the real-time budget and publishes
    // twice a second, with how many parts ran; the status line is formatted on the UI thread.
    double           winUs = 0.0, winBudgetUs = 0.0, winPeak = 0.0;
    std::atomic<int> shownAvg{0}, shownPeak{0};   // percent
    std::atomic<int> shownRunning{0};             // parts running, of Engine::kParts
    int              lastAvg = -1, lastPeak = -1, lastRunning = -1;
};

Plugin* self(AEffect* e) { return static_cast<Plugin*>(e->object); }

// Copies at most cap - 1 bytes, never cutting a UTF-8 character in half.
void copyStr(void* dst, const std::string& s, size_t cap) {
    if (!dst || cap == 0) return;
    size_t n = std::min(s.size(), cap - 1);
    if (n < s.size())
        while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;   // s[n] continues a character
    std::memcpy(dst, s.data(), n);
    static_cast<char*>(dst)[n] = 0;
}

// The status line: what the plugin costs, the average and the worst block of the last half second, and
// how many of its parts were running.
std::string statusText(const Plugin* p) {
    char b[64];
    std::snprintf(b, sizeof b, "CPU %d%%   PEAK %d%%   %d OF %d ON", p->shownAvg.load(), p->shownPeak.load(),
                  p->shownRunning.load(), Engine::kParts);
    return b;
}

// --- parameters (UI thread) ---------------------------------------------------------------

// The memory something is about to need, made before the audio thread asks for it (R42): the Echo's lines when the Echo is
// switched on, or the selected Move is one that works with them; Time's ring when Time is on, or the selected Move plays from
// it (a tile tap selects its Move). A switch set, a Move picked, a preset or a project loaded: every way to need it ends here.
// Cheap once it is done (an atomic load); the audio thread takes the memory up when it has arrived, and until then the part is
// silent and costs nothing. UI thread.
void provide(Plugin* p) {
    const Surface& s = p->surface;
    bool echo = s.get(P_ECHO_ON) > 0.5f, time = s.get(P_TIME_ON) > 0.5f;
    const int picked = static_cast<int>(std::lround(s.get(P_MOVE) * static_cast<float>(PARAM_INFO[P_MOVE].nopts - 1)));   // 0: None
    if (picked >= 1 && picked <= Moves::count()) {
        const int parts = Moves::partsOf(picked - 1);
        echo = echo || (parts & kPartEcho) != 0;
        time = time || (parts & kPartTime) != 0;
    }
    if (echo) p->engine.allocateEcho();
    if (time) p->engine.allocateTime();
}

// Nothing may throw into MPC (a preset load reads a file, allocates, parses).
float getParameter(AEffect* e, int32_t i) {
    try {
        return self(e)->surface.get(i);
    } catch (...) {
        return 0.0f;
    }
}

void setParameter(AEffect* e, int32_t i, float v) {
    try {
        Surface& s = self(e)->surface;
        const bool traced = i >= 0 && i < P_COUNT && tracing();
        const float before = traced ? s.get(i) : 0.0f;   // what MPC last read back
        s.set(i, v);
        provide(self(e));
        if (traced)
            trace("%p set %3d %-18s %.4f  read %.4f -> %.4f  \"%s\"", static_cast<void*>(e), static_cast<int>(i),
                  PARAM_INFO[i].key, static_cast<double>(v), static_cast<double>(before), static_cast<double>(s.get(i)),
                  s.display(i).c_str());
    } catch (...) {
    }
}

// --- audio thread -------------------------------------------------------------------------

// MPC's tempo and song position: the grid follows them. A host callback, so in MPC's own FP mode.
ef::Transport readTransport(Plugin* p) {
    ef::Transport tr;
    if (!p->master) return tr;
    const intptr_t r = p->master(&p->fx, vst::audioMasterGetTime, 0, vst::kVstTempoValid | vst::kVstPpqPosValid, nullptr, 0.0f);
    if (const VstTimeInfo* t = reinterpret_cast<const VstTimeInfo*>(r)) {
        // A NaN or absurd value would stall the grid or spin it: ignore it.
        if ((t->flags & vst::kVstTempoValid) && std::isfinite(t->tempo) && t->tempo >= 1.0 && t->tempo <= 1000.0) tr.bpm = t->tempo;
        tr.valid = (t->flags & vst::kVstPpqPosValid) != 0 && std::isfinite(t->ppqPos) && std::fabs(t->ppqPos) < 1e9;
        tr.beats = tr.valid ? t->ppqPos + p->ppqOffset * tr.bpm / 60.0 / static_cast<double>(kSampleRate) : 0.0;
        tr.playing = (t->flags & vst::kVstTransportPlaying) != 0;
    }
    return tr;
}

// What leaves the plugin is a number and never past +18 dBFS: the extreme corners of the settings or a bug
// can't blast or poison MPC's mix. Sane levels pass untouched; a NaN or an infinity from the host is silence.
constexpr float kCeiling = 7.9432823f;   // +18 dBFS
void guard(float* x, int n) {
    for (int i = 0; i < n; ++i) {
        const float v = x[i];
        x[i] = std::isfinite(v) ? (v > -kCeiling && v < kCeiling ? v : v >= kCeiling ? kCeiling : -kCeiling) : 0.0f;
    }
}

void runBlock(Plugin* p, float* L, float* R, int n, const ef::Transport& tr) {
    // The sound only changes when a parameter does: then the engine takes a new snapshot. Looked
    // at only when something was written since the last look.
    const uint32_t writes = p->surface.writes();   // before the snapshot: a later write shows next block
    if (!p->havePatch || writes != p->seenWrites) {
        float fresh[P_COUNT];
        if (p->surface.snapshot(fresh)) {   // mid-preset: false, look again next block
            p->seenWrites = writes;
            std::memcpy(p->snapshot, fresh, sizeof fresh);
            p->engine.setParams(p->snapshot);
            p->havePatch = true;
        }
    }
    if (p->clear.exchange(false)) {
        p->surface.dropMoveCalls();   // a tap made while it was off is not played when it comes back
        p->engine.reset();
    }
    MoveCall call;   // the UI thread's taps, GO, Move changes and loads, in the order they were made
    while (p->surface.takeMoveCall(call)) p->engine.moveCall(call);
    p->engine.render(L, R, n, tr);
    p->surface.moveLit(p->engine.litMoves());
    p->tail.store(p->engine.tailSamples(), std::memory_order_relaxed);
    guard(L, n);
    guard(R, n);
}

void meter(Plugin* p, double us, int n) {
    const double budget = static_cast<double>(n) * 1e6 / kSampleRate;
    p->winUs += us;
    p->winBudgetUs += budget;
    p->winPeak = std::max(p->winPeak, us / budget);
    if (p->winBudgetUs < 500000.0) return;

    const int avg = static_cast<int>(std::lround(100.0 * p->winUs / p->winBudgetUs));
    const int peak = static_cast<int>(std::lround(100.0 * p->winPeak));
    const int running = p->engine.running();
    p->winUs = p->winBudgetUs = p->winPeak = 0.0;
    p->shownAvg.store(avg);
    p->shownPeak.store(peak);
    p->shownRunning.store(running);
    if (avg != p->lastAvg || peak != p->lastPeak || running != p->lastRunning) {
        p->lastAvg = avg;
        p->lastPeak = peak;
        p->lastRunning = running;
        p->surface.textsChanged();   // told by the next block's notify, within its 50 ms gap
    }
}

void hostAutomate(void* ctx, int index, float value) {
    Plugin* p = static_cast<Plugin*>(ctx);
    if (p->master) p->master(&p->fx, vst::audioMasterAutomate, index, 0, nullptr, value);
}

void hostUpdate(void* ctx) {
    Plugin* p = static_cast<Plugin*>(ctx);
    if (p->master) p->master(&p->fx, vst::audioMasterUpdateDisplay, 0, 0, nullptr, 0.0f);
}

void processReplacing(AEffect* e, float** in, float** out, int32_t n) {
    if (!out || !out[0] || !out[1] || n <= 0) return;
    Plugin* p = self(e);
    const double t0 = threadCpuUs();
    // MPC processes in place (in == out); a host that doesn't gets its input copied over first, and
    // one without input buffers gets silence processed.
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    for (int c = 0; c < 2; ++c) {
        if (in && in[c]) {
            if (in[c] != out[c]) std::memmove(out[c], in[c], bytes);
        } else {
            std::memset(out[c], 0, bytes);
        }
    }
    try {
        const ef::Transport tr = readTransport(p);
        FlushDenormals ftz;
        runBlock(p, out[0], out[1], n, tr);
    } catch (...) {   // nothing may throw into MPC: an escaping exception ends the whole process
        std::memset(out[0], 0, bytes);
        std::memset(out[1], 0, bytes);
    }
    p->surface.notify(hostAutomate, hostUpdate, p, n);
    meter(p, threadCpuUs() - t0, n);
}

// Legacy accumulating entry point (MPC uses processReplacing): sub-blocks of kScratch, each in
// its own place on the host's timeline, added to the output.
void process(AEffect* e, float** in, float** out, int32_t n) {
    if (!out || !out[0] || !out[1]) return;
    Plugin* p = self(e);
    for (int32_t pos = 0; pos < n; pos += kScratch) {
        const int32_t m = std::min<int32_t>(kScratch, n - pos);
        float* subIn[2] = {in && in[0] ? in[0] + pos : nullptr, in && in[1] ? in[1] + pos : nullptr};
        float* tmp[2] = {p->scratch[0], p->scratch[1]};
        p->ppqOffset = pos;
        processReplacing(e, subIn, tmp, m);
        for (int32_t i = 0; i < m; ++i) {
            out[0][pos + i] += tmp[0][i];
            out[1][pos + i] += tmp[1][i];
        }
    }
    p->ppqOffset = 0;
}

// --- the dispatcher -----------------------------------------------------------------------

int32_t canDo(const char* s) {
    static const char* const yes[] = {"receiveVstTimeInfo", "plugAsChannelInsert", "plugAsSend", "2in2out"};
    static const char* const no[] = {"receiveVstEvents", "receiveVstMidiEvent", "sendVstEvents", "sendVstMidiEvent",
                                     "bypass", "offline", "midiProgramNames"};
    for (const char* y : yes)
        if (!std::strcmp(s, y)) return 1;
    for (const char* x : no)
        if (!std::strcmp(s, x)) return -1;
    return 0;   // don't know
}

intptr_t dispatch(Plugin* p, int32_t op, int32_t idx, intptr_t val, void* ptr) {
    const bool validIdx = idx >= 0 && idx < P_COUNT;
    switch (op) {
        case vst::effOpen: return 1;
        case vst::effClose: delete p; return 1;
        case vst::effGetProgram: return 0;
        case vst::effGetProgramName: copyStr(ptr, kPlugName, 24); return 0;
        case vst::effGetPlugCategory: return vst::kPlugCategEffect;
        case vst::effGetEffectName:
        case vst::effGetProductString: copyStr(ptr, kPlugName, 32); return 1;
        case vst::effGetVendorString: copyStr(ptr, kPlugVendor, 32); return 1;
        case vst::effGetVendorVersion: return kPlugVersion;
        case vst::effGetVstVersion: return 2400;
        case vst::effCanBeAutomated: return p->surface.automatable(idx) ? 1 : 0;
        case vst::effGetParamName: copyStr(ptr, validIdx ? PARAM_INFO[idx].name : "", kTextCap); return 0;
        case vst::effGetParamLabel: copyStr(ptr, "", 8); return 0;
        case vst::effGetParamDisplay:
            if (!validIdx) copyStr(ptr, "", kTextCap);
            else if (idx == P_STATUS) copyStr(ptr, statusText(p), kTextCap);
            else copyStr(ptr, p->surface.display(idx), kTextCap);
            return 0;
        case vst::effSetSampleRate:   // MPC OS is fixed at 44.1 kHz; the chain is built for it
        case vst::effSetBlockSize: return 1;
        case vst::effMainsChanged:
            if (val == 0) {
                if (!p->suspended.exchange(true)) p->suspendedAt.store(nowMs());   // a second (0) keeps the first time
            } else if (p->suspended.exchange(false) && nowMs() - p->suspendedAt.load() > kLongSuspendMs) {
                p->clear.store(true);
            }
            return 0;
        case vst::effSetBypass: return 0;   // no soft bypass: MPC's ON button suspends instead
        case vst::effGetTailSize: return std::max<intptr_t>(p->tail.load(), 1);   // 1: no tail
        case vst::effSetSpeakerArrangement: {
            const auto* inArr = reinterpret_cast<const VstSpeakerArrangementHead*>(val);
            const auto* outArr = static_cast<const VstSpeakerArrangementHead*>(ptr);
            return inArr && outArr && inArr->numChannels == 2 && outArr->numChannels == 2 ? 1 : 0;
        }
        case vst::effCanDo: return ptr ? canDo(static_cast<const char*>(ptr)) : 0;
        case vst::effGetChunk:
            if (!ptr) return 0;
            p->chunk = saveState(p->surface, false);
            *static_cast<void**>(ptr) = const_cast<char*>(p->chunk.c_str());
            return static_cast<intptr_t>(p->chunk.size() + 1);
        case vst::effSetChunk: {
            if (!ptr || val <= 0 || val > (1 << 20)) return 0;   // a state is ~1 KB; more is not ours
            std::string s(static_cast<const char*>(ptr), static_cast<size_t>(val));
            while (!s.empty() && s.back() == '\0') s.pop_back();
            if (!loadState(p->surface, s, false)) return 0;
            provide(p);   // a project with the Echo or Time on, or a Move that needs them selected, has its memory from the load
            return 1;
        }
        default: return 0;
    }
}

intptr_t dispatcher(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float /*opt*/) {
    try {
        return dispatch(self(e), op, idx, val, ptr);
    } catch (...) {
        return 0;
    }
}

// Every instance its own random numbers (the browser's RND). GF_FIXED_SEED: the same every time.
uint32_t instanceSeed(const void* p) {
    static std::atomic<uint32_t> count{0};
    const char* fixed = std::getenv("GF_FIXED_SEED");
    if (fixed && *fixed) return 0x9E3779B9u;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) ^ static_cast<uint32_t>(ts.tv_nsec) ^
                 (count.fetch_add(1) * 0x9E3779B9u);
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return h ? h : 1u;
}

AEffect* createPlugin(audioMasterCallback master) {
    Plugin* p = new Plugin();
    p->master = master;
    p->surface.seed(instanceSeed(p) * 0x2545F491u + 1u);

    AEffect* e = &p->fx;
    std::memset(e, 0, sizeof(*e));
    e->magic            = vst::kMagic;
    e->dispatcher       = dispatcher;
    e->process          = process;
    e->setParameter     = setParameter;
    e->getParameter     = getParameter;
    e->processReplacing = processReplacing;
    e->numParams        = P_COUNT;
    e->numInputs        = 2;
    e->numOutputs       = 2;
    e->flags            = vst::effFlagsCanReplacing | vst::effFlagsProgramChunks;
    e->uniqueID         = kPlugUid;
    e->version          = kPlugVersion;
    e->object           = p;
    return e;
}

} // namespace

// Nothing may throw into MPC: a failed creation reports "no plugin" instead.
extern "C" __attribute__((visibility("default"))) AEffect* VSTPluginMain(audioMasterCallback master) {
    try {
        return createPlugin(master);
    } catch (...) {
        return nullptr;
    }
}
