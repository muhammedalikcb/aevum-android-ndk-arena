// SPDX-License-Identifier: Apache-2.0
#include "compactor.h"

#include <cstdlib>
#include <cstring>
#include <new>

#include "profiler.h"

#if defined(__linux__)
#  include <unistd.h>
#  include <fcntl.h>
#  include <sched.h>
#endif

namespace aevum {

// ---------------------------------------------------------------------------
// OffsetVec - malloc-backed growable stack (compactor scratch, never in arena).
// ---------------------------------------------------------------------------
bool OffsetVec::push(uint64_t v) noexcept {
  if (size_ == cap_) {
    uint64_t ncap = cap_ ? cap_ * 2 : 1024;
    auto* nd = static_cast<uint64_t*>(std::realloc(data_, ncap * sizeof(uint64_t)));
    if (!nd) return false;
    data_ = nd; cap_ = ncap;
  }
  data_[size_++] = v;
  return true;
}

// ---------------------------------------------------------------------------
// Small SATB visitor helper: trace over an object's reference fields.
// ---------------------------------------------------------------------------
namespace {
struct TraceCtx {
  void (*fn)(TraceCtx*, SelfHealingRefBase*) noexcept;
  ManagedHeap* heap;
  void* user;
};
void invokeVisitor(void* ctx, void* ref_field) noexcept {
  auto* tc = static_cast<TraceCtx*>(ctx);
  tc->fn(tc, reinterpret_cast<SelfHealingRefBase*>(ref_field));
}
}  // namespace

// ---------------------------------------------------------------------------
// create / attach
// ---------------------------------------------------------------------------
Status ManagedHeap::create(Arena* backing, uint64_t space_bytes, ManagedHeap* out) noexcept {
  if (!backing || !backing->valid() || !out) return Status::kInvalidArg;
  const uint64_t ds = backing->dataStart();
  const uint64_t rb = backing->regionBytes();
  const uint64_t hdr_bytes = alignUp(sizeof(ManagedHeader), kCacheLine);
  space_bytes = alignUp(space_bytes, 16);
  // Two spaces plus the header must fit in the region.
  if (ds + hdr_bytes + 2 * space_bytes > rb) return Status::kNoMemory;

  auto* base = static_cast<uint8_t*>(backing->mutableBase());
  auto* h = reinterpret_cast<ManagedHeader*>(base + ds);
  std::memset(h, 0, sizeof(ManagedHeader));
  h->magic = kManagedMagic;
  h->version = 1;
  h->space_bytes = space_bytes;
  h->space_off[0] = ds + hdr_bytes;
  h->space_off[1] = ds + hdr_bytes + space_bytes;
  h->live_alloc.store((uint64_t{0} << 63) | h->space_off[0], std::memory_order_relaxed);
  for (uint32_t i = 0; i < kManagedRoots; ++i)
    h->live_root[i].store(ManagedRef::kNull, std::memory_order_relaxed);

  // Seed both durable checkpoints; slot 0 valid (gen 1, space 0), slot 1 older.
  ManagedCommit& c0 = h->commit[0];
  std::memset(&c0, 0, sizeof(ManagedCommit));
  c0.generation = 1; c0.active_space = 0; c0.bump = h->space_off[0];
  c0.crc = 0; c0.crc = crc32c(&c0, sizeof(ManagedCommit));
  ManagedCommit& c1 = h->commit[1];
  std::memset(&c1, 0, sizeof(ManagedCommit));
  c1.crc = crc32c(&c1, sizeof(ManagedCommit));
  h->active_commit.store(0, std::memory_order_release);

  out->backing_ = backing;
  out->hdr_ = h;
  backing->reserveAllForManaged();     // disable the raw allocator on this region
  return backing->commit();            // persist the fresh managed header
}

int ManagedHeap::newestValidCommit() const noexcept {
  int chosen = -1; uint64_t best = 0;
  for (int s = 0; s < 2; ++s) {
    ManagedCommit c = hdr_->commit[s];
    uint32_t stored = c.crc; c.crc = 0; c.reserved = 0;
    if (crc32c(&c, sizeof(ManagedCommit)) != stored) continue;
    if (chosen < 0 || hdr_->commit[s].generation >= best) { chosen = s; best = hdr_->commit[s].generation; }
  }
  return chosen;
}

void ManagedHeap::restoreLiveFromCommit(int slot) noexcept {
  const ManagedCommit& c = hdr_->commit[slot];
  hdr_->live_alloc.store((c.active_space << 63) | (c.bump & kBumpMask), std::memory_order_relaxed);
  for (uint32_t i = 0; i < kManagedRoots; ++i)
    hdr_->live_root[i].store(c.root[i], std::memory_order_relaxed);
  hdr_->active_commit.store(static_cast<uint64_t>(slot), std::memory_order_release);
}

Status ManagedHeap::attach(Arena* backing, ManagedHeap* out) noexcept {
  if (!backing || !backing->valid() || !out) return Status::kInvalidArg;
  const uint64_t ds = backing->dataStart();
  const uint64_t rb = backing->regionBytes();
  const uint64_t hdr_bytes = alignUp(sizeof(ManagedHeader), kCacheLine);
  if (ds > rb || rb - ds < hdr_bytes) return Status::kCorrupt;
  auto* base = static_cast<uint8_t*>(backing->mutableBase());
  auto* h = reinterpret_cast<ManagedHeader*>(base + ds);
  if (h->magic != kManagedMagic) return Status::kCorrupt;
  // The ref bounds checks trust this geometry, so make sure both semispaces sit
  // after the header and inside the mapping, laid out exactly as create() does.
  if (h->space_bytes == 0 || h->space_off[0] < ds + hdr_bytes || h->space_off[0] > rb ||
      h->space_bytes > (rb - h->space_off[0]) / 2 ||
      h->space_off[1] != h->space_off[0] + h->space_bytes)
    return Status::kCorrupt;
  out->backing_ = backing;
  out->hdr_ = h;
  const int slot = out->newestValidCommit();
  if (slot < 0) return Status::kCorrupt;
  out->restoreLiveFromCommit(slot);    // discards any half-finished compaction
  backing->reserveAllForManaged();
  return Status::kOk;
}

// ---------------------------------------------------------------------------
// allocation
// ---------------------------------------------------------------------------
uint64_t ManagedHeap::bumpAllocPacked(uint64_t total, uint32_t* out_space) noexcept {
  uint64_t cur = hdr_->live_alloc.load(std::memory_order_acquire);
  for (;;) {
    const uint32_t space = static_cast<uint32_t>(cur >> 63);
    const uint64_t bump = cur & kBumpMask;
    const uint64_t aligned = alignUp(bump, 16);
    const uint64_t nb = aligned + total;
    if (nb > hdr_->space_off[space] + hdr_->space_bytes) return 0;  // space full
    const uint64_t packed = (uint64_t{space} << 63) | nb;
    if (hdr_->live_alloc.compare_exchange_weak(cur, packed,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
      *out_space = space;
      return aligned;  // header offset (block start)
    }
  }
}

ManagedRef ManagedHeap::alloc(uint32_t type_id) noexcept {
  if (!valid()) return ManagedRef{ManagedRef::kNull};
  const uint32_t payload_size = TypeRegistry::fixedSize(type_id);
  if (payload_size == 0) return ManagedRef{ManagedRef::kNull};  // unregistered / zero-size
  const uint64_t total = sizeof(ObjectHeader) + alignUp(payload_size, 16);

  // Bracket bump+initialize behind this thread's in_write flag so the ragged
  // safepoint can guarantee the object is fully formed before evacuation could
  // ever read its space. store-then-seq_cst pairing with raggedSafepoint().
  MutatorContext* m = mutatorSelf();
  if (m) m->in_write.fetch_add(1, std::memory_order_seq_cst);
  uint32_t space;
  const uint64_t hdr_off = bumpAllocPacked(total, &space);
  if (hdr_off == 0) {
    if (m) m->in_write.fetch_sub(1, std::memory_order_seq_cst);
    return ManagedRef{ManagedRef::kNull};  // space full - caller should compact()
  }
  const uint64_t payload_off = hdr_off + sizeof(ObjectHeader);

  // Zero the payload, then null every reference field, so a trace of a
  // not-yet-populated object never follows a stale pointer from a previous
  // occupant of this recycled space.
  std::memset(payloadAt(payload_off), 0, alignUp(payload_size, 16));
  auto* h = headerAt(payload_off);
  h->fwd.store(ObjectHeader::kNotForwarded, std::memory_order_relaxed);
  h->type_id = type_id;
  // A cycle-time allocation lands in to-space (the collector never evacuates
  // to-space), so it survives regardless of mark; a constant avoids any shared
  // epoch read.
  h->mark.store(0, std::memory_order_relaxed);
  {
    TraceCtx tc{[](TraceCtx*, SelfHealingRefBase* f) noexcept { f->rawSetRel(kShNull); }, this, nullptr};
    TypeRegistry::forEachRef(type_id, payloadAt(payload_off), &tc, &invokeVisitor);
  }
  std::atomic_thread_fence(std::memory_order_release);
  if (m) m->in_write.fetch_sub(1, std::memory_order_seq_cst);
  return ManagedRef{payload_off};
}

// ---------------------------------------------------------------------------
// resolve (read / write barriers surfaced as heap ops)
// ---------------------------------------------------------------------------
namespace {
// Follow a forwarding chain from an absolute payload pointer to the canonical
// copy. Safe to call any time: fwd == 0 means not forwarded.
inline uint8_t* followFwd(uint8_t* payload) noexcept {
  for (;;) {
    auto* h = reinterpret_cast<ObjectHeader*>(payload - sizeof(ObjectHeader));
    const int64_t f = h->fwd.load(std::memory_order_acquire);
    if (f == ObjectHeader::kNotForwarded) return payload;
    payload += f;
  }
}
}  // namespace

void* ManagedHeap::resolveRead(ManagedRef ref) noexcept {
  if (!refInHeap(ref)) return nullptr;
  return followFwd(base() + ref.off);
}

uint64_t ManagedHeap::payloadBytes(ManagedRef ref) const noexcept {
  if (!refInHeap(ref)) return 0;
  const uint64_t bytes = alignUp(TypeRegistry::fixedSize(headerAt(ref.off)->type_id), 16);
  const uint32_t space = (ref.off >= hdr_->space_off[1]) ? 1u : 0u;
  const uint64_t end = hdr_->space_off[space] + hdr_->space_bytes;
  return bytes <= end - ref.off ? bytes : 0;
}

bool ManagedHeap::refFieldOk(ManagedRef ref, uint32_t field_offset) const noexcept {
  const uint64_t bytes = payloadBytes(ref);
  if (bytes < kRefFieldBytes || field_offset > bytes - kRefFieldBytes ||
      (field_offset % kRefFieldBytes) != 0)
    return false;
  return TypeRegistry::isRefField(headerAt(ref.off)->type_id, field_offset);
}

void* ManagedHeap::beginWrite(ManagedRef ref) noexcept {
  // Announce this thread as writing FIRST, then read the phase - both seq_cst.
  // This store-before-load pairs with raggedSafepoint()'s store(phase)-then-
  // load(in_write): if the compactor observes this thread in a gap (in_write==0)
  // it is guaranteed our NEXT phase read sees the evacuating phase and redirects
  // the write to to-space, so no write can land in from-space during evacuation.
  MutatorContext* m = mutatorSelf();
  if (m) m->in_write.fetch_add(1, std::memory_order_seq_cst);
  if (!refInHeap(ref)) return nullptr;   // still counted: the caller pairs with endWrite()

  CycleContext* c = activeCycle().load(std::memory_order_acquire);
  if (c != nullptr && c->phase.load(std::memory_order_seq_cst) >= Phase::kEvac) {
    // From-space membership from IMMUTABLE space bounds + the ATOMIC alloc cursor
    // (its space bit is to-space during a cycle). If the object is in the
    // non-active (from) space, cooperatively evacuate so the write lands on the
    // canonical to-space copy.
    const uint32_t alloc_space =
        static_cast<uint32_t>(hdr_->live_alloc.load(std::memory_order_acquire) >> 63);
    const uint32_t obj_space = (ref.off >= hdr_->space_off[1]) ? 1u : 0u;
    if (obj_space != alloc_space) return base() + evacuateObject(ref.off);
  }
  return followFwd(base() + ref.off);  // already canonical, or no active evacuation
}

void ManagedHeap::endWrite() noexcept {
  if (MutatorContext* m = mutatorSelf()) m->in_write.fetch_sub(1, std::memory_order_seq_cst);
}

Status ManagedHeap::setRoot(uint32_t idx, ManagedRef ref) noexcept {
  if (!valid() || idx >= kManagedRoots) return Status::kInvalidArg;
  if (!ref.isNull() && !refInHeap(ref)) return Status::kInvalidArg;
  hdr_->live_root[idx].store(ref.off, std::memory_order_release);
  return Status::kOk;
}

ManagedRef ManagedHeap::getRoot(uint32_t idx) noexcept {
  if (!valid() || idx >= kManagedRoots) return ManagedRef{ManagedRef::kNull};
  const uint64_t off = hdr_->live_root[idx].load(std::memory_order_acquire);
  if (off == ManagedRef::kNull) return ManagedRef{ManagedRef::kNull};
  return ManagedRef{ offOf(followFwd(base() + off)) };  // follow forwarding during a cycle
}

uint64_t ManagedHeap::liveBumpBytes() noexcept {
  const uint64_t cur = hdr_->live_alloc.load(std::memory_order_acquire);
  const uint32_t space = static_cast<uint32_t>(cur >> 63);
  return (cur & kBumpMask) - hdr_->space_off[space];
}

// ---------------------------------------------------------------------------
// evacuation of a single object (compactor AND cooperative mutators)
// ---------------------------------------------------------------------------
uint64_t ManagedHeap::evacuateObject(uint64_t old_payload_off) noexcept {
  auto* oh = headerAt(old_payload_off);
  int64_t f = oh->fwd.load(std::memory_order_acquire);
  if (f != ObjectHeader::kNotForwarded) return old_payload_off + static_cast<uint64_t>(f);

  const uint32_t type_id = oh->type_id;
  const uint32_t payload_size = TypeRegistry::fixedSize(type_id);
  const uint64_t total = sizeof(ObjectHeader) + alignUp(payload_size, 16);

  uint32_t space;
  const uint64_t new_hdr_off = bumpAllocPacked(total, &space);
  if (new_hdr_off == 0) return old_payload_off;  // to-space full - cannot happen if sized == from
  const uint64_t new_payload_off = new_hdr_off + sizeof(ObjectHeader);

  // Set the new header's fields explicitly (never memcpy the OLD header: its
  // atomic fwd is being raced by cooperating evacuators' CAS). Then copy ONLY
  // the payload - from-space payloads are strictly read-only during kEvac (the
  // read barrier does not self-heal from-space fields), so this is race-free.
  auto* nh = headerAt(new_payload_off);
  nh->fwd.store(ObjectHeader::kNotForwarded, std::memory_order_relaxed);
  nh->mark.store(0, std::memory_order_relaxed);  // to-space; mark unused this cycle
  nh->type_id = type_id;
  std::memcpy(base() + new_payload_off, base() + old_payload_off, alignUp(payload_size, 16));

  // Re-base every reference field by -D so the copy points at the SAME targets
  // the original did (those targets are fixed up to their to-space copies in
  // Phase 3). D is a within-region delta, invariant under re-mapping.
  const int64_t D = static_cast<int64_t>(new_payload_off) - static_cast<int64_t>(old_payload_off);
  {
    struct Rebase { int64_t D; };
    Rebase rb{D};
    TraceCtx tc{nullptr, this, &rb};
    tc.fn = [](TraceCtx* c, SelfHealingRefBase* fld) noexcept {
      const int64_t r = fld->rawRel();
      if (r != kShNull) fld->rawSetRel(r - static_cast<Rebase*>(c->user)->D);
    };
    TypeRegistry::forEachRef(type_id, payloadAt(new_payload_off), &tc, &invokeVisitor);
  }

  // Publish: install forwarding on the OLD header. Winner-take-all so the
  // compactor and any cooperating mutator converge on one canonical copy.
  int64_t expected = ObjectHeader::kNotForwarded;
  if (oh->fwd.compare_exchange_strong(expected, D,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
    return new_payload_off;                       // we won
  }
  return old_payload_off + static_cast<uint64_t>(expected);  // lost race; our copy leaks until flip
}

// ---------------------------------------------------------------------------
// Phase 1: concurrent SATB marking
// ---------------------------------------------------------------------------
void ManagedHeap::markPhase(CompactStats* s) noexcept {
  auto tryMark = [&](uint64_t payload_off) noexcept {
    if (payload_off == ManagedRef::kNull) return;
    auto* h = headerAt(payload_off);
    if (h->mark.load(std::memory_order_relaxed) == epoch_) return;  // already live
    h->mark.store(epoch_, std::memory_order_relaxed);
    worklist_.push(payload_off);                       // traverse (from OR to space)
    if (inFromSpace(payload_off - sizeof(ObjectHeader))) {
      // Only from-space objects are evacuated. Recording the reachable set here
      // (rather than a linear scan) means the compactor only ever touches fully
      // initialized, linked objects - never a slot a concurrent allocation is
      // still writing - which is what removes the last scan/alloc data race.
      evac_list_.push(payload_off);
      s->live_objects++;
      s->live_bytes += sizeof(ObjectHeader) + alignUp(TypeRegistry::fixedSize(h->type_id), 16);
    }
  };

  auto traceOne = [&](uint64_t payload_off) noexcept {
    auto* h = headerAt(payload_off);
    struct MC { ManagedHeap* heap; decltype(tryMark)* mark; };
    MC mc{this, &tryMark};
    TraceCtx tc{nullptr, this, &mc};
    tc.fn = [](TraceCtx* c, SelfHealingRefBase* fld) noexcept {
      void* t = fld->rawTarget();               // no forwarding during kMark
      if (t) {
        auto* mc = static_cast<MC*>(c->user);
        (*mc->mark)(mc->heap->offOf(t));
      }
    };
    TypeRegistry::forEachRef(h->type_id, payloadAt(payload_off), &tc, &invokeVisitor);
  };

  // Seed from durable roots.
  for (uint32_t i = 0; i < kManagedRoots; ++i)
    tryMark(hdr_->live_root[i].load(std::memory_order_acquire));

  // Mark to a fixpoint, interleaving drains of EVERY mutator thread's own SATB
  // ring. Bounded pass count guards against mutators deleting references faster
  // than we can drain - in which case the whole cycle aborts cleanly (nothing
  // has been evacuated yet).
  for (int pass = 0; pass < 64; ++pass) {
    while (!worklist_.empty()) traceOne(worklist_.pop());
    bool all_empty = true;
    for (MutatorContext* m = mutatorListHead(); m != nullptr; m = m->next) {
      if (m->satb.overflowed()) { s->aborted = true; return; }
      m->satb.drain([&](void* p) noexcept { tryMark(offOf(p)); });
      if (!m->satb.empty()) all_empty = false;
    }
    if (worklist_.empty() && all_empty) return;  // converged
  }
  s->aborted = true;  // did not converge
}

// ---------------------------------------------------------------------------
// Ragged safepoint: make from-space read-only for evacuation WITHOUT a global
// stop. Phase is already at kEvac. Walk the mutator registry and, for each
// thread, wait until it is observed once in an `in_write == 0` gap. Threads
// never wait on each other; each is caught between write scopes within one
// scope's time. After a thread is observed in a gap, the seq_cst pairing in
// beginWrite guarantees its subsequent writes see kEvac and redirect to to-space,
// so no write can touch from-space from here on.
// ---------------------------------------------------------------------------
void ManagedHeap::raggedSafepoint() noexcept {
  // Re-scan until the registry head is stable, so a thread that registers mid-
  // handshake is still caught. A thread that registers AFTER the last scan
  // necessarily performs its first phase read after our kEvac store is globally
  // visible, so it redirects to to-space on its own and needs no check.
  MutatorContext* checked_until = nullptr;
  for (;;) {
    MutatorContext* head = mutatorListHead();
    if (head == checked_until) break;
    for (MutatorContext* m = head; m != checked_until; m = m->next) {
      while (m->in_write.load(std::memory_order_seq_cst) != 0) {
#if defined(__aarch64__)
        asm volatile("yield");   // CPU hint only (compactor thread, never a mutator)
#endif
      }
    }
    checked_until = head;
  }
}

// ---------------------------------------------------------------------------
// Phase 2: evacuation of the marked from-space set (no linear scan).
// evac_list_ holds the OLD (from-space) payload offsets discovered by marking.
// ---------------------------------------------------------------------------
void ManagedHeap::evacuatePhase(CompactStats* s) noexcept {
  for (uint64_t i = 0; i < evac_list_.size(); ++i) {
    evacuateObject(evac_list_.at(i));  // installs forwarding (idempotent w.r.t. cooperators)
    s->evacuated++;
  }
}

// ---------------------------------------------------------------------------
// Phase 3: proactive pointer fixup + root fixup. Walk each evacuated object's
// TO-space copy and rewrite its reference fields (currently pointing at the
// originals) to the originals' canonical to-space copies.
// ---------------------------------------------------------------------------
void ManagedHeap::fixupPhase() noexcept {
  auto fixField = [](void* /*ctx*/, void* ref_field) noexcept {
    auto* fld = reinterpret_cast<SelfHealingRefBase*>(ref_field);
    void* t = fld->rawTarget();
    if (!t) return;
    auto* canonical = followFwd(static_cast<uint8_t*>(t));
    if (canonical != t)
      fld->rawSetRel(canonical - reinterpret_cast<uint8_t*>(fld));
  };
  for (uint64_t i = 0; i < evac_list_.size(); ++i) {
    const uint64_t old_payload_off = evac_list_.at(i);
    auto* newp = followFwd(base() + old_payload_off);      // its to-space copy
    auto* h = reinterpret_cast<ObjectHeader*>(newp - sizeof(ObjectHeader));
    TypeRegistry::forEachRef(h->type_id, newp, this, fixField);
  }
  // Fix roots to their canonical (to-space) copies.
  for (uint32_t i = 0; i < kManagedRoots; ++i) {
    const uint64_t off = hdr_->live_root[i].load(std::memory_order_acquire);
    if (off == ManagedRef::kNull) continue;
    auto* canonical = followFwd(base() + off);
    hdr_->live_root[i].store(offOf(canonical), std::memory_order_release);
  }
}

// ---------------------------------------------------------------------------
// durable commit of the compacted graph
// ---------------------------------------------------------------------------
Status ManagedHeap::writeCommit(uint32_t active_space, uint64_t bump) noexcept {
  const uint64_t cur = hdr_->active_commit.load(std::memory_order_acquire);
  const uint64_t nxt = 1 - cur;
  ManagedCommit& c = hdr_->commit[nxt];
  c.generation = hdr_->commit[cur].generation + 1;
  c.active_space = active_space;
  c.bump = bump;
  for (uint32_t i = 0; i < kManagedRoots; ++i)
    c.root[i] = hdr_->live_root[i].load(std::memory_order_acquire);
  c.reserved = 0;
  c.crc = 0;
  c.crc = crc32c(&c, sizeof(ManagedCommit));
  // Flush region (arena msyncs everything), publish the slot, flush again.
  Status st = backing_->commit();
  if (!ok(st)) return st;
  hdr_->active_commit.store(nxt, std::memory_order_release);
  return backing_->commit();
}

// ---------------------------------------------------------------------------
// E-core affinity for the compactor thread
// ---------------------------------------------------------------------------
void ManagedHeap::pinCompactorToLittleCores() noexcept {
#if defined(__linux__)
  // Read cpu_capacity for each online CPU; pin to the minimum-capacity cluster.
  uint64_t cap[256];
  int ncpu = 0;
  uint64_t min_cap = ~0ull;
  for (int i = 0; i < 256; ++i) {
    char path[64];
    // build "/sys/devices/system/cpu/cpuN/cpu_capacity"
    int n = 0;
    const char* a = "/sys/devices/system/cpu/cpu";
    while (a[n]) { path[n] = a[n]; ++n; }
    n += 0;
    int v = i, digits = 0, tmp = v; char rev[8];
    do { rev[digits++] = char('0' + tmp % 10); tmp /= 10; } while (tmp);
    for (int d = digits - 1; d >= 0; --d) path[n++] = rev[d];
    const char* b = "/cpu_capacity";
    int m = 0; while (b[m]) path[n++] = b[m++];
    path[n] = '\0';
    int fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { cap[i] = 0; if (i > 0) { ncpu = i; break; } else { ncpu = 0; continue; } }
    char buf[16]; ssize_t r = ::read(fd, buf, sizeof(buf) - 1); ::close(fd);
    uint64_t val = 0;
    for (ssize_t k = 0; k < r && buf[k] >= '0' && buf[k] <= '9'; ++k) val = val * 10 + (buf[k] - '0');
    cap[i] = val; ncpu = i + 1;
    if (val && val < min_cap) min_cap = val;
  }
  if (ncpu <= 0 || min_cap == ~0ull) return;  // capacities unreadable - leave scheduling to EAS
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int i = 0; i < ncpu; ++i)
    if (cap[i] == min_cap) CPU_SET(i, &set);
  ::sched_setaffinity(0, sizeof(set), &set);  // best-effort; intersected with the app cpuset
#endif
}

// ---------------------------------------------------------------------------
// compact() - the full concurrent cycle
// ---------------------------------------------------------------------------
Status ManagedHeap::compact(CompactStats* stats) noexcept {
  AEVUM_TRACE("Aevum.compact");
  CompactStats local{};
  CompactStats* s = stats ? stats : &local;
  *s = CompactStats{};
  if (!valid()) return Status::kInvalidArg;
  if (activeCycle().load(std::memory_order_acquire) != nullptr) return Status::kAlreadyAttached;

  pinCompactorToLittleCores();
  worklist_.clear();
  evac_list_.clear();

  // Clear every registered thread's SATB ring before we start (buffers are
  // quiescent here - no cycle is published yet, so storeRef pushes nothing). This
  // discards any stale entries a prior aborted cycle may have left behind.
  for (MutatorContext* m = mutatorListHead(); m != nullptr; m = m->next) {
    m->satb.drain([](void*) noexcept {});
    m->satb.resetOverflow();
  }

  // Ragged safepoint BEFORE the flip: drain any cooperative evacuation still in
  // flight from the previous cycle. Such an evacuation captured the OLD live_alloc
  // as its destination space; if it straddled the flip below it would land its
  // copy in the wrong (about-to-be from) space. No cycle is published yet, so no
  // NEW cooperative evacuation can begin while we drain.
  raggedSafepoint();

  const uint64_t cur = hdr_->live_alloc.load(std::memory_order_acquire);
  const uint32_t from_space = static_cast<uint32_t>(cur >> 63);
  const uint32_t to_space = 1u - from_space;
  from_space_ = from_space;   // compactor-private: drives inFromSpace() during marking
  ++epoch_;

  // Publish the (tiny, mutator-visible) cycle context: just the phase word.
  ctx_.phase.store(Phase::kMark, std::memory_order_release);
  activeCycle().store(&ctx_, std::memory_order_release);

  // Atomically flip allocation to to-space AND capture the from-space high-water.
  const uint64_t old_alloc = hdr_->live_alloc.exchange(
      (uint64_t{to_space} << 63) | hdr_->space_off[to_space], std::memory_order_acq_rel);
  const uint64_t from_used = (old_alloc & kBumpMask) - hdr_->space_off[from_space];

  // Phase 1: mark (concurrent, SATB) - builds the reachable from-space set.
  { AEVUM_TRACE("Aevum.mark"); markPhase(s); }
  if (s->aborted) {
    // Roll back cleanly: restore allocation to from-space, drop the cycle. No
    // object was evacuated, so the graph is untouched.
    hdr_->live_alloc.store(old_alloc, std::memory_order_release);
    ctx_.phase.store(Phase::kIdle, std::memory_order_release);
    activeCycle().store(nullptr, std::memory_order_release);
    return Status::kProbeFailed;
  }
  s->reclaimed_bytes = (from_used >= s->live_bytes) ? (from_used - s->live_bytes) : 0;

  // Move to evacuation, then RAGGED-SAFEPOINT the mutators: this makes from-space
  // read-only without stopping the world. seq_cst so it pairs with beginWrite's
  // announce-then-read. After it returns, every mutator's writes target to-space.
  ctx_.phase.store(Phase::kEvac, std::memory_order_seq_cst);
  { AEVUM_TRACE("Aevum.safepoint"); raggedSafepoint(); }

  // Phase 2: evacuate the marked set (from-space is now write-quiescent).
  { AEVUM_TRACE("Aevum.evacuate"); evacuatePhase(s); }
  AEVUM_COUNTER("Aevum.live_bytes", static_cast<int64_t>(s->live_bytes));
  AEVUM_COUNTER("Aevum.reclaimed_bytes", static_cast<int64_t>(s->reclaimed_bytes));

  // Phase 3: proactive fixup + roots. After this, every live pointer and root
  // names its to-space copy directly, so the forwarding barrier is no longer
  // needed by reads.
  ctx_.phase.store(Phase::kFixup, std::memory_order_release);
  { AEVUM_TRACE("Aevum.fixup"); fixupPhase(); }

  // End the cycle. No global drain is needed: the NEXT cycle's ragged safepoint
  // (at its own kMark->kEvac) is what makes its from-space - this cycle's
  // to-space - read-only before it evacuates, so a write lingering out of this
  // cycle is handed off cleanly there. Lowering the phase first (seq_cst) means
  // any write starting now sees kIdle and targets to-space directly.
  ctx_.phase.store(Phase::kIdle, std::memory_order_seq_cst);

  // Durable commit of the compacted graph (active_space -> to_space).
  const uint64_t to_bump = hdr_->live_alloc.load(std::memory_order_acquire) & kBumpMask;
  Status cst;
  { AEVUM_TRACE("Aevum.commit"); cst = writeCommit(to_space, to_bump); }

  // Retire the cycle. Allocation already targets to_space; forwarding is inert.
  activeCycle().store(nullptr, std::memory_order_release);
  return cst;
}

}  // namespace aevum
