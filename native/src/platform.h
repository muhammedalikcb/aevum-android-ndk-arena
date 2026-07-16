// SPDX-License-Identifier: Apache-2.0
// Aevum - platform abstraction layer.
//
// Everything OS/arch specific funnels through here so the algorithmic core
// (arena, allocator, offset pointers, swizzle) stays pure and host-verifiable.
// The same translation units compile for:
//   * aarch64-linux-android  (the real target)
//   * the host (darwin/linux) for correctness verification of the core
//
// Nothing here allocates on the JVM heap, takes a JNI call, or blocks the
// caller. Timing and atomics degrade to portable primitives off aarch64.

#ifndef AEVUM_PLATFORM_H_
#define AEVUM_PLATFORM_H_

#include <cstddef>
#include <cstdint>
#include <atomic>

#if defined(__linux__)
#  include <unistd.h>
#  include <sys/mman.h>
#  include <sys/syscall.h>
#  include <fcntl.h>
#  include <cerrno>
#  include <ctime>
#endif

#if defined(__APPLE__)
#  include <unistd.h>
#  include <sys/mman.h>
#  include <mach/mach_time.h>
#  include <cerrno>
#endif

namespace aevum {

// ---- Cache geometry ------------------------------------------------------
// CTR_EL0 reports 64-byte lines on every A14/15-class ARM core. We hard-code
// the conservative-correct value: over-aligning to 64 never introduces false
// sharing, and cross-cluster (big<->LITTLE) coherency traffic is the exact
// asymmetric cost this padding exists to avoid.
inline constexpr std::size_t kCacheLine = 64;
inline constexpr std::size_t kPageSize  = 4096;  // AOSP guarantees 4K for app anon; 16K devices handled at map time.

// Force a field onto its own cache line.
#define AEVUM_CACHELINE_ALIGNED alignas(::aevum::kCacheLine)

// ---- Monotonic timestamp -------------------------------------------------
// On aarch64 we read CNTVCT_EL0 directly (the RDTSC-equivalent, ~2-4ns, no
// syscall, async-signal-safe - critical because the swizzle fault handler
// needs a timestamp without touching libc). cntfrq_el0 gives the tick rate
// (typically 19.2MHz on Qualcomm). Off aarch64 we fall back to a monotonic
// clock so host verification still produces real numbers.
#if defined(__aarch64__)
inline uint64_t rawTicks() noexcept {
  uint64_t v;
  asm volatile("mrs %0, cntvct_el0" : "=r"(v));
  return v;
}
inline uint64_t ticksPerSecond() noexcept {
  uint64_t f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f ? f : 19200000ull;
}
inline uint64_t nowNanos() noexcept {
  // Scale ticks->ns without overflow: (ticks / freq)*1e9 + remainder.
  const uint64_t f = ticksPerSecond();
  const uint64_t t = rawTicks();
  return (t / f) * 1000000000ull + ((t % f) * 1000000000ull) / f;
}
#elif defined(__APPLE__)
inline uint64_t nowNanos() noexcept {
  static mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t x{};
    mach_timebase_info(&x);
    return x;
  }();
  return mach_absolute_time() * tb.numer / tb.denom;
}
inline uint64_t rawTicks() noexcept { return nowNanos(); }
#else
inline uint64_t nowNanos() noexcept {
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}
inline uint64_t rawTicks() noexcept { return nowNanos(); }
#endif

// ---- CRC32C (Castagnoli) -------------------------------------------------
// Used for torn-write detection on the arena superblock and per-region
// integrity. ARMv8-A CRC32 extension when present; a correct, table-free
// software fallback otherwise. Both compute the identical Castagnoli CRC so a
// region written on-device verifies on host and vice versa.
namespace detail {

inline uint32_t crc32c_sw(uint32_t crc, const void* data, std::size_t len) noexcept {
  // Castagnoli polynomial, reflected: 0x82F63B78. Bit-serial but exact;
  // only used where the hardware path is unavailable (host verification).
  const auto* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0x82F63B78u & (~(crc & 1u) + 1u));
    }
  }
  return ~crc;
}

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
inline uint32_t crc32c_hw(uint32_t crc, const void* data, std::size_t len) noexcept {
  const auto* p = static_cast<const uint8_t*>(data);
  while (len >= 8) {
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    crc = __builtin_arm_crc32cd(crc, v);
    p += 8; len -= 8;
  }
  if (len >= 4) { uint32_t v; __builtin_memcpy(&v, p, 4); crc = __builtin_arm_crc32cw(crc, v); p += 4; len -= 4; }
  if (len >= 2) { uint16_t v; __builtin_memcpy(&v, p, 2); crc = __builtin_arm_crc32ch(crc, v); p += 2; len -= 2; }
  if (len >= 1) { crc = __builtin_arm_crc32cb(crc, *p); }
  return crc;
}
#endif

}  // namespace detail

// crc==0 seed convention; returns the finalized Castagnoli CRC32C.
inline uint32_t crc32c(const void* data, std::size_t len) noexcept {
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
  return detail::crc32c_hw(0u, data, len);
#else
  return detail::crc32c_sw(0u, data, len);
#endif
}

// ---- Memory-fence helpers (documented intent at call sites) --------------
// ARM64 has a weak memory model; every cross-thread publish/consume in the
// allocator and swizzle path names its ordering explicitly. These wrappers
// exist so the intent reads clearly in the algorithmic code.
inline void publishFence()  noexcept { std::atomic_thread_fence(std::memory_order_release); }
inline void consumeFence()  noexcept { std::atomic_thread_fence(std::memory_order_acquire); }
inline void fullFence()     noexcept { std::atomic_thread_fence(std::memory_order_seq_cst); }

// ---- Result type ---------------------------------------------------------
// The core never throws across the C ABI / JNI boundary. Every fallible
// operation returns a status; the JNI layer maps it to a Kotlin result.
enum class Status : int32_t {
  kOk               = 0,
  kNoMemory         = -1,   // arena exhausted / size class full
  kInvalidArg       = -2,
  kUnsupported      = -3,   // platform lacks a required syscall (e.g. memfd)
  kCorrupt          = -4,   // CRC / generation mismatch on attach
  kAlreadyAttached  = -5,
  kIoError          = -6,
  kProbeFailed      = -7,   // optional accelerator (uffd) unavailable - non-fatal
};

inline bool ok(Status s) noexcept { return s == Status::kOk; }

}  // namespace aevum

#endif  // AEVUM_PLATFORM_H_
