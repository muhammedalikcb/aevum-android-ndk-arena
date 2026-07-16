// SPDX-License-Identifier: Apache-2.0
// Aevum-Defrag - app-owned concurrent copying (mark-compact) engine.
//
// A managed heap is two semispaces carved from an Arena's region. Objects are
// bump-allocated into the active space; there is no per-object free, because
// reclamation is compaction: compact() evacuates the live graph into the other
// space and flips, leaving all dead objects behind at once.
//
// compact() runs WITHOUT stopping the world. Mutators keep reading and writing
// at full rate; correctness comes from the barriers in SelfHealingOffsetPtr plus
// two invariants:
//   1. During a cycle, NEW allocations and all mutator WRITES land in to-space
//      (allocation is flipped to to-space at cycle start; resolveMutable()
//      evacuates-on-write). Therefore from-space is quiescent (read-only) for the
//      whole cycle, so copying it is always consistent.
//   2. The DURABLE commit still names from-space until the cycle fully completes.
//      A crash mid-compaction leaves from-space intact and the durable header
//      pointing at it, so attach() recovers the exact pre-compaction graph and
//      the half-built to-space is ignored.
//
// Concurrency contract for the mutator (the same barrier contract every managed
// runtime imposes, made explicit here): reach objects only through ManagedRef /
// SelfHealingOffsetPtr; take a WRITE pointer with resolveMutable() and a READ
// pointer with resolveRead(); do not cache a raw payload pointer across an
// alloc() or compact(). Under this contract the engine loses no update and never
// exposes a dangling reference.

#ifndef AEVUM_COMPACTOR_H_
#define AEVUM_COMPACTOR_H_

#include <cstdint>
#include <atomic>

#include "arena.h"
#include "self_healing_ptr.h"
#include "type_registry.h"

namespace aevum {

inline constexpr uint64_t kManagedMagic  = 0x4145'5644'4652'4731ull;  // "AEVDFRG1"
inline constexpr uint32_t kManagedRoots  = 32;
inline constexpr uint64_t kSpaceBit      = 1ull << 63;         // top bit of the packed alloc word
inline constexpr uint64_t kBumpMask      = ~kSpaceBit;

// Durable checkpoint of the managed heap. Double-buffered exactly like the
// arena's CommitSlot; the newest CRC-valid one wins on attach.
struct ManagedCommit {
  uint64_t generation;
  uint64_t active_space;              // 0/1 - the space holding the live graph
  uint64_t bump;                      // alloc high-water within active_space
  uint64_t root[kManagedRoots];       // durable managed graph roots (payload offsets)
  uint32_t crc;
  uint32_t reserved;
};

// In-region managed-heap header. Placed at the arena's data_start. Live fields
// are working state (reset from the newest ManagedCommit on attach); the commit
// slots are the crash-consistent truth.
struct alignas(kCacheLine) ManagedHeader {
  uint64_t magic;
  uint64_t version;
  uint64_t space_bytes;              // capacity of EACH semispace
  uint64_t space_off[2];             // base-relative start of each semispace

  // Packed live allocation cursor: bit63 = space index, bits0..62 = base-relative
  // bump. Packed so the space+cursor switch at cycle start is a single atomic
  // store no mutator can tear.
  AEVUM_CACHELINE_ALIGNED std::atomic<uint64_t> live_alloc;
  AEVUM_CACHELINE_ALIGNED std::atomic<uint64_t> live_root[kManagedRoots];
  AEVUM_CACHELINE_ALIGNED std::atomic<uint64_t> active_commit;
  ManagedCommit commit[2];
};

// A growable, off-arena scratch list the compactor uses for its mark worklist
// and evacuation record. Backed by malloc (compactor thread only; never on the
// JVM heap, never in the arena).
class OffsetVec {
 public:
  ~OffsetVec() noexcept { std::free(data_); }
  bool push(uint64_t v) noexcept;
  uint64_t pop() noexcept { return data_[--size_]; }
  bool empty() const noexcept { return size_ == 0; }
  uint64_t size() const noexcept { return size_; }
  uint64_t at(uint64_t i) const noexcept { return data_[i]; }
  void clear() noexcept { size_ = 0; }
 private:
  uint64_t* data_ = nullptr;
  uint64_t size_ = 0, cap_ = 0;
};

struct CompactStats {
  uint64_t live_objects = 0;
  uint64_t live_bytes = 0;
  uint64_t reclaimed_bytes = 0;
  uint64_t evacuated = 0;
  bool aborted = false;
};

// The managed heap + compactor. Move-only; owns no OS resource itself (the
// backing Arena does), so teardown is trivial.
class ManagedHeap {
 public:
  ManagedHeap() noexcept = default;

  // Carve two semispaces of `space_bytes` each from `backing` and initialize the
  // header. Freezes the backing's raw allocator. `backing` must outlive this.
  static Status create(Arena* backing, uint64_t space_bytes, ManagedHeap* out) noexcept;
  // Reattach to a managed heap previously created in `backing`.
  static Status attach(Arena* backing, ManagedHeap* out) noexcept;

  bool valid() const noexcept { return hdr_ != nullptr; }

  // Allocate a managed object of registered type `type_id`. During a cycle the
  // object is born in to-space, pre-marked live. Returns null on space overflow
  // (the caller should compact()).
  ManagedRef alloc(uint32_t type_id) noexcept;

  // READ pointer: canonical payload (follows forwarding during a cycle). Reads
  // never race the compactor (from-space is read-only for the whole cycle), so
  // no scope is needed for reading.
  void* resolveRead(ManagedRef ref) noexcept;

  // WRITE access is scoped: a WriteScope evacuates-on-write (so the write always
  // lands on the canonical to-space copy) AND registers the write as in-flight so
  // the compactor cannot end a cycle - and hence cannot let the next cycle reuse
  // this object's space - while a write to it is outstanding. This is what keeps
  // an app write from straddling a compaction. Hold a WriteScope only for the
  // duration of the write; do not cache its pointer.
  void* beginWrite(ManagedRef ref) noexcept;  // -> canonical ptr; pair with endWrite()
  void endWrite() noexcept;

  Status setRoot(uint32_t idx, ManagedRef ref) noexcept;
  ManagedRef getRoot(uint32_t idx) noexcept;

  // The durable ManagedRef for a canonical payload pointer (obtained from a
  // resolveRead / SelfHealingOffsetPtr::get()). Lets an app navigate by pointer
  // and then take a write pointer for the object it arrived at.
  ManagedRef refOf(void* canonical_payload) noexcept {
    return canonical_payload ? ManagedRef{ offOf(canonical_payload) } : ManagedRef{ManagedRef::kNull};
  }

  // Run a full concurrent compaction cycle on the CALLING thread (intended to be
  // a background thread; it pins itself to the LITTLE cluster for its duration).
  // On success advances the durable commit to the compacted graph. Never blocks
  // a mutator. Returns kOk (compacted), kProbeFailed (aborted safely, e.g. SATB
  // overflow - nothing changed durably), or an error.
  Status compact(CompactStats* stats) noexcept;

  uint64_t liveBumpBytes() noexcept;  // bytes used in the active space

 private:
  // pointer/offset helpers
  uint8_t* base() const noexcept { return static_cast<uint8_t*>(backing_->mutableBase()); }
  uint64_t offOf(void* p) const noexcept { return static_cast<uint64_t>(static_cast<uint8_t*>(p) - base()); }
  ObjectHeader* headerAt(uint64_t payload_off) const noexcept {
    return reinterpret_cast<ObjectHeader*>(base() + payload_off - sizeof(ObjectHeader));
  }
  void* payloadAt(uint64_t payload_off) const noexcept { return base() + payload_off; }
  static uint64_t totalOf(uint32_t type_id) noexcept {
    return sizeof(ObjectHeader) + alignUp(TypeRegistry::fixedSize(type_id), 16);
  }
  uint64_t bumpAllocPacked(uint64_t total, uint32_t* out_space) noexcept;
  void restoreLiveFromCommit(int slot) noexcept;
  int newestValidCommit() const noexcept;
  Status writeCommit(uint32_t active_space, uint64_t bump) noexcept;

  void pinCompactorToLittleCores() noexcept;

  // phases
  void markPhase(CompactStats* s) noexcept;
  void raggedSafepoint() noexcept;                // hands-shake each mutator through an in_write gap
  void evacuatePhase(CompactStats* s) noexcept;   // evacuates the marked from-space set
  void fixupPhase() noexcept;
  uint64_t evacuateObject(uint64_t old_payload_off) noexcept;  // -> new payload off (canonical)

  Arena* backing_ = nullptr;
  ManagedHeader* hdr_ = nullptr;
  uint32_t epoch_ = 0;              // compactor-thread only: never read by a mutator
  uint32_t from_space_ = 0;         // compactor-thread only: which space is being evacuated
  bool inFromSpace(uint64_t off) const noexcept {
    return off >= hdr_->space_off[from_space_] &&
           off <  hdr_->space_off[from_space_] + hdr_->space_bytes;
  }

  // cycle scratch (compactor thread only)
  CycleContext ctx_{};
  OffsetVec worklist_{};
  OffsetVec evac_list_{};
};

// RAII write access to a managed object. Construct it, write through get()/as<T>,
// let it destruct. The write is guaranteed to land on the canonical copy and to
// complete within a single compaction cycle. Move-only, non-copyable.
class WriteScope {
 public:
  WriteScope(ManagedHeap* heap, ManagedRef ref) noexcept
      : heap_(heap), payload_(heap->beginWrite(ref)) {}
  ~WriteScope() noexcept { if (heap_) heap_->endWrite(); }
  WriteScope(const WriteScope&) = delete;
  WriteScope& operator=(const WriteScope&) = delete;
  WriteScope(WriteScope&& o) noexcept : heap_(o.heap_), payload_(o.payload_) { o.heap_ = nullptr; }

  void* get() const noexcept { return payload_; }
  template <typename T> T* as() const noexcept { return static_cast<T*>(payload_); }
  explicit operator bool() const noexcept { return payload_ != nullptr; }

 private:
  ManagedHeap* heap_;
  void* payload_;
};

}  // namespace aevum

#endif  // AEVUM_COMPACTOR_H_
