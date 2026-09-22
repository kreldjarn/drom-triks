#pragma once
#include <atomic>
#include <cstddef>

namespace drom {

/// Lock-free single-producer / single-consumer ring buffer.
///
/// Exactly one thread may Push and exactly one may Pop — here the main loop
/// produces and the audio callback consumes. That constraint is what makes it
/// lock-free with only two atomics and no compare-exchange, which matters
/// because the consumer is a real-time callback that must never block.
///
/// Capacity is N-1: one slot is always left empty so full and empty are
/// distinguishable without a separate count that both sides would have to
/// write.
template <typename T, size_t N>
class SpscQueue
{
    static_assert(N >= 2, "need at least two slots");
    static_assert((N & (N - 1)) == 0, "N must be a power of two for the mask");

  public:
    /// Producer side only. Returns false if full, rather than blocking or
    /// overwriting — a dropped UI event is far better than a stalled callback.
    bool Push(const T &item)
    {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kMask;
        // acquire: don't let the slot write float above the tail read.
        if(next == tail_.load(std::memory_order_acquire))
            return false;
        buf_[head] = item;
        // release: the item must be visible before the consumer sees the index.
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Consumer side only.
    bool Pop(T &out)
    {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if(tail == head_.load(std::memory_order_acquire))
            return false;
        out = buf_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool empty() const
    {
        return head_.load(std::memory_order_acquire)
               == tail_.load(std::memory_order_acquire);
    }

    size_t size() const
    {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & kMask;
    }

    static constexpr size_t capacity() { return N - 1; }

  private:
    static constexpr size_t kMask = N - 1;

    T                   buf_[N];
    std::atomic<size_t> head_{0}; ///< written by the producer only
    std::atomic<size_t> tail_{0}; ///< written by the consumer only
};

} // namespace drom
