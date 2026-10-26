#pragma once
// A lock-free ring that carries log entries from the threads that must not touch a file (MPC's audio
// workers, its UI side) to the monitor thread, which writes them out (plugin/probe.h). Any number of
// threads push, one pops, in the order the pushes were claimed. A push into a full ring is dropped
// and counted, never waited for.
//
// A push claims a position, fills that slot and publishes it by writing the position into the slot's
// `seq`; the pop takes a slot only once `seq` says it is published. A position is claimed only while
// fewer than N entries are unpopped, so a slot is never claimed twice before it is popped.
//
// 32-bit atomics only: 64-bit ones aren't lock-free everywhere on 32-bit ARM. T is a plain struct.
#include <atomic>
#include <cstdint>

namespace ef {

template <typename T, uint32_t N>
class Ring {
    static_assert(N > 0 && (N & (N - 1)) == 0, "the size is a power of two");

public:
    bool push(const T& v) {
        uint32_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            // Signed: a pos read just before the pops that overtook it is behind the tail, not full.
            if (static_cast<int32_t>(pos - tail_.load(std::memory_order_acquire)) >= static_cast<int32_t>(N)) {
                dropped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;   // pos: the head now, on failure
        }
        Slot& s = slots_[pos & (N - 1)];
        s.value = v;
        s.seq.store(pos + 1, std::memory_order_release);
        return true;
    }

    // The monitor thread only.
    bool pop(T& out) {
        const uint32_t pos = tail_.load(std::memory_order_relaxed);
        Slot& s = slots_[pos & (N - 1)];
        if (s.seq.load(std::memory_order_acquire) != pos + 1) return false;   // empty, or a push still filling it
        out = s.value;
        tail_.store(pos + 1, std::memory_order_release);
        return true;
    }

    uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    struct Slot {
        std::atomic<uint32_t> seq{0};
        T value{};
    };
    Slot slots_[N];
    std::atomic<uint32_t> head_{0}, tail_{0}, dropped_{0};
};

} // namespace ef
