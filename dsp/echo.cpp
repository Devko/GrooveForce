// Echo: see echo.h.
#include "echo.h"

#include "slew.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <new>

namespace gf {

namespace {

// The gains' fastest move a sample: a full scale in a hair over 1 ms (as Pump's edge, so rounding never makes a step over 1/44.1).
constexpr float kSlope = 1.0f / (0.001001f * ef::kRate);
constexpr float kFadeStep = 1.0f / (0.010f * ef::kRate);     // a line on or off: 10 ms
constexpr float kFreezeStep = 1.0f / (0.020f * ef::kRate);   // the freeze: 20 ms
constexpr float kDuckLaw = 16.0f;                            // EffectForce's
constexpr float kKeyClamp = 8.0f;                            // +18 dBFS: a wild sample must not hold the wet down for long
constexpr float kTiny = 1e-18f;
constexpr float kInputClamp = 4.0f;                          // the most the lines take (the Echo sums before them)
constexpr float kModTime = 0.02f;                            // modTime 1 at the modulator's 1: 2% of the time
constexpr float kModOctaves = 4.0f;                          // modCutoff 1 at the modulator's 1: 4 octaves
constexpr float kQuarterPi = 0.78539816339f;
constexpr int64_t kForever = 1 << 30;                        // as the lines' tail for feedback 1

// Set when the plugin loads (no guard to take on the audio thread).
const float kDuckAttack = ef::smoothCoef(0.005f), kDuckFall = ef::smoothCoef(0.25f);

// Where each line's input comes from in each routing: the send (x) and the read of one other line, if any.
struct Wire {
    bool x;
    int src;
};
constexpr Wire kWire[Echo::kRoutings][Echo::kLines] = {
    /* Parallel */ {{true, -1}, {true, -1}, {true, -1}, {true, -1}},
    /* Serial   */ {{true, -1}, {false, 0}, {false, 1}, {false, 2}},
    /* Pairs    */ {{true, -1}, {false, 0}, {true, -1}, {false, 2}},
    /* Ring     */ {{true, 3}, {false, 0}, {false, 1}, {false, 2}},   // (when all four are on; wiring() closes it over the lines that are on)
    /* Spread   */ {{true, -1}, {false, 0}, {false, 0}, {false, 0}},
};

// The routing as it is wired for the lines that are on (R46): the table's, except that a Ring closes over the lines that are on,
// each taking the previous one's feed, the first of them the send as well, and a lone one feeding itself. A line that is not on
// (off, or fading out) is in no Ring. src[i] is -1 where line i takes no feed.
void wiring(int routing, const bool* on, bool* x, int* src) {
    for (int i = 0; i < Echo::kLines; ++i) {
        x[i] = kWire[routing][i].x;
        src[i] = kWire[routing][i].src;
    }
    if (routing != Echo::kRing) return;
    int first = -1, prev = -1;
    for (int i = 0; i < Echo::kLines; ++i) {
        x[i] = false;
        src[i] = -1;
        if (!on[i]) continue;
        if (first < 0) {
            first = i;
            x[i] = true;
        }
        src[i] = prev;
        prev = i;
    }
    if (first >= 0) src[first] = prev;   // the last of them: the first itself, if it is alone
}

float cleanInput(float v) { return std::isfinite(v) ? std::clamp(v, -kInputClamp, kInputClamp) : 0.0f; }

} // namespace

Echo::Echo() = default;

Echo::~Echo() { delete store_.load(std::memory_order_acquire); }

bool Echo::allocate() {
    if (store_.load(std::memory_order_acquire)) return true;
    try {
        // Owned until handed over: a failed line frees the ones before it; two callers at once keep one.
        auto s = std::make_unique<Store>();
        for (EchoLine& line : s->line)
            if (!line.allocate()) return false;
        Store* none = nullptr;
        if (store_.compare_exchange_strong(none, s.get(), std::memory_order_acq_rel)) s.release();
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

float Echo::peakStored() const {
    const Store* s = store_.load(std::memory_order_acquire);
    float m = 0.0f;
    if (s)
        for (const EchoLine& line : s->line) {
            const float p = line.peakStored();
            if (!(p <= m)) m = p;   // (a NaN or infinity is kept)
        }
    return m;
}

void Echo::reset() {
    st_ = store_.load(std::memory_order_acquire);
    for (int i = 0; i < kLines; ++i) {
        slot_[i] = Slot();
        mod_[i].reset();
        if (st_) st_->line[i].reset();
    }
    for (ef::Ramp& g : feedG_) g = ef::Ramp();
    for (ef::Ramp& g : swap_) g = ef::Ramp();
    det_.reset();
    fol_.reset();
    detLive_ = folLive_ = false;
    sens_ = -1.0f;
    freeze_ = 0.0f;
    duckEnv_ = 0.0f;
    fresh_ = true;
    active_ = awake_ = xfade_ = false;
    running_ = 0;
}

// What a line does in this chunk: where its fade is going, and whether it runs at all. Back on after
// resting it starts clean, unless it comes back before its fade-out is over.
void Echo::arm(int i, bool jump) {
    Slot& s = slot_[i];
    s.goal = p_.line[i].on ? 1.0f : 0.0f;
    if (jump) s.fade = s.goal;
    if (st_ && s.goal > 0.0f && s.fade == 0.0f && s.dirty) {
        st_->line[i].reset();
        mod_[i].reset();
        s.dirty = false;
    }
    s.runs = st_ != nullptr && (s.fade > 0.0f || s.goal > 0.0f);
    if (!s.runs) s.live = false;
}

void Echo::set(const Params& in, const ef::Transport& t) {
    st_ = store_.load(std::memory_order_acquire);
    t_ = t;
    p_ = in;
    p_.routing = std::clamp(in.routing, 0, kRoutings - 1);
    p_.send = ef::clampParam(in.send, 0.0f, 1.0f, 1.0f);
    p_.duck = ef::clampParam(in.duck, 0.0f, 1.0f, 0.0f);
    p_.glide = std::clamp(in.glide, 0, 1);
    p_.width = ef::clampParam(in.width, 0.0f, 1.5f, 1.0f);
    p_.level = ef::clampParam(in.level, 0.0f, 1.0f, 1.0f);
    p_.feedbackOffset = ef::clampParam(in.feedbackOffset, -1.0f, 1.0f, 0.0f);
    p_.toneOct = ef::clampParam(in.toneOct, -4.0f, 4.0f, 0.0f);
    p_.sensitivity = ef::clampParam(in.sensitivity, 0.0f, 1.0f, 0.5f);
    for (Line& l : p_.line) {
        l.level = ef::clampParam(l.level, 0.0f, 1.0f, 1.0f);
        l.pan = ef::clampParam(l.pan, -1.0f, 1.0f, 0.0f);
        l.modCutoff = ef::clampParam(l.modCutoff, -1.0f, 1.0f, 0.0f);
        l.modTime = ef::clampParam(l.modTime, -1.0f, 1.0f, 0.0f);
        l.modLevel = ef::clampParam(l.modLevel, 0.0f, 1.0f, 0.0f);
    }

    const bool jump = fresh_ && st_ != nullptr;
    active_ = false;
    for (int i = 0; i < kLines; ++i) {
        arm(i, jump);
        active_ = active_ || slot_[i].runs;
    }

    // The wiring, for the lines that are on; the coefficients jump to it when nothing hears them, else they cross to it in process().
    bool on[kLines];
    for (int i = 0; i < kLines; ++i) on[i] = slot_[i].runs && slot_[i].goal > 0.0f;
    wiring(p_.routing, on, wireX_, wireSrc_);
    bool there = true;
    for (int i = 0; i < kLines; ++i) {
        const float tx = wireX_[i] ? 1.0f : 0.0f;
        if (fresh_ || !active_) wx_[i] = tx;
        there = there && wx_[i] == tx;
        for (int k = 0; k < kLines; ++k) {
            const float ty = wireSrc_[i] == k ? 1.0f : 0.0f;
            if (fresh_ || !active_) wy_[i][k] = ty;
            there = there && wy_[i][k] == ty;
        }
    }
    xfade_ = !there;
    fresh_ = false;
}

// The controls are where they are going: the Echo has been resting and its values jump.
void Echo::wake() {
    freeze_ = p_.freeze ? 1.0f : 0.0f;
    send_.jump(p_.send * (1.0f - freeze_));
    level_.jump(p_.level);
    width_.jump(p_.width);
    duckGain_.jump(1.0f);
    duckEnv_ = 0.0f;
}

void Echo::process(const float* inL, const float* inR, const float* keyL, const float* keyR, float* wetL, float* wetR, int n) {
    n = std::min(n, ef::kChunk);
    if (n <= 0) return;
    running_ = 0;
    if (!active_) {   // not allocated, or every line off and faded out: nothing runs
        std::fill(wetL, wetL + n, 0.0f);
        std::fill(wetR, wetR + n, 0.0f);
        awake_ = false;
        detLive_ = folLive_ = false;
        return;
    }
    if (!awake_) wake();
    awake_ = true;

    const float count = static_cast<float>(n);
    const float slope = kSlope * count;   // the most a gain moves in this chunk
    inL = inL ? inL : zero_;
    inR = inR ? inR : zero_;
    const float* kL = keyL ? keyL : zero_;
    const float* kR = keyR ? keyR : zero_;

    // Freeze and send, level and width: moved toward where they are going, in a straight line across the chunk.
    freeze_ = toward(freeze_, p_.freeze ? 1.0f : 0.0f, kFreezeStep * count);
    send_.to(toward(send_.value(), p_.send * (1.0f - freeze_), slope), n);
    level_.to(toward(level_.value(), p_.level, slope), n);
    width_.to(toward(width_.value(), p_.width, slope), n);

    // The duck: the key's envelope, and the gain it asks for at the chunk's end.
    float duckT = 1.0f;
    if (p_.duck > 0.0f) {
        float env = duckEnv_;
        for (int k = 0; k < n; ++k) {
            const float level = std::min(std::max(std::fabs(ef::sanitize(kL[k])), std::fabs(ef::sanitize(kR[k]))), kKeyClamp) + kTiny;
            env += (level > env ? kDuckAttack : kDuckFall) * (level - env);
        }
        duckEnv_ = env;
        duckT = 1.0f / (1.0f + kDuckLaw * p_.duck * env);
    } else {
        duckEnv_ = 0.0f;
    }
    duckGain_.to(toward(duckGain_.value(), duckT, slope), n);

    // The key's detector and follower (R41): one for the whole Echo, run in the chunks where a line that runs needs
    // them, and every line's modulator is given what they found. After a chunk they skipped they start from nothing (R47):
    // what they held is the key of a while ago.
    bool needEnv = false, needFollow = false;
    for (int i = 0; i < kLines; ++i)
        if (slot_[i].runs) {
            needEnv = needEnv || p_.line[i].mod.mode == EchoMod::kEnv;
            needFollow = needFollow || p_.line[i].mod.mode == EchoMod::kFollow;
        }
    int onset = -1;
    float followed = 0.0f;
    if (needEnv) {
        if (!detLive_) det_.reset();
        if (p_.sensitivity != sens_) {
            sens_ = p_.sensitivity;
            det_.setSensitivity(sens_);
        }
        onset = det_.process(kL, kR, n);
    }
    if (needFollow) {
        if (!folLive_) fol_.reset();
        followed = fol_.process(kL, kR, n);
    }
    detLive_ = needEnv;
    folLive_ = needFollow;

    // Phase 1, every line that runs: its modulator, its parameters, and its read (faded).
    const bool ring = p_.routing == kRing;   // R39: there a line's feedback is its send round the ring, and it has none of its own
    float feedT[kLines];   // the gain each line's read is passed on at, where it is going
    bool fresh[kLines];
    int swapMode[kLines];  // R45: 0 the feed is passed on as it is, 1 with its sides swapped, 2 in between (the swap is moving)
    bool needX = xfade_;
    bool isSrc[kLines];
    for (int k = 0; k < kLines; ++k) isSrc[k] = xfade_;
    for (int i = 0; i < kLines; ++i) {
        feedT[i] = 1.0f;
        fresh[i] = false;
        swapMode[i] = 0;
        Slot& s = slot_[i];
        if (!s.runs) continue;
        ++running_;
        needX = needX || wireX_[i];
        if (wireSrc_[i] >= 0) isSrc[wireSrc_[i]] = true;
        const Line& l = p_.line[i];
        EchoLine& line = st_->line[i];
        fresh[i] = !s.live;   // coming on from rest: the fade covers it, the values start where they are going
        s.live = s.dirty = true;

        EchoMod::Params mp = l.mod;
        mp.sensitivity = p_.sensitivity;
        mp.swing = p_.swing;
        const float m = mod_[i].nextShared(mp, t_, n, onset, followed);
        s.mod = fresh[i] ? m : toward(s.mod, m, slope);

        // R45: in a Ring a ping-pong line runs as Stereo inside (the cross-over of its repeats lives in the own feedback a Ring
        // does not have) and passes its feed on with the sides swapped, so the ping-pong goes round the ring without a loss.
        const bool pingPong = ring && l.p.mode == EchoLine::kPingPong;
        EchoLine::Params lp = l.p;
        lp.glide = p_.glide;
        if (pingPong) lp.mode = EchoLine::kStereo;
        const float base = std::clamp(ef::clampParam(l.p.feedback, 0.0f, 1.0f, 0.0f) + p_.feedbackOffset, 0.0f, 1.0f);
        const float fb = base + (1.0f - base) * freeze_;
        lp.feedback = ring ? 0.0f : fb;
        lp.timeMod = 1.0f + kModTime * l.modTime * s.mod;
        lp.cutoffModOct = p_.toneOct + kModOctaves * l.modCutoff * s.mod;
        line.set(lp, t_);
        line.read(yL_[i], yR_[i], n);

        // On or off: the fade runs along the read and along the input.
        s.fading = s.fade != s.goal;
        if (s.fading) {
            float f = s.fade;
            const float step = s.goal > f ? kFadeStep : -kFadeStep;
            for (int k = 0; k < n; ++k) {
                f = std::clamp(f + step, 0.0f, 1.0f);
                fadeBuf_[i][k] = f;
                yL_[i][k] *= f;
                yR_[i][k] *= f;
            }
            s.fade = f;
        }
        // What is passed on to the other lines: in a Ring the feedback, frozen the feeds stop (R44: the lines hold what they
        // have, and a feed that went on would swell the lines downstream into their limiters), times the loopScale (R36).
        const float sc = line.loopScale();
        feedT[i] = (ring ? fb : 1.0f - freeze_) * (sc >= 0.0f && sc <= 1.0f ? sc : 1.0f);
        ef::Ramp& sw = swap_[i];
        if (fresh[i]) sw.jump(pingPong ? 1.0f : 0.0f);
        else sw.to(toward(sw.value(), pingPong ? 1.0f : 0.0f, kFadeStep * count), n);
        swapMode[i] = sw.moving() || (sw.value() > 0.0f && sw.value() < 1.0f) ? 2 : (sw.value() >= 1.0f ? 1 : 0);
        if (swapMode[i] == 2)
            for (int k = 0; k < n; ++k) swapBuf_[i][k] = sw.next();

        // The heard gains: level, the modulator's dip, the pan.
        if (l.pan != s.pan) {
            const float a = kQuarterPi * l.pan;
            const float c = std::cos(a), sn = std::sin(a);
            s.panL = c - sn;
            s.panR = c + sn;
            s.pan = l.pan;
        }
        const float g = l.level * (1.0f - l.modLevel * (1.0f - s.mod));
        if (fresh[i]) {
            s.gl.jump(g * s.panL);
            s.gr.jump(g * s.panR);
        } else {
            s.gl.to(toward(s.gl.value(), g * s.panL, slope), n);
            s.gr.to(toward(s.gr.value(), g * s.panR, slope), n);
        }
    }

    // What is heard: the lines' reads at their gains.
    std::fill(hL_, hL_ + n, 0.0f);
    std::fill(hR_, hR_ + n, 0.0f);
    const float* feedL[kLines];
    const float* feedR[kLines];
    for (int i = 0; i < kLines; ++i) {
        feedL[i] = feedR[i] = zero_;
        Slot& s = slot_[i];
        if (!s.runs) continue;
        const float *yl = yL_[i], *yr = yR_[i];
        if (s.gl.moving() || s.gr.moving()) {
            for (int k = 0; k < n; ++k) {
                hL_[k] += yl[k] * s.gl.next();
                hR_[k] += yr[k] * s.gr.next();
            }
        } else {
            const float gl = s.gl.value(), gr = s.gr.value();
            for (int k = 0; k < n; ++k) {
                hL_[k] += yl[k] * gl;
                hR_[k] += yr[k] * gr;
            }
        }
        // What the routing passes on: the read at its feed gain (1 in the feed-forward routings, the feedback in a
        // Ring, times the line's loopScale), the gain moved in a straight line across the chunk and no faster than a
        // full scale in 1 ms; with its sides swapped, as far as the line's ping-pong goes round a Ring.
        ef::Ramp& fg = feedG_[i];
        if (fresh[i] || !isSrc[i]) fg.jump(feedT[i]);
        else fg.to(toward(fg.value(), feedT[i], slope), n);
        feedL[i] = yl;
        feedR[i] = yr;
        if (isSrc[i] && swapMode[i] == 1) {   // swapped all the way: the sides trade places
            for (int k = 0; k < n; ++k) {
                const float g = fg.next();
                sL_[i][k] = g * yr[k];
                sR_[i][k] = g * yl[k];
            }
            feedL[i] = sL_[i];
            feedR[i] = sR_[i];
        } else if (isSrc[i] && swapMode[i] == 2) {
            for (int k = 0; k < n; ++k) {
                const float g = fg.next(), sv = swapBuf_[i][k];
                const float l = yl[k], r = yr[k];
                sL_[i][k] = g * (l + sv * (r - l));
                sR_[i][k] = g * (r + sv * (l - r));
            }
            feedL[i] = sL_[i];
            feedR[i] = sR_[i];
        } else if (isSrc[i] && (fg.moving() || fg.value() != 1.0f)) {
            if (fg.moving()) {
                for (int k = 0; k < n; ++k) {
                    const float g = fg.next();
                    sL_[i][k] = yl[k] * g;
                    sR_[i][k] = yr[k] * g;
                }
            } else {
                const float g = fg.value();
                for (int k = 0; k < n; ++k) {
                    sL_[i][k] = yl[k] * g;
                    sR_[i][k] = yr[k] * g;
                }
            }
            feedL[i] = sL_[i];
            feedR[i] = sR_[i];
        }
    }

    // The send: the input, sanitised, at its gain.
    if (needX) {
        if (send_.moving() || send_.value() != 1.0f) {
            for (int k = 0; k < n; ++k) {
                const float g = send_.next();
                xL_[k] = cleanInput(inL[k]) * g;
                xR_[k] = cleanInput(inR[k]) * g;
            }
        } else {
            for (int k = 0; k < n; ++k) {
                xL_[k] = cleanInput(inL[k]);
                xR_[k] = cleanInput(inR[k]);
            }
        }
    }
    // R45: a ping-pong line in a Ring takes the send as its mono sum in L only (the line's own rule, which is for the send alone there).
    const float* xl[kLines];
    const float* xr[kLines];
    for (int i = 0; i < kLines; ++i) {
        xl[i] = xL_;
        xr[i] = xR_;
        if (needX && slot_[i].runs && swapMode[i] && (wireX_[i] || xfade_)) {
            if (swapMode[i] == 1) {
                for (int k = 0; k < n; ++k) {
                    xpL_[i][k] = 0.5f * (xL_[k] + xR_[k]);
                    xpR_[i][k] = 0.0f;
                }
            } else {
                for (int k = 0; k < n; ++k) {
                    const float mono = 0.5f * (xL_[k] + xR_[k]), sv = swapBuf_[i][k];
                    xpL_[i][k] = xL_[k] + sv * (mono - xL_[k]);
                    xpR_[i][k] = xR_[k] * (1.0f - sv);
                }
            }
            xl[i] = xpL_[i];
            xr[i] = xpR_[i];
        }
    }

    // Phase 2: each line's input, made from the send and the others' reads by the routing, and written.
    for (int i = 0; i < kLines; ++i) {
        Slot& s = slot_[i];
        if (!s.runs) continue;
        const float *aL = zero_, *aR = zero_;
        float *bL = inL_[i], *bR = inR_[i];
        if (xfade_) {
            // The routing is changing: the input is the sum of the send and the four reads along their coefficients.
            const float tx = wireX_[i] ? 1.0f : 0.0f;
            for (int k = 0; k < n; ++k) {
                const float step = kSlope * static_cast<float>(k + 1);
                const float cx = toward(wx_[i], tx, step);
                float l = cx * xl[i][k], r = cx * xr[i][k];
                for (int j = 0; j < kLines; ++j) {
                    const float cy = toward(wy_[i][j], wireSrc_[i] == j ? 1.0f : 0.0f, step);
                    l += cy * feedL[j][k];
                    r += cy * feedR[j][k];
                }
                bL[k] = l;
                bR[k] = r;
            }
            aL = bL;
            aR = bR;
        } else {
            const int src = wireSrc_[i] >= 0 && slot_[wireSrc_[i]].runs ? wireSrc_[i] : -1;
            if (wireX_[i] && src < 0) {
                aL = xl[i];
                aR = xr[i];
            } else if (!wireX_[i] && src >= 0) {
                aL = feedL[src];
                aR = feedR[src];
            } else if (wireX_[i]) {
                for (int k = 0; k < n; ++k) {
                    bL[k] = xl[i][k] + feedL[src][k];
                    bR[k] = xr[i][k] + feedR[src][k];
                }
                aL = bL;
                aR = bR;
            }
        }
        if (s.fading) {
            for (int k = 0; k < n; ++k) {
                bL[k] = aL[k] * fadeBuf_[i][k];
                bR[k] = aR[k] * fadeBuf_[i][k];
            }
            aL = bL;
            aR = bR;
        }
        st_->line[i].write(aL, aR, n);
    }
    if (xfade_) {   // the coefficients have moved on, running or not
        bool there = true;
        for (int i = 0; i < kLines; ++i) {
            const float tx = wireX_[i] ? 1.0f : 0.0f;
            wx_[i] = toward(wx_[i], tx, slope);
            there = there && wx_[i] == tx;
            for (int j = 0; j < kLines; ++j) {
                const float ty = wireSrc_[i] == j ? 1.0f : 0.0f;
                wy_[i][j] = toward(wy_[i][j], ty, slope);
                there = there && wy_[i][j] == ty;
            }
        }
        xfade_ = !there;
    }

    // The wet: width, then the duck and the level.
    const bool wide = width_.moving() || width_.value() != 1.0f;
    const bool gain = duckGain_.moving() || level_.moving() || duckGain_.value() != 1.0f || level_.value() != 1.0f;
    for (int k = 0; k < n; ++k) {
        float l = hL_[k], r = hR_[k];
        if (wide) {
            const float m = 0.5f * (l + r), d = 0.5f * (l - r) * width_.next();
            l = m + d;
            r = m - d;
        }
        if (gain) {
            const float g = duckGain_.next() * level_.next();
            l *= g;
            r *= g;
        }
        wetL[k] = l;
        wetR[k] = r;
    }
}

// How long it rings, along the routing: a line's own tail plus the longest of what feeds it.
int Echo::tailSamples() const {
    if (!st_) return 0;
    int count = 0;
    for (const Slot& s : slot_) count += s.runs ? 1 : 0;
    if (count == 0) return 0;
    if (p_.freeze) return static_cast<int>(kForever);   // it holds
    int64_t own[kLines], reach[kLines];
    for (int i = 0; i < kLines; ++i) own[i] = reach[i] = slot_[i].runs ? st_->line[i].tailSamples() : 0;
    if (p_.routing == kRing) {
        // R39, R46: the lines have no feedback of their own; it goes round the lines that are on, a trip at a time, at the
        // product of their feedbacks. Lines fading out ring out on their own.
        double product = 1.0;
        int64_t trip = 0, fading = 0;
        int ons = 0;
        for (int i = 0; i < kLines; ++i) {
            if (!slot_[i].runs) continue;
            if (slot_[i].goal > 0.0f) {
                product *= std::clamp(ef::clampParam(p_.line[i].p.feedback, 0.0f, 1.0f, 0.0f) + p_.feedbackOffset, 0.0f, 1.0f);
                trip += own[i];
                ++ons;
            } else {
                fading = std::max(fading, own[i]);
            }
        }
        if (ons == 0) return static_cast<int>(fading);
        if (!(product < 0.999999)) return static_cast<int>(kForever);   // it holds for ever
        const double trips = product > 1e-9 ? std::floor(std::log(1e-3) / std::log(product)) + 1.0 : 1.0;   // down to -60 dB
        return static_cast<int>(std::min(std::max(trips * static_cast<double>(trip), static_cast<double>(fading)), static_cast<double>(kForever)));
    }
    const Wire* wire = kWire[p_.routing];
    for (int pass = 0; pass < kLines; ++pass)   // (the other routings have no cycle)
        for (int i = 0; i < kLines; ++i)
            if (slot_[i].runs) {
                const int src = wire[i].src;
                reach[i] = std::min(own[i] + (src >= 0 && slot_[src].runs ? reach[src] : 0), kForever);
            }
    int64_t longest = 0;
    for (int i = 0; i < kLines; ++i) longest = std::max(longest, reach[i]);
    return static_cast<int>(longest);
}

} // namespace gf
