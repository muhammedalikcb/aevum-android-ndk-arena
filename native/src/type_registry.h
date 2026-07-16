// SPDX-License-Identifier: Apache-2.0
// Aevum-Defrag - object type registry and managed object header.
//
// The Aevum arena has no reflection: a managed object is opaque bytes, so the
// compactor cannot know where the reference fields are unless each type tells
// it. Every managed type registers one O(1) trace function that visits exactly
// its reference fields. The object's `type_id` (stored in its header, and hence
// persisted) indexes this registry, so after process death the app re-registers
// the same id -> trace mapping and the graph is walkable again.
//
// The registry holds code pointers (trace functions), which are never persisted;
// only the small integer `type_id` lives in the region. The app owns the id
// space and must register ids deterministically across runs.

#ifndef AEVUM_TYPE_REGISTRY_H_
#define AEVUM_TYPE_REGISTRY_H_

#include <cstdint>
#include <atomic>

#include "platform.h"

namespace aevum {

inline constexpr uint32_t kMaxTypes = 1024;
inline constexpr uint32_t kMaxRefFields = 32;  // reference fields per layout-registered type

// Per-managed-object header. Sits immediately before the user payload; the
// payload therefore stays 16-byte aligned. Every field here is either
// position-independent or a within-region delta, so the header survives
// re-mapping. Forwarding is a transient, runtime-only concept: it is only ever
// consulted while a compaction cycle is active (see self_healing_ptr.h), so a
// stale value persisted by a crash mid-cycle is inert on reattach.
struct alignas(16) ObjectHeader {
  // 0 == not forwarded. Otherwise the SIGNED within-region delta
  // (newPayloadOffset - oldPayloadOffset). Because it is a difference of two
  // offsets in the same region, it is invariant under re-mapping. Installed
  // exactly once per cycle via CAS(0 -> delta), store-release.
  std::atomic<int64_t> fwd;
  // Live-epoch stamp. Equals the compactor's current epoch iff this object was
  // marked reachable in the in-progress (or last) cycle.
  std::atomic<uint32_t> mark;
  uint32_t type_id;

  static constexpr int64_t kNotForwarded = 0;
};
static_assert(sizeof(ObjectHeader) == 16, "payload alignment depends on a 16-byte object header");

// A managed reference: a base-relative offset to a managed object's PAYLOAD
// (the ObjectHeader is at payload_off - sizeof(ObjectHeader)). Durable across
// process death, like Handle, but distinguished by type for the managed heap.
struct ManagedRef {
  uint64_t off;                       // 0 == null
  static constexpr uint64_t kNull = 0;
  bool isNull() const noexcept { return off == kNull; }
};

// The visitor is invoked once per reference field with the ADDRESS of that
// field (a SelfHealingOffsetPtr, passed as void* to avoid a header cycle). The
// trace function reads the object's fixed layout and hands each ref field to the
// visitor - O(1) in the number of fields, no scanning, no reflection.
using FieldVisitor = void (*)(void* ctx, void* ref_field) noexcept;
using TraceFn      = void (*)(void* payload, void* ctx, FieldVisitor visit) noexcept;

// Process-global type table. Registration is expected at startup / after attach,
// before any compaction. Lookups on the hot path are a single relaxed load.
class TypeRegistry {
 public:
  // Register (or re-register, idempotently) a type. `fixed_size` is the payload
  // size for fixed-layout types, or 0 for variable-size objects whose true size
  // the caller records elsewhere (the compactor then uses the block capacity).
  static Status registerType(uint32_t type_id, TraceFn trace, uint32_t fixed_size) noexcept {
    if (type_id >= kMaxTypes || trace == nullptr) return Status::kInvalidArg;
    slots()[type_id].size.store(fixed_size, std::memory_order_relaxed);
    // Publish the trace pointer last, with release, so a reader that acquires it
    // also sees the size.
    slots()[type_id].trace.store(reinterpret_cast<void*>(trace), std::memory_order_release);
    return Status::kOk;
  }

  // Register a type by the byte offsets of its SelfHealingOffsetPtr fields. This
  // is the reflection-free path a managed runtime (Kotlin/JNI) uses: it need only
  // report where the pointers are. A built-in generic trace walks the offsets.
  static Status registerLayout(uint32_t type_id, const uint32_t* offsets,
                               uint32_t count, uint32_t fixed_size) noexcept {
    if (type_id >= kMaxTypes || count > kMaxRefFields) return Status::kInvalidArg;
    Slot& s = slots()[type_id];
    for (uint32_t i = 0; i < count; ++i) s.offsets[i] = offsets[i];
    s.num_offsets = count;
    s.size.store(fixed_size, std::memory_order_relaxed);
    s.trace.store(nullptr, std::memory_order_relaxed);   // use the generic offset walk
    s.has_layout.store(true, std::memory_order_release);
    return Status::kOk;
  }

  static TraceFn trace(uint32_t type_id) noexcept {
    if (type_id >= kMaxTypes) return nullptr;
    return reinterpret_cast<TraceFn>(slots()[type_id].trace.load(std::memory_order_acquire));
  }
  static uint32_t fixedSize(uint32_t type_id) noexcept {
    if (type_id >= kMaxTypes) return 0;
    return slots()[type_id].size.load(std::memory_order_relaxed);
  }

  // The single tracing entry point used everywhere in the compactor: dispatch to
  // a registered native TraceFn, or walk registered field offsets. Either way,
  // every reference field of `payload` is handed to `visit`.
  static void forEachRef(uint32_t type_id, void* payload, void* ctx, FieldVisitor visit) noexcept {
    if (type_id >= kMaxTypes) return;
    Slot& s = slots()[type_id];
    if (TraceFn tf = reinterpret_cast<TraceFn>(s.trace.load(std::memory_order_acquire))) {
      tf(payload, ctx, visit);
      return;
    }
    if (s.has_layout.load(std::memory_order_acquire)) {
      auto* base = static_cast<uint8_t*>(payload);
      const uint32_t n = s.num_offsets;
      for (uint32_t i = 0; i < n; ++i) visit(ctx, base + s.offsets[i]);
    }
  }

 private:
  struct Slot {
    std::atomic<void*> trace{nullptr};
    std::atomic<uint32_t> size{0};
    std::atomic<bool> has_layout{false};
    uint32_t num_offsets = 0;
    uint32_t offsets[kMaxRefFields] = {0};
  };
  // Function-local static: zero-initialized, no static-init-order fiasco.
  static Slot* slots() noexcept {
    static Slot table[kMaxTypes];
    return table;
  }
};

}  // namespace aevum

#endif  // AEVUM_TYPE_REGISTRY_H_
