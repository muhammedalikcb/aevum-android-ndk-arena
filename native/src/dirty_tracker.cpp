// SPDX-License-Identifier: Apache-2.0
#include "dirty_tracker.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#if defined(__linux__) || defined(__APPLE__)
#  include <unistd.h>
#  include <signal.h>
#  include <sys/mman.h>
#endif

namespace aevum {
namespace {

// A process-global registry the signal handler consults to decide whether a
// fault belongs to an armed arena. Fixed size and lock-free on the read path so
// the handler is async-signal-safe: it only performs atomic loads, one atomic
// fetch_or, and a single mprotect (a bare syscall with no libc state).
constexpr int kMaxSlots = 8;

struct Slot {
  std::atomic<uint8_t*> base{nullptr};   // published last, with release
  std::atomic<uint64_t> lo{0};
  std::atomic<uint64_t> hi{0};
  std::atomic<uint64_t> ps{0};           // runtime page size
  std::atomic<uint64_t>* bits = nullptr; // one bit per page from base (whole region)
  uint64_t page_shift = 12;
};

Slot g_slots[kMaxSlots];
std::atomic<uint32_t> g_reg_lock{0};      // guards arm/disarm mutation, never taken in the handler
std::atomic<bool> g_installed{false};

#if defined(__linux__) || defined(__APPLE__)
struct sigaction g_prev_segv;
struct sigaction g_prev_bus;

inline uint64_t alignDownPS(uint64_t v, uint64_t ps) { return v & ~(ps - 1); }

void chainTo(struct sigaction* prev, int sig, siginfo_t* si, void* uctx) {
  if (prev->sa_flags & SA_SIGINFO) {
    if (prev->sa_sigaction) { prev->sa_sigaction(sig, si, uctx); return; }
  } else if (prev->sa_handler == SIG_IGN) {
    return;
  } else if (prev->sa_handler != SIG_DFL) {
    prev->sa_handler(sig);
    return;
  }
  // Default action: restore the default disposition and re-raise so the process
  // dies with the correct signal instead of looping.
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  ::sigaction(sig, &dfl, nullptr);
  ::raise(sig);
}

void faultHandler(int sig, siginfo_t* si, void* uctx) {
  auto* addr = static_cast<uint8_t*>(si ? si->si_addr : nullptr);
  if (addr) {
    for (int i = 0; i < kMaxSlots; ++i) {
      uint8_t* b = g_slots[i].base.load(std::memory_order_acquire);
      if (!b) continue;
      const uint64_t lo = g_slots[i].lo.load(std::memory_order_relaxed);
      const uint64_t hi = g_slots[i].hi.load(std::memory_order_relaxed);
      uint8_t* plo = b + lo;
      uint8_t* phi = b + hi;
      if (addr >= plo && addr < phi) {
        const uint64_t ps = g_slots[i].ps.load(std::memory_order_relaxed);
        uint8_t* page = reinterpret_cast<uint8_t*>(
            alignDownPS(reinterpret_cast<uintptr_t>(addr), ps));
        const uint64_t pidx = static_cast<uint64_t>(page - b) >> g_slots[i].page_shift;
        g_slots[i].bits[pidx >> 6].fetch_or(1ull << (pidx & 63), std::memory_order_relaxed);
        // Unprotect exactly this page and let the faulting store retry.
        ::mprotect(page, static_cast<size_t>(ps), PROT_READ | PROT_WRITE);
        return;
      }
    }
  }
  chainTo(sig == SIGBUS ? &g_prev_bus : &g_prev_segv, sig, si, uctx);
}

bool installHandlerOnce() {
  bool expected = false;
  if (!g_installed.compare_exchange_strong(expected, true)) return true;  // already
  struct sigaction sa{};
  sa.sa_sigaction = faultHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
  sigemptyset(&sa.sa_mask);
  if (::sigaction(SIGSEGV, &sa, &g_prev_segv) != 0) { g_installed.store(false); return false; }
  if (::sigaction(SIGBUS, &sa, &g_prev_bus) != 0) {
    ::sigaction(SIGSEGV, &g_prev_segv, nullptr);
    g_installed.store(false);
    return false;
  }
  return true;
}
#endif  // linux || apple

void lockReg() {
  uint32_t e = 0;
  while (!g_reg_lock.compare_exchange_weak(e, 1, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) { e = 0; }
}
void unlockReg() { g_reg_lock.store(0, std::memory_order_release); }

}  // namespace

Status DirtyTracker::arm(void* base, uint64_t lo, uint64_t hi) noexcept {
#if defined(__linux__) || defined(__APPLE__)
  if (armed()) return Status::kAlreadyAttached;
  const uint64_t ps = static_cast<uint64_t>(::sysconf(_SC_PAGESIZE));
  if (ps == 0 || (ps & (ps - 1))) return Status::kUnsupported;  // require power-of-two page
  const uint64_t plo = lo & ~(ps - 1);
  const uint64_t phi = (hi + ps - 1) & ~(ps - 1);
  if (phi <= plo) return Status::kInvalidArg;

  if (!installHandlerOnce()) return Status::kProbeFailed;

  // Bit per page across the WHOLE region so later bump growth needs no realloc.
  // We only know [plo,phi) now; size to phi (the current high-water) which the
  // arena caps at region_bytes.
  const uint64_t pages = phi >> __builtin_ctzll(ps);
  const uint64_t words = (pages + 63) / 64;
  auto* bits = static_cast<std::atomic<uint64_t>*>(std::calloc(words, sizeof(std::atomic<uint64_t>)));
  if (!bits) return Status::kNoMemory;

  lockReg();
  int slot = -1;
  for (int i = 0; i < kMaxSlots; ++i)
    if (g_slots[i].base.load(std::memory_order_relaxed) == nullptr) { slot = i; break; }
  if (slot < 0) { unlockReg(); std::free(bits); return Status::kNoMemory; }

  g_slots[slot].lo.store(plo, std::memory_order_relaxed);
  g_slots[slot].hi.store(phi, std::memory_order_relaxed);
  g_slots[slot].ps.store(ps, std::memory_order_relaxed);
  g_slots[slot].page_shift = static_cast<uint64_t>(__builtin_ctzll(ps));
  g_slots[slot].bits = bits;
  g_slots[slot].base.store(static_cast<uint8_t*>(base), std::memory_order_release);
  unlockReg();

  slot_ = slot; base_ = base; lo_ = plo; hi_ = phi;
  if (::mprotect(static_cast<uint8_t*>(base) + plo, static_cast<size_t>(phi - plo),
                 PROT_READ) != 0) {
    disarm();
    return Status::kProbeFailed;
  }
  return Status::kOk;
#else
  (void)base; (void)lo; (void)hi;
  return Status::kUnsupported;
#endif
}

void DirtyTracker::drainAndRearm(uint64_t new_hi, void* ctx, EmitFn emit) noexcept {
#if defined(__linux__) || defined(__APPLE__)
  if (!armed()) return;
  Slot& s = g_slots[slot_];
  const uint64_t ps = s.ps.load(std::memory_order_relaxed);
  const uint64_t shift = s.page_shift;

  // Unprotect the whole tracked range so the emit/msync below can read freely
  // and so newly grown pages are writable while we re-arm.
  ::mprotect(static_cast<uint8_t*>(base_) + lo_, static_cast<size_t>(hi_ - lo_),
             PROT_READ | PROT_WRITE);

  // Mark bump growth [hi_, new_hi) dirty.
  uint64_t grow_hi = hi_;
  if (new_hi > hi_) {
    const uint64_t gphi = (new_hi + ps - 1) & ~(ps - 1);
    for (uint64_t off = hi_; off < gphi; off += ps) {
      const uint64_t pidx = off >> shift;
      s.bits[pidx >> 6].fetch_or(1ull << (pidx & 63), std::memory_order_relaxed);
    }
    grow_hi = gphi;
  }

  // Emit + clear every dirty page, coalescing contiguous runs into one call.
  const uint64_t pages = grow_hi >> shift;
  uint64_t run_start = 0, run_len = 0;
  for (uint64_t p = lo_ >> shift; p < pages; ++p) {
    const uint64_t w = p >> 6, b = 1ull << (p & 63);
    if (s.bits[w].load(std::memory_order_relaxed) & b) {
      s.bits[w].fetch_and(~b, std::memory_order_relaxed);
      if (run_len == 0) run_start = p << shift;
      run_len += ps;
    } else if (run_len) {
      emit(ctx, run_start, run_len);
      run_len = 0;
    }
  }
  if (run_len) emit(ctx, run_start, run_len);

  // Extend coverage and re-protect everything for the next epoch.
  hi_ = grow_hi;
  s.hi.store(hi_, std::memory_order_relaxed);
  ::mprotect(static_cast<uint8_t*>(base_) + lo_, static_cast<size_t>(hi_ - lo_), PROT_READ);
#else
  (void)new_hi; (void)ctx; (void)emit;
#endif
}

void DirtyTracker::disarm() noexcept {
#if defined(__linux__) || defined(__APPLE__)
  if (!armed()) return;
  ::mprotect(static_cast<uint8_t*>(base_) + lo_, static_cast<size_t>(hi_ - lo_),
             PROT_READ | PROT_WRITE);
  lockReg();
  Slot& s = g_slots[slot_];
  s.base.store(nullptr, std::memory_order_release);
  std::atomic<uint64_t>* bits = s.bits;
  s.bits = nullptr;
  unlockReg();
  std::free(bits);
  slot_ = -1; base_ = nullptr; lo_ = hi_ = 0;
#endif
}

}  // namespace aevum
