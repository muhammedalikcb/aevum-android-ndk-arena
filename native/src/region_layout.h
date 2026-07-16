// SPDX-License-Identifier: Apache-2.0
// Aevum - on-region binary layout.
//
// This is the single source of truth for what lives inside a mapped Aevum
// region. Because the region is persisted verbatim (memfd/tmpfs or a private
// file) and re-mapped at an arbitrary base after process death, every field is
// either position-independent or a base-relative offset. There are NO raw
// pointers in the region. The layout is fixed-size and versioned so a region
// written by one build maps in another.
//
// Durability model (stated honestly): the LIVE control block is mutated
// lock-free by allocators at runtime. A durable snapshot is taken only at
// commit(); a crash loses allocations made since the last commit but never
// corrupts the region, because commit is double-buffered and CRC-guarded.

#ifndef AEVUM_REGION_LAYOUT_H_
#define AEVUM_REGION_LAYOUT_H_

#include <cstdint>
#include <atomic>

#include "platform.h"
#include "offset_ptr.h"

namespace aevum {

inline constexpr uint64_t kMagic       = 0x4145'5655'4d31'0000ull;  // "AEVUM1\0\0"
inline constexpr uint32_t kFormatVer   = 1;
inline constexpr uint32_t kMaxClasses  = 64;   // segregated size classes (actual count <= this)
inline constexpr uint32_t kNumRoots    = 64;   // named durable graph entry points
inline constexpr uint32_t kMaxPayload  = 65536;  // largest single object; graphs compose smaller nodes

// Block header prefix on every allocation. 16 bytes so the payload that follows
// is 16-byte aligned. Bytes [0..7] are reused as the intrusive free-list "next"
// while the block sits on a free list; the class/magic in [8..15] are never
// touched by the intrusive link, which is what makes O(1) double-free detection
// coexist with the lock-free Treiber stack.
struct BlockHeader {
  uint64_t intrusive_next;  // base-relative offset of next free block (free state only)
  uint32_t magic;           // kBlockAlloc / kBlockFree - best-effort double-free guard
  uint32_t class_idx;       // owning size class
};
static_assert(sizeof(BlockHeader) == 16, "payload alignment depends on a 16-byte header");

inline constexpr uint32_t kBlockAlloc = 0xA11C'A7EDu;
inline constexpr uint32_t kBlockFree  = 0xF6EE'B10Cu;

// A packed free-list head: 48-bit block offset + 16-bit ABA tag in one word so
// the whole head swings with a single 64-bit CAS. Offset 0 means "empty" (the
// header occupies offset 0, so no real block is ever there).
inline constexpr uint64_t kOffMask = 0x0000'FFFF'FFFF'FFFFull;  // 48 bits -> 256 TB reach
inline constexpr uint64_t kTagInc  = 0x0001'0000'0000'0000ull;

inline uint64_t packHead(uint64_t off, uint64_t tag) noexcept {
  return (off & kOffMask) | (tag << 48);
}
inline uint64_t headOff(uint64_t packed) noexcept { return packed & kOffMask; }
inline uint64_t headTag(uint64_t packed) noexcept { return packed >> 48; }

// Durable checkpoint. Two of these are double-buffered; the newest one whose
// CRC validates is authoritative on attach. Everything here is a snapshot of
// the live control block plus the root table.
struct CommitSlot {
  uint64_t generation;               // monotonic; newest valid wins
  uint64_t bump;                     // high-water allocation offset
  uint64_t freelist[kMaxClasses];    // per-class head offsets (tag dropped; irrelevant once quiesced)
  uint64_t root[kNumRoots];          // named entry-point offsets
  uint32_t crc;                      // crc32c over this slot with crc field zeroed
  uint32_t reserved;
};

// The region header. Cache-line padding isolates each hot atomic so unrelated
// allocator classes never false-share across the big<->LITTLE coherency
// boundary, where a line transfer costs asymmetrically more than within a
// cluster.
struct RegionHeader {
  // --- immutable identity (written once at create) ---
  uint64_t magic;
  uint32_t format_version;
  uint32_t flags;
  uint64_t region_bytes;
  uint64_t data_start;               // first allocatable offset (page-aligned)
  uint32_t num_classes;              // actual populated size-class count
  uint32_t reserved0;
  uint64_t class_size[kMaxClasses];  // payload capacity of each class (excludes 16B header)

  // --- live, lock-free control block (working state) ---
  AEVUM_CACHELINE_ALIGNED std::atomic<uint64_t> live_bump;
  AEVUM_CACHELINE_ALIGNED std::atomic<uint64_t> live_freelist[kMaxClasses];
  AEVUM_CACHELINE_ALIGNED std::atomic<uint64_t> live_root[kNumRoots];
  AEVUM_CACHELINE_ALIGNED std::atomic<uint32_t> commit_lock;  // 0=free,1=held (futex-style spin)
  AEVUM_CACHELINE_ALIGNED std::atomic<uint64_t> active_commit; // index of newest durable slot

  // --- durable checkpoints ---
  CommitSlot commit[2];
};

// Resolve a base-relative offset to a typed pointer within a mapping.
template <typename T>
inline T* at(void* base, uint64_t off) noexcept {
  return reinterpret_cast<T*>(static_cast<uint8_t*>(base) + off);
}
template <typename T>
inline const T* at(const void* base, uint64_t off) noexcept {
  return reinterpret_cast<const T*>(static_cast<const uint8_t*>(base) + off);
}

inline uint64_t alignUp(uint64_t v, uint64_t a) noexcept { return (v + (a - 1)) & ~(a - 1); }

}  // namespace aevum

#endif  // AEVUM_REGION_LAYOUT_H_
