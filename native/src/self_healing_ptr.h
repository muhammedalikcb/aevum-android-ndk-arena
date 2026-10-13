// SPDX-License-Identifier: Apache-2.0
// Aevum-Defrag - self-healing reference with concurrent-compaction barriers.
//
// SelfHealingOffsetPtr is the ONLY reference type the compactor understands. It
// is a self-relative offset (like OffsetPtr) plus two barriers that make a
// no-stop-the-world copying compaction correct:
//
//   * READ barrier (resolvePayload): if a compaction cycle has begun evacuating
//     and this reference points into from-space, follow the target object's
//     forwarding chain to its to-space copy and atomically self-heal the stored
//     offset. Wait-free: a bounded chain walk plus one relaxed store.
//   * SATB WRITE barrier (storeRef): while marking is in progress, log the
//     overwritten (old) reference so a concurrently deleted edge cannot hide a
//     still-live object from the collector (snapshot-at-the-beginning).
//
// When no cycle is active - the overwhelming common case - both barriers are a
// single relaxed load of the global cycle pointer (null) plus the plain offset
// resolve. There is no lock anywhere on any mutator path.

#ifndef AEVUM_SELF_HEALING_PTR_H_
#define AEVUM_SELF_HEALING_PTR_H_

#include <cstdint>
#include <atomic>

#include "platform.h"
#include "type_registry.h"
#include "safepoint.h"

namespace aevum {

// Reuse the OffsetPtr null convention (offset 1 is unreachable for any aligned
// target, and 0 is a legal self-reference).
inline constexpr int64_t kShNull = 1;

enum class Phase : uint32_t { kIdle = 0, kMark = 1, kEvac = 2, kFixup = 3 };

// The shared state of an in-progress compaction cycle. One per compacting heap.
// Deliberately just a phase word: the SATB deletion barrier writes to the
// mutator's OWN ThreadLocalSatbBuffer (no shared, contended buffer), so `phase`
// is the only cycle field a mutator barrier reads.
struct alignas(kCacheLine) CycleContext {
  std::atomic<Phase> phase{Phase::kIdle};
};

// A single active cycle at a time across the process (compaction is a heavy
// background operation; heaps compact one at a time). Barriers gate on this
// being non-null, so the inactive path is one relaxed load.
inline std::atomic<CycleContext*>& activeCycle() noexcept {
  static std::atomic<CycleContext*> g{nullptr};
  return g;
}

// Non-templated base carrying the offset word and both barriers so a trace
// callback can operate on any reference field uniformly (it receives a
// SelfHealingRefBase*).
class SelfHealingRefBase {
 public:
  // READ barrier. Returns the absolute payload pointer of the CURRENT canonical
  // copy of the target (following the forwarding chain during evacuation), or
  // nullptr. It is strictly READ-ONLY: it never writes a field. That is what
  // keeps from-space quiescent for the whole cycle - the compactor's memcpy of a
  // from-space object can never race a barrier write - so no stop-the-world is
  // needed and no shared per-object "healed" state exists to corrupt. Proactive
  // fixup (Phase 3) is what durably rewrites the pointers; this just redirects
  // reads in the meantime. Objects that are not forwarded (fwd == 0), i.e. every
  // object once no cycle is active, cost a single extra acquire load.
  void* resolvePayload() noexcept {
    const int64_t r = rel_.load(std::memory_order_acquire);
    if (r == kShNull) return nullptr;
    auto* payload = reinterpret_cast<uint8_t*>(this) + r;

    CycleContext* c = activeCycle().load(std::memory_order_acquire);
    if (c != nullptr) {
      const Phase ph = c->phase.load(std::memory_order_acquire);
      if (ph == Phase::kEvac || ph == Phase::kFixup) {
        for (;;) {
          auto* h = reinterpret_cast<ObjectHeader*>(payload - sizeof(ObjectHeader));
          const int64_t f = h->fwd.load(std::memory_order_acquire);
          if (f == ObjectHeader::kNotForwarded) break;
          payload += f;
        }
      }
    }
    return payload;
  }

  // SATB WRITE barrier. The field being written MUST belong to an object taken
  // for writing via resolveMutable() (i.e. a to-space / canonical object), and
  // `canonical_payload` MUST be a canonical target from resolveRead/resolveMutable.
  void storeRef(void* canonical_payload) noexcept {
    CycleContext* c = activeCycle().load(std::memory_order_acquire);
    if (c != nullptr && c->phase.load(std::memory_order_acquire) == Phase::kMark) {
      // Snapshot-at-the-beginning: log the edge we are about to delete so a
      // concurrently unlinked-then-relinked object cannot escape marking. The log
      // goes to THIS thread's own SATB ring - no shared buffer, no CAS. Logging a
      // to-space target too is harmless (the compactor marks it and never
      // evacuates to-space), so no bounds test is needed.
      const int64_t r = rel_.load(std::memory_order_acquire);
      if (r != kShNull) {
        if (MutatorContext* m = mutatorSelf())
          m->satb.push(reinterpret_cast<uint8_t*>(this) + r);
      }
    }
    const int64_t nr = canonical_payload
        ? (reinterpret_cast<uint8_t*>(canonical_payload) - reinterpret_cast<uint8_t*>(this))
        : kShNull;
    rel_.store(nr, std::memory_order_release);
  }

  bool isNull() const noexcept { return rel_.load(std::memory_order_relaxed) == kShNull; }

  // --- compactor-only raw accessors (bypass the barriers) ---------------
  int64_t rawRel() const noexcept { return rel_.load(std::memory_order_acquire); }
  void rawSetRel(int64_t r) noexcept { rel_.store(r, std::memory_order_release); }
  // The uncorrected target payload (no forwarding follow) - used by the
  // compactor while it holds the invariant that from-space is quiescent.
  void* rawTarget() noexcept {
    const int64_t r = rel_.load(std::memory_order_acquire);
    return r == kShNull ? nullptr : reinterpret_cast<uint8_t*>(this) + r;
  }

 protected:
  std::atomic<int64_t> rel_{kShNull};
};
static_assert(sizeof(SelfHealingRefBase) == sizeof(int64_t),
              "reference must be one 64-bit word for stable on-region layout");
static_assert(sizeof(SelfHealingRefBase) == kRefFieldBytes &&
              alignof(SelfHealingRefBase) == kRefFieldBytes,
              "TypeRegistry validates field offsets against this size and alignment");

// Typed convenience wrapper. get()/set() carry the barriers; the payload type T
// is the user's object struct.
template <typename T>
class SelfHealingOffsetPtr : public SelfHealingRefBase {
 public:
  T* get() noexcept { return reinterpret_cast<T*>(resolvePayload()); }
  void set(T* canonical_payload) noexcept { storeRef(canonical_payload); }
  void clear() noexcept { storeRef(nullptr); }
};

}  // namespace aevum

#endif  // AEVUM_SELF_HEALING_PTR_H_
