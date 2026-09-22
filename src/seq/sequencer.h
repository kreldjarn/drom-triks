#pragma once
#include <cstddef>
#include <cstdint>
#include "pattern.h"

namespace drom {

/// Sample-accurate step sequencer.
///
/// The clock runs inside the audio callback with a fixed-point accumulator, so
/// tick boundaries never drift and every trigger carries the exact sample
/// offset within the block at which it should sound. Firing on block
/// boundaries instead would put +/-0.67 ms of jitter on every hit at a
/// 32-sample block — audible smearing on hats, and it ruins flams.
class Sequencer
{
  public:
    struct Event
    {
        uint8_t     track    = 0;
        uint16_t    offset   = 0; ///< samples into the current block
        float       velocity = 1.f;
        const Step *step     = nullptr; ///< null for ratchet repeats
    };

    void Init(float sample_rate)
    {
        sample_rate_ = sample_rate;
        SetTempo(120.f);
        Reset();
    }

    void SetPattern(Pattern *p)
    {
        pattern_ = p;
        if(p)
            SetTempo(p->bpm_x10 / 10.f);
    }

    void SetTempo(float bpm)
    {
        if(bpm < 20.f) bpm = 20.f;
        if(bpm > 999.f) bpm = 999.f;
        bpm_ = bpm;
        // Q16 fixed point: exact enough that a 10-minute run drifts well under
        // a sample, and cheap on a CPU with no FPU divide in the hot path.
        const double spt = static_cast<double>(sample_rate_) * 60.0
                           / (static_cast<double>(bpm) * kPpqn);
        samples_per_tick_q16_ = static_cast<uint32_t>(spt * 65536.0 + 0.5);
    }

    float tempo() const { return bpm_; }

    void Start()
    {
        Reset();
        playing_ = true;
    }

    void Continue() { playing_ = true; }
    void Stop() { playing_ = false; }
    bool playing() const { return playing_; }

    /// Absolute tick since Start(), for MIDI clock and display.
    uint64_t tick() const { return tick_; }

    /// Which position each track is currently on, for the playhead.
    int position(int track) const { return position_[track]; }

    /// Renders one audio block's worth of sequencer time.
    /// Returns the number of events written to `out`.
    size_t Process(size_t block_size, Event *out, size_t max_events)
    {
        size_t count = 0;
        if(!playing_ || !pattern_)
            return 0;

        // Walking sample by sample keeps the tick maths obviously correct, and
        // at a 32-sample block it is 32 add-compares — nothing on an M7.
        for(size_t i = 0; i < block_size; ++i)
        {
            ServiceRatchets(i, out, count, max_events);

            // Emit before accumulating, so tick 0 lands on sample 0. Emitting
            // after would make every tick fire a full tick late: the
            // accumulator has to *reach* one tick's worth before it trips.
            if(tick_pending_)
            {
                EmitTick(i, out, count, max_events);
                ++tick_;
                tick_pending_ = false;
            }

            pos_q16_ += 1u << 16;
            if(pos_q16_ >= samples_per_tick_q16_)
            {
                pos_q16_ -= samples_per_tick_q16_;
                tick_pending_ = true;
            }
        }
        return count;
    }

  private:
    void Reset()
    {
        tick_         = 0;
        pos_q16_      = 0;
        tick_pending_ = true; // the first sample of playback is tick 0
        for(int t = 0; t < kNumTracks; ++t)
        {
            position_[t]   = 0;
            ratchet_[t]    = Ratchet{};
        }
        rng_ = 0x9E3779B9u;
    }

    struct Ratchet
    {
        int32_t countdown = 0;
        int32_t interval  = 0;
        uint8_t left      = 0;
        float   velocity  = 1.f;
    };

    uint32_t NextRandom()
    {
        // xorshift32: deterministic, allocation-free, good enough for
        // probability and random direction. Seeded fixed so a pattern plays
        // the same way twice, which matters when you are tuning it.
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }

    /// Maps a position within the cycle to the step index that plays there.
    int StepForPosition(const Track &tr, int pos) const
    {
        const int len = tr.length;
        switch(tr.direction)
        {
            case Direction::Reverse: return len - 1 - (pos % len);
            case Direction::PingPong:
            {
                const int cycle = tr.positions_per_cycle();
                const int p     = pos % cycle;
                return p < len ? p : cycle - p;
            }
            case Direction::Random:
            case Direction::Forward:
            default: return pos % len;
        }
    }

    /// Swing delays odd positions. 50 is straight; 75 pushes them a quarter of
    /// a step late, which is about where classic MPC shuffle sits.
    int SwingTicks(int pos, int ticks_per_step) const
    {
        if((pos & 1) == 0)
            return 0;
        const int amount = static_cast<int>(pattern_->swing) - 50;
        return amount * ticks_per_step / 100;
    }

    void EmitTick(size_t offset, Event *out, size_t &count, size_t max)
    {
        for(int t = 0; t < kNumTracks; ++t)
        {
            Track &tr = pattern_->tracks[t];
            if(tr.muted || tr.length == 0)
                continue;

            const int tps        = tr.ticks_per_step();
            const int cycle      = tr.positions_per_cycle();
            const int loop_ticks = cycle * tps;
            if(loop_ticks <= 0)
                continue;

            const int local   = static_cast<int>(tick_ % loop_ticks);
            const int nominal = local / tps;

            // A step's micro offset moves it up to one step either way, so the
            // step that lands on this tick may be the neighbouring one. Check
            // the window rather than assuming position == step.
            if(local % tps == 0)
                position_[t] = nominal; // playhead follows the nominal grid

            int seen[3]   = {-1, -1, -1};
            int seen_count = 0;
            for(int d = -1; d <= 1; ++d)
            {
                const int pos = ((nominal + d) % cycle + cycle) % cycle;

                // On a 1- or 2-step cycle the window wraps onto itself, so the
                // same position would be evaluated more than once per tick.
                bool dup = false;
                for(int k = 0; k < seen_count; ++k)
                    if(seen[k] == pos)
                        dup = true;
                if(dup)
                    continue;
                seen[seen_count++] = pos;

                const int idx = StepForPosition(tr, pos);
                const Step &st = tr.steps[idx];

                int fire = pos * tps + st.micro + SwingTicks(pos, tps);
                fire     = ((fire % loop_ticks) + loop_ticks) % loop_ticks;
                if(fire != local)
                    continue;

                if(!st.active())
                    continue;
                if(st.probability < 100
                   && (NextRandom() % 100u) >= st.probability)
                    continue;

                float vel = st.velocity / 127.f;
                if(st.accent())
                    vel = vel * 0.7f + 0.3f;

                Push(out, count, max, Event{static_cast<uint8_t>(t),
                                            static_cast<uint16_t>(offset),
                                            vel,
                                            &st});

                // Ratchets subdivide the step, so their spacing is sub-tick and
                // they routinely land in later blocks. They are scheduled in
                // samples and serviced independently of the tick clock.
                if(st.ratchet > 1)
                {
                    const int64_t step_samples
                        = (static_cast<int64_t>(tps) * samples_per_tick_q16_) >> 16;
                    Ratchet &r = ratchet_[t];
                    r.interval = static_cast<int32_t>(step_samples / st.ratchet);
                    r.left     = static_cast<uint8_t>(st.ratchet - 1);
                    r.countdown = r.interval;
                    r.velocity  = vel;
                }
            }
        }
    }

    void ServiceRatchets(size_t offset, Event *out, size_t &count, size_t max)
    {
        for(int t = 0; t < kNumTracks; ++t)
        {
            Ratchet &r = ratchet_[t];
            if(r.left == 0)
                continue;
            if(--r.countdown > 0)
                continue;

            Push(out, count, max, Event{static_cast<uint8_t>(t),
                                        static_cast<uint16_t>(offset),
                                        r.velocity,
                                        nullptr});
            --r.left;
            r.countdown = r.interval;
        }
    }

    static void Push(Event *out, size_t &count, size_t max, const Event &e)
    {
        if(count < max)
            out[count++] = e;
    }

    Pattern *pattern_ = nullptr;
    float    sample_rate_ = 48000.f;
    float    bpm_         = 120.f;
    uint32_t samples_per_tick_q16_ = 0;
    uint32_t pos_q16_             = 0;
    uint64_t tick_                = 0;
    bool     playing_             = false;
    bool     tick_pending_        = true;
    int      position_[kNumTracks] = {};
    Ratchet  ratchet_[kNumTracks]  = {};
    uint32_t rng_ = 0x9E3779B9u;
};

} // namespace drom
