// SPDX-License-Identifier: Apache-2.0
// Aevum - lock-free segregated-fit allocator over a mapped region.
//
// Every allocation is rounded up to one of a fixed set of size classes; each
// class owns an independent lock-free Treiber stack, so allocation and free are
// wait-free-ish (a CAS loop with no lock, no syscall, no GC interaction).
// Exhausting a class's free list falls back to a single lock-free bump pointer.
//
// The allocator is non-owning: it operates on a RegionHeader that lives inside
// a mapping owned by the Arena. It holds no per-thread state and no JVM handle,
// so it is safe to call from any thread, including one spawned in native code.

#ifndef AEVUM_ALLOCATOR_H_
#define AEVUM_ALLOCATOR_H_

#include <cstdint>

#include "region_layout.h"

namespace aevum {

class Allocator {
 public:
  Allocator() noexcept = default;
  Allocator(void* base, RegionHeader* hdr) noexcept : base_(base), hdr_(hdr) {}

  // Populate the size-class table on a freshly created region.
  static void initClasses(RegionHeader* hdr) noexcept;

  // Returns the base-relative offset of a 16-byte-aligned payload of at least
  // `size` bytes, or 0 (Handle::kNull) if the arena is exhausted or the request
  // exceeds kMaxPayload. The payload is NOT zeroed on the fast (recycled) path;
  // freshly bumped memory is zero because the region is mmap'd zero-fill.
  uint64_t allocate(uint64_t size) noexcept;

  // Returns true on success, false on double-free / corruption (best effort).
  bool free(uint64_t payload_off) noexcept;

  uint64_t bytesInUse() const noexcept {
    return hdr_->live_bump.load(std::memory_order_relaxed) - hdr_->data_start;
  }

 private:
  int classFor(uint64_t size) const noexcept;      // -> class idx or -1
  uint64_t popFree(uint32_t cls) noexcept;         // -> block offset or 0
  void     pushFree(uint32_t cls, uint64_t block) noexcept;
  uint64_t bumpAlloc(uint64_t total) noexcept;     // -> block offset or 0

  void* base_ = nullptr;
  RegionHeader* hdr_ = nullptr;
};

}  // namespace aevum

#endif  // AEVUM_ALLOCATOR_H_
