// SPDX-License-Identifier: Apache-2.0
// Aevum - incremental checkpoint via write-protect fault tracking.
//
// commit() in full-flush mode msyncs the whole region; for a large arena with
// few mutated pages that is wasteful. DirtyTracker arms write-protection over
// the live data pages and installs a chained SIGSEGV/SIGBUS handler. The first
// write to a protected page faults; the handler records the page as dirty and
// unprotects it (async-signal-safe: atomics + mprotect only), then the write
// retries transparently. The next commit flushes only the dirty set.
//
// This is the same fault-driven page-servicing family ART's CMC collector uses,
// but built on mprotect + sigaction - both reachable from an unprivileged app,
// unlike userfaultfd, which the app seccomp policy blocks. It is strictly
// OPT-IN and degrades to full-flush if the platform refuses handler install.

#ifndef AEVUM_DIRTY_TRACKER_H_
#define AEVUM_DIRTY_TRACKER_H_

#include <cstdint>
#include <atomic>

#include "platform.h"

namespace aevum {

class DirtyTracker {
 public:
  DirtyTracker() noexcept = default;
  ~DirtyTracker() noexcept { disarm(); }
  DirtyTracker(const DirtyTracker&) = delete;
  DirtyTracker& operator=(const DirtyTracker&) = delete;

  // Movable: the global registry slot is keyed by index + mapping base, neither
  // of which changes when the owning Arena object moves, so transferring the
  // handle fields (and neutralizing the source) is sound.
  DirtyTracker(DirtyTracker&& o) noexcept
      : slot_(o.slot_), base_(o.base_), lo_(o.lo_), hi_(o.hi_) {
    o.slot_ = -1; o.base_ = nullptr; o.lo_ = o.hi_ = 0;
  }
  DirtyTracker& operator=(DirtyTracker&& o) noexcept {
    if (this != &o) {
      disarm();
      slot_ = o.slot_; base_ = o.base_; lo_ = o.lo_; hi_ = o.hi_;
      o.slot_ = -1; o.base_ = nullptr; o.lo_ = o.hi_ = 0;
    }
    return *this;
  }

  // Arm tracking over [base+lo, base+hi), page-aligned. Installs the global
  // handler on first use. Returns kProbeFailed (non-fatal) if mprotect or
  // sigaction is refused; the caller then keeps full-flush semantics.
  Status arm(void* base, uint64_t lo, uint64_t hi) noexcept;

  // Invoke emit(ctx, page_offset_from_base, page_len) for every page dirtied
  // since arm()/the previous drain, plus the freshly grown range [prev_hi,
  // new_hi) from bump allocation, then re-protect everything and clear the
  // dirty set. `new_hi` extends coverage to include that bump growth.
  using EmitFn = void (*)(void* ctx, uint64_t off, uint64_t len);
  void drainAndRearm(uint64_t new_hi, void* ctx, EmitFn emit) noexcept;

  void disarm() noexcept;
  bool armed() const noexcept { return slot_ >= 0; }

 private:
  friend struct DirtyRegistry;
  int slot_ = -1;          // index into the global registry, -1 if unarmed
  void* base_ = nullptr;
  uint64_t lo_ = 0, hi_ = 0;
};

}  // namespace aevum

#endif  // AEVUM_DIRTY_TRACKER_H_
