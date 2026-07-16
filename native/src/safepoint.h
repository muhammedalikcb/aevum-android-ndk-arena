// SPDX-License-Identifier: Apache-2.0
// Aevum-Defrag - ragged safepoints and per-thread SATB.
//
// The mutator/compactor boundary is coordinated entirely with per-thread state and
// a ragged handshake. The obvious alternative - a shared MPSC SATB buffer (every
// mutator CAS-ing one head) plus a global in-flight counter drained to zero - was
// measured and rejected: under saturating write load the counter almost never
// reaches zero, and compaction throughput collapses above ~16 threads.
//
// The design:
//   * Each mutator thread owns a cache-line-isolated MutatorContext holding an
//     SPSC SATB ring (no cross-thread CAS on the write barrier) and a single
//     `in_write` flag.
//   * To make from-space read-only for evacuation, the compactor does NOT wait
//     for a global zero. It walks the mutator registry and hands-shakes each
//     thread individually: observe that thread pass through an `in_write == 0`
//     gap once, then move on. Threads never wait on each other, and each is
//     caught in a gap within one write-scope, so the handshake stays cheap at
//     32/64 threads. Correctness rests on a store-then-seq_cst-load (Dekker)
//     pairing between beginWrite and the handshake.

#ifndef AEVUM_SAFEPOINT_H_
#define AEVUM_SAFEPOINT_H_

#include <cstdint>
#include <atomic>

#include "platform.h"

namespace aevum {

inline constexpr uint32_t kSatbCap = 8192;  // deleted-edge slots per thread per cycle (power of 2)

// Single-producer (owner thread) / single-consumer (compactor) SATB ring. No
// CAS: the producer owns head, the consumer owns tail.
class ThreadLocalSatbBuffer {
 public:
  void init(void** storage, uint32_t cap) noexcept { slots_ = storage; cap_ = cap; mask_ = cap - 1; }
  bool valid() const noexcept { return slots_ != nullptr; }

  // Owner thread only.
  void push(void* p) noexcept {
    const uint32_t h = head_.load(std::memory_order_relaxed);
    if (h - tail_.load(std::memory_order_acquire) >= cap_) {
      dropped_.store(true, std::memory_order_relaxed);
      return;
    }
    slots_[h & mask_] = p;
    head_.store(h + 1, std::memory_order_release);
  }

  // Compactor only.
  template <typename Sink>
  void drain(Sink&& sink) noexcept {
    uint32_t t = tail_.load(std::memory_order_relaxed);
    const uint32_t h = head_.load(std::memory_order_acquire);
    while (t != h) { sink(slots_[t & mask_]); ++t; }
    tail_.store(t, std::memory_order_release);
  }

  bool empty() const noexcept {
    return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
  }
  bool overflowed() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  void resetOverflow() noexcept { dropped_.store(false, std::memory_order_relaxed); }

 private:
  void** slots_ = nullptr;
  uint32_t cap_ = 0, mask_ = 0;
  AEVUM_CACHELINE_ALIGNED std::atomic<uint32_t> head_{0};  // producer cursor (own line)
  AEVUM_CACHELINE_ALIGNED std::atomic<uint32_t> tail_{0};  // consumer cursor (own line)
  std::atomic<bool> dropped_{false};
};

// One per mutator thread. Every hot field the compactor polls (`in_write`) and
// the SATB ring sit on their own cache lines so the E-core compactor polling a
// P-core mutator never steals its line.
struct alignas(kCacheLine) MutatorContext {
  // Ragged-safepoint flag: >0 while the thread is inside a WriteScope / alloc
  // init touching managed memory. Its own cache line.
  std::atomic<uint32_t> in_write{0};
  AEVUM_CACHELINE_ALIGNED ThreadLocalSatbBuffer satb;
  // Registry link - written once at registration, then immutable.
  MutatorContext* next = nullptr;
  std::atomic<bool> live{true};   // cleared when the owning thread exits
  void** satb_storage = nullptr;  // owned; intentionally not freed (see notes)
};

// Lazily create + register this thread's context. Cheap after the first call
// (a thread_local pointer load). Registration is a lock-free push onto a global
// list the compactor traverses read-only.
MutatorContext* mutatorSelf() noexcept;

// Head of the registry (for the compactor to iterate). May be null.
MutatorContext* mutatorListHead() noexcept;

}  // namespace aevum

#endif  // AEVUM_SAFEPOINT_H_
