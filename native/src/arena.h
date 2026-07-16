// SPDX-License-Identifier: Apache-2.0
// Aevum - the arena: RAII owner of a mapped, GC-invisible, persistable region.
//
// An Arena owns exactly one backing object (a private file or a memfd) and one
// MAP_SHARED mapping of it. Because the mapping is anonymous-shared/file-backed
// and never registered with the ART heap, the ART Concurrent Mark-Compact
// collector never marks, scans, or relocates a single byte of it: an object
// graph of any size costs zero GC root-scan time and produces zero compaction
// demand-faults. GC work is conserved - it can be relocated but not reduced -
// so the only way to eliminate it is to keep the objects off the heap entirely.
//
// The region is position-independent (all links are self-relative or
// base-relative), so after process death it re-maps at a fresh ASLR base in
// O(1) with no deserialization and no pointer fixup.

#ifndef AEVUM_ARENA_H_
#define AEVUM_ARENA_H_

#include <cstdint>

#include "allocator.h"
#include "dirty_tracker.h"
#include "region_layout.h"

namespace aevum {

class Arena {
 public:
  Arena() noexcept = default;
  ~Arena() noexcept { close(); }

  // Move-only: an Arena is a unique owner of an fd + mapping.
  Arena(Arena&& o) noexcept { moveFrom(o); }
  Arena& operator=(Arena&& o) noexcept { if (this != &o) { close(); moveFrom(o); } return *this; }
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;

  // --- construction -------------------------------------------------------
  // File-backed: durable across process death AND device reboot. `size_bytes`
  // is rounded up to a page. Works on host and Android.
  static Status createFile(const char* path, uint64_t size_bytes, Arena* out) noexcept;
  static Status attachFile(const char* path, Arena* out) noexcept;

  // memfd-backed (Linux/Android only): lives on tmpfs, survives process death
  // only while some process holds the returned fd (hand off via SCM_RIGHTS or
  // inherit across fork). Faster than file-backed; no reboot durability.
  static Status createMemfd(const char* name, uint64_t size_bytes, Arena* out) noexcept;
  static Status attachFd(int fd, Arena* out) noexcept;  // reattach a held/inherited memfd

  bool valid() const noexcept { return base_ != nullptr; }
  int  fd()    const noexcept { return fd_; }             // for memfd handoff
  uint64_t sizeBytes() const noexcept { return size_; }
  const void* base() const noexcept { return base_; }

  // --- managed-heap backing hooks (used by Aevum-Defrag) ------------------
  // These expose exactly what the copying compactor needs to run its two
  // semispaces inside this region without touching the raw allocator. An arena
  // used as a managed-heap backing is not also used for raw allocate().
  void* mutableBase() noexcept { return base_; }
  uint64_t dataStart() const noexcept { return hdr_ ? hdr_->data_start : 0; }
  uint64_t regionBytes() const noexcept { return hdr_ ? hdr_->region_bytes : 0; }
  // Freeze the raw allocator so a stray allocate() cannot collide with managed
  // semispaces carved from the same region.
  void reserveAllForManaged() noexcept {
    if (hdr_) hdr_->live_bump.store(hdr_->region_bytes, std::memory_order_release);
  }

  // --- allocation ---------------------------------------------------------
  Handle allocate(uint64_t size) noexcept { return Handle{ alloc_.allocate(size) }; }
  bool   free(Handle h) noexcept { return h.isNull() ? false : alloc_.free(h.off); }

  // Resolve a durable handle to an absolute payload pointer in THIS mapping.
  // The pointer is valid only for the lifetime of this Arena instance.
  void* resolve(Handle h) noexcept {
    return h.isNull() ? nullptr : at<void>(base_, h.off);
  }

  // Usable payload capacity of an allocated block (its size class), or 0 if the
  // handle is null / not a live Aevum block. Used to size a zero-copy view.
  uint64_t capacityOf(Handle h) noexcept {
    if (!valid() || h.isNull() || h.off < hdr_->data_start + sizeof(BlockHeader)) return 0;
    auto* bh = at<BlockHeader>(base_, h.off - sizeof(BlockHeader));
    if (bh->magic != kBlockAlloc || bh->class_idx >= hdr_->num_classes) return 0;
    return hdr_->class_size[bh->class_idx];
  }

  // --- durable graph roots ------------------------------------------------
  Status setRoot(uint32_t idx, Handle h) noexcept;
  Handle getRoot(uint32_t idx) noexcept;

  // --- durability ---------------------------------------------------------
  // Snapshot live control state + roots into the inactive checkpoint slot,
  // flush, then atomically publish it. Torn-write-safe and crash-consistent.
  Status commit() noexcept;

  // Opt in to incremental checkpointing: subsequent commit()s flush only pages
  // written since the previous commit (tracked via write-protect faults),
  // instead of the whole region. Returns kProbeFailed (non-fatal) if the
  // platform refuses handler install; commit() then stays in full-flush mode.
  // Trade-off: the first write to each page per epoch takes a fault, so this
  // wins only for large arenas committed frequently with a small working set.
  Status enableIncrementalCheckpoint() noexcept;
  bool incrementalCheckpoint() const noexcept { return dirty_.armed(); }

  uint64_t bytesInUse() noexcept { return alloc_.bytesInUse(); }

  // Explicit teardown (also run by the destructor).
  void close() noexcept;

 private:
  void moveFrom(Arena& o) noexcept;
  Status mapAndBind(int fd, uint64_t size, bool creating) noexcept;
  void   formatFresh() noexcept;
  Status adoptExisting() noexcept;  // validate + restore live state from newest commit

  int fd_ = -1;
  bool owns_unlink_ = false;
  char path_[512] = {0};   // file-backed path; retained for diagnostics only
  void* base_ = nullptr;
  uint64_t size_ = 0;
  RegionHeader* hdr_ = nullptr;
  Allocator alloc_{};
  DirtyTracker dirty_{};
};

}  // namespace aevum

#endif  // AEVUM_ARENA_H_
