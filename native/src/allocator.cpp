// SPDX-License-Identifier: Apache-2.0
#include "allocator.h"

namespace aevum {

// Build a geometric size-class ladder: 16 B up to kMaxPayload, ~1.25x growth,
// each rounded up to 16 so payloads stay 16-aligned. This bounds internal
// fragmentation to <25% while keeping the class count small enough that each
// class's head sits on its own cache line.
void Allocator::initClasses(RegionHeader* hdr) noexcept {
  uint32_t n = 0;
  uint64_t sz = 16;
  while (sz <= kMaxPayload && n < kMaxClasses) {
    hdr->class_size[n++] = sz;
    uint64_t next = alignUp(sz + sz / 4, 16);
    if (next <= sz) next = sz + 16;  // guard against no-progress at small sizes
    sz = next;
  }
  // Ensure the last class can hold exactly kMaxPayload.
  if (n > 0 && hdr->class_size[n - 1] < kMaxPayload && n < kMaxClasses) {
    hdr->class_size[n++] = kMaxPayload;
  }
  for (uint32_t i = n; i < kMaxClasses; ++i) hdr->class_size[i] = 0;
  hdr->num_classes = n;
}

int Allocator::classFor(uint64_t size) const noexcept {
  if (size == 0) size = 1;
  if (size > kMaxPayload) return -1;
  // Binary search the sorted class table for the smallest class >= size.
  uint32_t lo = 0, hi = hdr_->num_classes;
  while (lo < hi) {
    uint32_t mid = lo + ((hi - lo) >> 1);
    if (hdr_->class_size[mid] < size) lo = mid + 1;
    else hi = mid;
  }
  return static_cast<int>(lo);
}

uint64_t Allocator::popFree(uint32_t cls) noexcept {
  auto& head = hdr_->live_freelist[cls];
  uint64_t old = head.load(std::memory_order_acquire);
  for (;;) {
    const uint64_t off = headOff(old);
    if (off == 0) return 0;  // empty list -> caller falls back to bump
    // Read the intrusive next from the candidate block atomically: the field is
    // concurrently written by pushFree, so a plain load would be a data race.
    // Relaxed suffices - correctness comes from the head CAS below, which only
    // succeeds if the head (and thus this node) was unchanged, proving the value
    // we read was live. __atomic builtins are used instead of std::atomic_ref
    // for portability across the NDK and host toolchains.
    const uint64_t next =
        __atomic_load_n(&at<BlockHeader>(base_, off)->intrusive_next,
                        __ATOMIC_RELAXED) & kOffMask;
    const uint64_t neu = packHead(next, headTag(old) + 1);
    if (head.compare_exchange_weak(old, neu,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
      return off;
    }
  }
}

void Allocator::pushFree(uint32_t cls, uint64_t block) noexcept {
  auto& head = hdr_->live_freelist[cls];
  auto* bh = at<BlockHeader>(base_, block);
  uint64_t old = head.load(std::memory_order_acquire);
  for (;;) {
    // Link this block ahead of the current head, then publish with a release
    // CAS so any thread that later acquires the head also sees this store. The
    // store is a relaxed atomic (not a plain store) because popFree may read
    // this field concurrently; the release on the CAS provides the ordering.
    __atomic_store_n(&bh->intrusive_next, headOff(old), __ATOMIC_RELAXED);
    const uint64_t neu = packHead(block, headTag(old) + 1);
    if (head.compare_exchange_weak(old, neu,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
      return;
    }
  }
}

uint64_t Allocator::bumpAlloc(uint64_t total) noexcept {
  auto& bump = hdr_->live_bump;
  uint64_t old = bump.load(std::memory_order_acquire);
  for (;;) {
    const uint64_t aligned = alignUp(old, 16);
    const uint64_t neu = aligned + total;
    if (neu > hdr_->region_bytes) return 0;  // arena full
    if (bump.compare_exchange_weak(old, neu,
                                   std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
      return aligned;
    }
  }
}

uint64_t Allocator::allocate(uint64_t size) noexcept {
  const int cls = classFor(size);
  if (cls < 0) return Handle::kNull;

  uint64_t block = popFree(static_cast<uint32_t>(cls));
  if (block == 0) {
    block = bumpAlloc(sizeof(BlockHeader) + hdr_->class_size[cls]);
    if (block == 0) return Handle::kNull;
  }

  auto* bh = at<BlockHeader>(base_, block);
  bh->class_idx = static_cast<uint32_t>(cls);
  bh->magic = kBlockAlloc;
  // Publish the header before the caller can observe the payload via a root.
  std::atomic_thread_fence(std::memory_order_release);
  return block + sizeof(BlockHeader);
}

bool Allocator::free(uint64_t payload_off) noexcept {
  if (payload_off < hdr_->data_start + sizeof(BlockHeader)) return false;
  const uint64_t block = payload_off - sizeof(BlockHeader);
  auto* bh = at<BlockHeader>(base_, block);
  if (bh->magic != kBlockAlloc) return false;  // double free / not an Aevum block
  const uint32_t cls = bh->class_idx;
  if (cls >= hdr_->num_classes) return false;
  bh->magic = kBlockFree;
  pushFree(cls, block);
  return true;
}

}  // namespace aevum
